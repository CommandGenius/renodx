#define ImTextureID ImU64

#define DEBUG_LEVEL_0

#define RENODX_MODS_SWAPCHAIN_VERSION 2

#include <deps/imgui/imgui.h>
#include <include/reshade.hpp>
#include <Windows.h>

#include <d3d9.h>
#include <d3dcompiler.h>

#include <embed/shaders.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iomanip>
#include <iterator>
#include <limits>
#include <mutex>
#include <ranges>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "../../mods/shader.hpp"
#include "../../mods/swapchain.hpp"
#include "../../utils/resource.hpp"
#include "../../utils/settings.hpp"
#include "./shared.h"

namespace {

renodx::mods::shader::CustomShaders custom_shaders = {
    __ALL_CUSTOM_SHADERS,
};

ShaderInjectData shader_injection;

float current_settings_mode = 0;

constexpr bool DX9_READBACK_FIX_ENABLED = true;

constexpr bool DX9_READBACK_ENCODE_SRGB = true;

constexpr bool DX9_READBACK_FORCE_OPAQUE_ALPHA = false;

thread_local bool g_inside_dx9_replacement_copy = false;

uint32_t g_dx9_readback_success_logs = 0;
uint32_t g_dx9_readback_failure_logs = 0;
uint32_t g_dx9_chain_logs = 0;

struct ScopedDX9ReplacementCopy {
  ScopedDX9ReplacementCopy() { g_inside_dx9_replacement_copy = true; }
  ~ScopedDX9ReplacementCopy() { g_inside_dx9_replacement_copy = false; }

  ScopedDX9ReplacementCopy(const ScopedDX9ReplacementCopy&) = delete;
  ScopedDX9ReplacementCopy& operator=(const ScopedDX9ReplacementCopy&) = delete;
};

struct DX9CopyEndpoint {
  reshade::api::resource input = {0u};
  reshade::api::resource original = {0u};
  reshade::api::resource clone = {0u};

  reshade::api::resource_desc input_desc = {};
  reshade::api::resource_desc original_desc = {};
  reshade::api::resource_desc clone_desc = {};

  bool has_live_tracking = false;
  bool has_clone = false;
  bool input_is_clone = false;
  bool clone_enabled = false;
};

struct DX9CloneLookupCacheEntry {
  reshade::api::resource original = {0u};
};

std::mutex g_dx9_clone_lookup_cache_mutex;
std::unordered_map<uint64_t, DX9CloneLookupCacheEntry>
    g_dx9_clone_lookup_cache;

struct DX9ReadbackStagingCache {
  reshade::api::device* device = nullptr;
  reshade::api::resource resource = {0u};
  reshade::api::resource_desc desc = {};
};

std::mutex g_dx9_readback_staging_mutex;
DX9ReadbackStagingCache g_dx9_readback_staging_cache;

uint32_t g_dx9_clone_cache_hits = 0u;
uint32_t g_dx9_clone_cache_misses = 0u;
uint32_t g_dx9_staging_reuses = 0u;
uint32_t g_dx9_staging_creates = 0u;

void RememberDX9CloneMapping(const DX9CopyEndpoint& endpoint) {
  if (!endpoint.has_clone
      || endpoint.clone.handle == 0u
      || endpoint.original.handle == 0u
      || endpoint.clone.handle == endpoint.original.handle) {
    return;
  }

  std::scoped_lock lock(g_dx9_clone_lookup_cache_mutex);

  g_dx9_clone_lookup_cache[
      static_cast<uint64_t>(endpoint.clone.handle)] = {
          .original = endpoint.original,
      };
}

bool TryResolveDX9CloneFromCache(
    reshade::api::device* device,
    reshade::api::resource input,
    DX9CopyEndpoint& endpoint) {
  if (device == nullptr || input.handle == 0u) return false;

  DX9CloneLookupCacheEntry cached = {};

  {
    std::scoped_lock lock(g_dx9_clone_lookup_cache_mutex);

    const auto it = g_dx9_clone_lookup_cache.find(
        static_cast<uint64_t>(input.handle));

    if (it == g_dx9_clone_lookup_cache.end()) {
      ++g_dx9_clone_cache_misses;
      return false;
    }

    cached = it->second;
  }

  if (cached.original.handle == 0u) return false;

  bool valid = false;

  renodx::utils::resource::GetLiveResourceInfo(
      cached.original,
      [&](const renodx::utils::resource::ResourceInfo& info) {
        if (info.destroyed
            || info.resource.handle == 0u
            || info.clone.handle != input.handle) {
          return;
        }

        endpoint.has_live_tracking = true;
        endpoint.input_is_clone = true;
        endpoint.original = info.resource;
        endpoint.original_desc =
            info.desc.type != reshade::api::resource_type::unknown
                ? info.desc
                : endpoint.original_desc;
        endpoint.clone = input;
        endpoint.clone_desc =
            info.clone_desc.type != reshade::api::resource_type::unknown
                ? info.clone_desc
                : endpoint.input_desc;
        endpoint.has_clone = true;
        endpoint.clone_enabled = info.clone_enabled;

        valid = true;
      });

  if (valid) {
    ++g_dx9_clone_cache_hits;
    return true;
  }

  {
    std::scoped_lock lock(g_dx9_clone_lookup_cache_mutex);
    g_dx9_clone_lookup_cache.erase(
        static_cast<uint64_t>(input.handle));
  }

  ++g_dx9_clone_cache_misses;
  return false;
}

bool DX9StagingDescMatches(
    const reshade::api::resource_desc& cached,
    const reshade::api::resource_desc& wanted) {
  if (cached.type == reshade::api::resource_type::unknown
      || wanted.type == reshade::api::resource_type::unknown) {
    return false;
  }

  return cached.type == wanted.type
      && cached.heap == wanted.heap
      && cached.usage == wanted.usage
      && cached.texture.format == wanted.texture.format
      && cached.texture.width == wanted.texture.width
      && cached.texture.height == wanted.texture.height
      && cached.texture.depth_or_layers
          == wanted.texture.depth_or_layers
      && cached.texture.levels == wanted.texture.levels
      && cached.texture.samples == wanted.texture.samples;
}

bool EnsureDX9ReadbackStaging(
    reshade::api::device* device,
    const reshade::api::resource_desc& wanted_desc,
    reshade::api::resource& staging) {
  if (device == nullptr) return false;

  auto& cache = g_dx9_readback_staging_cache;

  if (cache.device == device
      && cache.resource.handle != 0u
      && DX9StagingDescMatches(cache.desc, wanted_desc)) {
    staging = cache.resource;
    ++g_dx9_staging_reuses;
    return true;
  }

  if (cache.device == device && cache.resource.handle != 0u) {
    device->destroy_resource(cache.resource);
  }

  cache = {};
  cache.device = device;

  if (!device->create_resource(
          wanted_desc,
          nullptr,
          reshade::api::resource_usage::copy_dest,
          &cache.resource)) {
    cache = {};
    return false;
  }

  cache.desc = wanted_desc;
  staging = cache.resource;
  ++g_dx9_staging_creates;
  return true;
}

void InvalidateDX9ReadbackStaging(
    reshade::api::device* device) {
  auto& cache = g_dx9_readback_staging_cache;

  if (device != nullptr
      && cache.device == device
      && cache.resource.handle != 0u) {
    device->destroy_resource(cache.resource);
  }

  cache = {};
}

void ClearDX9ReadbackOptimizationCaches() {
  {
    std::scoped_lock lock(g_dx9_clone_lookup_cache_mutex);
    g_dx9_clone_lookup_cache.clear();
  }

  g_dx9_readback_staging_cache = {};
}

bool IsTextureResource(const reshade::api::resource_desc& desc) {
  return desc.type == reshade::api::resource_type::surface
      || desc.type == reshade::api::resource_type::texture_1d
      || desc.type == reshade::api::resource_type::texture_2d
      || desc.type == reshade::api::resource_type::texture_3d;
}

bool SameTextureExtent(const reshade::api::resource_desc& left,
                       const reshade::api::resource_desc& right) {
  if (!IsTextureResource(left) || !IsTextureResource(right)) return false;

  return left.texture.width == right.texture.width
      && left.texture.height == right.texture.height
      && left.texture.depth_or_layers == right.texture.depth_or_layers;
}

bool ExactCopyCompatible(const reshade::api::resource_desc& source_desc,
                         const reshade::api::resource_desc& dest_desc) {
  if (source_desc.type != dest_desc.type) return false;
  if (!SameTextureExtent(source_desc, dest_desc)) return false;

  return source_desc.texture.format == dest_desc.texture.format
      && source_desc.texture.levels == dest_desc.texture.levels
      && source_desc.texture.samples == dest_desc.texture.samples;
}

DX9CopyEndpoint ResolveDX9CopyEndpoint(
    reshade::api::device* device,
    reshade::api::resource input) {
  DX9CopyEndpoint endpoint = {};
  endpoint.input = input;
  endpoint.original = input;

  if (device == nullptr || input.handle == 0u) return endpoint;

  endpoint.input_desc =
      renodx::utils::resource::GetResourceDesc(device, input);
  endpoint.original_desc = endpoint.input_desc;

  endpoint.has_live_tracking =
      renodx::utils::resource::GetLiveResourceInfo(
          input,
          [&](const renodx::utils::resource::ResourceInfo& info) {
            endpoint.original =
                info.resource.handle != 0u ? info.resource : input;
            endpoint.original_desc =
                info.desc.type != reshade::api::resource_type::unknown
                    ? info.desc
                    : endpoint.input_desc;

            endpoint.clone = info.clone;
            endpoint.clone_desc = info.clone_desc;
            endpoint.has_clone =
                info.clone.handle != 0u
                && info.clone_desc.type
                    != reshade::api::resource_type::unknown;
            endpoint.input_is_clone = info.is_clone;
            endpoint.clone_enabled = info.clone_enabled;
          });

  RememberDX9CloneMapping(endpoint);

  const bool may_be_clone_handle =
      endpoint.input_desc.type != reshade::api::resource_type::unknown
      && endpoint.input_desc.texture.format
          == reshade::api::format::r16g16b16a16_float;

  if (may_be_clone_handle
      && (!endpoint.has_clone || endpoint.input_is_clone)) {
    if (TryResolveDX9CloneFromCache(
            device,
            input,
            endpoint)) {
      RememberDX9CloneMapping(endpoint);
      return endpoint;
    }

    bool found_parent = false;

    renodx::utils::resource::ForEachResourceInfo(
        [&](const renodx::utils::resource::ResourceInfo& info) {
          if (found_parent) return;
          if (info.destroyed || info.clone.handle != input.handle) return;

          found_parent = true;
          endpoint.has_live_tracking = true;
          endpoint.input_is_clone = true;
          endpoint.original = info.resource;
          endpoint.original_desc = info.desc;
          endpoint.clone = input;
          endpoint.clone_desc =
              info.clone_desc.type != reshade::api::resource_type::unknown
                  ? info.clone_desc
                  : endpoint.input_desc;
          endpoint.has_clone = true;
          endpoint.clone_enabled = info.clone_enabled;
        });

    RememberDX9CloneMapping(endpoint);
  }

  return endpoint;
}

reshade::api::resource SelectCloneForCopy(const DX9CopyEndpoint& endpoint) {
  if (endpoint.has_clone && endpoint.clone.handle != 0u) {
    return endpoint.clone;
  }
  return endpoint.input;
}

reshade::api::resource_desc SelectCloneDescForCopy(
    const DX9CopyEndpoint& endpoint) {
  if (endpoint.has_clone
      && endpoint.clone_desc.type
          != reshade::api::resource_type::unknown) {
    return endpoint.clone_desc;
  }
  return endpoint.input_desc;
}

bool IsFloat16RGBA(reshade::api::format format) {
  return format == reshade::api::format::r16g16b16a16_float;
}

bool IsSupportedSDRReadbackFormat(reshade::api::format format) {
  switch (format) {
    case reshade::api::format::b8g8r8a8_unorm:
    case reshade::api::format::b8g8r8a8_unorm_srgb:
    case reshade::api::format::b8g8r8x8_unorm:
    case reshade::api::format::b8g8r8x8_unorm_srgb:
    case reshade::api::format::r8g8b8a8_unorm:
    case reshade::api::format::r8g8b8a8_unorm_srgb:
    case reshade::api::format::r8g8b8x8_unorm:
    case reshade::api::format::r8g8b8x8_unorm_srgb:
      return true;
    default:
      return false;
  }
}

bool IsBGRAReadbackFormat(reshade::api::format format) {
  switch (format) {
    case reshade::api::format::b8g8r8a8_unorm:
    case reshade::api::format::b8g8r8a8_unorm_srgb:
    case reshade::api::format::b8g8r8x8_unorm:
    case reshade::api::format::b8g8r8x8_unorm_srgb:
      return true;
    default:
      return false;
  }
}

bool IsX8ReadbackFormat(reshade::api::format format) {
  switch (format) {
    case reshade::api::format::b8g8r8x8_unorm:
    case reshade::api::format::b8g8r8x8_unorm_srgb:
    case reshade::api::format::r8g8b8x8_unorm:
    case reshade::api::format::r8g8b8x8_unorm_srgb:
      return true;
    default:
      return false;
  }
}

bool IsCPUVisibleReadbackHeap(reshade::api::memory_heap heap) {
  return heap == reshade::api::memory_heap::gpu_to_cpu
      || heap == reshade::api::memory_heap::cpu_only;
}


constexpr bool DX9_GPU_READBACK_BLIT_ENABLED = true;

uint32_t g_dx9_gpu_blit_success_logs = 0u;
uint32_t g_dx9_gpu_blit_failure_logs = 0u;

struct DX9GPUReadbackBlitVertex {
  float x;
  float y;
  float z;
  float w;
  float u;
  float v;
};

struct DX9GPUReadbackBlitCache {
  IDirect3DDevice9* device = nullptr;
  IDirect3DVertexShader9* vertex_shader = nullptr;
  IDirect3DPixelShader9* pixel_shader = nullptr;
  IDirect3DVertexDeclaration9* vertex_declaration = nullptr;
  IDirect3DStateBlock9* state_block = nullptr;

  IDirect3DTexture9* source_scratch_texture = nullptr;
  uint32_t source_scratch_width = 0u;
  uint32_t source_scratch_height = 0u;
};

std::mutex g_dx9_gpu_blit_mutex;
DX9GPUReadbackBlitCache g_dx9_gpu_blit_cache;

template <typename T>
void ReleaseDX9COM(T*& object) {
  if (object != nullptr) {
    object->Release();
    object = nullptr;
  }
}

void DestroyDX9GPUReadbackBlitCacheLocked() {
  auto& cache = g_dx9_gpu_blit_cache;

  ReleaseDX9COM(cache.source_scratch_texture);
  ReleaseDX9COM(cache.state_block);
  ReleaseDX9COM(cache.vertex_declaration);
  ReleaseDX9COM(cache.pixel_shader);
  ReleaseDX9COM(cache.vertex_shader);

  cache = {};
}

void DestroyDX9GPUReadbackBlitCache(reshade::api::device* device = nullptr) {
  std::scoped_lock lock(g_dx9_gpu_blit_mutex);

  if (device != nullptr) {
    if (device->get_api() != reshade::api::device_api::d3d9) return;

    auto* native_device =
        reinterpret_cast<IDirect3DDevice9*>(device->get_native());
    if (native_device == nullptr
        || g_dx9_gpu_blit_cache.device != native_device) {
      return;
    }
  }

  DestroyDX9GPUReadbackBlitCacheLocked();
}

void OnDX9ReadbackDestroyDevice(reshade::api::device* device) {
  DestroyDX9GPUReadbackBlitCache(device);
}

void OnDX9ReadbackDestroySwapchain(
    reshade::api::swapchain* swapchain,
    bool resize) {
  (void)resize;
  if (swapchain == nullptr) return;

  DestroyDX9GPUReadbackBlitCache(swapchain->get_device());
}

void LogDX9GPUBlitFailure(const char* reason) {
  if (g_dx9_gpu_blit_failure_logs >= 8u) return;
  ++g_dx9_gpu_blit_failure_logs;

  std::stringstream stream;
  stream << "[RenoDX DX9 Readback GPU Blit] " << reason;
  reshade::log::message(
      reshade::log::level::warning,
      stream.str().c_str());
}

struct ScopedDX9NativeSurface {
  IDirect3DSurface9* surface = nullptr;
  bool owns_reference = false;

  ~ScopedDX9NativeSurface() {
    if (owns_reference && surface != nullptr) {
      surface->Release();
    }
  }

  ScopedDX9NativeSurface() = default;
  ScopedDX9NativeSurface(const ScopedDX9NativeSurface&) = delete;
  ScopedDX9NativeSurface& operator=(const ScopedDX9NativeSurface&) = delete;
};

bool AcquireDX9NativeSurface(
    reshade::api::resource resource,
    const reshade::api::resource_desc& desc,
    uint32_t subresource,
    ScopedDX9NativeSurface& output) {
  if (resource.handle == 0u) return false;

  if (desc.type == reshade::api::resource_type::surface) {
    output.surface =
        reinterpret_cast<IDirect3DSurface9*>(resource.handle);
    output.owns_reference = false;
    return output.surface != nullptr && subresource == 0u;
  }

  if (desc.type == reshade::api::resource_type::texture_2d) {
    auto* texture =
        reinterpret_cast<IDirect3DTexture9*>(resource.handle);
    if (texture == nullptr) return false;

    IDirect3DSurface9* surface = nullptr;
    const HRESULT hr = texture->GetSurfaceLevel(subresource, &surface);
    if (FAILED(hr) || surface == nullptr) return false;

    output.surface = surface;
    output.owns_reference = true;
    return true;
  }

  return false;
}

struct ScopedDX9NativeTexture {
  IDirect3DTexture9* texture = nullptr;
  bool owns_reference = false;

  ~ScopedDX9NativeTexture() {
    if (owns_reference && texture != nullptr) {
      texture->Release();
    }
  }

  ScopedDX9NativeTexture() = default;
  ScopedDX9NativeTexture(const ScopedDX9NativeTexture&) = delete;
  ScopedDX9NativeTexture& operator=(const ScopedDX9NativeTexture&) = delete;
};

bool TryAcquireDX9TextureContainer(
    reshade::api::resource resource,
    const reshade::api::resource_desc& desc,
    ScopedDX9NativeTexture& output) {
  if (resource.handle == 0u) return false;

  if (desc.type == reshade::api::resource_type::texture_2d) {
    output.texture =
        reinterpret_cast<IDirect3DTexture9*>(resource.handle);
    output.owns_reference = false;
    return output.texture != nullptr;
  }

  if (desc.type != reshade::api::resource_type::surface) return false;

  auto* surface =
      reinterpret_cast<IDirect3DSurface9*>(resource.handle);
  if (surface == nullptr) return false;

  IDirect3DTexture9* texture = nullptr;
  const HRESULT hr = surface->GetContainer(
      __uuidof(IDirect3DTexture9),
      reinterpret_cast<void**>(&texture));
  if (FAILED(hr) || texture == nullptr) return false;

  output.texture = texture;
  output.owns_reference = true;
  return true;
}

static constexpr char DX9_READBACK_BLIT_VERTEX_HLSL[] = R"hlsl(
float4 gInvTargetSize : register(c0);

struct VSInput {
    float4 position : POSITION0;
    float2 texcoord : TEXCOORD0;
};

struct VSOutput {
    float4 position : POSITION0;
    float2 texcoord : TEXCOORD0;
};

VSOutput main(VSInput input) {
    VSOutput output;
    output.position = input.position;
    output.position.xy += float2(-gInvTargetSize.x, gInvTargetSize.y)
                        * output.position.w;
    output.texcoord = input.texcoord;
    return output;
}
)hlsl";

static constexpr char DX9_READBACK_BLIT_PIXEL_HLSL[] = R"hlsl(
sampler2D gSource : register(s0);

float3 LinearToSRGB(float3 linearColor) {
    linearColor = saturate(linearColor);
    const float3 low = linearColor * 12.92f;
    const float3 high = 1.055f * pow(linearColor, 1.0f / 2.4f) - 0.055f;
    const float3 useHigh = step(0.0031308f, linearColor);
    return lerp(low, high, useHigh);
}

float4 main(float2 texcoord : TEXCOORD0) : COLOR0 {
    const float4 source = tex2D(gSource, texcoord);
    return float4(LinearToSRGB(source.rgb), saturate(source.a));
}
)hlsl";

using DX9D3DCompileFn = HRESULT(WINAPI*)(
    LPCVOID,
    SIZE_T,
    LPCSTR,
    const D3D_SHADER_MACRO*,
    ID3DInclude*,
    LPCSTR,
    LPCSTR,
    UINT,
    UINT,
    ID3DBlob**,
    ID3DBlob**);

DX9D3DCompileFn LoadDX9D3DCompiler(HMODULE& module_out) {
  module_out = nullptr;

  static constexpr const wchar_t* COMPILER_DLLS[] = {
      L"d3dcompiler_47.dll",
      L"d3dcompiler_46.dll",
      L"d3dcompiler_43.dll",
  };

  for (const wchar_t* dll_name : COMPILER_DLLS) {
    HMODULE module = LoadLibraryW(dll_name);
    if (module == nullptr) continue;

    auto compile = reinterpret_cast<DX9D3DCompileFn>(
        GetProcAddress(module, "D3DCompile"));
    if (compile != nullptr) {
      module_out = module;
      return compile;
    }

    FreeLibrary(module);
  }

  return nullptr;
}

bool CompileDX9ReadbackShader(
    const char* source,
    const char* profile,
    std::vector<DWORD>& bytecode_out) {
  bytecode_out.clear();
  if (source == nullptr || profile == nullptr) return false;

  HMODULE compiler_module = nullptr;
  DX9D3DCompileFn compile = LoadDX9D3DCompiler(compiler_module);
  if (compile == nullptr || compiler_module == nullptr) {
    LogDX9GPUBlitFailure(
        "Could not load d3dcompiler_47/46/43.dll for the cached blit shader");
    return false;
  }

  ID3DBlob* code_blob = nullptr;
  ID3DBlob* error_blob = nullptr;
  const HRESULT hr = compile(
      source,
      std::strlen(source),
      "RenoDX DX9 readback blit",
      nullptr,
      nullptr,
      "main",
      profile,
      D3DCOMPILE_OPTIMIZATION_LEVEL3,
      0u,
      &code_blob,
      &error_blob);

  if (FAILED(hr) || code_blob == nullptr) {
    if (error_blob != nullptr && error_blob->GetBufferPointer() != nullptr) {
      std::string message = "D3DCompile failed for ";
      message += profile;
      message += ": ";
      message.append(
          static_cast<const char*>(error_blob->GetBufferPointer()),
          error_blob->GetBufferSize());
      LogDX9GPUBlitFailure(message.c_str());
    } else {
      std::string message = "D3DCompile failed for ";
      message += profile;
      LogDX9GPUBlitFailure(message.c_str());
    }

    if (error_blob != nullptr) error_blob->Release();
    if (code_blob != nullptr) code_blob->Release();
    FreeLibrary(compiler_module);
    return false;
  }

  const size_t byte_size = code_blob->GetBufferSize();
  if (byte_size == 0u || (byte_size % sizeof(DWORD)) != 0u) {
    LogDX9GPUBlitFailure("D3DCompile returned invalid DX9 shader bytecode");
    if (error_blob != nullptr) error_blob->Release();
    code_blob->Release();
    FreeLibrary(compiler_module);
    return false;
  }

  bytecode_out.resize(byte_size / sizeof(DWORD));
  std::memcpy(
      bytecode_out.data(),
      code_blob->GetBufferPointer(),
      byte_size);

  if (error_blob != nullptr) error_blob->Release();
  code_blob->Release();
  FreeLibrary(compiler_module);
  return true;
}

bool EnsureDX9GPUReadbackPipelineLocked(IDirect3DDevice9* device) {
  if (device == nullptr) return false;

  auto& cache = g_dx9_gpu_blit_cache;

  if (cache.device != nullptr && cache.device != device) {
    DestroyDX9GPUReadbackBlitCacheLocked();
  }

  cache.device = device;

  if (cache.vertex_shader == nullptr) {
    std::vector<DWORD> bytecode;
    if (!CompileDX9ReadbackShader(
            DX9_READBACK_BLIT_VERTEX_HLSL,
            "vs_3_0",
            bytecode)) {
      return false;
    }

    const HRESULT hr = device->CreateVertexShader(
        bytecode.data(),
        &cache.vertex_shader);
    if (FAILED(hr) || cache.vertex_shader == nullptr) {
      LogDX9GPUBlitFailure("Could not create the cached vs_3_0 blit shader");
      return false;
    }
  }

  if (cache.pixel_shader == nullptr) {
    std::vector<DWORD> bytecode;
    if (!CompileDX9ReadbackShader(
            DX9_READBACK_BLIT_PIXEL_HLSL,
            "ps_3_0",
            bytecode)) {
      return false;
    }

    const HRESULT hr = device->CreatePixelShader(
        bytecode.data(),
        &cache.pixel_shader);
    if (FAILED(hr) || cache.pixel_shader == nullptr) {
      LogDX9GPUBlitFailure("Could not create the cached ps_3_0 SDR conversion shader");
      return false;
    }
  }

  if (cache.vertex_declaration == nullptr) {
    static const D3DVERTEXELEMENT9 declaration[] = {
        {0u, 0u, D3DDECLTYPE_FLOAT4, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_POSITION, 0u},
        {0u, 16u, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT,
         D3DDECLUSAGE_TEXCOORD, 0u},
        D3DDECL_END(),
    };

    const HRESULT hr = device->CreateVertexDeclaration(
        declaration,
        &cache.vertex_declaration);
    if (FAILED(hr) || cache.vertex_declaration == nullptr) {
      LogDX9GPUBlitFailure("Could not create the cached fullscreen vertex declaration");
      return false;
    }
  }

  if (cache.state_block == nullptr) {
    const HRESULT hr = device->CreateStateBlock(
        D3DSBT_ALL,
        &cache.state_block);
    if (FAILED(hr) || cache.state_block == nullptr) {
      LogDX9GPUBlitFailure("Could not create the cached D3D9 state block");
      return false;
    }
  }

  return true;
}

bool EnsureDX9GPUReadbackScratchTextureLocked(
    IDirect3DDevice9* device,
    uint32_t width,
    uint32_t height) {
  auto& cache = g_dx9_gpu_blit_cache;

  if (cache.source_scratch_texture != nullptr
      && cache.source_scratch_width == width
      && cache.source_scratch_height == height) {
    return true;
  }

  ReleaseDX9COM(cache.source_scratch_texture);
  cache.source_scratch_width = 0u;
  cache.source_scratch_height = 0u;

  const HRESULT hr = device->CreateTexture(
      width,
      height,
      1u,
      D3DUSAGE_RENDERTARGET,
      D3DFMT_A16B16G16R16F,
      D3DPOOL_DEFAULT,
      &cache.source_scratch_texture,
      nullptr);

  if (FAILED(hr) || cache.source_scratch_texture == nullptr) {
    LogDX9GPUBlitFailure("Could not create/reuse the FP16 sampling scratch texture");
    return false;
  }

  cache.source_scratch_width = width;
  cache.source_scratch_height = height;
  return true;
}

bool PrepareDX9GPUReadbackSourceTextureLocked(
    IDirect3DDevice9* device,
    reshade::api::resource source,
    const reshade::api::resource_desc& source_desc,
    IDirect3DTexture9*& texture_out,
    ScopedDX9NativeTexture& direct_texture) {
  texture_out = nullptr;

  if (TryAcquireDX9TextureContainer(
          source,
          source_desc,
          direct_texture)) {
    texture_out = direct_texture.texture;
    return texture_out != nullptr;
  }

  if (!EnsureDX9GPUReadbackScratchTextureLocked(
          device,
          source_desc.texture.width,
          source_desc.texture.height)) {
    return false;
  }

  ScopedDX9NativeSurface source_surface;
  if (!AcquireDX9NativeSurface(
          source,
          source_desc,
          0u,
          source_surface)) {
    return false;
  }

  IDirect3DSurface9* scratch_surface = nullptr;
  const HRESULT get_surface_hr =
      g_dx9_gpu_blit_cache.source_scratch_texture->GetSurfaceLevel(
          0u,
          &scratch_surface);
  if (FAILED(get_surface_hr) || scratch_surface == nullptr) {
    return false;
  }

  const HRESULT blit_hr = device->StretchRect(
      source_surface.surface,
      nullptr,
      scratch_surface,
      nullptr,
      D3DTEXF_NONE);

  scratch_surface->Release();

  if (FAILED(blit_hr)) {
    LogDX9GPUBlitFailure("Could not copy the FP16 surface into the sampling texture");
    return false;
  }

  texture_out = g_dx9_gpu_blit_cache.source_scratch_texture;
  return true;
}

struct ScopedDX9RenderStateRestore {
  IDirect3DDevice9* device = nullptr;
  IDirect3DStateBlock9* state_block = nullptr;
  std::array<IDirect3DSurface9*, 4u> render_targets = {};
  std::array<bool, 4u> has_render_target = {};
  IDirect3DSurface9* depth_stencil = nullptr;
  bool has_depth_stencil = false;
  uint32_t render_target_count = 1u;
  bool captured = false;

  bool Capture(
      IDirect3DDevice9* native_device,
      IDirect3DStateBlock9* cached_state_block) {
    device = native_device;
    state_block = cached_state_block;
    if (device == nullptr || state_block == nullptr) return false;

    D3DCAPS9 caps = {};
    if (SUCCEEDED(device->GetDeviceCaps(&caps))) {
      render_target_count =
          std::clamp<uint32_t>(caps.NumSimultaneousRTs, 1u, 4u);
    }

    for (uint32_t slot = 0u; slot < render_target_count; ++slot) {
      IDirect3DSurface9* target = nullptr;
      if (SUCCEEDED(device->GetRenderTarget(slot, &target))
          && target != nullptr) {
        render_targets[slot] = target;
        has_render_target[slot] = true;
      }
    }

    IDirect3DSurface9* depth = nullptr;
    if (SUCCEEDED(device->GetDepthStencilSurface(&depth))
        && depth != nullptr) {
      depth_stencil = depth;
      has_depth_stencil = true;
    }

    if (FAILED(state_block->Capture())) {
      return false;
    }

    captured = true;
    return true;
  }

  ~ScopedDX9RenderStateRestore() {
    if (device != nullptr && captured) {
      for (uint32_t slot = 0u; slot < render_target_count; ++slot) {
        if (slot == 0u || has_render_target[slot]) {
          device->SetRenderTarget(
              slot,
              has_render_target[slot] ? render_targets[slot] : nullptr);
        }
      }

      device->SetDepthStencilSurface(
          has_depth_stencil ? depth_stencil : nullptr);

      state_block->Apply();
    }

    for (auto*& target : render_targets) {
      if (target != nullptr) {
        target->Release();
        target = nullptr;
      }
    }

    if (depth_stencil != nullptr) {
      depth_stencil->Release();
      depth_stencil = nullptr;
    }
  }
};

bool BlitDX9HDRCloneToOriginalSDR(
    reshade::api::device* device,
    reshade::api::resource float_source,
    const reshade::api::resource_desc& float_source_desc,
    reshade::api::resource sdr_target,
    const reshade::api::resource_desc& sdr_target_desc) {
  if (!DX9_GPU_READBACK_BLIT_ENABLED
      || device == nullptr
      || device->get_api() != reshade::api::device_api::d3d9
      || float_source.handle == 0u
      || sdr_target.handle == 0u
      || float_source.handle == sdr_target.handle) {
    return false;
  }

  if (!IsFloat16RGBA(float_source_desc.texture.format)
      || !IsSupportedSDRReadbackFormat(sdr_target_desc.texture.format)
      || !SameTextureExtent(float_source_desc, sdr_target_desc)
      || float_source_desc.texture.depth_or_layers != 1u
      || sdr_target_desc.texture.depth_or_layers != 1u
      || float_source_desc.texture.samples != 1u
      || sdr_target_desc.texture.samples != 1u
      || IsCPUVisibleReadbackHeap(sdr_target_desc.heap)) {
    return false;
  }

  auto* native_device =
      reinterpret_cast<IDirect3DDevice9*>(device->get_native());
  if (native_device == nullptr) return false;

  std::scoped_lock blit_lock(g_dx9_gpu_blit_mutex);

  if (!EnsureDX9GPUReadbackPipelineLocked(native_device)) {
    return false;
  }

  ScopedDX9NativeTexture direct_source_texture;
  IDirect3DTexture9* source_texture = nullptr;
  if (!PrepareDX9GPUReadbackSourceTextureLocked(
          native_device,
          float_source,
          float_source_desc,
          source_texture,
          direct_source_texture)
      || source_texture == nullptr) {
    return false;
  }

  ScopedDX9NativeSurface target_surface;
  if (!AcquireDX9NativeSurface(
          sdr_target,
          sdr_target_desc,
          0u,
          target_surface)
      || target_surface.surface == nullptr) {
    return false;
  }

  ScopedDX9RenderStateRestore restore;
  if (!restore.Capture(
          native_device,
          g_dx9_gpu_blit_cache.state_block)) {
    return false;
  }

  for (DWORD stage = 0u; stage < 16u; ++stage) {
    native_device->SetTexture(stage, nullptr);
  }

  if (FAILED(native_device->SetRenderTarget(0u, target_surface.surface))) {
    return false;
  }

  for (uint32_t slot = 1u; slot < restore.render_target_count; ++slot) {
    native_device->SetRenderTarget(slot, nullptr);
  }

  native_device->SetDepthStencilSurface(nullptr);

  D3DVIEWPORT9 viewport = {
      0u,
      0u,
      sdr_target_desc.texture.width,
      sdr_target_desc.texture.height,
      0.0f,
      1.0f,
  };
  if (FAILED(native_device->SetViewport(&viewport))) return false;

  RECT scissor = {
      0,
      0,
      static_cast<LONG>(sdr_target_desc.texture.width),
      static_cast<LONG>(sdr_target_desc.texture.height),
  };
  native_device->SetScissorRect(&scissor);

  native_device->SetRenderState(D3DRS_ZENABLE, FALSE);
  native_device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
  native_device->SetRenderState(D3DRS_ALPHATESTENABLE, FALSE);
  native_device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
  native_device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
  native_device->SetRenderState(D3DRS_STENCILENABLE, FALSE);
  native_device->SetRenderState(D3DRS_FOGENABLE, FALSE);
  native_device->SetRenderState(D3DRS_SCISSORTESTENABLE, FALSE);
  native_device->SetRenderState(D3DRS_CULLMODE, D3DCULL_NONE);
  native_device->SetRenderState(
      D3DRS_COLORWRITEENABLE,
      D3DCOLORWRITEENABLE_RED
          | D3DCOLORWRITEENABLE_GREEN
          | D3DCOLORWRITEENABLE_BLUE
          | D3DCOLORWRITEENABLE_ALPHA);

  native_device->SetRenderState(D3DRS_SRGBWRITEENABLE, FALSE);

  native_device->SetSamplerState(0u, D3DSAMP_ADDRESSU, D3DTADDRESS_CLAMP);
  native_device->SetSamplerState(0u, D3DSAMP_ADDRESSV, D3DTADDRESS_CLAMP);
  native_device->SetSamplerState(0u, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
  native_device->SetSamplerState(0u, D3DSAMP_MINFILTER, D3DTEXF_POINT);
  native_device->SetSamplerState(0u, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
  native_device->SetSamplerState(0u, D3DSAMP_SRGBTEXTURE, 0u);

  if (FAILED(native_device->SetVertexDeclaration(
          g_dx9_gpu_blit_cache.vertex_declaration))) {
    return false;
  }
  if (FAILED(native_device->SetVertexShader(
          g_dx9_gpu_blit_cache.vertex_shader))) {
    return false;
  }
  if (FAILED(native_device->SetPixelShader(
          g_dx9_gpu_blit_cache.pixel_shader))) {
    return false;
  }
  if (FAILED(native_device->SetTexture(0u, source_texture))) {
    return false;
  }

  const float inv_size[4] = {
      1.0f / static_cast<float>(sdr_target_desc.texture.width),
      1.0f / static_cast<float>(sdr_target_desc.texture.height),
      0.0f,
      0.0f,
  };
  native_device->SetVertexShaderConstantF(0u, inv_size, 1u);

  static const DX9GPUReadbackBlitVertex vertices[] = {
      {-1.0f,  1.0f, 0.0f, 1.0f, 0.0f, 0.0f},
      { 1.0f,  1.0f, 0.0f, 1.0f, 1.0f, 0.0f},
      {-1.0f, -1.0f, 0.0f, 1.0f, 0.0f, 1.0f},
      { 1.0f, -1.0f, 0.0f, 1.0f, 1.0f, 1.0f},
  };

  const HRESULT draw_hr = native_device->DrawPrimitiveUP(
      D3DPT_TRIANGLESTRIP,
      2u,
      vertices,
      sizeof(DX9GPUReadbackBlitVertex));

  native_device->SetTexture(0u, nullptr);

  if (FAILED(draw_hr)) {
    LogDX9GPUBlitFailure("Fullscreen HDR -> SDR DrawPrimitiveUP failed");
    return false;
  }

  return true;
}

bool TryConvertDX9FloatReadbackWithGPUBlit(
    reshade::api::command_list* cmd_list,
    const DX9CopyEndpoint& source_endpoint,
    reshade::api::resource dest,
    const reshade::api::resource_desc& dest_desc) {
  auto* device = cmd_list != nullptr ? cmd_list->get_device() : nullptr;
  if (device == nullptr
      || device->get_api() != reshade::api::device_api::d3d9
      || dest.handle == 0u
      || !IsCPUVisibleReadbackHeap(dest_desc.heap)) {
    return false;
  }

  const reshade::api::resource float_source =
      SelectCloneForCopy(source_endpoint);
  const reshade::api::resource_desc float_source_desc =
      SelectCloneDescForCopy(source_endpoint);

  const reshade::api::resource sdr_original = source_endpoint.original;
  const reshade::api::resource_desc& sdr_original_desc =
      source_endpoint.original_desc;

  if (float_source.handle == 0u
      || sdr_original.handle == 0u
      || float_source.handle == sdr_original.handle
      || !IsFloat16RGBA(float_source_desc.texture.format)
      || !IsSupportedSDRReadbackFormat(sdr_original_desc.texture.format)
      || !IsSupportedSDRReadbackFormat(dest_desc.texture.format)
      || sdr_original_desc.texture.format != dest_desc.texture.format
      || !SameTextureExtent(float_source_desc, sdr_original_desc)
      || !SameTextureExtent(sdr_original_desc, dest_desc)
      || float_source_desc.texture.samples != 1u
      || sdr_original_desc.texture.samples != 1u
      || dest_desc.texture.samples != 1u) {
    return false;
  }

  if (!BlitDX9HDRCloneToOriginalSDR(
          device,
          float_source,
          float_source_desc,
          sdr_original,
          sdr_original_desc)) {
    return false;
  }

  auto* native_device =
      reinterpret_cast<IDirect3DDevice9*>(device->get_native());
  if (native_device == nullptr) return false;

  ScopedDX9NativeSurface original_surface;
  ScopedDX9NativeSurface readback_surface;
  if (!AcquireDX9NativeSurface(
          sdr_original,
          sdr_original_desc,
          0u,
          original_surface)
      || !AcquireDX9NativeSurface(
          dest,
          dest_desc,
          0u,
          readback_surface)) {
    return false;
  }

  const HRESULT readback_hr = native_device->GetRenderTargetData(
      original_surface.surface,
      readback_surface.surface);
  if (FAILED(readback_hr)) {
    LogDX9GPUBlitFailure(
        "GPU SDR blit succeeded but native GetRenderTargetData failed");
    return false;
  }

  if (g_dx9_gpu_blit_success_logs < 8u) {
    ++g_dx9_gpu_blit_success_logs;

    std::stringstream stream;
    stream << "[RenoDX DX9 Readback GPU Blit] GPU-converted FP16 -> SDR -> CPU (";
    stream << float_source_desc.texture.width << "x";
    stream << float_source_desc.texture.height << ", ";
    stream << float_source_desc.texture.format << " -> ";
    stream << sdr_original_desc.texture.format << ")";
    reshade::log::message(
        reshade::log::level::info,
        stream.str().c_str());
  }

  return true;
}

float HalfToFloat(uint16_t value) {
  const uint32_t sign = (value >> 15u) & 0x1u;
  const uint32_t exponent = (value >> 10u) & 0x1Fu;
  const uint32_t mantissa = value & 0x3FFu;

  float result = 0.0f;

  if (exponent == 0u) {
    result = std::ldexp(static_cast<float>(mantissa), -24);
  } else if (exponent == 0x1Fu) {
    if (mantissa == 0u) {
      result = std::numeric_limits<float>::infinity();
    } else {
      result = std::numeric_limits<float>::quiet_NaN();
    }
  } else {
    result = std::ldexp(
        1.0f + static_cast<float>(mantissa) / 1024.0f,
        static_cast<int>(exponent) - 15);
  }

  return sign != 0u ? -result : result;
}

float SanitizeUnit(float value) {
  if (!std::isfinite(value)) return 0.0f;
  return std::clamp(value, 0.0f, 1.0f);
}

float LinearToSRGB(float linear) {
  linear = SanitizeUnit(linear);

  if (linear <= 0.0031308f) {
    return 12.92f * linear;
  }

  return 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
}

const std::array<uint8_t, 4096u>& GetDX9SRGBEncodeLUT() {
  static const std::array<uint8_t, 4096u> table = []() {
    std::array<uint8_t, 4096u> result = {};
    for (size_t i = 0u; i < result.size(); ++i) {
      const float linear =
          static_cast<float>(i) / static_cast<float>(result.size() - 1u);
      const float srgb = LinearToSRGB(linear);
      result[i] = static_cast<uint8_t>(
          std::clamp(
              static_cast<int>(std::lround(srgb * 255.0f)),
              0,
              255));
    }
    return result;
  }();
  return table;
}

uint8_t FloatToUNorm8(float value, bool encode_srgb) {
  value = SanitizeUnit(value);
  if (encode_srgb) {
    const auto& table = GetDX9SRGBEncodeLUT();
    const size_t index = static_cast<size_t>(std::clamp(
        static_cast<int>(std::lround(
            value * static_cast<float>(table.size() - 1u))),
        0,
        static_cast<int>(table.size() - 1u)));
    return table[index];
  }
  return static_cast<uint8_t>(
      std::clamp(
          static_cast<int>(std::lround(value * 255.0f)),
          0,
          255));
}

void WriteSDRPixel(
    uint8_t* dest,
    reshade::api::format dest_format,
    float red,
    float green,
    float blue,
    float alpha) {
  const uint8_t r = FloatToUNorm8(red, DX9_READBACK_ENCODE_SRGB);
  const uint8_t g = FloatToUNorm8(green, DX9_READBACK_ENCODE_SRGB);
  const uint8_t b = FloatToUNorm8(blue, DX9_READBACK_ENCODE_SRGB);

  const bool force_opaque =
      DX9_READBACK_FORCE_OPAQUE_ALPHA
      || IsX8ReadbackFormat(dest_format);

  const uint8_t a =
      force_opaque ? 255u : FloatToUNorm8(alpha, false);

  if (IsBGRAReadbackFormat(dest_format)) {
    dest[0] = b;
    dest[1] = g;
    dest[2] = r;
    dest[3] = a;
  } else {
    dest[0] = r;
    dest[1] = g;
    dest[2] = b;
    dest[3] = a;
  }
}

void LogDX9ReadbackFailure(
    const char* reason,
    const reshade::api::resource_desc& source_desc,
    const reshade::api::resource_desc& dest_desc) {
  if (g_dx9_readback_failure_logs >= 8u) return;
  ++g_dx9_readback_failure_logs;

  std::stringstream stream;
  stream << "[RenoDX DX9 Readback] " << reason;
  stream << " (source: " << source_desc.texture.format;
  stream << ", destination: " << dest_desc.texture.format;
  stream << ", size: " << source_desc.texture.width;
  stream << "x" << source_desc.texture.height << ")";
  reshade::log::message(
      reshade::log::level::warning,
      stream.str().c_str());
}

bool TryChainDX9CopyResource(
    reshade::api::command_list* cmd_list,
    const DX9CopyEndpoint& source_endpoint,
    const DX9CopyEndpoint& dest_endpoint) {
  if (!source_endpoint.has_clone || !dest_endpoint.has_clone) return false;

  const reshade::api::resource selected_source =
      SelectCloneForCopy(source_endpoint);
  const reshade::api::resource selected_dest =
      SelectCloneForCopy(dest_endpoint);

  if (selected_source.handle == 0u || selected_dest.handle == 0u) return false;

  if (selected_source.handle == source_endpoint.input.handle
      && selected_dest.handle == dest_endpoint.input.handle) {
    return false;
  }

  const reshade::api::resource_desc selected_source_desc =
      SelectCloneDescForCopy(source_endpoint);
  const reshade::api::resource_desc selected_dest_desc =
      SelectCloneDescForCopy(dest_endpoint);

  if (!ExactCopyCompatible(
          selected_source_desc,
          selected_dest_desc)) {
    return false;
  }

  {
    ScopedDX9ReplacementCopy guard;
    cmd_list->copy_resource(selected_source, selected_dest);
  }

  if (g_dx9_chain_logs < 8u) {
    ++g_dx9_chain_logs;
    std::stringstream stream;
    stream << "[RenoDX DX9 Readback] Chained copy_resource through clones: ";
    stream << selected_source_desc.texture.format;
    stream << " -> " << selected_dest_desc.texture.format;
    reshade::log::message(
        reshade::log::level::info,
        stream.str().c_str());
  }

  return true;
}

bool TryRedirectDX9CloneReadbackToOriginal(
    reshade::api::command_list* cmd_list,
    const DX9CopyEndpoint& source_endpoint,
    reshade::api::resource dest,
    const reshade::api::resource_desc& dest_desc) {
  if (!source_endpoint.input_is_clone) return false;
  if (source_endpoint.original.handle == 0u
      || source_endpoint.original.handle
          == source_endpoint.input.handle) {
    return false;
  }
  if (!IsCPUVisibleReadbackHeap(dest_desc.heap)) return false;
  if (!ExactCopyCompatible(
          source_endpoint.original_desc,
          dest_desc)) {
    return false;
  }

  {
    ScopedDX9ReplacementCopy guard;
    cmd_list->copy_resource(source_endpoint.original, dest);
  }

  if (g_dx9_chain_logs < 8u) {
    ++g_dx9_chain_logs;
    std::stringstream stream;
    stream << "[RenoDX DX9 Readback] Redirected clone readback to its ";
    stream << "matching original SDR resource (";
    stream << source_endpoint.original_desc.texture.format;
    stream << " -> " << dest_desc.texture.format << ")";
    reshade::log::message(
        reshade::log::level::info,
        stream.str().c_str());
  }

  return true;
}

bool TryConvertDX9FloatReadbackToSDR(
    reshade::api::command_list* cmd_list,
    const DX9CopyEndpoint& source_endpoint,
    reshade::api::resource dest,
    const reshade::api::resource_desc& dest_desc) {
  auto* device = cmd_list != nullptr ? cmd_list->get_device() : nullptr;
  if (device == nullptr) return false;

  const reshade::api::resource float_source =
      SelectCloneForCopy(source_endpoint);
  const reshade::api::resource_desc float_source_desc =
      SelectCloneDescForCopy(source_endpoint);

  if (float_source.handle == 0u) return false;
  if (!IsTextureResource(float_source_desc)
      || !IsTextureResource(dest_desc)) {
    return false;
  }
  if (!IsFloat16RGBA(float_source_desc.texture.format)) return false;
  if (!IsSupportedSDRReadbackFormat(dest_desc.texture.format)) return false;
  if (!IsCPUVisibleReadbackHeap(dest_desc.heap)) return false;
  if (!SameTextureExtent(float_source_desc, dest_desc)) return false;
  if (float_source_desc.texture.depth_or_layers != 1u
      || dest_desc.texture.depth_or_layers != 1u) {
    return false;
  }
  if (float_source_desc.texture.samples != 1u
      || dest_desc.texture.samples != 1u) {
    return false;
  }

  reshade::api::resource_desc staging_desc = float_source_desc;
  staging_desc.heap = reshade::api::memory_heap::gpu_to_cpu;
  staging_desc.usage = reshade::api::resource_usage::copy_dest;
  staging_desc.flags = {};
  staging_desc.texture.depth_or_layers = 1u;
  staging_desc.texture.levels = 1u;
  staging_desc.texture.samples = 1u;

  reshade::api::resource staging = {0u};

  std::scoped_lock staging_lock(g_dx9_readback_staging_mutex);

  if (!EnsureDX9ReadbackStaging(
          device,
          staging_desc,
          staging)) {
    LogDX9ReadbackFailure(
        "Could not create/reuse the float16 CPU staging surface",
        float_source_desc,
        dest_desc);
    return false;
  }

  {
    ScopedDX9ReplacementCopy guard;
    cmd_list->copy_resource(float_source, staging);
  }

  reshade::api::subresource_data source_data = {};
  if (!device->map_texture_region(
          staging,
          0u,
          nullptr,
          reshade::api::map_access::read_only,
          &source_data)) {
    InvalidateDX9ReadbackStaging(device);
    LogDX9ReadbackFailure(
        "Could not map the float16 CPU staging surface",
        float_source_desc,
        dest_desc);
    return false;
  }

  reshade::api::subresource_data dest_data = {};
  if (!device->map_texture_region(
          dest,
          0u,
          nullptr,
          reshade::api::map_access::write_only,
          &dest_data)) {
    device->unmap_texture_region(staging, 0u);
    LogDX9ReadbackFailure(
        "Could not map the game's 8-bit readback surface",
        float_source_desc,
        dest_desc);
    return false;
  }

  const uint32_t width = float_source_desc.texture.width;
  const uint32_t height = float_source_desc.texture.height;

  const auto* source_base =
      static_cast<const uint8_t*>(source_data.data);
  auto* dest_base =
      static_cast<uint8_t*>(dest_data.data);

  for (uint32_t y = 0u; y < height; ++y) {
    const auto* source_row =
        source_base + static_cast<size_t>(y) * source_data.row_pitch;
    auto* dest_row =
        dest_base + static_cast<size_t>(y) * dest_data.row_pitch;

    for (uint32_t x = 0u; x < width; ++x) {
      const auto* source_pixel =
          reinterpret_cast<const uint16_t*>(
              source_row + static_cast<size_t>(x) * 8u);
      auto* dest_pixel =
          dest_row + static_cast<size_t>(x) * 4u;

      WriteSDRPixel(
          dest_pixel,
          dest_desc.texture.format,
          HalfToFloat(source_pixel[0]),
          HalfToFloat(source_pixel[1]),
          HalfToFloat(source_pixel[2]),
          HalfToFloat(source_pixel[3]));
    }
  }

  device->unmap_texture_region(dest, 0u);
  device->unmap_texture_region(staging, 0u);

  if (g_dx9_readback_success_logs < 8u) {
    ++g_dx9_readback_success_logs;

    std::stringstream stream;
    stream << "[RenoDX DX9 Readback] Replaced incompatible float16 -> SDR ";
    stream << "GetRenderTargetData copy (";
    stream << float_source_desc.texture.format;
    stream << " -> " << dest_desc.texture.format;
    stream << ", " << width << "x" << height << ")";
    reshade::log::message(
        reshade::log::level::info,
        stream.str().c_str());
  }

  return true;
}

bool OnDX9CopyResource(
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    reshade::api::resource dest) {
  if (!DX9_READBACK_FIX_ENABLED
      || g_inside_dx9_replacement_copy
      || cmd_list == nullptr
      || source.handle == 0u
      || dest.handle == 0u) {
    return false;
  }

  auto* device = cmd_list->get_device();
  if (device == nullptr
      || device->get_api() != reshade::api::device_api::d3d9) {
    return false;
  }

  const DX9CopyEndpoint source_endpoint =
      ResolveDX9CopyEndpoint(device, source);
  const DX9CopyEndpoint dest_endpoint =
      ResolveDX9CopyEndpoint(device, dest);

  if (TryChainDX9CopyResource(
          cmd_list,
          source_endpoint,
          dest_endpoint)) {
    return true;
  }

  const reshade::api::resource_desc& dest_desc = dest_endpoint.input_desc;

  if (TryConvertDX9FloatReadbackWithGPUBlit(
          cmd_list,
          source_endpoint,
          dest,
          dest_desc)) {
    return true;
  }

  if (TryRedirectDX9CloneReadbackToOriginal(
          cmd_list,
          source_endpoint,
          dest,
          dest_desc)) {
    return true;
  }

  if (TryConvertDX9FloatReadbackToSDR(
          cmd_list,
          source_endpoint,
          dest,
          dest_desc)) {
    return true;
  }

  return false;
}

bool OnDX9CopyTextureRegion(
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    uint32_t source_subresource,
    const reshade::api::subresource_box* source_box,
    reshade::api::resource dest,
    uint32_t dest_subresource,
    const reshade::api::subresource_box* dest_box,
    reshade::api::filter_mode filter) {
  if (!DX9_READBACK_FIX_ENABLED
      || g_inside_dx9_replacement_copy
      || cmd_list == nullptr
      || source.handle == 0u
      || dest.handle == 0u) {
    return false;
  }

  auto* device = cmd_list->get_device();
  if (device == nullptr
      || device->get_api() != reshade::api::device_api::d3d9) {
    return false;
  }

  const DX9CopyEndpoint source_endpoint =
      ResolveDX9CopyEndpoint(device, source);
  const DX9CopyEndpoint dest_endpoint =
      ResolveDX9CopyEndpoint(device, dest);

  if (!source_endpoint.has_clone || !dest_endpoint.has_clone) return false;

  const reshade::api::resource selected_source =
      SelectCloneForCopy(source_endpoint);
  const reshade::api::resource selected_dest =
      SelectCloneForCopy(dest_endpoint);

  const reshade::api::resource_desc selected_source_desc =
      SelectCloneDescForCopy(source_endpoint);
  const reshade::api::resource_desc selected_dest_desc =
      SelectCloneDescForCopy(dest_endpoint);

  if (!SameTextureExtent(
          source_endpoint.original_desc,
          selected_source_desc)
      || !SameTextureExtent(
          dest_endpoint.original_desc,
          selected_dest_desc)
      || selected_source_desc.texture.format
          != selected_dest_desc.texture.format) {
    return false;
  }

  if (selected_source.handle == source.handle
      && selected_dest.handle == dest.handle) {
    return false;
  }

  {
    ScopedDX9ReplacementCopy guard;
    cmd_list->copy_texture_region(
        selected_source,
        source_subresource,
        source_box,
        selected_dest,
        dest_subresource,
        dest_box,
        filter);
  }

  return true;
}

bool OnDX9ResolveTextureRegion(
    reshade::api::command_list* cmd_list,
    reshade::api::resource source,
    uint32_t source_subresource,
    const reshade::api::subresource_box* source_box,
    reshade::api::resource dest,
    uint32_t dest_subresource,
    uint32_t dest_x,
    uint32_t dest_y,
    uint32_t dest_z,
    reshade::api::format format) {
  if (!DX9_READBACK_FIX_ENABLED
      || g_inside_dx9_replacement_copy
      || cmd_list == nullptr
      || source.handle == 0u
      || dest.handle == 0u) {
    return false;
  }

  (void)format;

  auto* device = cmd_list->get_device();
  if (device == nullptr
      || device->get_api() != reshade::api::device_api::d3d9) {
    return false;
  }

  const DX9CopyEndpoint source_endpoint =
      ResolveDX9CopyEndpoint(device, source);
  const DX9CopyEndpoint dest_endpoint =
      ResolveDX9CopyEndpoint(device, dest);

  if (!source_endpoint.has_clone || !dest_endpoint.has_clone) return false;

  const reshade::api::resource selected_source =
      SelectCloneForCopy(source_endpoint);
  const reshade::api::resource selected_dest =
      SelectCloneForCopy(dest_endpoint);

  const reshade::api::resource_desc selected_source_desc =
      SelectCloneDescForCopy(source_endpoint);
  const reshade::api::resource_desc selected_dest_desc =
      SelectCloneDescForCopy(dest_endpoint);

  if (!SameTextureExtent(
          source_endpoint.original_desc,
          selected_source_desc)
      || !SameTextureExtent(
          dest_endpoint.original_desc,
          selected_dest_desc)
      || selected_source_desc.texture.format
          != selected_dest_desc.texture.format) {
    return false;
  }

  if (selected_source.handle == source.handle
      && selected_dest.handle == dest.handle) {
    return false;
  }

  const reshade::api::format selected_format =
      selected_dest_desc.texture.format;

  {
    ScopedDX9ReplacementCopy guard;
    cmd_list->resolve_texture_region(
        selected_source,
        source_subresource,
        source_box,
        selected_dest,
        dest_subresource,
        dest_x,
        dest_y,
        dest_z,
        selected_format);
  }

  return true;
}

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
            if (value == 0) return shader_injection.gamma_correction + 1.f;
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
            if (value == 0) return shader_injection.intermediate_encoding;
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
};

void OnPresetOff() {
}

const auto UPGRADE_TYPE_NONE = 0.f;
const auto UPGRADE_TYPE_OUTPUT_SIZE = 1.f;
const auto UPGRADE_TYPE_OUTPUT_RATIO = 2.f;
const auto UPGRADE_TYPE_ANY = 3.f;

void OnPresent(reshade::api::command_queue* queue,
               reshade::api::swapchain* swapchain,
               const reshade::api::rect* source_rect,
               const reshade::api::rect* dest_rect,
               uint32_t dirty_rect_count,
               const reshade::api::rect* dirty_rects) {
  if (queue == nullptr) return;

  auto* device = queue->get_device();
  if (device == nullptr) return;

  if (device->get_api() == reshade::api::device_api::opengl) {
    shader_injection.custom_flip_uv_y = 1.f;
  }
}

bool initialized = false;

}

extern "C" __declspec(dllexport) constexpr const char* NAME = "RenoDX";
extern "C" __declspec(dllexport) constexpr const char* DESCRIPTION = "RenoDX Need for Speed: Most Wanted (2005)";

BOOL APIENTRY DllMain(HMODULE h_module, DWORD fdw_reason, LPVOID lpv_reserved) {
  switch (fdw_reason) {
    case DLL_PROCESS_ATTACH:
      if (!reshade::register_addon(h_module)) return FALSE;

      if (!initialized) {
        renodx::mods::shader::force_pipeline_cloning = true;
        renodx::mods::shader::expected_constant_buffer_space = 50;
        renodx::mods::shader::expected_constant_buffer_index = 13;
        renodx::mods::shader::allow_multiple_push_constants = true;
        renodx::mods::shader::constant_buffer_offset = 50 * 4;

        renodx::mods::swapchain::expected_constant_buffer_index = 13;
        renodx::mods::swapchain::expected_constant_buffer_space = 50;
        renodx::mods::swapchain::use_resource_cloning = true;
        renodx::mods::swapchain::set_color_space = false;
        renodx::mods::swapchain::use_device_proxy = true;
        renodx::mods::swapchain::proxy_device_api = reshade::api::device_api::d3d11;
        renodx::mods::swapchain::proxy_skip_host_present = true;
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

        reshade::register_event<reshade::addon_event::present>(OnPresent);

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

        {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "SwapChainDeviceProxyBaseWaitIdle",
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              .default_value = 0.f,
              .label = "Base Wait Idle",
              .section = "Display Proxy",
              .tooltip = "Waits for the D3D9 device before the proxy reads the shared frame",
              .labels = {"Off", "On"},
              .is_global = true,
              .is_visible = []() { return current_settings_mode >= 2; },
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          renodx::mods::swapchain::device_proxy_wait_idle_source = (setting->GetValue() == 1.f);
          settings.push_back(setting);
        }

        {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "SwapChainDeviceProxyProxyWaitIdle",
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              .default_value = 0.f,
              .label = "Proxy Wait Idle",
              .section = "Display Proxy",
              .tooltip = "Waits for the proxy device after it consumes the shared frame",
              .labels = {"Off", "On"},
              .is_global = true,
              .is_visible = []() { return current_settings_mode >= 2; },
          };
          renodx::utils::settings::LoadSetting(renodx::utils::settings::global_name, setting);
          renodx::mods::swapchain::device_proxy_wait_idle_destination = (setting->GetValue() == 1.f);
          settings.push_back(setting);
        }

        for (const auto& [key, format] : UPGRADE_TARGETS) {
          auto* setting = new renodx::utils::settings::Setting{
              .key = "Upgrade_" + key,
              .value_type = renodx::utils::settings::SettingValueType::INTEGER,
              .default_value = 0.f,
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
            renodx::mods::swapchain::resource_upgrade_infos.push_back({
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


        const reshade::api::format scene_intermediate_formats[] = {
            reshade::api::format::r8g8b8a8_unorm,
            reshade::api::format::r8g8b8a8_typeless,
            reshade::api::format::r8g8b8a8_unorm_srgb,
            reshade::api::format::b8g8r8a8_unorm,
            reshade::api::format::r10g10b10a2_unorm,
            reshade::api::format::b10g10r10a2_unorm,
        };

        const float scene_intermediate_aspect_ratios[] = {
            16.f / 9.f,
            16.f / 10.f,
            24.f / 10.f,
            43.f / 18.f,
            64.f / 27.f,
        };

        for (const auto old_format : scene_intermediate_formats) {
          for (const float aspect_ratio : scene_intermediate_aspect_ratios) {
            renodx::mods::swapchain::resource_upgrade_infos.push_back({
                .old_format = old_format,
                .new_format = reshade::api::format::r16g16b16a16_float,
                .ignore_size = false,
                .use_resource_view_cloning = true,
                .use_resource_view_hot_swap = false,
                .aspect_ratio = aspect_ratio,
                .aspect_ratio_tolerance = 0.001f,
                .usage_include = reshade::api::resource_usage::render_target,
                .name = "Scene Intermediate",
            });
          }
        }

        reshade::register_event<reshade::addon_event::destroy_device>(
            OnDX9ReadbackDestroyDevice);
        reshade::register_event<reshade::addon_event::destroy_swapchain>(
            OnDX9ReadbackDestroySwapchain);
        reshade::register_event<reshade::addon_event::copy_resource>(
            OnDX9CopyResource);
        reshade::register_event<reshade::addon_event::copy_texture_region>(
            OnDX9CopyTextureRegion);
        reshade::register_event<reshade::addon_event::resolve_texture_region>(
            OnDX9ResolveTextureRegion);

        initialized = true;
      }

      break;
    case DLL_PROCESS_DETACH:
      ClearDX9ReadbackOptimizationCaches();
      reshade::unregister_event<reshade::addon_event::resolve_texture_region>(
          OnDX9ResolveTextureRegion);
      reshade::unregister_event<reshade::addon_event::copy_texture_region>(
          OnDX9CopyTextureRegion);
      reshade::unregister_event<reshade::addon_event::copy_resource>(
          OnDX9CopyResource);
      reshade::unregister_event<reshade::addon_event::destroy_swapchain>(
          OnDX9ReadbackDestroySwapchain);
      reshade::unregister_event<reshade::addon_event::destroy_device>(
          OnDX9ReadbackDestroyDevice);
      reshade::unregister_event<reshade::addon_event::present>(OnPresent);
      break;
  }

  renodx::utils::settings::Use(fdw_reason, &settings, &OnPresetOff);
  renodx::mods::swapchain::Use(fdw_reason, &shader_injection);
  renodx::mods::shader::Use(fdw_reason, custom_shaders, &shader_injection);

  if (fdw_reason == DLL_PROCESS_DETACH) {
    reshade::unregister_addon(h_module);
  }

  return TRUE;
}
