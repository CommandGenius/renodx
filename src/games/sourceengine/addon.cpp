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
#include "./eye_adaptation.hpp"
#include "./portal2_hashes.hpp"
#include "./shared.h"

namespace {

ShaderInjectData shader_injection = {.scene_exposure = 1.f, .bloom_strength = 0.25f};

bool engine_post_drawn = false;

void MarkPostDrawn() {
  engine_post_drawn = true;
  shader_injection.scene_post_drawn = 1.f;
}

bool bloom_chain_drawn = false;

bool histogram_drawn = false;
bool histogram_drawn_last_frame = false;
bool perspective_drawn = false;

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

using GetRenderTargetFunction = HRESULT(STDMETHODCALLTYPE*)(IDirect3DDevice9*, DWORD, IDirect3DSurface9**);
GetRenderTargetFunction original_get_render_target = nullptr;
bool hook_create_texture = false;

HRESULT CurrentRenderTarget(IDirect3DDevice9* device, IDirect3DSurface9** target) {
  return original_get_render_target != nullptr ? original_get_render_target(device, 0, target) : device->GetRenderTarget(0, target);
}

HRESULT STDMETHODCALLTYPE OnGetRenderTarget(IDirect3DDevice9* device, DWORD index, IDirect3DSurface9** target) {
  const HRESULT hr = original_get_render_target(device, index, target);
  if (FAILED(hr) || index != 0 || *target == nullptr) return hr;
  static IDirect3DSurface9* clone_surface = nullptr;
  static uint64_t clone_handle = 0;
  static IDirect3DSurface9* original_surface = nullptr;
  static IDirect3DSurface9* other_surface = nullptr;
  if (*target == other_surface) return hr;
  bool valid = false;
  if (*target == clone_surface) {
    renodx::utils::resource::GetResourceInfo({reinterpret_cast<uint64_t>(original_surface)}, [&](const renodx::utils::resource::ResourceInfo& info) {
      valid = info.is_swap_chain && !info.destroyed && info.clone.handle == clone_handle;
    });
  }
  if (!valid) {
    clone_surface = nullptr;
    clone_handle = 0;
    original_surface = nullptr;
    renodx::utils::resource::ForEachResourceInfo([&](const renodx::utils::resource::ResourceInfo& info) {
      if (!info.is_swap_chain || info.destroyed || info.clone.handle == 0u) return;
      auto* clone = reinterpret_cast<IDirect3DResource9*>(info.clone.handle);
      IDirect3DSurface9* surface = clone->GetType() == D3DRTYPE_SURFACE ? static_cast<IDirect3DSurface9*>(clone) : nullptr;
      if (clone->GetType() == D3DRTYPE_TEXTURE && SUCCEEDED(static_cast<IDirect3DTexture9*>(clone)->GetSurfaceLevel(0, &surface))) surface->Release();
      if (surface == nullptr || surface != *target) return;
      clone_surface = surface;
      clone_handle = info.clone.handle;
      original_surface = reinterpret_cast<IDirect3DSurface9*>(info.resource.handle);
    });
    if (original_surface == nullptr) {
      other_surface = *target;
      return hr;
    }
  }
  (*target)->Release();
  *target = original_surface;
  original_surface->AddRef();
  return hr;
}

void OnInitDevice(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d9) return;
  void** vtable = *reinterpret_cast<void***>(device->get_native());
  const auto patch = [vtable](size_t slot, void** original, void* hook) {
    if (*original != nullptr) return;
    DWORD protect = 0;
    if (!VirtualProtect(&vtable[slot], sizeof(void*), PAGE_READWRITE, &protect)) return;
    *original = vtable[slot];
    vtable[slot] = hook;
    VirtualProtect(&vtable[slot], sizeof(void*), protect, &protect);
  };
  if (hook_create_texture) patch(23, reinterpret_cast<void**>(&original_create_texture), reinterpret_cast<void*>(&OnCreateTexture));
  patch(38, reinterpret_cast<void**>(&original_get_render_target), reinterpret_cast<void*>(&OnGetRenderTarget));
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
    encode_scene_shader = post_upgrade_shader = nullptr;
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
    CurrentRenderTarget(device, &previous_target_);
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

float current_light_scale = 1.f;
bool light_scale_used = false;

sourceengine::EyeAdaptation eye_adaptation;

float SceneExposureFor(float light_scale) {
  return light_scale > 0.f ? eye_adaptation.exposure / light_scale : eye_adaptation.exposure;
}

IDirect3DSurface9* CaptureSceneCopy(IDirect3DDevice9* device) {
  IDirect3DBaseTexture9* texture = nullptr;
  device->GetTexture(0, &texture);
  if (scene_copy_texture != nullptr) scene_copy_texture->Release();
  scene_copy_texture = texture;
  histogram_drawn = true;
  if (current_light_scale != 1.f) light_scale_used = true;
  shader_injection.scene_exposure = SceneExposureFor(current_light_scale);
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
  eye_adaptation.Update(device, untonemapped_texture, current_light_scale);
  return source;
}

bool raw_scene_tone_mapped = false;

void TonemapRawScene(IDirect3DDevice9* device, IDirect3DSurface9* target, const D3DSURFACE_DESC& desc) {
  raw_scene_tone_mapped = true;
  if (!histogram_drawn) {
    untonemapped_texture = MatchTexture(device, untonemapped_texture, desc);
    IDirect3DSurface9* keep = nullptr;
    if (untonemapped_texture != nullptr && SUCCEEDED(untonemapped_texture->GetSurfaceLevel(0, &keep))) {
      device->StretchRect(target, nullptr, keep, nullptr, D3DTEXF_NONE);
      keep->Release();
    }
    shader_injection.scene_exposure = SceneExposureFor(current_light_scale);
  }
  if (desc.Format == D3DFMT_A16B16G16R16F && untonemapped_texture != nullptr) {
    const float params[4] = {0.f, 1.f, 0.f, 0.f};
    IDirect3DBaseTexture9* inputs[] = {untonemapped_texture, untonemapped_texture, untonemapped_texture};
    DrawFullscreen(device, PostShader(device, &post_upgrade_shader, __post_upgrade), inputs, target, params);
    MarkPostDrawn();
  }
}

bool OnUiDraw(reshade::api::command_list* cmd_list) {
  if (engine_post_drawn || bloom_chain_drawn || raw_scene_tone_mapped) return true;
  auto* device = GetNativeDevice(cmd_list);
  if (histogram_drawn) {
    if (untonemapped_texture == nullptr) return true;
  } else {
    float view_projection[4][4] = {};
    device->GetVertexShaderConstantF(8, view_projection[0], 4);
    const bool vgui = view_projection[3][0] == 0.f && view_projection[3][1] == 0.f && view_projection[3][2] == 0.f
                      && std::abs(view_projection[0][0]) < 0.1f;
    if (!perspective_drawn || !vgui || histogram_drawn_last_frame) return true;
  }
  IDirect3DSurface9* target = nullptr;
  if (FAILED(CurrentRenderTarget(device, &target)) || target == nullptr) return true;
  D3DSURFACE_DESC desc = {};
  target->GetDesc(&desc);
  if (histogram_drawn || desc.Format == D3DFMT_A16B16G16R16F) TonemapRawScene(device, target, desc);
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
  if (FAILED(CurrentRenderTarget(device, &target)) || target == nullptr) return;
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
      MarkPostDrawn();
    }
  }
  target->Release();
}

extern renodx::mods::shader::CustomShaders custom_shaders;

reshade::api::resource_view pending_scene_target = {0u};

void OnBindRenderTargets(reshade::api::command_list* cmd_list, uint32_t count, const reshade::api::resource_view* rtvs, reshade::api::resource_view) {
  pending_scene_target = {0u};
  if (cmd_list->get_device()->get_api() != reshade::api::device_api::d3d9 || count == 0 || rtvs[0].handle == 0u) return;
  renodx::utils::resource::GetResourceViewInfo(rtvs[0], [&](const renodx::utils::resource::ResourceViewInfo& info) {
    if (!info.is_clone && !info.clone_enabled && info.clone_target != nullptr && info.clone_target->use_resource_view_hot_swap) {
      pending_scene_target = rtvs[0];
    }
  });
}

void ActivateSceneTarget(reshade::api::command_list* cmd_list, IDirect3DDevice9* device, bool perspective) {
  const auto target = pending_scene_target;
  pending_scene_target = {0u};
  IDirect3DSurface9* depth = nullptr;
  device->GetDepthStencilSurface(&depth);
  if (depth == nullptr) return;
  if (!perspective || !renodx::mods::swapchain::ActivateCloneHotSwap(cmd_list->get_device(), target)) {
    depth->Release();
    return;
  }
  const auto clone = renodx::utils::resource::upgrade::GetResourceViewClone(target);
  if (clone.handle != 0u) {
    D3DVIEWPORT9 viewport = {};
    RECT scissor = {};
    device->GetViewport(&viewport);
    device->GetScissorRect(&scissor);
    device->StretchRect(reinterpret_cast<IDirect3DSurface9*>(target.handle & ~1ull), nullptr, reinterpret_cast<IDirect3DSurface9*>(clone.handle & ~1ull), nullptr, D3DTEXF_NONE);
    cmd_list->bind_render_targets_and_depth_stencil(1, &clone, {reinterpret_cast<uint64_t>(depth)});
    device->SetViewport(&viewport);
    device->SetScissorRect(&scissor);
  }
  depth->Release();
}

template <typename F>
bool OnGenericDraw(reshade::api::command_list* cmd_list, F&& draw) {
  if (cmd_list->get_device()->get_api() != reshade::api::device_api::d3d9) return false;
  auto* device = GetNativeDevice(cmd_list);
  if (!perspective_drawn || pending_scene_target.handle != 0u) {
    float w_row[4] = {};
    device->GetVertexShaderConstantF(11, w_row, 1);
    const bool perspective = w_row[0] != 0.f || w_row[1] != 0.f || w_row[2] != 0.f;
    perspective_drawn |= perspective;
    if (pending_scene_target.handle != 0u) ActivateSceneTarget(cmd_list, device, perspective);
  }
  if (perspective_drawn && !histogram_drawn) {
    float light_scale[4] = {1.f, 1.f, 1.f, 1.f};
    device->GetPixelShaderConstantF(30, light_scale, 1);
    if (light_scale[0] > 0.f) current_light_scale = light_scale[0];
  }
  if (!histogram_drawn || engine_post_drawn || untonemapped_texture == nullptr) return false;
  auto* shader_state = renodx::utils::shader::GetCurrentState(cmd_list);
  if (shader_state != nullptr && custom_shaders.contains(renodx::utils::shader::GetCurrentPixelShaderHash(shader_state))) return false;
  const bool reads_scene_at_s1 = BoundTextureIsSceneCopy(device, 1);
  const bool reads_scene_at_s0 = !reads_scene_at_s1 && !bloom_chain_drawn && BoundTextureIsSceneCopy(device, 0);
  if (!reads_scene_at_s0 && !reads_scene_at_s1) return false;
  IDirect3DSurface9* target = nullptr;
  if (FAILED(CurrentRenderTarget(device, &target)) || target == nullptr) return false;
  D3DSURFACE_DESC desc = {};
  target->GetDesc(&desc);
  target->Release();
  D3DSURFACE_DESC scene_desc = {};
  untonemapped_texture->GetLevelDesc(0, &scene_desc);
  if (reads_scene_at_s0 && desc.Width < scene_desc.Width) {
    OnVanillaDownsampleDraw(cmd_list);
    draw();
    OnVanillaDownsampleDrawn(cmd_list);
    return true;
  }
  if (reads_scene_at_s1 && desc.Width == scene_desc.Width && desc.Height == scene_desc.Height && desc.Format == D3DFMT_A16B16G16R16F) {
    OnVanillaEnginePostDrawFullBloom(cmd_list);
    draw();
    OnVanillaEnginePostDrawn(cmd_list);
    return true;
  }
  return false;
}

bool OnDraw(reshade::api::command_list* cmd_list, uint32_t vertex_count, uint32_t instance_count, uint32_t first_vertex, uint32_t first_instance) {
  return OnGenericDraw(cmd_list, [&] { cmd_list->draw(vertex_count, instance_count, first_vertex, first_instance); });
}

bool OnDrawIndexed(reshade::api::command_list* cmd_list, uint32_t index_count, uint32_t instance_count, uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
  return OnGenericDraw(cmd_list, [&] { cmd_list->draw_indexed(index_count, instance_count, first_index, vertex_offset, first_instance); });
}

IDirect3DTexture9* readback_texture = nullptr;
IDirect3DTexture9* scene_resolve_texture = nullptr;
IDirect3DTexture9* readback_scene_texture = nullptr;

void OnDestroyDevice(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d9) return;
  for (auto** texture : {&untonemapped_texture, &graded_texture, &encoded_texture, &readback_texture, &readback_scene_texture, &scene_resolve_texture}) {
    if (*texture != nullptr) (*texture)->Release();
    *texture = nullptr;
  }
  for (auto** shader : {&encode_scene_shader, &post_upgrade_shader}) {
    if (*shader != nullptr) (*shader)->Release();
    *shader = nullptr;
  }
  eye_adaptation.Release();
  post_shader_device = nullptr;
}

bool OnEnginePostReplace(reshade::api::command_list* cmd_list) {
  shader_injection.bloom_valid = bloom_chain_drawn ? 1.f : 0.f;
  MarkPostDrawn();
  return true;
}

bool IsThumbnailTarget(const D3DSURFACE_DESC& desc) {
  return desc.Pool == D3DPOOL_DEFAULT && (desc.Usage & D3DUSAGE_RENDERTARGET) != 0 && desc.Width <= 512 && desc.Height <= 512;
}

IDirect3DSurface9* SurfaceOf(uint64_t handle, uint32_t subresource) {
  auto* resource = reinterpret_cast<IDirect3DResource9*>(handle);
  IDirect3DSurface9* surface = nullptr;
  if (resource == nullptr) return nullptr;
  if (resource->GetType() == D3DRTYPE_TEXTURE) {
    static_cast<IDirect3DTexture9*>(resource)->GetSurfaceLevel(subresource, &surface);
  } else if (resource->GetType() == D3DRTYPE_SURFACE) {
    surface = static_cast<IDirect3DSurface9*>(resource);
    surface->AddRef();
  }
  return surface;
}

bool OnReadback(reshade::api::command_list* cmd_list, reshade::api::resource source, uint32_t source_subresource, IDirect3DSurface9* dest_surface, const D3DSURFACE_DESC& desc) {
  uint64_t clone = 0;
  renodx::utils::resource::GetResourceInfo(source, [&clone](const renodx::utils::resource::ResourceInfo& info) { clone = info.clone.handle; });
  auto* clone_resource = reinterpret_cast<IDirect3DResource9*>(clone);
  if (clone_resource == nullptr || clone_resource->GetType() != D3DRTYPE_TEXTURE || (desc.Format != D3DFMT_A8R8G8B8 && desc.Format != D3DFMT_X8R8G8B8)) return false;
  auto* device = GetNativeDevice(cmd_list);
  readback_texture = MatchTexture(device, readback_texture, desc);
  IDirect3DSurface9* encoded = nullptr;
  if (readback_texture == nullptr || FAILED(readback_texture->GetSurfaceLevel(0, &encoded))) return false;
  IDirect3DBaseTexture9* inputs[] = {static_cast<IDirect3DTexture9*>(clone_resource)};
  DrawFullscreen(device, PostShader(device, &encode_scene_shader, __encode_scene), inputs, encoded);
  const HRESULT hr = device->GetRenderTargetData(encoded, dest_surface);
  encoded->Release();
  std::stringstream log;
  log << "sourceengine: readback of clone subresource " << source_subresource << " to format " << desc.Format << " " << desc.Width << "x" << desc.Height << " hr " << std::hex << static_cast<uint32_t>(hr);
  reshade::log::message(reshade::log::level::info, log.str().c_str());
  return SUCCEEDED(hr);
}

bool CopyFromSwapChain(reshade::api::command_list* cmd_list, reshade::api::resource source, const reshade::api::subresource_box* source_box, reshade::api::resource dest, uint32_t dest_subresource, const reshade::api::subresource_box* dest_box, reshade::api::filter_mode filter) {
  uint64_t scene_handle = 0;
  renodx::utils::resource::GetResourceInfo(source, [&scene_handle](const renodx::utils::resource::ResourceInfo& info) {
    if (info.is_swap_chain) scene_handle = info.clone.handle;
  });
  IDirect3DSurface9* scene_surface = SurfaceOf(scene_handle, 0);
  if (scene_surface == nullptr) return false;
  auto* device = GetNativeDevice(cmd_list);
  D3DSURFACE_DESC scene_desc = {};
  scene_surface->GetDesc(&scene_desc);
  if (scene_desc.MultiSampleType != D3DMULTISAMPLE_NONE) {
    scene_resolve_texture = MatchTexture(device, scene_resolve_texture, scene_desc);
    IDirect3DSurface9* resolved = nullptr;
    if (scene_resolve_texture == nullptr || FAILED(scene_resolve_texture->GetSurfaceLevel(0, &resolved))) {
      scene_surface->Release();
      return false;
    }
    const HRESULT resolve_hr = device->StretchRect(scene_surface, nullptr, resolved, nullptr, D3DTEXF_NONE);
    scene_surface->Release();
    scene_surface = resolved;
    if (FAILED(resolve_hr)) {
      reshade::log::message(reshade::log::level::warning, "sourceengine: swap chain resolve failed");
      scene_surface->Release();
      return false;
    }
  }

  IDirect3DSurface9* dest_level = SurfaceOf(dest.handle, dest_subresource);
  if (dest_level == nullptr) {
    scene_surface->Release();
    return false;
  }
  renodx::mods::swapchain::ActivateCloneHotSwap(cmd_list->get_device(), {reinterpret_cast<uint64_t>(dest_level)});
  dest_level->Release();
  const uint64_t dest_clone = renodx::utils::resource::upgrade::GetResourceClone(dest).handle;
  IDirect3DSurface9* dest_surface = SurfaceOf(dest_clone != 0u ? dest_clone : dest.handle, dest_subresource);
  if (dest_surface == nullptr) {
    scene_surface->Release();
    return false;
  }
  D3DSURFACE_DESC desc = {};
  dest_surface->GetDesc(&desc);
  const auto to_rect = [](const reshade::api::subresource_box* box) {
    return box == nullptr ? std::optional<RECT>{} : std::optional<RECT>{{static_cast<LONG>(box->left), static_cast<LONG>(box->top), static_cast<LONG>(box->right), static_cast<LONG>(box->bottom)}};
  };
  const auto source_rect = to_rect(source_box);
  const auto dest_rect = to_rect(dest_box);
  const auto stretch = [&](IDirect3DSurface9* target, const RECT* target_rect, D3DTEXTUREFILTERTYPE texture_filter) {
    return device->StretchRect(scene_surface, source_rect ? &*source_rect : nullptr, target, target_rect, texture_filter);
  };
  HRESULT hr = E_FAIL;
  if (dest_clone == 0u && IsThumbnailTarget(desc) && desc.Format != D3DFMT_A16B16G16R16F) {
    D3DSURFACE_DESC scaled_desc = desc;
    scaled_desc.Format = D3DFMT_A16B16G16R16F;
    readback_scene_texture = MatchTexture(device, readback_scene_texture, scaled_desc);
    IDirect3DSurface9* scaled = nullptr;
    if (readback_scene_texture != nullptr && SUCCEEDED(readback_scene_texture->GetSurfaceLevel(0, &scaled))) {
      hr = stretch(scaled, dest_rect ? &*dest_rect : nullptr, D3DTEXF_LINEAR);
      if (SUCCEEDED(hr)) {
        IDirect3DBaseTexture9* inputs[] = {readback_scene_texture};
        DrawFullscreen(device, PostShader(device, &encode_scene_shader, __encode_scene), inputs, dest_surface);
      }
      scaled->Release();
    }
    std::stringstream log;
    log << "sourceengine: thumbnail copy to format " << desc.Format << " " << desc.Width << "x" << desc.Height << " hr " << std::hex << static_cast<uint32_t>(hr);
    reshade::log::message(reshade::log::level::info, log.str().c_str());
  } else {
    hr = stretch(dest_surface, dest_rect ? &*dest_rect : nullptr, filter == reshade::api::filter_mode::min_mag_mip_point ? D3DTEXF_POINT : D3DTEXF_LINEAR);
    if (FAILED(hr)) {
      std::stringstream log;
      log << "sourceengine: swap chain copy to format " << desc.Format << " " << desc.Width << "x" << desc.Height << " (clone " << dest_clone << ") failed hr " << std::hex << static_cast<uint32_t>(hr);
      reshade::log::message(reshade::log::level::warning, log.str().c_str());
    }
  }
  dest_surface->Release();
  scene_surface->Release();
  return SUCCEEDED(hr);
}

bool OnCopyTextureRegion(reshade::api::command_list* cmd_list, reshade::api::resource source, uint32_t source_subresource, const reshade::api::subresource_box* source_box, reshade::api::resource dest, uint32_t dest_subresource, const reshade::api::subresource_box* dest_box, reshade::api::filter_mode filter) {
  if (cmd_list->get_device()->get_api() != reshade::api::device_api::d3d9) return false;
  if (auto* dest_resource = reinterpret_cast<IDirect3DResource9*>(dest.handle); dest_resource->GetType() == D3DRTYPE_SURFACE) {
    D3DSURFACE_DESC desc = {};
    auto* dest_surface = static_cast<IDirect3DSurface9*>(dest_resource);
    if (SUCCEEDED(dest_surface->GetDesc(&desc)) && desc.Pool == D3DPOOL_SYSTEMMEM) return OnReadback(cmd_list, source, source_subresource, dest_surface, desc);
  }
  return CopyFromSwapChain(cmd_list, source, source_box, dest, dest_subresource, dest_box, filter);
}

bool OnResolveTextureRegion(reshade::api::command_list* cmd_list, reshade::api::resource source, uint32_t, const reshade::api::subresource_box* source_box, reshade::api::resource dest, uint32_t dest_subresource, uint32_t dest_x, uint32_t dest_y, uint32_t dest_z, reshade::api::format) {
  if (cmd_list->get_device()->get_api() != reshade::api::device_api::d3d9) return false;
  reshade::api::subresource_box dest_box = {};
  if (source_box != nullptr) {
    dest_box = {dest_x, dest_y, dest_z, dest_x + source_box->width(), dest_y + source_box->height(), dest_z + source_box->depth()};
  } else {
    const auto desc = cmd_list->get_device()->get_resource_desc(source);
    dest_box = {dest_x, dest_y, dest_z, dest_x + desc.texture.width, dest_y + desc.texture.height, dest_z + 1};
  }
  return CopyFromSwapChain(cmd_list, source, source_box, dest, dest_subresource, &dest_box, reshade::api::filter_mode::min_mag_mip_point);
}

void OnScenePresent(reshade::api::command_queue* queue, reshade::api::swapchain* swapchain, const reshade::api::rect* source_rect, const reshade::api::rect* dest_rect, uint32_t dirty_rect_count, const reshade::api::rect* dirty_rects) {
  if (queue->get_device()->get_api() != reshade::api::device_api::d3d9) return;
  auto* device = reinterpret_cast<IDirect3DDevice9*>(queue->get_device()->get_native());
  if (!histogram_drawn && histogram_drawn_last_frame && light_scale_used && !engine_post_drawn && !raw_scene_tone_mapped && perspective_drawn) {
    IDirect3DSurface9* target = nullptr;
    if (SUCCEEDED(CurrentRenderTarget(device, &target)) && target != nullptr) {
      D3DSURFACE_DESC desc = {};
      target->GetDesc(&desc);
      if (desc.Format == D3DFMT_A16B16G16R16F) TonemapRawScene(device, target, desc);
      target->Release();
    }
  }
  shader_injection.scene_tone_mapped = engine_post_drawn ? 1.f : 0.f;
  engine_post_drawn = false;
  shader_injection.scene_post_drawn = 0.f;
  bloom_chain_drawn = false;
  histogram_drawn_last_frame = histogram_drawn;
  histogram_drawn = false;
  raw_scene_tone_mapped = false;
  perspective_drawn = false;
  scene_encoded = false;
}

IDirect3DBaseTexture9* bloom_add_previous_texture = nullptr;
const D3DSAMPLERSTATETYPE BLOOM_ADD_SAMPLER_STATES[] = {D3DSAMP_SRGBTEXTURE, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER};
DWORD bloom_add_previous_sampler_states[std::size(BLOOM_ADD_SAMPLER_STATES)] = {};

bool OnLuminanceCompareReplace(reshade::api::command_list* cmd_list) {
  auto* native_device = GetNativeDevice(cmd_list);
  IDirect3DSurface9* source = CaptureSceneCopy(native_device);
  if (source != nullptr) source->Release();
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
  MarkPostDrawn();
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
        for (const auto format : {reshade::api::format::b8g8r8a8_unorm, reshade::api::format::b8g8r8x8_unorm}) {
          renodx::mods::swapchain::swap_chain_upgrade_targets.push_back({
              .old_format = format,
              .new_format = reshade::api::format::r16g16b16a16_float,
              .use_resource_view_cloning = true,
              .use_resource_view_hot_swap = true,
              .dimensions = {.width = 1024, .height = 1024},
              .usage_include = reshade::api::resource_usage::render_target,
          });
        }

        reshade::register_event<reshade::addon_event::present>(OnScenePresent);
        reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(OnBindRenderTargets);
        reshade::register_event<reshade::addon_event::draw>(OnDraw);
        reshade::register_event<reshade::addon_event::draw_indexed>(OnDrawIndexed);
        reshade::register_event<reshade::addon_event::copy_texture_region>(OnCopyTextureRegion);
        reshade::register_event<reshade::addon_event::resolve_texture_region>(OnResolveTextureRegion);
        reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);
        wchar_t executable_path[MAX_PATH] = {};
        GetModuleFileNameW(nullptr, executable_path, MAX_PATH);
        const wchar_t* executable_name = wcsrchr(executable_path, L'\\');
        const wchar_t* executable = executable_name != nullptr ? executable_name + 1 : executable_path;
        hook_create_texture = _wcsicmp(executable, L"bms.exe") == 0;
        reshade::register_event<reshade::addon_event::init_device>(OnInitDevice);
        if (hook_create_texture) reshade::register_event<reshade::addon_event::destroy_resource>(OnDestroyResource);

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
    add_engine_post(HL2_ENGINE_POST_HASHES, &OnVanillaEnginePostDrawFullBloom);
  }
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
