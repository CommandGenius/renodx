/*
 * Copyright (C) 2026 Carlos Lopez
 * SPDX-License-Identifier: MIT
 */

#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#define RENODX_MODS_SWAPCHAIN_VERSION 2

#include <d3d9.h>

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>

#include <embed/shaders.h>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/settings.hpp"
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

void DrawFullscreen(IDirect3DDevice9* device, IDirect3DPixelShader9* shader, std::span<IDirect3DBaseTexture9* const> textures, IDirect3DSurface9* target, const float* constants_c0 = nullptr) {
  if (shader == nullptr || target == nullptr) return;
  D3DSURFACE_DESC target_desc = {};
  target->GetDesc(&target_desc);
  IDirect3DStateBlock9* state = nullptr;
  if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state))) return;
  IDirect3DSurface9* previous_target = nullptr;
  device->GetRenderTarget(0, &previous_target);
  IDirect3DSurface9* previous_depth = nullptr;
  device->GetDepthStencilSurface(&previous_depth);

  device->SetRenderTarget(0, target);
  device->SetDepthStencilSurface(nullptr);
  device->SetVertexShader(nullptr);
  device->SetPixelShader(shader);
  if (constants_c0 != nullptr) device->SetPixelShaderConstantF(0, constants_c0, 1);
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

  const float w = static_cast<float>(target_desc.Width) - 0.5f;
  const float h = static_cast<float>(target_desc.Height) - 0.5f;
  const float quad[4][6] = {
      {-0.5f, -0.5f, 0.f, 1.f, 0.f, 0.f},
      {w, -0.5f, 0.f, 1.f, 1.f, 0.f},
      {-0.5f, h, 0.f, 1.f, 0.f, 1.f},
      {w, h, 0.f, 1.f, 1.f, 1.f},
  };
  device->DrawPrimitiveUP(D3DPT_TRIANGLESTRIP, 2, quad, sizeof(quad[0]));

  device->SetRenderTarget(0, previous_target);
  device->SetDepthStencilSurface(previous_depth);
  if (previous_target != nullptr) previous_target->Release();
  if (previous_depth != nullptr) previous_depth->Release();
  state->Apply();
  state->Release();
}

IDirect3DSurface9* CaptureSceneCopy(IDirect3DDevice9* device) {
  IDirect3DBaseTexture9* texture = nullptr;
  device->GetTexture(0, &texture);
  if (scene_copy_texture != nullptr) scene_copy_texture->Release();
  scene_copy_texture = texture;
  histogram_drawn = true;
  float light_scale[4] = {1.f, 1.f, 1.f, 1.f};
  device->GetPixelShaderConstantF(30, light_scale, 1);
  shader_injection.scene_exposure = light_scale[0] > 0.f ? 1.f / light_scale[0] : 1.f;
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

bool OnVanillaHistogramDraw(reshade::api::command_list* cmd_list) {
  auto* device = GetNativeDevice(cmd_list);
  IDirect3DSurface9* source = CaptureSceneCopy(device);
  if (source == nullptr) return true;
  IDirect3DBaseTexture9* inputs[] = {untonemapped_texture};
  DrawFullscreen(device, PostShader(device, &encode_scene_shader, __encode_scene), inputs, source);
  source->Release();
  device->SetSamplerState(0, D3DSAMP_SRGBTEXTURE, TRUE);
  return true;
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

bool OnVanillaDownsampleDraw(reshade::api::command_list* cmd_list) {
  bloom_chain_drawn = true;
  return true;
}

bool OnVanillaEnginePostDraw(reshade::api::command_list* cmd_list) {
  auto* device = GetNativeDevice(cmd_list);
  if (bloom_texture != nullptr) bloom_texture->Release();
  bloom_texture = nullptr;
  device->GetTexture(0, &bloom_texture);
  device->GetPixelShaderConstantF(5, bloom_factor, 1);
  return true;
}

void OnVanillaEnginePostDrawn(reshade::api::command_list* cmd_list) {
  if (untonemapped_texture == nullptr || !histogram_drawn) return;
  auto* device = GetNativeDevice(cmd_list);
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
      const float factor[4] = {bloom_factor[0] * 0.5f, 0.f, 0.f, 0.f};
      IDirect3DBaseTexture9* inputs[] = {graded_texture, untonemapped_texture, bloom_texture};
      DrawFullscreen(device, PostShader(device, &post_upgrade_shader, __post_upgrade), inputs, target, factor);
      engine_post_drawn = true;
    }
  }
  target->Release();
}

void OnDestroyDevice(reshade::api::device* device) {
  if (device->get_api() != reshade::api::device_api::d3d9) return;
  for (auto** texture : {&untonemapped_texture, &graded_texture}) {
    if (*texture != nullptr) (*texture)->Release();
    *texture = nullptr;
  }
  for (auto** shader : {&encode_scene_shader, &post_upgrade_shader}) {
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
  shader_injection.scene_tone_mapped = engine_post_drawn ? 1.f : 0.f;
  engine_post_drawn = false;
  bloom_chain_drawn = false;
  histogram_drawn = false;
  raw_scene_tone_mapped = false;
}

IDirect3DBaseTexture9* bloom_add_previous_texture = nullptr;
const D3DSAMPLERSTATETYPE BLOOM_ADD_SAMPLER_STATES[] = {D3DSAMP_SRGBTEXTURE, D3DSAMP_ADDRESSU, D3DSAMP_ADDRESSV, D3DSAMP_MINFILTER, D3DSAMP_MAGFILTER};
DWORD bloom_add_previous_sampler_states[std::size(BLOOM_ADD_SAMPLER_STATES)] = {};

bool OnLuminanceCompareReplace(reshade::api::command_list* cmd_list) {
  auto* native_device = GetNativeDevice(cmd_list);
  IDirect3DSurface9* source = CaptureSceneCopy(native_device);
  if (source != nullptr) source->Release();
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
    {0xEA4B3EE9, {.crc32 = 0xEA4B3EE9, .on_draw = &OnVanillaHistogramDraw}},
    {0xCFB5A0D0, {.crc32 = 0xCFB5A0D0, .on_draw = &OnVanillaDownsampleDraw}},
    {0xF6ED64EA, {.crc32 = 0xF6ED64EA, .on_draw = &OnVanillaDownsampleDraw}},
    PORTAL2_ENGINE_POST_ENTRIES,
    {0x6236B99B, {.crc32 = 0x6236B99B, .on_draw = &OnUiDraw}},
    {0xCFAFE6F6, {.crc32 = 0xCFAFE6F6, .on_draw = &OnUiDraw}},
    {0x201ADBD3, {.crc32 = 0x201ADBD3, .on_draw = &OnUiDraw}},
    {0x030AF021, {.crc32 = 0x030AF021, .on_draw = &OnUiDraw}},
    {0x23B789C1, {.crc32 = 0x23B789C1, .code = __0x23B789C1, .on_replace = &OnGammaSpaceDrawReplace, .on_draw = &OnUiDraw}},
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
        .default_value = 1.f,
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
        reshade::register_event<reshade::addon_event::destroy_device>(OnDestroyDevice);

        initialized = true;
      }

      break;
    case DLL_PROCESS_DETACH:
      reshade::unregister_addon(h_module);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  return TRUE;
}
