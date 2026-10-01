/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <d3d9.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace sourceengine {

struct EyeAdaptation {
  float percent_bright_pixels = 20.f;
  float percent_target = 60.f;
  float min_avg_lum = 3.f;
  float exposure_min = 2.f;
  float exposure_max = 4.f;
  float rate = 1.f;
  float accelerate_down = 3.f;
  float exposure = 1.f;

  void Update(IDirect3DDevice9* device, IDirect3DTexture9* scene, float light_scale) {
    if (!EnsureResources(device)) return;
    IDirect3DSurface9* source = nullptr;
    IDirect3DSurface9* sample = nullptr;
    if (SUCCEEDED(scene->GetSurfaceLevel(0, &source)) && SUCCEEDED(sample_texture_->GetSurfaceLevel(0, &sample))) {
      if (SUCCEEDED(device->StretchRect(source, nullptr, sample, nullptr, D3DTEXF_LINEAR))) {
        readback_valid_[readback_index_] = SUCCEEDED(device->GetRenderTargetData(sample, readback_[readback_index_]));
      }
    }
    if (sample != nullptr) sample->Release();
    if (source != nullptr) source->Release();
    readback_index_ ^= 1;
    if (readback_valid_[readback_index_]) Measure(light_scale);
    Advance();
  }

  void Release() {
    if (sample_texture_ != nullptr) sample_texture_->Release();
    sample_texture_ = nullptr;
    for (auto*& surface : readback_) {
      if (surface != nullptr) surface->Release();
      surface = nullptr;
    }
    readback_valid_[0] = readback_valid_[1] = false;
    device_ = nullptr;
  }

 private:
  static constexpr UINT WIDTH = 128;
  static constexpr UINT HEIGHT = 72;
  static constexpr int AVERAGE_COUNT = 10;

  static float HalfToFloat(uint16_t half) {
    const uint32_t sign = (half & 0x8000u) << 16;
    uint32_t exponent = (half >> 10) & 0x1Fu;
    uint32_t mantissa = half & 0x3FFu;
    uint32_t bits;
    if (exponent == 0u) {
      if (mantissa == 0u) {
        bits = sign;
      } else {
        exponent = 127u - 15u + 1u;
        while ((mantissa & 0x400u) == 0u) {
          mantissa <<= 1;
          --exponent;
        }
        bits = sign | (exponent << 23) | ((mantissa & 0x3FFu) << 13);
      }
    } else if (exponent == 0x1Fu) {
      bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
      bits = sign | ((exponent + 127u - 15u) << 23) | (mantissa << 13);
    }
    float value;
    memcpy(&value, &bits, sizeof(value));
    return value;
  }

  static float Encode(float linear) {
    linear = std::clamp(linear, 0.f, 1.f);
    return linear <= 0.0031308f ? linear * 12.92f : 1.055f * std::pow(linear, 1.f / 2.4f) - 0.055f;
  }

  bool EnsureResources(IDirect3DDevice9* device) {
    if (device_ != device) Release();
    device_ = device;
    if (sample_texture_ == nullptr) {
      device->CreateTexture(WIDTH, HEIGHT, 1, D3DUSAGE_RENDERTARGET, D3DFMT_A16B16G16R16F, D3DPOOL_DEFAULT, &sample_texture_, nullptr);
    }
    for (auto*& surface : readback_) {
      if (surface == nullptr) device->CreateOffscreenPlainSurface(WIDTH, HEIGHT, D3DFMT_A16B16G16R16F, D3DPOOL_SYSTEMMEM, &surface, nullptr);
    }
    return sample_texture_ != nullptr && readback_[0] != nullptr && readback_[1] != nullptr;
  }

  void Measure(float light_scale) {
    D3DLOCKED_RECT locked = {};
    if (FAILED(readback_[readback_index_]->LockRect(&locked, nullptr, D3DLOCK_READONLY))) return;
    const float display_scale = light_scale > 0.f ? exposure / light_scale : exposure;
    luminance_.resize(static_cast<size_t>(WIDTH) * HEIGHT);
    for (UINT y = 0; y < HEIGHT; ++y) {
      const auto* row = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(locked.pBits) + y * locked.Pitch);
      for (UINT x = 0; x < WIDTH; ++x) {
        const float r = Encode(HalfToFloat(row[x * 4 + 0]) * display_scale);
        const float g = Encode(HalfToFloat(row[x * 4 + 1]) * display_scale);
        const float b = Encode(HalfToFloat(row[x * 4 + 2]) * display_scale);
        luminance_[y * WIDTH + x] = 0.2125f * r + 0.7154f * g + 0.0721f * b;
      }
    }
    readback_[readback_index_]->UnlockRect();

    const auto location = [&](float percent_brightest) {
      const auto index = static_cast<size_t>(std::clamp(1.f - percent_brightest * 0.01f, 0.f, 0.9999f) * static_cast<float>(luminance_.size()));
      std::nth_element(luminance_.begin(), luminance_.begin() + static_cast<ptrdiff_t>(index), luminance_.end());
      return std::max(luminance_[index], 0.0001f);
    };
    float bright_location = location(percent_bright_pixels);
    const float target = percent_target * 0.01f;
    if (std::abs(bright_location - target) < 1.f / 32.f) bright_location = target;
    float scalar = target / bright_location * exposure;
    const float average_scalar = min_avg_lum * 0.01f / location(50.f) * exposure;
    if (average_scalar > scalar) scalar = average_scalar;
    scalar = std::clamp(scalar, exposure_min, exposure_max);

    if (target_count_ < AVERAGE_COUNT) {
      targets_[target_count_++] = scalar;
      goal_ = scalar;
      return;
    }
    std::copy(targets_ + 1, targets_ + AVERAGE_COUNT, targets_);
    targets_[AVERAGE_COUNT - 1] = scalar;
    float sum = 0.f;
    float weights = 0.f;
    for (int i = 0; i < AVERAGE_COUNT; ++i) {
      const float weight = static_cast<float>(std::abs(i - AVERAGE_COUNT / 2)) / static_cast<float>(AVERAGE_COUNT / 2);
      sum += weight * targets_[i];
      weights += weight;
    }
    goal_ = std::clamp(sum / weights, exposure_min, exposure_max);
  }

  void Advance() {
    const auto now = std::chrono::steady_clock::now();
    const float elapsed = last_time_.time_since_epoch().count() == 0 ? 0.f : std::min(std::chrono::duration<float>(now - last_time_).count(), 0.1f);
    last_time_ = now;
    float alpha = rate * elapsed;
    if (accelerate_down > 0.f && goal_ < exposure) alpha *= accelerate_down;
    alpha = std::clamp(alpha, 0.f, 1.f);
    exposure = goal_ * alpha + exposure * (1.f - alpha);
  }

  IDirect3DDevice9* device_ = nullptr;
  IDirect3DTexture9* sample_texture_ = nullptr;
  IDirect3DSurface9* readback_[2] = {};
  bool readback_valid_[2] = {};
  int readback_index_ = 0;
  std::vector<float> luminance_;
  float targets_[AVERAGE_COUNT] = {};
  int target_count_ = 0;
  float goal_ = 1.f;
  std::chrono::steady_clock::time_point last_time_{};
};

}  // namespace sourceengine
