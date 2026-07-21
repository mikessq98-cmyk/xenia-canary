/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2018 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/d3d12/d3d12_util.h"

#include "xenia/base/assert.h"
#include "xenia/base/logging.h"

namespace xe {
namespace ui {
namespace d3d12 {
namespace util {

constexpr D3D12_HEAP_PROPERTIES kHeapPropertiesDefault = {
    D3D12_HEAP_TYPE_DEFAULT};
constexpr D3D12_HEAP_PROPERTIES kHeapPropertiesUpload = {
    D3D12_HEAP_TYPE_UPLOAD};
constexpr D3D12_HEAP_PROPERTIES kHeapPropertiesReadback = {
    D3D12_HEAP_TYPE_READBACK};

ID3D12RootSignature* CreateRootSignature(
    const D3D12Provider& provider, const D3D12_ROOT_SIGNATURE_DESC& desc) {
  ID3DBlob* blob;
  ID3DBlob* error_blob = nullptr;
  if (FAILED(provider.SerializeRootSignature(
          &desc, D3D_ROOT_SIGNATURE_VERSION_1, &blob, &error_blob))) {
    XELOGE("Failed to serialize a root signature");
    if (error_blob != nullptr) {
      XELOGE("{}",
             reinterpret_cast<const char*>(error_blob->GetBufferPointer()));
      error_blob->Release();
    }
    return nullptr;
  }
  if (error_blob != nullptr) {
    error_blob->Release();
  }
  ID3D12RootSignature* root_signature = nullptr;
  provider.GetDevice()->CreateRootSignature(0, blob->GetBufferPointer(),
                                            blob->GetBufferSize(),
                                            IID_PPV_ARGS(&root_signature));
  blob->Release();
  return root_signature;
}

#if XE_PLATFORM_WINRT
static DWORD StorePipelineCreationExceptionCode(DWORD code, DWORD* code_out);

// Must contain no objects requiring unwinding for __try to be legal.
static ID3D12PipelineState* CreateComputePipelineGuarded(
    ID3D12Device* device, const D3D12_COMPUTE_PIPELINE_STATE_DESC* desc,
    DWORD* exception_code_out) {
  *exception_code_out = 0;
  ID3D12PipelineState* pipeline = nullptr;
  __try {
    device->CreateComputePipelineState(desc, IID_PPV_ARGS(&pipeline));
    return pipeline;
  } __except (StorePipelineCreationExceptionCode(GetExceptionCode(),
                                                 exception_code_out)) {
    return nullptr;
  }
}
#endif  // XE_PLATFORM_WINRT

ID3D12PipelineState* CreateComputePipeline(
    ID3D12Device* device, const void* shader, size_t shader_size,
    ID3D12RootSignature* root_signature) {
  D3D12_COMPUTE_PIPELINE_STATE_DESC desc;
  desc.pRootSignature = root_signature;
  desc.CS.pShaderBytecode = shader;
  desc.CS.BytecodeLength = shader_size;
  desc.NodeMask = 0;
  desc.CachedPSO.pCachedBlob = nullptr;
  desc.CachedPSO.CachedBlobSizeInBytes = 0;
  desc.Flags = D3D12_PIPELINE_STATE_FLAG_NONE;
#if XE_PLATFORM_WINRT
  // The Xbox UWP driver's shader compiler can crash (access violation) inside
  // pipeline creation - under memory pressure even on valid shaders. Compute
  // pipeline creation was the last unguarded path into it (observed as an
  // unhandled XBSC_XS.dll crash on the GPU thread); contain it like the
  // graphics one - callers already handle a null return.
  DWORD creation_exception_code = 0;
  ID3D12PipelineState* pipeline =
      CreateComputePipelineGuarded(device, &desc, &creation_exception_code);
  if (creation_exception_code != 0) {
    XELOGE(
        "CreateComputePipeline: the driver's shader compiler CRASHED "
        "(exception 0x{:08X}) - the crash was contained, the pipeline is "
        "unavailable",
        uint32_t(creation_exception_code));
  }
  return pipeline;
#else
  ID3D12PipelineState* pipeline = nullptr;
  device->CreateComputePipelineState(&desc, IID_PPV_ARGS(&pipeline));
  return pipeline;
#endif  // XE_PLATFORM_WINRT
}

void CreateBufferRawSRV(ID3D12Device* device,
                        D3D12_CPU_DESCRIPTOR_HANDLE handle,
                        ID3D12Resource* buffer, uint32_t size,
                        uint64_t offset) {
  assert_false(size & (D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT - 1));
  assert_false(offset & (D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT - 1));
  D3D12_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = DXGI_FORMAT_R32_TYPELESS;
  desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Buffer.FirstElement = offset >> 2;
  desc.Buffer.NumElements = size >> 2;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
  device->CreateShaderResourceView(buffer, &desc, handle);
}

void CreateBufferRawUAV(ID3D12Device* device,
                        D3D12_CPU_DESCRIPTOR_HANDLE handle,
                        ID3D12Resource* buffer, uint32_t size,
                        uint64_t offset) {
  assert_false(size & (D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT - 1));
  assert_false(offset & (D3D12_RAW_UAV_SRV_BYTE_ALIGNMENT - 1));
  D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
  desc.Format = DXGI_FORMAT_R32_TYPELESS;
  desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  desc.Buffer.FirstElement = offset >> 2;
  desc.Buffer.NumElements = size >> 2;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.CounterOffsetInBytes = 0;
  desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
  device->CreateUnorderedAccessView(buffer, nullptr, &desc, handle);
}

void CreateBufferTypedSRV(ID3D12Device* device,
                          D3D12_CPU_DESCRIPTOR_HANDLE handle,
                          ID3D12Resource* buffer, DXGI_FORMAT format,
                          uint32_t num_elements, uint64_t first_element) {
  D3D12_SHADER_RESOURCE_VIEW_DESC desc;
  desc.Format = format;
  desc.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
  desc.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
  desc.Buffer.FirstElement = first_element;
  desc.Buffer.NumElements = num_elements;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_NONE;
  device->CreateShaderResourceView(buffer, &desc, handle);
}

void CreateBufferTypedUAV(ID3D12Device* device,
                          D3D12_CPU_DESCRIPTOR_HANDLE handle,
                          ID3D12Resource* buffer, DXGI_FORMAT format,
                          uint32_t num_elements, uint64_t first_element) {
  D3D12_UNORDERED_ACCESS_VIEW_DESC desc;
  desc.Format = format;
  desc.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
  desc.Buffer.FirstElement = first_element;
  desc.Buffer.NumElements = num_elements;
  desc.Buffer.StructureByteStride = 0;
  desc.Buffer.CounterOffsetInBytes = 0;
  desc.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_NONE;
  device->CreateUnorderedAccessView(buffer, nullptr, &desc, handle);
}

static DWORD StorePipelineCreationExceptionCode(DWORD code, DWORD* code_out) {
  *code_out = code;
  return EXCEPTION_EXECUTE_HANDLER;
}

HRESULT CreateGraphicsPipelineStateGuarded(
    ID3D12Device* device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC* desc,
    REFIID riid, void** pipeline_out, DWORD* exception_code_out) {
  // Must contain no objects requiring unwinding for __try to be legal.
  *exception_code_out = 0;
  __try {
    return device->CreateGraphicsPipelineState(desc, riid, pipeline_out);
  } __except (StorePipelineCreationExceptionCode(GetExceptionCode(),
                                                 exception_code_out)) {
    return E_FAIL;
  }
}

}  // namespace util
}  // namespace d3d12
}  // namespace ui
}  // namespace xe
