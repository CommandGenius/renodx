/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#define RENODX_MODS_SWAPCHAIN_VERSION 2

#include <d3d11.h>
#include <d3d9.h>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/directx.hpp"
#include "../../utils/settings.hpp"
#include "./map_exposure.hpp"
#include "./portal2_hashes.hpp"
#include "./shared.h"

namespace {

ShaderInjectData shader_injection = {.scene_exposure = 1.f};

bool engine_post_drawn = false;

bool bloom_chain_drawn = false;

bool histogram_drawn = false;

bool OnBloomDownsampleReplace(reshade::api::command_list* cmd_list) {
  bloom_chain_drawn = true;
  return true;
}

IDirect3DBaseTexture9* scene_copy_texture = nullptr;

IDirect3DDevice9* GetNativeDevice(reshade::api::command_list* cmd_list) {
  return reinterpret_cast<IDirect3DDevice9*>(cmd_list->get_device()->get_native());
}

bool OnGammaSpaceDrawReplace(reshade::api::command_list* cmd_list) {
  DWORD srgb_write = 0;
  GetNativeDevice(cmd_list)->GetRenderState(D3DRS_SRGBWRITEENABLE, &srgb_write);
  shader_injection.srgb_write_off = srgb_write == 0 ? 1.f : 0.f;
  return true;
}

using CreateTextureFunction = HRESULT(STDMETHODCALLTYPE*)(
    IDirect3DDevice9*, UINT, UINT, UINT, DWORD, D3DFORMAT, D3DPOOL, IDirect3DTexture9**, HANDLE*);
CreateTextureFunction original_create_texture = nullptr;
ID3D11Device* shared_texture_device = nullptr;
std::unordered_map<uint64_t, ID3D11Texture2D*> shared_textures;
std::mutex shared_textures_mutex;

HRESULT STDMETHODCALLTYPE OnCreateTexture(
    IDirect3DDevice9* device, UINT width, UINT height, UINT levels, DWORD usage,
    D3DFORMAT format, D3DPOOL pool, IDirect3DTexture9** texture, HANDLE* shared_handle) {
  DXGI_FORMAT dxgi_format = DXGI_FORMAT_UNKNOWN;
  if (format == D3DFMT_A8R8G8B8) dxgi_format = DXGI_FORMAT_B8G8R8A8_UNORM;
  if (format == D3DFMT_X8R8G8B8) dxgi_format = DXGI_FORMAT_B8G8R8X8_UNORM;
  if (format == D3DFMT_A16B16G16R16F) dxgi_format = DXGI_FORMAT_R16G16B16A16_FLOAT;
  if (shared_handle == nullptr || *shared_handle != nullptr || pool != D3DPOOL_DEFAULT
      || levels > 1 || dxgi_format == DXGI_FORMAT_UNKNOWN) {
    return original_create_texture(device, width, height, levels, usage, format, pool, texture, shared_handle);
  }

  std::scoped_lock lock(shared_textures_mutex);
  if (shared_texture_device == nullptr && renodx::utils::directx::Initialize()) {
    renodx::utils::directx::pD3D11CreateDevice(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
        &shared_texture_device, nullptr, nullptr);
  }
  ID3D11Texture2D* shared_texture = nullptr;
  IDXGIResource* dxgi_resource = nullptr;
  HANDLE handle = nullptr;
  if (shared_texture_device != nullptr) {
    const D3D11_TEXTURE2D_DESC desc = {
        .Width = width,
        .Height = height,
        .MipLevels = 1,
        .ArraySize = 1,
        .Format = dxgi_format,
        .SampleDesc = {.Count = 1},
        .Usage = D3D11_USAGE_DEFAULT,
        .BindFlags = D3D11_BIND_SHADER_RESOURCE | D3D11_BIND_RENDER_TARGET,
        .MiscFlags = D3D11_RESOURCE_MISC_SHARED,
    };
    if (SUCCEEDED(shared_texture_device->CreateTexture2D(&desc, nullptr, &shared_texture))
        && SUCCEEDED(shared_texture->QueryInterface(IID_PPV_ARGS(&dxgi_resource)))) {
      dxgi_resource->GetSharedHandle(&handle);
      dxgi_resource->Release();
    }
  }
  if (handle != nullptr) {
    *shared_handle = handle;
    const HRESULT hr = original_create_texture(device, width, height, levels, usage, format, pool, texture, shared_handle);
    if (SUCCEEDED(hr)) {
      shared_textures[reinterpret_cast<uint64_t>(*texture)] = shared_texture;
      std::stringstream info;
      info << "[sourceengine] shared texture " << width << "x" << height << " format " << format << " created on D3D11";
      reshade::log::message(reshade::log::level::info, info.str().c_str());
      return hr;
    }
    *shared_handle = nullptr;
  }
  if (shared_texture != nullptr) shared_texture->Release();
  reshade::log::message(reshade::log::level::warning, "[sourceengine] shared texture fell back to D3D9 creation");
  return original_create_texture(device, width, height, levels, usage, format, pool, texture, shared_handle);
}

void OnInitDevice(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d9 || original_create_texture != nullptr) return;
  void** vtable = *reinterpret_cast<void***>(device->get_native());
  DWORD protect = 0;
  if (!VirtualProtect(&vtable[23], sizeof(void*), PAGE_READWRITE, &protect)) return;
  original_create_texture = reinterpret_cast<CreateTextureFunction>(vtable[23]);
  vtable[23] = reinterpret_cast<void*>(&OnCreateTexture);
  VirtualProtect(&vtable[23], sizeof(void*), protect, &protect);
}

void OnDestroyResource(reshade::api::device* device, reshade::api::resource resource) {
  if (device->get_api() != reshade::api::device_api::d3d9) return;
  std::scoped_lock lock(shared_textures_mutex);
  if (auto it = shared_textures.find(resource.handle); it != shared_textures.end()) {
    it->second->Release();
    shared_textures.erase(it);
  }
}

IDirect3DTexture9* untonemapped_texture = nullptr;
IDirect3DTexture9* graded_texture = nullptr;
IDirect3DBaseTexture9* bloom_texture = nullptr;
float bloom_factor[4] = {};
IDirect3DPixelShader9* encode_scene_shader = nullptr;
IDirect3DPixelShader9* post_upgrade_shader = nullptr;
IDirect3DPixelShader9* exposure_measure_shader = nullptr;
IDirect3DDevice9* post_shader_device = nullptr;

IDirect3DTexture9* MatchTexture(IDirect3DDevice9* device, IDirect3DTexture9* texture, const D3DSURFACE_DESC& desc) {
  D3DSURFACE_DESC current = {};
  if (texture != nullptr && SUCCEEDED(texture->GetLevelDesc(0, &current)) && current.Width == desc.Width && current.Height == desc.Height && current.Format == desc.Format) {
    return texture;
  }
  if (texture != nullptr) texture->Release();
  texture = nullptr;
  device->CreateTexture(desc.Width, desc.Height, 1, D3DUSAGE_RENDERTARGET, desc.Format, D3DPOOL_DEFAULT, &texture, nullptr);
  return texture;
}

IDirect3DPixelShader9* PostShader(IDirect3DDevice9* device, IDirect3DPixelShader9** shader, std::span<const uint8_t> code) {
  if (post_shader_device != device) {
    if (encode_scene_shader != nullptr) encode_scene_shader->Release();
    if (post_upgrade_shader != nullptr) post_upgrade_shader->Release();
    if (exposure_measure_shader != nullptr) exposure_measure_shader->Release();
    encode_scene_shader = post_upgrade_shader = exposure_measure_shader = nullptr;
    post_shader_device = device;
  }
  if (*shader == nullptr) device->CreatePixelShader(reinterpret_cast<const DWORD*>(code.data()), shader);
  return *shader;
}

class FullscreenPass {
 public:
  FullscreenPass(IDirect3DDevice9* device, IDirect3DPixelShader9* shader, std::span<IDirect3DBaseTexture9* const> textures, IDirect3DSurface9* target)
      : device_(device) {
    if (shader == nullptr || target == nullptr || FAILED(device->CreateStateBlock(D3DSBT_ALL, &state_))) return;
    D3DSURFACE_DESC target_desc = {};
    target->GetDesc(&target_desc);
    device->GetRenderTarget(0, &previous_target_);
    device->GetDepthStencilSurface(&previous_depth_);

    device->SetRenderTarget(0, target);
    device->SetDepthStencilSurface(nullptr);
    device->SetVertexShader(nullptr);
    device->SetPixelShader(shader);
    device->SetFVF(D3DFVF_XYZRHW | D3DFVF_TEX1);
    for (DWORD slot = 0; slot < textures.size(); ++slot) {
      device->SetTexture(slot, textures[slot]);
      device->SetSamplerState(slot, D3DSAMP_SRGBTEXTURE, FALSE);
      device->SetSamplerState(slot, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
      device->SetSamplerState(slot, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
      device->SetSamplerState(slot, D3DSAMP_MINFILTER, D3DTEXF_POINT);
      device->SetSamplerState(slot, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
      device->SetSamplerState(slot, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    }
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
    device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
    device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
    device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
    device->SetRenderState(D3DRS_FOGENABLE, FALSE);
    device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xF);
    device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);
    const D3DVIEWPORT9 viewport = {0, 0, target_desc.Width, target_desc.Height, 0.f, 1.f};
    device->SetViewport(&viewport);
    width_ = static_cast<float>(target_desc.Width) - 0.5f;
    height_ = static_cast<float>(target_desc.Height) - 0.5f;
  }

  ~FullscreenPass() {
    if (state_ == nullptr) return;
    device_->SetRenderTarget(0, previous_target_);
    device_->SetDepthStencilSurface(previous_depth_);
    if (previous_target_ != nullptr) previous_target_->Release();
    if (previous_depth_ != nullptr) previous_depth_->Release();
    state_->Apply();
    state_->Release();
  }

  void Draw(const float* constants_c0 = nullptr) {
    if (state_ == nullptr) return;
    if (constants_c0 != nullptr) device_->SetPixelShaderConstantF(0, constants_c0, 1);
    const float quad[4][6] = {
        {-0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f},
        {width_, -0.5f, 0.f, 1.f, 1.f, 0.f},
        {-0.5f, height_, 0.f, 1.f, 0.f, 1.f},
        {width_, height_, 0.f, 1.f, 1.f, 1.f},
    };
    device_->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));
  }

 private:
  IDirect3DDevice9* device_;
  IDirect3DStateBlock9* state_ = nullptr;
  IDirect3DSurface9* previous_target_ = nullptr;
  IDirect3DSurface9* previous_depth_ = nullptr;
  float width_ = 0.f;
  float height_ = 0.f;
};

void DrawFullscreen(IDirect3DDevice9* device, IDirect3DPixelShader9* shader, std::span<IDirect3DBaseTexture9* const> textures, IDirect3DSurface9* target, const float* constants_c0 = nullptr) {
  FullscreenPass(device, shader, textures, target).Draw(constants_c0);
}

DWORD sampler0_srgb_at_bind = 0;
bool histogram_reads_linear = false;
bool original_algorithm = false;
float current_light_scale = 1.f;
bool light_scale_used = false;

constexpr int EXPOSURE_FRAMES_IN_FLIGHT = 3;
struct ExposureMeasurement {
  IDirect3DQuery9* queries[map_exposure::BINS] = {};
  bool issued = false;
  uint32_t level = 0;
};
ExposureMeasurement exposure_measurements[EXPOSURE_FRAMES_IN_FLIGHT];
int exposure_measurement_index = 0;
IDirect3DTexture9* exposure_sample_texture = nullptr;
IDirect3DTexture9* exposure_count_texture = nullptr;
std::array<double, map_exposure::SETTLE_BINS> exposure_settles = {};
double exposure_frames = 0.0;
uint32_t exposure_level = 0;
float map_light_scale = 0.f;
float level_light_scale_max = 0.f;

void OnPushDescriptors(reshade::api::command_list* cmd_list, reshade::api::shader_stage stages, reshade::api::pipeline_layout layout, uint32_t layout_param, const reshade::api::descriptor_table_update& update) {
  if (cmd_list->get_device()->get_api() != reshade::api::device_api::d3d9) return;
  if (update.type != reshade::api::descriptor_type::sampler_with_resource_view || update.binding != 0) return;
  GetNativeDevice(cmd_list)->GetSamplerState(0, D3DSAMP_SRGBTEXTURE, &sampler0_srgb_at_bind);
}

void TrackLevelLoad(float light_scale) {
  static auto previous_capture = std::chrono::steady_clock::time_point{};
  static bool resumed = false;
  const auto now = std::chrono::steady_clock::now();
  resumed |= now - previous_capture > std::chrono::milliseconds(500);
  previous_capture = now;
  if (light_scale == 1.f) return;
  if (resumed && light_scale < 0.97f * level_light_scale_max) {
    exposure_settles = {};
    exposure_frames = 0.0;
    map_light_scale = 0.f;
    level_light_scale_max = 0.f;
    ++exposure_level;
  }
  resumed = false;
  level_light_scale_max = std::max(level_light_scale_max, light_scale);
}

void CollectExposureMeasurement(ExposureMeasurement* measurement) {
  DWORD counts[map_exposure::BINS] = {};
  double total = 0.0;
  for (int i = 0; i < map_exposure::BINS; ++i) {
    if (measurement->queries[i] == nullptr || measurement->queries[i]->GetData(&counts[i], sizeof(DWORD), 0) != S_OK) return;
    total += counts[i];
  }
  measurement->issued = false;
  if (measurement->level != exposure_level || total <= 0.0) return;
  std::array<double, map_exposure::BINS> share = {};
  for (int i = 0; i < map_exposure::BINS; ++i) share[i] = counts[i] / total;
  const double settled = map_exposure::Settle(share, histogram_reads_linear, original_algorithm, map_exposure::SETTLE_LOWEST, map_exposure::SETTLE_HIGHEST);
  exposure_settles[map_exposure::SettleBin(settled)] += 1.0;
  exposure_frames += 1.0;
  const double maximum = level_light_scale_max > 0.f ? level_light_scale_max : 1.f;
  map_light_scale = static_cast<float>(map_exposure::MeanSettle(exposure_settles, std::min(0.5, maximum), maximum));
}

void MeasureExposure(IDirect3DDevice9* device) {
  if (!light_scale_used || untonemapped_texture == nullptr || current_light_scale <= 0.f) return;
  if (current_light_scale == 1.f && level_light_scale_max > 0.f) return;
  for (auto& pending : exposure_measurements) {
    if (pending.issued) CollectExposureMeasurement(&pending);
  }
  static auto previous_issue = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now - previous_issue < std::chrono::milliseconds(50)) return;
  auto& measurement = exposure_measurements[exposure_measurement_index];
  if (measurement.issued) return;
  previous_issue = now;
  exposure_measurement_index = (exposure_measurement_index + 1) % EXPOSURE_FRAMES_IN_FLIGHT;

  for (auto*& query : measurement.queries) {
    if (query == nullptr) device->CreateQuery(D3DQUERYTYPE_OCCLUSION, &query);
    if (query == nullptr) return;
  }
  D3DSURFACE_DESC scene_desc = {};
  untonemapped_texture->GetLevelDesc(0, &scene_desc);
  D3DSURFACE_DESC sample_desc = scene_desc;
  sample_desc.Width = 240;
  sample_desc.Height = 135;
  exposure_sample_texture = MatchTexture(device, exposure_sample_texture, sample_desc);
  sample_desc.Format = D3DFMT_A8R8G8B8;
  exposure_count_texture = MatchTexture(device, exposure_count_texture, sample_desc);
  if (exposure_sample_texture == nullptr || exposure_count_texture == nullptr) return;

  IDirect3DSurface9* scene = nullptr;
  IDirect3DSurface9* sample = nullptr;
  IDirect3DSurface9* count = nullptr;
  untonemapped_texture->GetSurfaceLevel(0, &scene);
  exposure_sample_texture->GetSurfaceLevel(0, &sample);
  exposure_count_texture->GetSurfaceLevel(0, &count);
  const RECT region = {
      static_cast<LONG>(scene_desc.Width * 0.05f), static_cast<LONG>(scene_desc.Height * 0.075f),
      static_cast<LONG>(scene_desc.Width * 0.95f), static_cast<LONG>(scene_desc.Height * 0.925f)};
  device->StretchRect(scene, &region, sample, nullptr, D3DTEXF_POINT);
  {
    IDirect3DBaseTexture9* inputs[] = {exposure_sample_texture};
    FullscreenPass pass(device, PostShader(device, &exposure_measure_shader, __exposure_measure), inputs, count);
    for (int i = 0; i < map_exposure::BINS; ++i) {
      const float bin[4] = {
          i == 0 ? -1.f : static_cast<float>(map_exposure::BinEdge(i)),
          i == map_exposure::BINS - 1 ? 1e30f : static_cast<float>(map_exposure::BinEdge(i + 1)),
          1.f / current_light_scale, 0.f};
      measurement.queries[i]->Issue(D3DISSUE_BEGIN);
      pass.Draw(bin);
      measurement.queries[i]->Issue(D3DISSUE_END);
    }
  }
  measurement.issued = true;
  measurement.level = exposure_level;
  for (auto* surface : {scene, sample, count}) {
    if (surface != nullptr) surface->Release();
  }
}

IDirect3DSurface9* CaptureSceneCopy(IDirect3DDevice9* device) {
  IDirect3DBaseTexture9* texture = nullptr;
  device->GetTexture(0, &texture);
  if (scene_copy_texture != nullptr) scene_copy_texture->Release();
  scene_copy_texture = texture;
  histogram_drawn = true;
  float light_scale[4] = {1.f, 1.f, 1.f, 1.f};
  device->GetPixelShaderConstantF(30, light_scale, 1);
  current_light_scale = light_scale[0];
  histogram_reads_linear = sampler0_srgb_at_bind != 0;
  if (current_light_scale != 1.f) light_scale_used = true;
  TrackLevelLoad(current_light_scale);
  if (current_light_scale > 0.f) {
    const float shown = (light_scale_used && map_light_scale > 0.f) ? map_light_scale : 1.f;
    shader_injection.scene_exposure = shown / current_light_scale;
  }
  if (texture == nullptr || texture->GetType() != D3DRTYPE_TEXTURE) return nullptr;
  auto* copy = static_cast<IDirect3DTexture9*>(texture);
  D3DSURFACE_DESC desc = {};
  copy->GetLevelDesc(0, &desc);
  if (desc.Format != D3DFMT_A16B16G16R16F) return nullptr;
  untonemapped_texture = MatchTexture(device, untonemapped_texture, desc);
  if (untonemapped_texture == nullptr) return nullptr;
  IDirect3DSurface9* source = nullptr;
  IDirect3DSurface9* keep = nullptr;
  copy->GetSurfaceLevel(0, &source);
  untonemapped_texture->GetSurfaceLevel(0, &keep);
  device->StretchRect(source, nullptr, keep, nullptr, D3DTEXF_NONE);
  keep->Release();
  return source;
}

bool raw_scene_tone_mapped = false;

bool OnUiDraw(reshade::api::command_list* cmd_list) {
  if (!histogram_drawn || engine_post_drawn || bloom_chain_drawn || raw_scene_tone_mapped || untonemapped_texture == nullptr) return true;
  raw_scene_tone_mapped = true;
  auto* device = GetNativeDevice(cmd_list);
  IDirect3DSurface9* target = nullptr;
  if (FAILED(device->GetRenderTarget(0, &target)) || target == nullptr) return true;
  D3DSURFACE_DESC desc = {};
  target->GetDesc(&desc);
  if (desc.Format == D3DFMT_A16B16G16R16F) {
    const float params[4] = {0.f, 1.f, 0.f, 0.f};
    IDirect3DBaseTexture9* inputs[] = {untonemapped_texture, untonemapped_texture, untonemapped_texture};
    DrawFullscreen(device, PostShader(device, &post_upgrade_shader, __post_upgrade), inputs, target, params);
    engine_post_drawn = true;
  }
  target->Release();
  return true;
}

IDirect3DTexture9* encoded_texture = nullptr;
bool scene_encoded = false;

IDirect3DTexture9* EncodedScene(IDirect3DDevice9* device) {
  if (scene_encoded) return encoded_texture;
  scene_encoded = true;
  D3DSURFACE_DESC desc = {};
  untonemapped_texture->GetLevelDesc(0, &desc);
  encoded_texture = MatchTexture(device, encoded_texture, desc);
  IDirect3DSurface9* target = nullptr;
  if (encoded_texture != nullptr && SUCCEEDED(encoded_texture->GetSurfaceLevel(0, &target))) {
    IDirect3DBaseTexture9* inputs[] = {untonemapped_texture};
    DrawFullscreen(device, PostShader(device, &encode_scene_shader, __encode_scene), inputs, target);
    target->Release();
  }
  return encoded_texture;
}
IDirect3DBaseTexture9* previous_texture_slot[2] = {};

bool BoundTextureIsSceneCopy(IDirect3DDevice9* device, DWORD slot) {
  if (engine_post_drawn) return false;
  IDirect3DBaseTexture9* texture = nullptr;
  device->GetTexture(slot, &texture);
  const bool matches = texture != nullptr && texture == scene_copy_texture;
  if (texture != nullptr) texture->Release();
  return matches;
}

void SwapTextureSlot(IDirect3DDevice9* device, DWORD slot, IDirect3DBaseTexture9* replacement) {
  previous_texture_slot[slot] = nullptr;
  device->GetTexture(slot, &previous_texture_slot[slot]);
  device->SetTexture(slot, replacement);
}

void RestoreTextureSlot(IDirect3DDevice9* device, DWORD slot) {
  device->SetTexture(slot, previous_texture_slot[slot]);
  if (previous_texture_slot[slot] != nullptr) previous_texture_slot[slot]->Release();
  previous_texture_slot[slot] = nullptr;
}

bool downsample_swapped = false;

bool OnVanillaDownsampleDraw(reshade::api::command_list* cmd_list) {
  auto* device = GetNativeDevice(cmd_list);
  downsample_swapped = untonemapped_texture != nullptr && histogram_drawn && BoundTextureIsSceneCopy(device, 0)
                       && EncodedScene(device) != nullptr;
  shader_injection.linear_input = (!downsample_swapped && untonemapped_texture != nullptr && histogram_drawn) ? 2.f : 0.f;
  if (!downsample_swapped) return true;
  bloom_chain_drawn = true;
  SwapTextureSlot(device, 0, encoded_texture);
  return true;
}

bool OnVanillaDownsampleLinearDraw(reshade::api::command_list* cmd_list) {
  auto* device = GetNativeDevice(cmd_list);
  downsample_swapped = untonemapped_texture != nullptr && histogram_drawn && BoundTextureIsSceneCopy(device, 0);
  if (!downsample_swapped) return true;
  bloom_chain_drawn = true;
  SwapTextureSlot(device, 0, untonemapped_texture);
  return true;
}

void OnVanillaDownsampleDrawn(reshade::api::command_list* cmd_list) {
  if (downsample_swapped) RestoreTextureSlot(GetNativeDevice(cmd_list), 0);
  downsample_swapped = false;
}

float bloom_factor_weight = 0.5f;
bool post_swapped = false;

bool OnVanillaEnginePostDraw(reshade::api::command_list* cmd_list) {
  auto* device = GetNativeDevice(cmd_list);
  if (bloom_texture != nullptr) bloom_texture->Release();
  bloom_texture = nullptr;
  device->GetTexture(0, &bloom_texture);
  device->GetPixelShaderConstantF(5, bloom_factor, 1);
  bloom_factor_weight = 0.5f;
  post_swapped = untonemapped_texture != nullptr && histogram_drawn && BoundTextureIsSceneCopy(device, 1)
                 && EncodedScene(device) != nullptr;
  if (post_swapped) SwapTextureSlot(device, 1, encoded_texture);
  return true;
}

bool OnVanillaEnginePostDrawFullBloom(reshade::api::command_list* cmd_list) {
  OnVanillaEnginePostDraw(cmd_list);
  bloom_factor_weight = 1.f;
  return true;
}

void OnVanillaEnginePostDrawn(reshade::api::command_list* cmd_list) {
  if (untonemapped_texture == nullptr || !histogram_drawn) return;
  auto* device = GetNativeDevice(cmd_list);
  if (post_swapped) RestoreTextureSlot(device, 1);
  post_swapped = false;
  IDirect3DSurface9* target = nullptr;
  if (FAILED(device->GetRenderTarget(0, &target)) || target == nullptr) return;
  D3DSURFACE_DESC desc = {};
  target->GetDesc(&desc);
  if (desc.Format == D3DFMT_A16B16G16R16F) {
    graded_texture = MatchTexture(device, graded_texture, desc);
    IDirect3DSurface9* graded = nullptr;
    if (graded_texture != nullptr && SUCCEEDED(graded_texture->GetSurfaceLevel(0, &graded))) {
      device->StretchRect(target, nullptr, graded, nullptr, D3DTEXF_NONE);
      graded->Release();
      shader_injection.bloom_valid = bloom_chain_drawn ? 1.f : 0.f;
      const float factor[4] = {bloom_factor[0] * bloom_factor_weight, 0.f, 0.f, 0.f};
      IDirect3DBaseTexture9* inputs[] = {graded_texture, untonemapped_texture, bloom_texture};
      DrawFullscreen(device, PostShader(device, &post_upgrade_shader, __post_upgrade), inputs, target, factor);
      engine_post_drawn = true;
    }
  }
  target->Release();
}

void OnDestroyDevice(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d9) return;
  for (auto& measurement : exposure_measurements) {
    for (auto*& query : measurement.queries) {
      if (query != nullptr) query->Release();
      query = nullptr;
    }
    measurement.issued = false;
  }
  for (auto** texture : {&untonemapped_texture, &graded_texture, &encoded_texture, &exposure_sample_texture, &exposure_count_texture}) {
    if (*texture != nullptr) (*texture)->Release();
    *texture = nullptr;
  }
  for (auto** shader : {&encode_scene_shader, &post_upgrade_shader, &exposure_measure_shader}) {
    if (*shader != nullptr) (*shader)->Release();
    *shader = nullptr;
  }
  post_shader_device = nullptr;
}

bool OnEnginePostReplace(reshade::api::command_list* cmd_list) {
  shader_injection.bloom_valid = bloom_chain_drawn ? 1.f : 0.f;
  engine_post_drawn = true;
  return true;
}

void OnScenePresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain, const reshade::api::rect* source_rect, const reshade::api::rect* dest_rect, uint32_t dirty_rect_count, const reshade::api::rect* dirty_rects) {
  if (queue->get_device()->get_api() != reshade::api::device_api::d3d9) return;
  if (histogram_drawn) MeasureExposure(reinterpret_cast<IDirect3DDevice9*>(queue->get_device()->get_native()));
  shader_injection.scene_tone_mapped = engine_post_drawn ? 1.f : 0.f;
  engine_post_drawn = false;
  bloom_chain_drawn = false;
  histogram_drawn = false;
  raw_scene_tone_mapped = false;
  scene_encoded = false;
}

IDirect3DBaseTexture9* bloom_add_previous_texture = nullptr;
const D3DSAMPLERSTATETYPE BLOOM_ADD_SAMPLER_STATES[] = {D3DSAMP_SRGBTEXTURE, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER};
DWORD bloom_add_previous_sampler_states[std::size(BLOOM_ADD_SAMPLER_STATES)] = {};

bool OnLuminanceCompareReplace(reshade::api::command_list* cmd_list) {
  auto* native_device = GetNativeDevice(cmd_list);
  IDirect3DSurface9* source = CaptureSceneCopy(native_device);
  if (source != nullptr) source->Release();
  float bucket[4] = {};
  native_device->GetPixelShaderConstantF(0, bucket, 1);
  if (bucket[0] == 0.f && bucket[1] < 50000.f) original_algorithm = bucket[1] < 0.01f;
  native_device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, TRUE);
  return true;
}

bool OnBloomAddReplace(reshade::api::command_list* cmd_list) {
  if (scene_copy_texture == nullptr || !bloom_chain_drawn || !histogram_drawn) {
    return false;
  }
  auto* native_device = GetNativeDevice(cmd_list);

  bloom_add_previous_texture = nullptr;
  native_device->GetTexture(1, &bloom_add_previous_texture);
  native_device->SetTexture(1, scene_copy_texture);
  for (size_t i = 0; i < std::size(BLOOM_ADD_SAMPLER_STATES); ++i) {
    native_device->GetSamplerState(1, BLOOM_ADD_SAMPLER_STATES[i], &bloom_add_previous_sampler_states[i]);
  }
  native_device->SetSamplerState(1, D3DSAMP_SRGBTEXTURE, FALSE);
  native_device->SetSamplerState(1, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  native_device->SetSamplerState(1, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  native_device->SetSamplerState(1, D3DSAMP_MINFILTER, D3DTEXF_LINEAR);
  native_device->SetSamplerState(1, D3DSAMP_MAGFILTER, D3DTEXF_LINEAR);
  native_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);

  shader_injection.bloom_valid = bloom_chain_drawn ? 1.f : 0.f;
  engine_post_drawn = true;
  return true;
}

void OnBloomAddDrawn(reshade::api::command_list* cmd_list) {
  auto* native_device = GetNativeDevice(cmd_list);
  native_device->SetTexture(1, bloom_add_previous_texture);
  if (bloom_add_previous_texture != nullptr) {
    bloom_add_previous_texture->Release();
    bloom_add_previous_texture = nullptr;
  }
  for (size_t i = 0; i < std::size(BLOOM_ADD_SAMPLER_STATES); ++i) {
    native_device->SetSamplerState(1, BLOOM_ADD_SAMPLER_STATES[i], bloom_add_previous_sampler_states[i]);
  }
  native_device->SetRenderState(D3DRS_ALPHABLENDENABLE, TRUE);
}

#define ENGINE_POST_SHADER(crc32) CustomShaderEntryCallback(crc32, &OnEnginePostReplace)

renodx::mods::shader::CustomShaders custom_shaders = {
    ENGINE_POST_SHADER(0xC928B79F),
    ENGINE_POST_SHADER(0x022CAEF3),
    ENGINE_POST_SHADER(0x284DA795),
    ENGINE_POST_SHADER(0x4836A341),
    ENGINE_POST_SHADER(0x629A61C7),
    ENGINE_POST_SHADER(0x14EF05A9),
    ENGINE_POST_SHADER(0xAD6A8621),
    ENGINE_POST_SHADER(0x93183146),
    ENGINE_POST_SHADER(0x919AFDB5),
    ENGINE_POST_SHADER(0x8647806C),
    ENGINE_POST_SHADER(0xBDC26F46),
    ENGINE_POST_SHADER(0x0760463A),
    ENGINE_POST_SHADER(0x5F45D51B),
    ENGINE_POST_SHADER(0xAB62999F),
    ENGINE_POST_SHADER(0x74F651C4),
    ENGINE_POST_SHADER(0x24C758FA),
    ENGINE_POST_SHADER(0x5B738579),
    ENGINE_POST_SHADER(0x23775101),
    ENGINE_POST_SHADER(0xF9E3448C),
    ENGINE_POST_SHADER(0x4D163ECE),
    ENGINE_POST_SHADER(0xCC4B3F9B),
    ENGINE_POST_SHADER(0x2ECE3141),
    ENGINE_POST_SHADER(0x1441B2B5),
    ENGINE_POST_SHADER(0xCEA2627C),
    ENGINE_POST_SHADER(0xE437F066),
    {0xC5D92350, {.crc32 = 0xC5D92350, .code = __0xC5D92350, .on_replace = &OnBloomAddReplace, .on_drawn = &OnBloomAddDrawn}},
    {0x8B8861AD, {.crc32 = 0x8B8861AD, .code = __0x8B8861AD, .on_replace = &OnLuminanceCompareReplace}},
    CustomShaderEntryCallback(0x5C3593EB, &OnBloomDownsampleReplace),
    ENGINE_POST_SHADER(0x831313E4),
    ENGINE_POST_SHADER(0xBB93772C),
    {0xEA4B3EE9, {.crc32 = 0xEA4B3EE9, .code = __0xEA4B3EE9, .on_replace = &OnLuminanceCompareReplace}},
    {0x5E9FC94B, {.crc32 = 0x5E9FC94B, .code = __0x5E9FC94B, .on_replace = &OnLuminanceCompareReplace}},
    {0xE0D9427D, {.crc32 = 0xE0D9427D, .code = __0xE0D9427D, .on_replace = &OnLuminanceCompareReplace}},
    {0x16C33CB7, {.crc32 = 0x16C33CB7, .on_draw = &OnVanillaDownsampleDraw, .on_drawn = &OnVanillaDownsampleDrawn}},
    {0x5EBAB2E8, {.crc32 = 0x5EBAB2E8, .on_draw = &OnVanillaDownsampleDraw, .on_drawn = &OnVanillaDownsampleDrawn}},
    {0x271A2CA1, {.crc32 = 0x271A2CA1, .on_draw = &OnVanillaDownsampleDraw, .on_drawn = &OnVanillaDownsampleDrawn}},
    {0xB12B99BE, {.crc32 = 0xB12B99BE, .on_draw = &OnVanillaDownsampleDraw, .on_drawn = &OnVanillaDownsampleDrawn}},
    {0xCFB5A0D0, {.crc32 = 0xCFB5A0D0, .code = __0xCFB5A0D0, .on_replace = &OnVanillaDownsampleDraw, .on_drawn = &OnVanillaDownsampleDrawn}},
    {0xF6ED64EA, {.crc32 = 0xF6ED64EA, .code = __0xF6ED64EA, .on_replace = &OnVanillaDownsampleLinearDraw, .on_drawn = &OnVanillaDownsampleDrawn}},
    CustomShaderEntry(0xF820D96F),
    CustomShaderEntry(0x9D06155A),
    CustomShaderEntry(0xF990E8E5),
    CustomShaderEntry(0x9010CD7F),
    {0x6236B99B, {.crc32 = 0x6236B99B, .on_draw = &OnUiDraw}},
    {0xCFAFE6F6, {.crc32 = 0xCFAFE6F6, .on_draw = &OnUiDraw}},
    {0x201ADBD3, {.crc32 = 0x201ADBD3, .on_draw = &OnUiDraw}},
    {0x030AF021, {.crc32 = 0x030AF021, .on_draw = &OnUiDraw}},
    {0x23B789C1, {.crc32 = 0x23B789C1, .code = __0x23B789C1, .on_replace = &OnGammaSpaceDrawReplace, .on_draw = &OnUiDraw}},
    {0x0DEE26BF, {.crc32 = 0x0DEE26BF, .code = __0x0DEE26BF, .on_replace = &OnGammaSpaceDrawReplace, .on_draw = &OnUiDraw}},
    {0xEE27D62A, {.crc32 = 0xEE27D62A, .code = __0xEE27D62A, .on_replace = &OnGammaSpaceDrawReplace, .on_draw = &OnUiDraw}},
    {0xBB6A22F0, {.crc32 = 0xBB6A22F0, .code = __0xBB6A22F0, .on_replace = &OnGammaSpaceDrawReplace, .on_draw = &OnUiDraw}},
    {0x20B2481D, {.crc32 = 0x20B2481D, .code = __0x20B2481D, .on_replace = &OnGammaSpaceDrawReplace, .on_draw = &OnUiDraw}},
    CustomShaderEntryCallback(0x51AF5BEF, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xAF2589CC, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x8837F356, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x8E02BBBE, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x0BF891AA, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xCAC51D67, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x0842E39E, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x170AD9CC, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x1B84CD39, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x2263A555, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x2C39628C, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x36A5FFB0, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x4FDAD9B4, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x50CE71BB, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x591A1062, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x6387F691, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x69129D1B, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x8C2D1C85, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x8F1BCED2, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x9DEA8FAD, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xAF86C7DF, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xB51EBBA2, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xC640A8A5, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xCF6CC274, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x1016E67B, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x119CBCD8, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x312EC476, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x3157AC53, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x4CC17B01, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x630A37BC, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x6A52A25B, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x6C513BEB, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xCFF674D2, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xD78AD20F, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xFC95BEB7, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xFEEE531F, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x0A5F8217, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x241E712F, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x439B3680, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x71A6DE00, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x9085F662, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xE7AEFC2F, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x264317C2, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x385CF8DA, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x3F6A24C7, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x67740C79, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x68D7064B, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x73F59BC3, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0x79E07AFC, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xBD7927FA, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xBFA43DA4, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xC1823A2C, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xE95B8DB9, &OnGammaSpaceDrawReplace),
    CustomShaderEntryCallback(0xF024CD51, &OnGammaSpaceDrawReplace),
    CustomShaderEntry(0x65FAE654),
    __ALL_CUSTOM_SHADERS,
};

float current_settings_mode = 0;

renodx::utils::settings::Settings settings = {
    new renodx::utils::settings::Setting{
        .key = "SettingsMode",
        .binding = &current_settings_mode,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .can_reset = false,
        .label = "Settings Mode",
        .labels = {"Simple", "Intermediate", "Advanced"},
        .is_global = true,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapType",
        .binding = &shader_injection.tone_map_type,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 2.f,
        .can_reset = true,
        .label = "Tone Mapper",
        .section = "Tone Mapping",
        .tooltip = "Sets the tone mapper type",
        .labels = {"Vanilla", "None", "RenoDRT"},
        .parse = [](float value) {
          if (value < 0.5f) return 0.f;
          if (value < 1.5f) return 1.f;
          return 3.f;
        },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapPeakNits",
        .binding = &shader_injection.peak_white_nits,
        .default_value = 1000.f,
        .can_reset = false,
        .label = "Peak Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of peak white in nits",
        .min = 48.f,
        .max = 4000.f,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapGameNits",
        .binding = &shader_injection.diffuse_white_nits,
        .default_value = 203.f,
        .label = "Game Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the value of 100% white in nits",
        .min = 48.f,
        .max = 500.f,
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapUINits",
        .binding = &shader_injection.graphics_white_nits,
        .default_value = 203.f,
        .label = "UI Brightness",
        .section = "Tone Mapping",
        .tooltip = "Sets the brightness of UI and HUD elements in nits",
        .min = 48.f,
        .max = 500.f,
    },
    new renodx::utils::settings::Setting{
        .key = "GammaCorrection",
        .binding = &shader_injection.gamma_correction,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 1.f,
        .label = "Gamma Correction",
        .section = "Tone Mapping",
        .tooltip = "Emulates a display EOTF.",
        .labels = {"Off", "2.2", "BT.1886"},
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapScaling",
        .binding = &shader_injection.tone_map_per_channel,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Scaling",
        .section = "Tone Mapping",
        .tooltip = "Luminance scales colors consistently while per-channel saturates and blows out sooner",
        .labels = {"Luminance", "Per Channel"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapWorkingColorSpace",
        .binding = &shader_injection.tone_map_working_color_space,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Working Color Space",
        .section = "Tone Mapping",
        .labels = {"BT709", "BT2020", "AP1"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapHueProcessor",
        .binding = &shader_injection.tone_map_hue_processor,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Hue Processor",
        .section = "Tone Mapping",
        .tooltip = "Selects hue processor",
        .labels = {"OKLab", "ICtCp", "darkTable UCS"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapHueCorrection",
        .binding = &shader_injection.tone_map_hue_correction,
        .default_value = 100.f,
        .label = "Hue Correction",
        .section = "Tone Mapping",
        .tooltip = "Hue retention strength.",
        .min = 0.f,
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value * 0.01f; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapHueShift",
        .binding = &shader_injection.tone_map_hue_shift,
        .default_value = 50.f,
        .label = "Hue Shift",
        .section = "Tone Mapping",
        .tooltip = "Hue-shift emulation strength.",
        .min = 0.f,
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value * 0.01f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapClampColorSpace",
        .binding = &shader_injection.tone_map_clamp_color_space,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Clamp Color Space",
        .section = "Tone Mapping",
        .tooltip = "Hue-shift emulation strength.",
        .labels = {"None", "BT709", "BT2020", "AP1"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value - 1.f; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ToneMapClampPeak",
        .binding = &shader_injection.tone_map_clamp_peak,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Clamp Peak",
        .section = "Tone Mapping",
        .tooltip = "Hue-shift emulation strength.",
        .labels = {"None", "BT709", "BT2020", "AP1"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value - 1.f; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeExposure",
        .binding = &shader_injection.tone_map_exposure,
        .default_value = 1.f,
        .label = "Exposure",
        .section = "Color Grading",
        .max = 2.f,
        .format = "%.2f",
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlights",
        .binding = &shader_injection.tone_map_highlights,
        .default_value = 50.f,
        .label = "Highlights",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeShadows",
        .binding = &shader_injection.tone_map_shadows,
        .default_value = 50.f,
        .label = "Shadows",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeContrast",
        .binding = &shader_injection.tone_map_contrast,
        .default_value = 50.f,
        .label = "Contrast",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeSaturation",
        .binding = &shader_injection.tone_map_saturation,
        .default_value = 50.f,
        .label = "Saturation",
        .section = "Color Grading",
        .max = 100.f,
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeHighlightSaturation",
        .binding = &shader_injection.tone_map_highlight_saturation,
        .default_value = 50.f,
        .label = "Highlight Saturation",
        .section = "Color Grading",
        .tooltip = "Adds or removes highlight color.",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value * 0.02f; },
        .is_visible = []() { return current_settings_mode >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeBlowout",
        .binding = &shader_injection.tone_map_blowout,
        .default_value = 0.f,
        .label = "Blowout",
        .section = "Color Grading",
        .tooltip = "Controls highlight desaturation due to overexposure.",
        .max = 100.f,
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeFlare",
        .binding = &shader_injection.tone_map_flare,
        .default_value = 0.f,
        .label = "Flare",
        .section = "Color Grading",
        .tooltip = "Flare/Glare Compensation",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type == 3; },
        .parse = [](float value) { return value * 0.02f; },
    },
    new renodx::utils::settings::Setting{
        .key = "ColorGradeScene",
        .binding = &shader_injection.color_grade_strength,
        .default_value = 100.f,
        .label = "Scene Grading",
        .section = "Color Grading",
        .tooltip = "Scene grading as applied by the game",
        .max = 100.f,
        .is_enabled = []() { return shader_injection.tone_map_type > 0; },
        .parse = [](float value) { return value * 0.01f; },
    },
    new renodx::utils::settings::Setting{
        .key = "SwapChainCustomColorSpace",
        .binding = &shader_injection.swap_chain_custom_color_space,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Custom Color Space",
        .section = "Display Output",
        .tooltip = "Selects output color space"
                   "\nUS Modern for BT.709 D65."
                   "\nJPN Modern for BT.709 D93."
                   "\nUS CRT for BT.601 (NTSC-U)."
                   "\nJPN CRT for BT.601 ARIB-TR-B9 D93 (NTSC-J)."
                   "\nDefault: US CRT",
        .labels = {
            "US Modern",
            "JPN Modern",
            "US CRT",
            "JPN CRT",
        },
        .is_visible = []() { return settings[0]->GetValue() >= 1; },
    },
    new renodx::utils::settings::Setting{
        .key = "IntermediateDecoding",
        .binding = &shader_injection.intermediate_encoding,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Intermediate Encoding",
        .section = "Display Output",
        .labels = {"Auto", "None", "SRGB", "2.2", "2.4"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) {
            if (value == 0) return 0.f;
            return value - 1.f; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "SwapChainDecoding",
        .binding = &shader_injection.swap_chain_decoding,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Swapchain Decoding",
        .section = "Display Output",
        .labels = {"Auto", "None", "SRGB", "2.2", "2.4"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) {
            if (value == 0) return -1.f;
            return value - 1.f; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "SwapChainGammaCorrection",
        .binding = &shader_injection.swap_chain_gamma_correction,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 0.f,
        .label = "Gamma Correction",
        .section = "Display Output",
        .labels = {"None", "2.2", "2.4"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
    new renodx::utils::settings::Setting{
        .key = "SwapChainClampColorSpace",
        .binding = &shader_injection.swap_chain_clamp_color_space,
        .value_type = renodx::utils::settings::SettingValueType::INTEGER,
        .default_value = 2.f,
        .label = "Clamp Color Space",
        .section = "Display Output",
        .labels = {"None", "BT709", "BT2020", "AP1"},
        .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
        .parse = [](float value) { return value - 1.f; },
        .is_visible = []() { return current_settings_mode >= 2; },
    },
};

const std::unordered_map<std::string, reshade::api::format> UPGRADE_TARGETS = {
    {"R8G8B8A8_TYPELESS", reshade::api::format::r8g8b8a8_typeless},
    {"B8G8R8A8_TYPELESS", reshade::api::format::b8g8r8a8_typeless},
    {"R8G8B8A8_UNORM", reshade::api::format::r8g8b8a8_unorm},
    {"B8G8R8A8_UNORM", reshade::api::format::b8g8r8a8_unorm},
    {"B8G8R8X8_UNORM", reshade::api::format::b8g8r8x8_unorm},
    {"R8G8B8A8_SNORM", reshade::api::format::r8g8b8a8_snorm},
    {"R8G8B8A8_UNORM_SRGB", reshade::api::format::r8g8b8a8_unorm_srgb},
    {"B8G8R8A8_UNORM_SRGB", reshade::api::format::b8g8r8a8_unorm_srgb},
    {"B8G8R8X8_UNORM_SRGB", reshade::api::format::b8g8r8x8_unorm_srgb},
    {"R10G10B10A2_TYPELESS", reshade::api::format::r10g10b10a2_typeless},
    {"R10G10B10A2_UNORM", reshade::api::format::r10g10b10a2_unorm},
    {"B10G10R10A2_UNORM", reshade::api::format::b10g10r10a2_unorm},
    {"R11G11B10_FLOAT", reshade::api::format::r11g11b10_float},
    {"R16G16B16A16_TYPELESS", reshade::api::format::r16g16b16a16_typeless},
};

void OnPresetOff() {
  //   renodx::utils::settings::UpdateSetting("toneMapType", 0.f);
  //   renodx::utils::settings::UpdateSetting("toneMapPeakNits", 203.f);
  //   renodx::utils::settings::UpdateSetting("toneMapGameNits", 203.f);
  //   renodx::utils::settings::UpdateSetting("toneMapUINits", 203.f);
  //   renodx::utils::settings::UpdateSetting("toneMapGammaCorrection", 0);
  //   renodx::utils::settings::UpdateSetting("colorGradeExposure", 1.f);
  //   renodx::utils::settings::UpdateSetting("colorGradeHighlights", 50.f);
  //   renodx::utils::settings::UpdateSetting("colorGradeShadows", 50.f);
  //   renodx::utils::settings::UpdateSetting("colorGradeContrast", 50.f);
  //   renodx::utils::settings::UpdateSetting("colorGradeSaturation", 50.f);
  //   renodx::utils::settings::UpdateSetting("colorGradeLUTStrength", 100.f);
  //   renodx::utils::settings::UpdateSetting("colorGradeLUTScaling", 0.f);
}

const auto UPGRADE_TYPE_NONE = 0.f;
const auto UPGRADE_TYPE_OUTPUT_SIZE = 1.f;
const auto UPGRADE_TYPE_OUTPUT_RATIO = 2.f;
const auto UPGRADE_TYPE_ANY = 3.f;

bool initialized = false;

}  // namespace

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX Source Engine";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;

      if (!initialized) {
        // while (!IsDebuggerPresent()) Sleep(100);

        renodx::mods::shader::force_pipeline_cloning = true;
        renodx::mods::shader::expected_constant_buffer_space = 50;
        renodx::mods::shader::expected_constant_buffer_index = 13;
        renodx::mods::shader::allow_multiple_push_constants = true;
        renodx::mods::shader::constant_buffer_offset = 210 * 4;

        renodx::mods::swapchain::expected_constant_buffer_index = 13;
        renodx::mods::swapchain::expected_constant_buffer_space = 50;
        // renodx::mods::swapchain::target_format = reshade::api::format::b8g8r8a8_unorm;
        // renodx::mods::swapchain::target_color_space = reshade::api::color_space::srgb_nonlinear;
        renodx::mods::swapchain::use_resource_cloning = true;
        renodx::mods::swapchain::set_color_space = false;
        renodx::mods::swapchain::use_device_proxy = true;
        renodx::mods::swapchain::proxy_skip_host_present = true;
        renodx::mods::swapchain::device_proxy_wait_idle_source = true;
        renodx::mods::swapchain::swap_chain_proxy_shaders = {
            {
                reshade::api::device_api::d3d11,
                {
                    .vertex_shader = __swap_chain_proxy_vertex_shader_dx11,
                    .pixel_shader = __swap_chain_proxy_pixel_shader_dx11,
                },
            },
            {
                reshade::api::device_api::d3d12,
                {
                    .vertex_shader = __swap_chain_proxy_vertex_shader_dx12,
                    .pixel_shader = __swap_chain_proxy_pixel_shader_dx12,
                },
            },
        };

        {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "SwapChainForceBorderless",
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              .default_value = 0.f,
              .label = "Force Borderless",
              .section = "Display Output",
              .tooltip = "Forces fullscreen to be borderless for proper HDR",
              .labels = {
                  "Disabled",
                  "Enabled",
              },
              .on_change_value = [](float previous, float current) { renodx::mods::swapchain::force_borderless = (current == 1.f); },
              .is_global = true,
              .is_visible = []() { return current_settings_mode >= 2; },
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          renodx::mods::swapchain::force_borderless = (setting->GetValue() == 1.f);
          settings.push_back(setting);
        }

        {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "SwapChainPreventFullscreen",
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              .default_value = 0.f,
              .label = "Prevent Fullscreen",
              .section = "Display Output",
              .tooltip = "Prevent exclusive fullscreen for proper HDR",
              .labels = {
                  "Disabled",
                  "Enabled",
              },
              .on_change_value = [](float previous, float current) { renodx::mods::swapchain::prevent_full_screen = (current == 1.f); },
              .is_global = true,
              .is_visible = []() { return current_settings_mode >= 2; },
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          renodx::mods::swapchain::prevent_full_screen = (setting->GetValue() == 1.f);
          settings.push_back(setting);
        }

        {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "SwapChainEncoding",
              .binding = &shader_injection.swap_chain_encoding,
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              .default_value = 5.f,
              .label = "Encoding",
              .section = "Display Output",
              .labels = {"None", "SRGB", "2.2", "2.4", "HDR10", "scRGB"},
              .is_enabled = []() { return shader_injection.tone_map_type >= 1; },
              .on_change_value = [](float previous, float current) {
                bool is_hdr10 = current == 4;
                shader_injection.swap_chain_encoding_color_space = (is_hdr10 ? 1.f : 0.f);
                // return void
              },
              .is_global = true,
              .is_visible = []() { return current_settings_mode >= 2; },
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          bool is_hdr10 = setting->GetValue() == 4;
          renodx::mods::swapchain::SetUseHDR10(is_hdr10);
          shader_injection.swap_chain_encoding_color_space = is_hdr10 ? 1.f : 0.f;
          settings.push_back(setting);
        }

        for (const auto& [key, format] : UPGRADE_TARGETS) {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "Upgrade_" + key,
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              .default_value = 2.f,
              .label = key,
              .section = "Resource Upgrades",
              .labels = {
                  "Off",
                  "Output size",
                  "Output ratio",
                  "Any size",
              },
              .is_global = true,
              .is_visible = []() { return false; },
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          settings.push_back(setting);

          auto value = setting->GetValue();
          if (value > 0) {
            renodx::mods::swapchain::swap_chain_upgrade_targets.push_back({
                .old_format = format,
                .new_format = reshade::api::format::r16g16b16a16_float,
                .ignore_size = (value == UPGRADE_TYPE_ANY),
                .use_resource_view_cloning = true,
                .aspect_ratio = static_cast<float>((value == UPGRADE_TYPE_OUTPUT_RATIO)
                                                       ? renodx::mods::swapchain::SwapChainUpgradeTarget::BACK_BUFFER
                                                       : renodx::mods::swapchain::SwapChainUpgradeTarget::ANY),
                .usage_include = reshade::api::resource_usage::render_target,
            });
            std::stringstream s;
            s << "Applying user resource upgrade for ";
            s << format << ": " << value;
            reshade::log::message(reshade::log::level::info, s.str().c_str());
          }
        }

        reshade::register_event<reshade::addon_event::present>(OnScenePresent);
        reshade::register_event<reshade::addon_event::push_descriptors>(OnPushDescriptors);
        reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
        wchar_t executable_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, executable_path, MAX_PATH);
        const wchar_t* executable_name = wcsrchr(executable_path, L'\\');
        if (_wcsicmp(executable_name != nullptr ? executable_name + 1 : executable_path, L"bms.exe") == 0) {
          reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
          reshade::register_event<reshade::addon_event::destroy_resource>(OnDestroyResource);
        }

        initialized = true;
      }

      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  if (fdw_reason == DLL_PROCESS_ATTACH) {
    const auto add_engine_post = [](std::span<const uint32_t> hashes, bool (*on_draw)(reshade::api::command_list*)) {
      for (const auto hash : hashes) {
        custom_shaders.try_emplace(hash, renodx::mods::shader::CustomShader{.crc32 = hash, .on_draw = on_draw, .on_drawn = &OnVanillaEnginePostDrawn});
      }
    };
    add_engine_post(PORTAL2_ENGINE_POST_HASHES, &OnVanillaEnginePostDraw);
    add_engine_post(L4D2_ENGINE_POST_HASHES, &OnVanillaEnginePostDrawFullBloom);
    add_engine_post(BLACKMESA_ENGINE_POST_HASHES, &OnVanillaEnginePostDrawFullBloom);
    add_engine_post(MAPBASE_ENGINE_POST_HASHES, &OnVanillaEnginePostDrawFullBloom);
  }
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
