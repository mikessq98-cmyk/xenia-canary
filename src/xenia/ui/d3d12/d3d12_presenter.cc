/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#include "xenia/ui/d3d12/d3d12_presenter.h"

#include <climits>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "xenia/base/assert.h"
#include "xenia/base/cvar.h"
#include "xenia/base/logging.h"
#include "xenia/base/math.h"
#include "xenia/ui/d3d12/d3d12_provider.h"
#include "xenia/ui/d3d12/d3d12_util.h"
#include "xenia/ui/surface_win.h"
#if XE_PLATFORM_WINRT
#include "xenia/ui/surface_uwp.h"
#endif

DEFINE_bool(
    d3d12_allow_variable_refresh_rate_and_tearing, true,
    "In fullscreen, allow using variable refresh rate on displays supporting "
    "it. On displays not supporting VRR, screen tearing may occur in certain "
    "cases.",
    "D3D12");

DEFINE_string(
    postprocess_smaa, "",
    "SMAA (Enhanced Subpixel Morphological Anti-Aliasing) 1x applied to the "
    "game output at its native resolution, BEFORE any scaling "
    "(postprocess_scaling_and_sharpening) - preserving edge shapes better "
    "than FXAA with less blurring. Direct3D 12 only. Can be changed while "
    "running.\n"
    "Use: [none, low, medium, high, ultra]\n"
    " none (or empty, or any value not listed here):\n"
    "  SMAA disabled.\n"
    " low / medium / high / ultra:\n"
    "  SMAA quality preset (roughly 60% / 80% / 95% / 99% of the maximum "
    "achievable quality; higher presets search longer edges and handle "
    "diagonals and corners).",
    "Display");

DEFINE_string(
    postprocess_smaa_edge_detection, "luma",
    "Edge detection metric for SMAA (postprocess_smaa). Direct3D 12 only. "
    "Can be changed while running.\n"
    "Use: [luma, color, both]\n"
    " luma (or any other value):\n"
    "  Luminance-based - the SMAA default; cheapest, but transitions between "
    "differently colored surfaces of similar brightness may be missed.\n"
    " color / both:\n"
    "  Max per-channel color delta - detects a strict SUPERSET of the luma "
    "edges (mathematically, the max channel delta is always >= the "
    "luma-weighted delta), so this IS luma + color combined; catches "
    "chromatic aliasing the luma metric misses, slightly costlier.",
    "Display");

DEFINE_string(
    postprocess_sgsr_base, "catrom",
    "Base image reconstruction filter for SGSR "
    "(postprocess_scaling_and_sharpening = sgsr). SGSR itself only refines "
    "luminance on detected edges on top of this base, so the base filter "
    "defines the sharpness of flat areas and of all color information. "
    "Direct3D 12 only. Takes effect on the next emulator start.\n"
    "Use: [catrom, bilinear]\n"
    " catrom:\n"
    "  Catmull-Rom bicubic - noticeably sharper than bilinear, slightly "
    "costlier (9 texture samples instead of 1 for the base).\n"
    " bilinear (or any other value):\n"
    "  Plain bilinear - the original SGSR behavior.",
    "Display");

namespace xe {
namespace ui {
namespace d3d12 {

// Whether the swap-chain Present forces vertical sync (SyncInterval != 0), which
// makes the host window system block on the CPU when presenting. This MUST match
// the SyncInterval actually passed to IDXGISwapChain::Present in
// PaintAndPresentImpl: on Xbox UWP it's Present(1, 0) (vsync), everywhere else
// Present(0, ...) (no vsync). Reported to the Presenter as is_vsync_implicit so
// it keeps the blocking present off the guest-output (GPU-emulation) thread
// instead of stalling emulation on every vblank.
#if XE_PLATFORM_WINRT
static constexpr bool kPresentForcesVsync = true;
#else
static constexpr bool kPresentForcesVsync = false;
#endif

// Generated with `xb buildshaders`.
namespace shaders {
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_bilinear_dither_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_bilinear_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_resample_dither_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_resample_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_sharpen_dither_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_cas_sharpen_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_easu_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_rcas_dither_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_ffx_fsr_rcas_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_sgsr_catrom_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_sgsr_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/guest_output_triangle_strip_rect_vs.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_blend_weight_high_flat_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_blend_weight_high_nodiag_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_blend_weight_high_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_blend_weight_high_vs.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_blend_weight_low_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_blend_weight_low_vs.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_blend_weight_medium_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_blend_weight_medium_vs.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_blend_weight_ultra_flat_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_blend_weight_ultra_nodiag_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_blend_weight_ultra_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_blend_weight_ultra_vs.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_edge_color_high_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_edge_color_low_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_edge_color_medium_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_edge_color_ultra_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_edge_luma_high_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_edge_luma_low_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_edge_luma_medium_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_edge_luma_ultra_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_edge_luma_vs.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_neighborhood_blend_ps.h"
#include "xenia/ui/shaders/bytecode/d3d12_5_1/smaa_neighborhood_blend_vs.h"
}  // namespace shaders

// SMAA lookup texture data (third_party/SMAA v2.8).
namespace smaa_textures {
#include "third_party/SMAA/Textures/AreaTex.h"
#include "third_party/SMAA/Textures/SearchTex.h"
}  // namespace smaa_textures

D3D12Presenter::~D3D12Presenter() {
  // Await completion of the usage of everything before destroying anything,
  // irrespective of the declaration order in the class.
  // From most likely the latest to most likely the earliest to be signaled, so
  // just one sleep will likely be needed.
  paint_context_.AwaitSwapChainUsageCompletion();
  if (guest_output_resource_refresher_completion_timeline_) {
    guest_output_resource_refresher_completion_timeline_->AwaitAllSubmissions();
  }
  if (ui_completion_timeline_) {
    ui_completion_timeline_->AwaitAllSubmissions();
  }
}

Surface::TypeFlags D3D12Presenter::GetSupportedSurfaceTypes() const {
  Surface::TypeFlags types = 0;
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES)
  types |= Surface::kTypeFlag_Win32Hwnd;
#endif
#if XE_PLATFORM_WINRT
  // Without advertising the CoreWindow surface, the App Container build supports
  // no surface type at all, so the window never creates a surface and never
  // paints (black screen). This pairs with the kTypeIndex_UWPCore swap-chain
  // creation case below.
  types |= Surface::kTypeFlag_UWPCore;
#endif
  return types;
}

bool D3D12Presenter::CaptureGuestOutput(RawImage& image_out) {
  Microsoft::WRL::ComPtr<ID3D12Resource> guest_output_resource;
  {
    uint32_t guest_output_mailbox_index;
    std::unique_lock<std::mutex> guest_output_consumer_lock(
        ConsumeGuestOutput(guest_output_mailbox_index, nullptr, nullptr));
    if (guest_output_mailbox_index != UINT32_MAX) {
      guest_output_resource =
          guest_output_resources_[guest_output_mailbox_index].second;
    }
    // Incremented the reference count of the guest output resource - safe to
    // leave the consumer critical section now.
  }
  if (!guest_output_resource) {
    return false;
  }

  ID3D12Device* device = provider_.GetDevice();

  D3D12_RESOURCE_DESC texture_desc = guest_output_resource->GetDesc();
  D3D12_TEXTURE_COPY_LOCATION copy_dest;
  UINT64 copy_dest_size;
  device->GetCopyableFootprints(&texture_desc, 0, 1, 0,
                                &copy_dest.PlacedFootprint, nullptr, nullptr,
                                &copy_dest_size);

  D3D12_RESOURCE_DESC buffer_desc;
  util::FillBufferResourceDesc(buffer_desc, copy_dest_size,
                               D3D12_RESOURCE_FLAG_NONE);
  Microsoft::WRL::ComPtr<ID3D12Resource> buffer;
  // Create zeroed not to leak data in the row padding.
  if (FAILED(device->CreateCommittedResource(
          &util::kHeapPropertiesReadback, D3D12_HEAP_FLAG_NONE, &buffer_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&buffer)))) {
    XELOGE("D3D12Presenter: Failed to create the guest output capture buffer");
    return false;
  }

  {
    Microsoft::WRL::ComPtr<ID3D12CommandAllocator> command_allocator;
    if (FAILED(
            device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                           IID_PPV_ARGS(&command_allocator)))) {
      XELOGE(
          "D3D12Presenter: Failed to create the guest output capturing command "
          "allocator");
      return false;
    }
    Microsoft::WRL::ComPtr<ID3D12GraphicsCommandList> command_list;
    if (FAILED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                                         command_allocator.Get(), nullptr,
                                         IID_PPV_ARGS(&command_list)))) {
      XELOGE(
          "D3D12Presenter: Failed to create the guest output capturing command "
          "list");
      return false;
    }

    D3D12_RESOURCE_BARRIER barrier;
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier.Transition.pResource = guest_output_resource.Get();
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = kGuestOutputInternalState;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    if constexpr (kGuestOutputInternalState !=
                  D3D12_RESOURCE_STATE_COPY_SOURCE) {
      command_list->ResourceBarrier(1, &barrier);
    }
    copy_dest.pResource = buffer.Get();
    copy_dest.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    D3D12_TEXTURE_COPY_LOCATION copy_source;
    copy_source.pResource = guest_output_resource.Get();
    copy_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copy_source.SubresourceIndex = 0;
    command_list->CopyTextureRegion(&copy_dest, 0, 0, 0, &copy_source, nullptr);
    if constexpr (kGuestOutputInternalState !=
                  D3D12_RESOURCE_STATE_COPY_SOURCE) {
      std::swap(barrier.Transition.StateBefore, barrier.Transition.StateAfter);
      command_list->ResourceBarrier(1, &barrier);
    }
    if (FAILED(command_list->Close())) {
      XELOGE(
          "D3D12Presenter: Failed to close the guest output capturing command "
          "list");
      return false;
    }

    Microsoft::WRL::ComPtr<ID3D12Fence> fence;
    const HRESULT fence_create_result =
        device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    if (FAILED(fence_create_result)) {
      XELOGE(
          "D3D12Presenter: Failed to create the guest output capturing fence, "
          "result 0x{:08X}",
          fence_create_result);
      return false;
    }
    ID3D12CommandQueue* const direct_queue = provider_.GetDirectQueue();
    ID3D12CommandList* execute_command_list = command_list.Get();
    direct_queue->ExecuteCommandLists(1, &execute_command_list);
    const HRESULT fence_signal_result = direct_queue->Signal(fence.Get(), 1);
    if (FAILED(fence_signal_result)) {
      XELOGE(
          "D3D12Presenter: Failed to enqueue signaling of the guest output "
          "capturing fence, result 0x{:08X}",
          fence_signal_result);
      return false;
    }
    const HRESULT fence_wait_result = fence->SetEventOnCompletion(1, nullptr);
    if (FAILED(fence_wait_result)) {
      XELOGE(
          "D3D12Presenter: Failed to await the guest output capturing fence, "
          "result 0x{:08X}",
          fence_wait_result);
      return false;
    }
  }

  D3D12_RANGE read_range;
  read_range.Begin = copy_dest.PlacedFootprint.Offset;
  read_range.End = copy_dest_size;
  void* mapping;
  if (FAILED(buffer->Map(0, &read_range, &mapping))) {
    XELOGE("D3D12Presenter: Failed to map the guest output capture buffer");
    return false;
  }
  image_out.width = uint32_t(texture_desc.Width);
  image_out.height = uint32_t(texture_desc.Height);
  image_out.stride = sizeof(uint32_t) * image_out.width;
  image_out.data.resize(image_out.stride * image_out.height);
  uint32_t* image_out_pixels =
      reinterpret_cast<uint32_t*>(image_out.data.data());
  for (uint32_t y = 0; y < image_out.height; ++y) {
    uint32_t* dest_row = &image_out_pixels[size_t(image_out.width) * y];
    const uint32_t* source_row = reinterpret_cast<const uint32_t*>(
        reinterpret_cast<const uint8_t*>(mapping) +
        copy_dest.PlacedFootprint.Offset +
        size_t(copy_dest.PlacedFootprint.Footprint.RowPitch) * y);
    for (uint32_t x = 0; x < image_out.width; ++x) {
      dest_row[x] = Packed10bpcRGBTo8bpcBytes(source_row[x]);
    }
  }
  // Unmapping will be done implicitly when the resource goes out of scope and
  // gets destroyed.
  return true;
}

Presenter::SurfacePaintConnectResult
D3D12Presenter::ConnectOrReconnectPaintingToSurfaceFromUIThread(
    Surface& new_surface, uint32_t new_surface_width,
    uint32_t new_surface_height, bool was_paintable,
    bool& is_vsync_implicit_out) {
  uint32_t new_swap_chain_width = std::min(
      new_surface_width, uint32_t(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION));
  uint32_t new_swap_chain_height = std::min(
      new_surface_height, uint32_t(D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION));

  // ConnectOrReconnectPaintingToSurfaceFromUIThread may be called only for the
  // surface of the current swap chain or when the old swap chain has already
  // been destroyed, if the surface is the same, try resizing.
  if (paint_context_.swap_chain) {
    if (was_paintable &&
        paint_context_.swap_chain_width == new_swap_chain_width &&
        paint_context_.swap_chain_height == new_swap_chain_height) {
      is_vsync_implicit_out = kPresentForcesVsync;
      return SurfacePaintConnectResult::kSuccessUnchanged;
    }
    paint_context_.AwaitSwapChainUsageCompletion();
    // Using the current swap_chain_allows_tearing_ value that's consistent with
    // the creation of the swap chain because ResizeBuffers can't toggle the
    // tearing flag.
    for (Microsoft::WRL::ComPtr<ID3D12Resource>& swap_chain_buffer_ref :
         paint_context_.swap_chain_buffers) {
      swap_chain_buffer_ref.Reset();
    }
    bool swap_chain_resized =
        SUCCEEDED(paint_context_.swap_chain->ResizeBuffers(
            0, UINT(new_swap_chain_width), UINT(new_swap_chain_height),
            DXGI_FORMAT_UNKNOWN,
            paint_context_.swap_chain_allows_tearing
                ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING
                : 0));
    if (swap_chain_resized) {
      for (uint32_t i = 0; i < PaintContext::kSwapChainBufferCount; ++i) {
        if (FAILED(paint_context_.swap_chain->GetBuffer(
                i, IID_PPV_ARGS(&paint_context_.swap_chain_buffers[i])))) {
          swap_chain_resized = false;
          break;
        }
      }
      if (swap_chain_resized) {
        paint_context_.swap_chain_width = new_swap_chain_width;
        paint_context_.swap_chain_height = new_swap_chain_height;
      }
    }
    if (!swap_chain_resized) {
      XELOGE("D3D12Presenter: Failed to resize a swap chain");
      // Failed to resize, retry creating from scratch.
      paint_context_.DestroySwapChain();
    }
  }

  if (!paint_context_.swap_chain) {
    // Create a new swap chain.
    Surface::TypeIndex surface_type = new_surface.GetType();
    DXGI_SWAP_CHAIN_DESC1 swap_chain_desc;
    swap_chain_desc.Width = UINT(new_swap_chain_width);
    swap_chain_desc.Height = UINT(new_swap_chain_height);
    swap_chain_desc.Format = kSwapChainFormat;
    swap_chain_desc.Stereo = false;
    swap_chain_desc.SampleDesc.Count = 1;
    swap_chain_desc.SampleDesc.Quality = 0;
    swap_chain_desc.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    swap_chain_desc.BufferCount = UINT(PaintContext::kSwapChainBufferCount);
    // DXGI_SCALING_STRETCH may cause the content to "shake" while resizing,
    // with relayout done for the guest output twice visually rather than once,
    // and the UI becoming stretched and then jumping to normal. If it's
    // possible to cover the entire surface without stretching, don't stretch.
    // After resizing, the presenter repaints as soon as possible anyway, so
    swap_chain_desc.Scaling = (new_swap_chain_width == new_surface_width &&
                               new_swap_chain_height == new_surface_height)
                                  ? DXGI_SCALING_NONE
                                  : DXGI_SCALING_STRETCH;
#if XE_PLATFORM_WINRT
    // CoreWindow swap chains on Xbox don't reliably display with
    // DXGI_SCALING_NONE: CreateSwapChainForCoreWindow accepts it, Present()
    // returns S_OK, but nothing is composited to the screen (black screen).
    // DXGI_SCALING_STRETCH is the only scaling mode the CoreWindow compositor
    // handles reliably here, so force it (matches the proven UWP port).
    swap_chain_desc.Scaling = DXGI_SCALING_STRETCH;
#endif
    swap_chain_desc.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    swap_chain_desc.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
    swap_chain_desc.Flags = 0;
    if (cvars::d3d12_allow_variable_refresh_rate_and_tearing &&
        dxgi_supports_tearing_) {
      // Allow tearing in borderless fullscreen to support variable refresh
      // rate.
      swap_chain_desc.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    IDXGIFactory2* dxgi_factory = provider_.GetDXGIFactory();
    ID3D12CommandQueue* direct_queue = provider_.GetDirectQueue();
    Microsoft::WRL::ComPtr<IDXGISwapChain1> swap_chain_1;
    switch (surface_type) {
#if WINAPI_FAMILY_PARTITION(WINAPI_PARTITION_DESKTOP | WINAPI_PARTITION_GAMES)
      case Surface::kTypeIndex_Win32Hwnd: {
        HWND surface_hwnd =
            static_cast<const Win32HwndSurface&>(new_surface).hwnd();
        if (FAILED(dxgi_factory->CreateSwapChainForHwnd(
                direct_queue, surface_hwnd, &swap_chain_desc, nullptr, nullptr,
                &swap_chain_1))) {
          XELOGE("D3D12Presenter: Failed to create a swap chain for the HWND");
          return SurfacePaintConnectResult::kFailure;
        }
        // Disable automatic Alt+Enter handling - DXGI fullscreen doesn't
        // support ALLOW_TEARING, and using custom fullscreen in ui::Win32Window
        // anyway as with Alt+Enter the menu is kept, state changes are tracked
        // better, and nothing is presented for some reason.
        dxgi_factory->MakeWindowAssociation(surface_hwnd,
                                            DXGI_MWA_NO_ALT_ENTER);
      } break;
#endif
#if XE_PLATFORM_WINRT
      case Surface::kTypeIndex_UWPCore: {
        // For D3D12, pDevice is the direct command queue; pWindow is the
        // CoreWindow IUnknown. Width/Height are already non-zero in the desc
        // (CoreWindow requires the flip model, which kSwapEffect already is).
        // https://learn.microsoft.com/windows/win32/api/dxgi1_2/nf-dxgi1_2-idxgifactory2-createswapchainforcorewindow
        ::IUnknown* core_window_abi =
            static_cast<const UWPCoreWindowSurface&>(new_surface)
                .core_window_abi();
        if (FAILED(dxgi_factory->CreateSwapChainForCoreWindow(
                direct_queue, core_window_abi, &swap_chain_desc, nullptr,
                &swap_chain_1))) {
          XELOGE(
              "D3D12Presenter: Failed to create a swap chain for the "
              "CoreWindow");
          return SurfacePaintConnectResult::kFailure;
        }
      } break;
#endif
      default:
        assert_unhandled_case(surface_type);
        XELOGE(
            "D3D12Presenter: Tried to create a swap chain for an unsupported "
            "Xenia surface type");
        return SurfacePaintConnectResult::kFailureSurfaceUnusable;
    }
    if (FAILED(swap_chain_1->QueryInterface(
            IID_PPV_ARGS(&paint_context_.swap_chain)))) {
      XELOGE(
          "D3D12Presenter: Failed to get version 3 of the swap chain "
          "interface");
      return SurfacePaintConnectResult::kFailure;
    }
    // From now on, in case of any failure, DestroySwapChain must be called
    // before returning.
    paint_context_.swap_chain_width = new_swap_chain_width;
    paint_context_.swap_chain_height = new_swap_chain_height;
    paint_context_.swap_chain_allows_tearing =
        (swap_chain_desc.Flags & DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING) != 0;
    for (uint32_t i = 0; i < PaintContext::kSwapChainBufferCount; ++i) {
      if (FAILED(paint_context_.swap_chain->GetBuffer(
              i, IID_PPV_ARGS(&paint_context_.swap_chain_buffers[i])))) {
        XELOGE(
            "D3D12Presenter: Failed to get buffer {} of a {}-buffer swap chain",
            i, PaintContext::kSwapChainBufferCount);
        paint_context_.DestroySwapChain();
        return SurfacePaintConnectResult::kFailure;
      }
    }
  }

  ID3D12Device* device = provider_.GetDevice();

  // Create the RTV descriptors.
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_heap_start =
      paint_context_.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_RENDER_TARGET_VIEW_DESC rtv_desc;
  rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
  rtv_desc.Texture2D.MipSlice = 0;
  rtv_desc.Texture2D.PlaneSlice = 0;
  for (uint32_t i = 0; i < PaintContext::kSwapChainBufferCount; ++i) {
    ID3D12Resource* swap_chain_buffer =
        paint_context_.swap_chain_buffers[i].Get();
    rtv_desc.Format = kSwapChainFormat;
    device->CreateRenderTargetView(
        swap_chain_buffer, &rtv_desc,
        provider_.OffsetRTVDescriptor(
            rtv_heap_start, PaintContext::kRTVIndexSwapChainBuffer0 + i));
  }

  is_vsync_implicit_out = kPresentForcesVsync;
  return SurfacePaintConnectResult::kSuccess;
}

void D3D12Presenter::DisconnectPaintingFromSurfaceFromUIThreadImpl() {
  paint_context_.DestroySwapChain();
}

bool D3D12Presenter::RefreshGuestOutputImpl(
    uint32_t mailbox_index, uint32_t frontbuffer_width,
    uint32_t frontbuffer_height,
    std::function<bool(GuestOutputRefreshContext& context)> refresher,
    bool& is_8bpc_out_ref) {
  assert_not_zero(frontbuffer_width);
  assert_not_zero(frontbuffer_height);
  std::pair<uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>&
      guest_output_resource_ref = guest_output_resources_[mailbox_index];
  if (guest_output_resource_ref.second) {
    D3D12_RESOURCE_DESC guest_output_resource_current_desc =
        guest_output_resource_ref.second->GetDesc();
    if (guest_output_resource_current_desc.Width != frontbuffer_width ||
        guest_output_resource_current_desc.Height != frontbuffer_height) {
      // Main target painting has its own reference to the textures for reading
      // in its own completion timeline, safe to release here.
      guest_output_resource_refresher_completion_timeline_
          ->AwaitSubmissionAndUpdateCompleted(guest_output_resource_ref.first);
      guest_output_resource_ref.second.Reset();
    }
  }
  if (!guest_output_resource_ref.second) {
    ID3D12Device* device = provider_.GetDevice();
    D3D12_RESOURCE_DESC guest_output_resource_new_desc;
    guest_output_resource_new_desc.Dimension =
        D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    guest_output_resource_new_desc.Alignment = 0;
    guest_output_resource_new_desc.Width = frontbuffer_width;
    guest_output_resource_new_desc.Height = frontbuffer_height;
    guest_output_resource_new_desc.DepthOrArraySize = 1;
    guest_output_resource_new_desc.MipLevels = 1;
    guest_output_resource_new_desc.Format = kGuestOutputFormat;
    guest_output_resource_new_desc.SampleDesc.Count = 1;
    guest_output_resource_new_desc.SampleDesc.Quality = 0;
    guest_output_resource_new_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    guest_output_resource_new_desc.Flags =
        D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
    if (FAILED(device->CreateCommittedResource(
            &util::kHeapPropertiesDefault,
            provider_.GetHeapFlagCreateNotZeroed(),
            &guest_output_resource_new_desc, kGuestOutputInternalState, nullptr,
            IID_PPV_ARGS(&guest_output_resource_ref.second)))) {
      XELOGE("D3D12Presenter: Failed to create the guest output {}x{} texture",
             frontbuffer_width, frontbuffer_height);
      return false;
    }
  }
  D3D12GuestOutputRefreshContext context(
      is_8bpc_out_ref, guest_output_resource_ref.second.Get());
  bool refresher_succeeded = refresher(context);
  // Even if the refresher has returned false, it still might have submitted
  // some commands referencing the resource. It's better to put an excessive
  // signal and wait slightly longer, for nothing important, while shutting down
  // than to destroy the resource while it's still in use.
  guest_output_resource_ref.first =
      guest_output_resource_refresher_completion_timeline_
          ->GetUpcomingSubmission();
  guest_output_resource_refresher_completion_timeline_->SignalAndAdvance(
      provider_.GetDirectQueue());
  return refresher_succeeded;
}

void D3D12Presenter::PaintContext::DestroySwapChain() {
  if (!swap_chain) {
    return;
  }
  AwaitSwapChainUsageCompletion();
  for (Microsoft::WRL::ComPtr<ID3D12Resource>& swap_chain_buffer_ref :
       swap_chain_buffers) {
    swap_chain_buffer_ref.Reset();
  }
  swap_chain.Reset();
  swap_chain_allows_tearing = false;
  swap_chain_height = 0;
  swap_chain_width = 0;
}

Presenter::PaintResult D3D12Presenter::PaintAndPresentImpl(
    bool execute_ui_drawers) {
  // Begin the command list with the command allocator not currently potentially
  // used on the GPU.
  const uint64_t current_paint_submission =
      paint_context_.paint_completion_timeline->GetUpcomingSubmission();
  const uint64_t command_allocator_count =
      UINT64(paint_context_.command_allocators.size());
  paint_context_.paint_completion_timeline
      ->AwaitMaxSubmissionsPendingAndUpdateCompleted(command_allocator_count);
  ID3D12CommandAllocator* command_allocator =
      paint_context_
          .command_allocators[current_paint_submission %
                              command_allocator_count]
          .Get();
  command_allocator->Reset();
  ID3D12GraphicsCommandList* command_list = paint_context_.command_list.Get();
  command_list->Reset(command_allocator, nullptr);

  ID3D12Device* device = provider_.GetDevice();

  // Obtain the RTV heap and the back buffer.
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_heap_start =
      paint_context_.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  UINT back_buffer_index =
      paint_context_.swap_chain->GetCurrentBackBufferIndex();
  D3D12_CPU_DESCRIPTOR_HANDLE back_buffer_rtv = provider_.OffsetRTVDescriptor(
      rtv_heap_start,
      PaintContext::kRTVIndexSwapChainBuffer0 + back_buffer_index);
  ID3D12Resource* back_buffer =
      paint_context_.swap_chain_buffers[back_buffer_index].Get();
  bool back_buffer_acquired = false;
  bool back_buffer_bound = false;
  bool back_buffer_clear_needed = true;
  constexpr float kBackBufferClearColor[] = {0.0f, 0.0f, 0.0f, 1.0f};

  // Draw the guest output.

  GuestOutputProperties guest_output_properties;
  GuestOutputPaintConfig guest_output_paint_config;
  Microsoft::WRL::ComPtr<ID3D12Resource> guest_output_resource;
  {
    uint32_t guest_output_mailbox_index;
    std::unique_lock<std::mutex> guest_output_consumer_lock(
        ConsumeGuestOutput(guest_output_mailbox_index, &guest_output_properties,
                           &guest_output_paint_config));
    if (guest_output_mailbox_index != UINT32_MAX) {
      guest_output_resource =
          guest_output_resources_[guest_output_mailbox_index].second;
    }
    // Incremented the reference count of the guest output resource - safe to
    // leave the consumer critical section now as everything here either will be
    // using the new reference or is exclusively owned by main target painting
    // (and multiple threads can't paint the main target at the same time).
  }

  if (guest_output_resource) {
    GuestOutputPaintFlow guest_output_flow = GetGuestOutputPaintFlow(
        guest_output_properties, paint_context_.swap_chain_width,
        paint_context_.swap_chain_height, D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION,
        D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, guest_output_paint_config);

    // Check if all guest output paint effects are supported by the
    // implementation.
    if (guest_output_flow.effect_count) {
      if (!guest_output_paint_final_pipelines_[size_t(
              guest_output_flow.effects[guest_output_flow.effect_count - 1])]) {
        guest_output_flow.effect_count = 0;
      }
      for (size_t i = 0; i + 1 < guest_output_flow.effect_count; ++i) {
        if (!guest_output_paint_intermediate_pipelines_[size_t(
                guest_output_flow.effects[i])]) {
          guest_output_flow.effect_count = 0;
          break;
        }
      }
      // If the configured effect can't be painted (for instance, its pipeline
      // couldn't be created because the driver's shader compiler crashed on
      // it), fall back to plain bilinear scaling instead of not displaying the
      // guest output at all.
      if (!guest_output_flow.effect_count &&
          guest_output_paint_config.GetEffect() !=
              GuestOutputPaintConfig::Effect::kBilinear) {
        GuestOutputPaintConfig bilinear_fallback_config =
            guest_output_paint_config;
        bilinear_fallback_config.SetEffect(
            GuestOutputPaintConfig::Effect::kBilinear);
        guest_output_flow = GetGuestOutputPaintFlow(
            guest_output_properties, paint_context_.swap_chain_width,
            paint_context_.swap_chain_height,
            D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION,
            D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION, bilinear_fallback_config);
        if (guest_output_flow.effect_count &&
            !guest_output_paint_final_pipelines_[size_t(
                guest_output_flow
                    .effects[guest_output_flow.effect_count - 1])]) {
          guest_output_flow.effect_count = 0;
        }
      }
    }

    if (guest_output_flow.effect_count) {
      ID3D12DescriptorHeap* view_heap = paint_context_.view_heap.Get();
      D3D12_CPU_DESCRIPTOR_HANDLE view_heap_cpu_start =
          view_heap->GetCPUDescriptorHandleForHeapStart();

      // Store the main target reference to the guest output texture so it's not
      // destroyed while it's still potentially in use by main target painting
      // queued on the GPU.
      size_t guest_output_resource_paint_ref_index = SIZE_MAX;
      size_t guest_output_resource_paint_ref_new_index = SIZE_MAX;
      // Try to find the existing reference to the same texture, or an already
      // released (or a taken, but never actually used) slot.
      for (size_t i = 0;
           i < paint_context_.guest_output_resource_paint_refs.size(); ++i) {
        const std::pair<uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>&
            guest_output_resource_paint_ref =
                paint_context_.guest_output_resource_paint_refs[i];
        if (guest_output_resource_paint_ref.second == guest_output_resource) {
          guest_output_resource_paint_ref_index = i;
          break;
        }
        if (guest_output_resource_paint_ref_new_index == SIZE_MAX &&
            (!guest_output_resource_paint_ref.second ||
             !guest_output_resource_paint_ref.first)) {
          guest_output_resource_paint_ref_new_index = i;
        }
      }
      if (guest_output_resource_paint_ref_index == SIZE_MAX) {
        // New texture - store the reference and create the descriptors.
        if (guest_output_resource_paint_ref_new_index == SIZE_MAX) {
          // Replace the earliest used reference.
          guest_output_resource_paint_ref_new_index = 0;
          for (size_t i = 1;
               i < paint_context_.guest_output_resource_paint_refs.size();
               ++i) {
            if (paint_context_.guest_output_resource_paint_refs[i].first <
                paint_context_
                    .guest_output_resource_paint_refs
                        [guest_output_resource_paint_ref_new_index]
                    .first) {
              guest_output_resource_paint_ref_new_index = i;
            }
          }
          // Await the completion of the usage of the old guest output
          // resource and its SRV descriptors.
          paint_context_.paint_completion_timeline
              ->AwaitSubmissionAndUpdateCompleted(
                  paint_context_
                      .guest_output_resource_paint_refs
                          [guest_output_resource_paint_ref_new_index]
                      .first);
        }
        guest_output_resource_paint_ref_index =
            guest_output_resource_paint_ref_new_index;
        // The actual submission index will be set if the texture is actually
        // used, not dropped due to some error.
        paint_context_.guest_output_resource_paint_refs
            [guest_output_resource_paint_ref_index] =
            std::make_pair(uint64_t(0), guest_output_resource);
        // Create the SRV descriptor of the new texture.
        D3D12_SHADER_RESOURCE_VIEW_DESC guest_output_resource_srv_desc;
        guest_output_resource_srv_desc.Format = kGuestOutputFormat;
        guest_output_resource_srv_desc.ViewDimension =
            D3D12_SRV_DIMENSION_TEXTURE2D;
        guest_output_resource_srv_desc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        guest_output_resource_srv_desc.Texture2D.MostDetailedMip = 0;
        guest_output_resource_srv_desc.Texture2D.MipLevels = 1;
        guest_output_resource_srv_desc.Texture2D.PlaneSlice = 0;
        guest_output_resource_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
        device->CreateShaderResourceView(
            guest_output_resource.Get(), &guest_output_resource_srv_desc,
            provider_.OffsetViewDescriptor(
                view_heap_cpu_start,
                PaintContext::kViewIndexGuestOutput0Srv +
                    uint32_t(guest_output_resource_paint_ref_index)));
      }

      // Make sure intermediate textures of the needed size are available, and
      // unneeded intermediate textures are destroyed.
      for (size_t i = 0; i < kMaxGuestOutputPaintEffects - 1; ++i) {
        std::pair<uint32_t, uint32_t> intermediate_needed_size(0, 0);
        if (i + 1 < guest_output_flow.effect_count) {
          intermediate_needed_size = guest_output_flow.effect_output_sizes[i];
        }
        Microsoft::WRL::ComPtr<ID3D12Resource>& intermediate_texture_ptr_ref =
            paint_context_.guest_output_intermediate_textures[i];
        std::pair<uint32_t, uint32_t> intermediate_current_size(0, 0);
        if (intermediate_texture_ptr_ref) {
          D3D12_RESOURCE_DESC intermediate_current_desc =
              intermediate_texture_ptr_ref->GetDesc();
          intermediate_current_size.first =
              uint32_t(intermediate_current_desc.Width);
          intermediate_current_size.second = intermediate_current_desc.Height;
        }
        if (intermediate_current_size != intermediate_needed_size) {
          if (intermediate_needed_size.first &&
              intermediate_needed_size.second) {
            // Need to replace immediately as a new texture with the requested
            // size is needed.
            if (intermediate_texture_ptr_ref) {
              paint_context_.paint_completion_timeline
                  ->AwaitSubmissionAndUpdateCompleted(
                      paint_context_
                          .guest_output_intermediate_texture_last_usage);
              intermediate_texture_ptr_ref.Reset();
            }
            // Resource.
            D3D12_RESOURCE_DESC intermediate_desc;
            intermediate_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            intermediate_desc.Alignment = 0;
            intermediate_desc.Width = intermediate_needed_size.first;
            intermediate_desc.Height = intermediate_needed_size.second;
            intermediate_desc.DepthOrArraySize = 1;
            intermediate_desc.MipLevels = 1;
            intermediate_desc.Format = kGuestOutputIntermediateFormat;
            intermediate_desc.SampleDesc.Count = 1;
            intermediate_desc.SampleDesc.Quality = 0;
            intermediate_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
            intermediate_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
            if (FAILED(device->CreateCommittedResource(
                    &util::kHeapPropertiesDefault,
                    provider_.GetHeapFlagCreateNotZeroed(), &intermediate_desc,
                    D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                    IID_PPV_ARGS(&intermediate_texture_ptr_ref)))) {
              XELOGE(
                  "D3D12Presenter: Failed to create a guest output "
                  "presentation intermediate texture");
              // Don't display the guest output, and don't try to create more
              // intermediate textures (only destroy them).
              guest_output_flow.effect_count = 0;
              continue;
            }
            ID3D12Resource* intermediate_texture =
                intermediate_texture_ptr_ref.Get();
            // SRV.
            D3D12_SHADER_RESOURCE_VIEW_DESC intermediate_srv_desc;
            intermediate_srv_desc.Format = kGuestOutputIntermediateFormat;
            intermediate_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            intermediate_srv_desc.Shader4ComponentMapping =
                D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            intermediate_srv_desc.Texture2D.MostDetailedMip = 0;
            intermediate_srv_desc.Texture2D.MipLevels = 1;
            intermediate_srv_desc.Texture2D.PlaneSlice = 0;
            intermediate_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
            device->CreateShaderResourceView(
                intermediate_texture, &intermediate_srv_desc,
                provider_.OffsetViewDescriptor(
                    view_heap_cpu_start,
                    uint32_t(
                        PaintContext::kViewIndexGuestOutputIntermediate0Srv +
                        i)));
            // RTV.
            D3D12_RENDER_TARGET_VIEW_DESC intermediate_rtv_desc;
            intermediate_rtv_desc.Format = kGuestOutputIntermediateFormat;
            intermediate_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            intermediate_rtv_desc.Texture2D.MipSlice = 0;
            intermediate_rtv_desc.Texture2D.PlaneSlice = 0;
            device->CreateRenderTargetView(
                intermediate_texture, &intermediate_rtv_desc,
                provider_.OffsetRTVDescriptor(
                    rtv_heap_start,
                    uint32_t(PaintContext::kRTVIndexGuestOutputIntermediate0 +
                             i)));
          } else {
            // Was previously needed, but not anymore - destroy when possible.
            if (intermediate_texture_ptr_ref &&
                paint_context_.paint_completion_timeline
                        ->GetCompletedSubmissionFromLastUpdate() >=
                    paint_context_
                        .guest_output_intermediate_texture_last_usage) {
              intermediate_texture_ptr_ref.Reset();
            }
          }
        }
      }

      if (guest_output_flow.effect_count) {
        paint_context_
            .guest_output_resource_paint_refs
                [guest_output_resource_paint_ref_index]
            .first = current_paint_submission;
        if (guest_output_flow.effect_count > 1) {
          paint_context_.guest_output_intermediate_texture_last_usage =
              current_paint_submission;
        }

        command_list->SetDescriptorHeaps(1, &view_heap);
      }

      // SMAA 1x pre-pass at the guest output resolution, before any scaling -
      // if it ran, the scaling chain reads the anti-aliased image instead of
      // the raw guest output.
      bool smaa_applied = false;
      if (guest_output_flow.effect_count) {
        size_t smaa_quality = GetSmaaQualityFromCvar();
        if (smaa_quality != SIZE_MAX) {
          smaa_applied = PaintSmaaPasses(
              command_list, guest_output_resource.Get(),
              guest_output_flow.properties.frontbuffer_width,
              guest_output_flow.properties.frontbuffer_height, smaa_quality,
              current_paint_submission);
        }
      }

      // This effect loop must not be aborted so the states of the resources
      // involved are consistent.
      D3D12_GPU_DESCRIPTOR_HANDLE view_heap_gpu_start =
          view_heap->GetGPUDescriptorHandleForHeapStart();
      for (size_t i = 0; i < guest_output_flow.effect_count; ++i) {
        bool is_final_effect = i + 1 >= guest_output_flow.effect_count;

        GuestOutputPaintEffect effect = guest_output_flow.effects[i];

        if (effect == GuestOutputPaintEffect::kSgsr) {
          // One-time activity marker for diagnosing the configuration.
          static bool sgsr_logged = false;
          if (!sgsr_logged) {
            sgsr_logged = true;
            uint32_t sgsr_input_width, sgsr_input_height;
            guest_output_flow.GetEffectInputSize(i, sgsr_input_width,
                                                 sgsr_input_height);
            XELOGI(
                "D3D12Presenter: SGSR upscaling active, {}x{} -> {}x{}, {} "
                "base",
                sgsr_input_width, sgsr_input_height,
                guest_output_flow.effect_output_sizes[i].first,
                guest_output_flow.effect_output_sizes[i].second,
                cvars::postprocess_sgsr_base == "catrom" ? "Catmull-Rom"
                                                         : "bilinear");
          }
        }

        ID3D12Resource* effect_dest_resource;
        int32_t effect_rect_x, effect_rect_y;
        if (is_final_effect) {
          effect_dest_resource = back_buffer;
          if (!back_buffer_acquired) {
            D3D12_RESOURCE_BARRIER barrier_present_to_rtv;
            barrier_present_to_rtv.Type =
                D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier_present_to_rtv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier_present_to_rtv.Transition.pResource = back_buffer;
            barrier_present_to_rtv.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier_present_to_rtv.Transition.StateBefore =
                D3D12_RESOURCE_STATE_PRESENT;
            barrier_present_to_rtv.Transition.StateAfter =
                D3D12_RESOURCE_STATE_RENDER_TARGET;
            command_list->ResourceBarrier(1, &barrier_present_to_rtv);
            back_buffer_acquired = true;
          }
          effect_rect_x = guest_output_flow.output_x;
          effect_rect_y = guest_output_flow.output_y;
        } else {
          effect_dest_resource =
              paint_context_.guest_output_intermediate_textures[i].Get();
          if (!i) {
            // If this is not the first effect, the transition has been done at
            // the end of the previous effect in a single command.
            D3D12_RESOURCE_BARRIER barrier_srv_to_rtv;
            barrier_srv_to_rtv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier_srv_to_rtv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier_srv_to_rtv.Transition.pResource = effect_dest_resource;
            barrier_srv_to_rtv.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier_srv_to_rtv.Transition.StateBefore =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier_srv_to_rtv.Transition.StateAfter =
                D3D12_RESOURCE_STATE_RENDER_TARGET;
            command_list->ResourceBarrier(1, &barrier_srv_to_rtv);
          }
          command_list->DiscardResource(effect_dest_resource, nullptr);
          effect_rect_x = 0;
          effect_rect_y = 0;
        }

        if (is_final_effect) {
          if (!back_buffer_bound) {
            command_list->OMSetRenderTargets(1, &back_buffer_rtv, true,
                                             nullptr);
            back_buffer_bound = true;
          }
        } else {
          D3D12_CPU_DESCRIPTOR_HANDLE intermediate_rtv =
              provider_.OffsetRTVDescriptor(
                  rtv_heap_start,
                  uint32_t(PaintContext::kRTVIndexGuestOutputIntermediate0 +
                           i));
          command_list->OMSetRenderTargets(1, &intermediate_rtv, true, nullptr);
          back_buffer_bound = false;
        }
        if (is_final_effect) {
          back_buffer_bound = true;
        }
        D3D12_RESOURCE_DESC effect_dest_resource_desc =
            effect_dest_resource->GetDesc();
        D3D12_VIEWPORT viewport;
        viewport.TopLeftX = 0.0f;
        viewport.TopLeftY = 0.0f;
        viewport.Width = float(effect_dest_resource_desc.Width);
        viewport.Height = float(effect_dest_resource_desc.Height);
        viewport.MinDepth = 0.0f;
        viewport.MaxDepth = 1.0f;
        command_list->RSSetViewports(1, &viewport);
        D3D12_RECT scissor;
        scissor.left = 0;
        scissor.top = 0;
        scissor.right = LONG(effect_dest_resource_desc.Width);
        scissor.bottom = LONG(effect_dest_resource_desc.Height);
        command_list->RSSetScissorRects(1, &scissor);

        command_list->SetPipelineState(
            is_final_effect
                ? guest_output_paint_final_pipelines_[size_t(effect)].Get()
                : guest_output_paint_intermediate_pipelines_[size_t(effect)]
                      .Get());
        GuestOutputPaintRootSignatureIndex
            guest_output_paint_root_signature_index =
                GetGuestOutputPaintRootSignatureIndex(effect);
        command_list->SetGraphicsRootSignature(
            guest_output_paint_root_signatures_
                [size_t(guest_output_paint_root_signature_index)]
                    .Get());

        UINT effect_src_view_index = UINT(
            i ? (PaintContext::kViewIndexGuestOutputIntermediate0Srv + (i - 1))
              : (smaa_applied
                     ? UINT(PaintContext::kViewIndexSmaaOutput)
                     : UINT(PaintContext::kViewIndexGuestOutput0Srv +
                            guest_output_resource_paint_ref_index)));
        command_list->SetGraphicsRootDescriptorTable(
            UINT(GuestOutputPaintRootParameter::kSource),
            provider_.OffsetViewDescriptor(view_heap_gpu_start,
                                           effect_src_view_index));

        GuestOutputPaintRectangleConstants effect_rect_constants;
        float effect_x_to_ndc = 2.0f / viewport.Width;
        float effect_y_to_ndc = 2.0f / viewport.Height;
        effect_rect_constants.x =
            -1.0f + float(effect_rect_x) * effect_x_to_ndc;
        // +Y is -V.
        effect_rect_constants.y = 1.0f - float(effect_rect_y) * effect_y_to_ndc;
        effect_rect_constants.width =
            float(guest_output_flow.effect_output_sizes[i].first) *
            effect_x_to_ndc;
        effect_rect_constants.height =
            -float(guest_output_flow.effect_output_sizes[i].second) *
            effect_y_to_ndc;
        command_list->SetGraphicsRoot32BitConstants(
            UINT(GuestOutputPaintRootParameter::kRectangle),
            sizeof(effect_rect_constants) / sizeof(uint32_t),
            &effect_rect_constants, 0);

        UINT effect_constants_size = 0;
        union {
          BilinearConstants bilinear;
          CasSharpenConstants cas_sharpen;
          CasResampleConstants cas_resample;
          FsrEasuConstants fsr_easu;
          FsrRcasConstants fsr_rcas;
          SgsrConstants sgsr;
        } effect_constants;
        switch (guest_output_paint_root_signature_index) {
          case kGuestOutputPaintRootSignatureIndexBilinear: {
            effect_constants_size = sizeof(effect_constants.bilinear);
            effect_constants.bilinear.Initialize(guest_output_flow, i);
          } break;
          case kGuestOutputPaintRootSignatureIndexCasSharpen: {
            effect_constants_size = sizeof(effect_constants.cas_sharpen);
            effect_constants.cas_sharpen.Initialize(guest_output_flow, i,
                                                    guest_output_paint_config);
          } break;
          case kGuestOutputPaintRootSignatureIndexCasResample: {
            effect_constants_size = sizeof(effect_constants.cas_resample);
            effect_constants.cas_resample.Initialize(guest_output_flow, i,
                                                     guest_output_paint_config);
          } break;
          case kGuestOutputPaintRootSignatureIndexFsrEasu: {
            effect_constants_size = sizeof(effect_constants.fsr_easu);
            effect_constants.fsr_easu.Initialize(guest_output_flow, i);
          } break;
          case kGuestOutputPaintRootSignatureIndexFsrRcas: {
            effect_constants_size = sizeof(effect_constants.fsr_rcas);
            effect_constants.fsr_rcas.Initialize(guest_output_flow, i,
                                                 guest_output_paint_config);
          } break;
          case kGuestOutputPaintRootSignatureIndexSgsr: {
            effect_constants_size = sizeof(effect_constants.sgsr);
            effect_constants.sgsr.Initialize(guest_output_flow, i,
                                             guest_output_paint_config);
          } break;
          default:
            break;
        }
        if (effect_constants_size) {
          command_list->SetGraphicsRoot32BitConstants(
              UINT(GuestOutputPaintRootParameter::kEffectConstants),
              effect_constants_size / sizeof(uint32_t), &effect_constants, 0);
        }

        command_list->IASetPrimitiveTopology(
            D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
        command_list->DrawInstanced(4, 1, 0, 0);

        if (is_final_effect) {
          // Clear the letterbox around the guest output if the guest output
          // doesn't cover the entire back buffer.
          if (guest_output_flow.letterbox_clear_rectangle_count) {
            D3D12_RECT letterbox_clear_d3d12_rectangles
                [GuestOutputPaintFlow::kMaxClearRectangles];
            for (size_t i = 0;
                 i < guest_output_flow.letterbox_clear_rectangle_count; ++i) {
              D3D12_RECT& letterbox_clear_d3d12_rectangle =
                  letterbox_clear_d3d12_rectangles[i];
              const GuestOutputPaintFlow::ClearRectangle&
                  letterbox_clear_rectangle =
                      guest_output_flow.letterbox_clear_rectangles[i];
              letterbox_clear_d3d12_rectangle.left =
                  LONG(letterbox_clear_rectangle.x);
              letterbox_clear_d3d12_rectangle.top =
                  LONG(letterbox_clear_rectangle.y);
              letterbox_clear_d3d12_rectangle.right =
                  LONG(letterbox_clear_rectangle.x +
                       letterbox_clear_rectangle.width);
              letterbox_clear_d3d12_rectangle.bottom =
                  LONG(letterbox_clear_rectangle.y +
                       letterbox_clear_rectangle.height);
            }
            command_list->ClearRenderTargetView(
                back_buffer_rtv, kBackBufferClearColor,
                UINT(guest_output_flow.letterbox_clear_rectangle_count),
                letterbox_clear_d3d12_rectangles);
          }
          back_buffer_clear_needed = false;
        } else {
          D3D12_RESOURCE_BARRIER barriers[2];
          UINT barrier_count = 0;
          // Transition the newly written intermediate image to SRV for use as
          // the source in the next effect.
          {
            assert_true(barrier_count < xe::countof(barriers));
            D3D12_RESOURCE_BARRIER& barrier_rtv_to_srv =
                barriers[barrier_count++];
            barrier_rtv_to_srv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier_rtv_to_srv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier_rtv_to_srv.Transition.pResource = effect_dest_resource;
            barrier_rtv_to_srv.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier_rtv_to_srv.Transition.StateBefore =
                D3D12_RESOURCE_STATE_RENDER_TARGET;
            barrier_rtv_to_srv.Transition.StateAfter =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
          }
          // Merge the current destination > next source transition with the
          // acquisition of the destination for the next effect.
          if (i + 2 < guest_output_flow.effect_count) {
            // The next effect won't be the last - transition the next
            // intermediate destination to RTV.
            assert_true(barrier_count < xe::countof(barriers));
            D3D12_RESOURCE_BARRIER& barrier_srv_to_rtv =
                barriers[barrier_count++];
            barrier_srv_to_rtv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
            barrier_srv_to_rtv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
            barrier_srv_to_rtv.Transition.pResource =
                paint_context_.guest_output_intermediate_textures[i + 1].Get();
            barrier_srv_to_rtv.Transition.Subresource =
                D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
            barrier_srv_to_rtv.Transition.StateBefore =
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
            barrier_srv_to_rtv.Transition.StateAfter =
                D3D12_RESOURCE_STATE_RENDER_TARGET;
          } else {
            // The next effect draws to the back buffer - merge into one
            // ResourceBarrier command.
            if (!back_buffer_acquired) {
              assert_true(barrier_count < xe::countof(barriers));
              D3D12_RESOURCE_BARRIER& barrier_present_to_rtv =
                  barriers[barrier_count++];
              barrier_present_to_rtv.Type =
                  D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
              barrier_present_to_rtv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
              barrier_present_to_rtv.Transition.pResource = back_buffer;
              barrier_present_to_rtv.Transition.Subresource =
                  D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
              barrier_present_to_rtv.Transition.StateBefore =
                  D3D12_RESOURCE_STATE_PRESENT;
              barrier_present_to_rtv.Transition.StateAfter =
                  D3D12_RESOURCE_STATE_RENDER_TARGET;
              back_buffer_acquired = true;
            }
          }
          if (barrier_count) {
            command_list->ResourceBarrier(barrier_count, barriers);
          }
        }
      }
    }
  }

  // Release main target guest output texture references that aren't needed
  // anymore (this is done after various potential guest-output-related main
  // target completion timeline waits so the completed submission value is the
  // most actual).
  uint64_t completed_paint_submission =
      paint_context_.paint_completion_timeline
          ->GetCompletedSubmissionFromLastUpdate();
  for (std::pair<uint64_t, Microsoft::WRL::ComPtr<ID3D12Resource>>&
           guest_output_resource_paint_ref :
       paint_context_.guest_output_resource_paint_refs) {
    if (!guest_output_resource_paint_ref.second ||
        guest_output_resource_paint_ref.second == guest_output_resource) {
      continue;
    }
    if (completed_paint_submission >= guest_output_resource_paint_ref.first) {
      guest_output_resource_paint_ref.second.Reset();
    }
  }

  // If no guest output has been drawn, the transitioned of the back buffer to
  // RTV hasn't been done yet, and it's needed to clear it, and optionally to
  // draw the UI.
  if (!back_buffer_acquired) {
    D3D12_RESOURCE_BARRIER barrier_present_to_rtv;
    barrier_present_to_rtv.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier_present_to_rtv.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    barrier_present_to_rtv.Transition.pResource = back_buffer;
    barrier_present_to_rtv.Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier_present_to_rtv.Transition.StateBefore =
        D3D12_RESOURCE_STATE_PRESENT;
    barrier_present_to_rtv.Transition.StateAfter =
        D3D12_RESOURCE_STATE_RENDER_TARGET;
    command_list->ResourceBarrier(1, &barrier_present_to_rtv);
    back_buffer_acquired = true;
  }

  if (back_buffer_clear_needed) {
    command_list->ClearRenderTargetView(back_buffer_rtv, kBackBufferClearColor,
                                        0, nullptr);
    back_buffer_clear_needed = false;
  }

  if (execute_ui_drawers) {
    // Draw the UI.
    if (!back_buffer_bound) {
      command_list->OMSetRenderTargets(1, &back_buffer_rtv, true, nullptr);
      back_buffer_bound = true;
    }
    D3D12UIDrawContext ui_draw_context(
        *this, paint_context_.swap_chain_width,
        paint_context_.swap_chain_height, command_list,
        ui_completion_timeline_->GetUpcomingSubmission(),
        ui_completion_timeline_->UpdateAndGetCompletedSubmission());
    ExecuteUIDrawersFromUIThread(ui_draw_context);
  }

  // End drawing to the back buffer.
  D3D12_RESOURCE_BARRIER barrier_rtv_to_present;
  barrier_rtv_to_present.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
  barrier_rtv_to_present.Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
  barrier_rtv_to_present.Transition.pResource = back_buffer;
  barrier_rtv_to_present.Transition.Subresource =
      D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  barrier_rtv_to_present.Transition.StateBefore =
      D3D12_RESOURCE_STATE_RENDER_TARGET;
  barrier_rtv_to_present.Transition.StateAfter = D3D12_RESOURCE_STATE_PRESENT;
  command_list->ResourceBarrier(1, &barrier_rtv_to_present);

  // Execute and present.
  // TODO(Triang3l): Error checking.
  command_list->Close();
  ID3D12CommandQueue* const direct_queue = provider_.GetDirectQueue();
  ID3D12CommandList* execute_command_list = command_list;
  direct_queue->ExecuteCommandLists(1, &execute_command_list);
  if (execute_ui_drawers) {
    ui_completion_timeline_->SignalAndAdvance(direct_queue);
  }
  paint_context_.paint_completion_timeline->SignalAndAdvance(direct_queue);
  // Present as soon as possible, without waiting for vsync (the host refresh
  // rate may be something like 144 Hz, which is not a multiple of the common
  // 30 Hz or 60 Hz guest refresh rate), and allowing dropping outdated queued
  // frames for lower latency. Also, if possible, allowing tearing to use
  // variable refresh rate in borderless fullscreen (note that if DXGI
  // fullscreen is ever used in, the allow tearing flag must not be passed in
  // fullscreen, but DXGI fullscreen is largely unneeded with the flip
  // presentation model used in Direct3D 12).
#if XE_PLATFORM_WINRT
  // CoreWindow swap chains don't reliably support DXGI_PRESENT_RESTART /
  // DXGI_PRESENT_ALLOW_TEARING; passing them makes Present fail silently (black
  // screen). Use a plain vsync present on UWP/Xbox.
  HRESULT present_result = paint_context_.swap_chain->Present(1, 0);
#else
  HRESULT present_result = paint_context_.swap_chain->Present(
      0, DXGI_PRESENT_RESTART | (paint_context_.swap_chain_allows_tearing
                                     ? DXGI_PRESENT_ALLOW_TEARING
                                     : 0));
#endif
  // Even if presentation has failed, work might have been enqueued anyway
  // internally before the failure according to Jesse Natalie from the DirectX
  // Discord server.
  paint_context_.present_completion_timeline->SignalAndAdvance(direct_queue);
  switch (present_result) {
    case DXGI_ERROR_DEVICE_REMOVED:
      return PaintResult::kGpuLostExternally;
    case DXGI_ERROR_DEVICE_RESET:
      return PaintResult::kGpuLostResponsible;
    default:
      return SUCCEEDED(present_result) ? PaintResult::kPresented
                                       : PaintResult::kNotPresented;
  }
}

bool D3D12Presenter::InitializeSurfaceIndependent() {
  // Check if DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING is supported.
  {
    Microsoft::WRL::ComPtr<IDXGIFactory5> dxgi_factory_5;
    if (SUCCEEDED(provider_.GetDXGIFactory()->QueryInterface(
            IID_PPV_ARGS(&dxgi_factory_5)))) {
      BOOL tearing_feature_data;
      dxgi_supports_tearing_ =
          SUCCEEDED(dxgi_factory_5->CheckFeatureSupport(
              DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tearing_feature_data,
              sizeof(tearing_feature_data))) &&
          tearing_feature_data;
    }
  }

  ID3D12Device* device = provider_.GetDevice();

  // Initialize static guest output painting objects.

  // Guest output painting root signatures.
  // One (texture) for bilinear, two (texture and constants) for AMD FidelityFX
  // CAS and FSR.
  D3D12_ROOT_PARAMETER guest_output_paint_root_parameters[UINT(
      GuestOutputPaintRootParameter::kCount)];
  // Source texture.
  D3D12_DESCRIPTOR_RANGE guest_output_paint_root_descriptor_range_source;
  guest_output_paint_root_descriptor_range_source.RangeType =
      D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  guest_output_paint_root_descriptor_range_source.NumDescriptors = 1;
  guest_output_paint_root_descriptor_range_source.BaseShaderRegister = 0;
  guest_output_paint_root_descriptor_range_source.RegisterSpace = 0;
  guest_output_paint_root_descriptor_range_source
      .OffsetInDescriptorsFromTableStart = 0;
  {
    D3D12_ROOT_PARAMETER& guest_output_paint_root_parameter_source =
        guest_output_paint_root_parameters[UINT(
            GuestOutputPaintRootParameter::kSource)];
    guest_output_paint_root_parameter_source.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    guest_output_paint_root_parameter_source.DescriptorTable
        .NumDescriptorRanges = 1;
    guest_output_paint_root_parameter_source.DescriptorTable.pDescriptorRanges =
        &guest_output_paint_root_descriptor_range_source;
    guest_output_paint_root_parameter_source.ShaderVisibility =
        D3D12_SHADER_VISIBILITY_PIXEL;
  }
  // Rectangle.
  {
    D3D12_ROOT_PARAMETER& guest_output_paint_root_parameter_rect =
        guest_output_paint_root_parameters[UINT(
            GuestOutputPaintRootParameter::kRectangle)];
    guest_output_paint_root_parameter_rect.ParameterType =
        D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
    guest_output_paint_root_parameter_rect.Constants.ShaderRegister = 0;
    guest_output_paint_root_parameter_rect.Constants.RegisterSpace = 0;
    guest_output_paint_root_parameter_rect.Constants.Num32BitValues =
        sizeof(GuestOutputPaintRectangleConstants) / sizeof(uint32_t);
    guest_output_paint_root_parameter_rect.ShaderVisibility =
        D3D12_SHADER_VISIBILITY_VERTEX;
  }
  // Pixel shader constants.
  D3D12_ROOT_PARAMETER& guest_output_paint_root_parameter_effect_constants =
      guest_output_paint_root_parameters[UINT(
          GuestOutputPaintRootParameter::kEffectConstants)];
  guest_output_paint_root_parameter_effect_constants.ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  guest_output_paint_root_parameter_effect_constants.Constants.ShaderRegister =
      0;
  guest_output_paint_root_parameter_effect_constants.Constants.RegisterSpace =
      0;
  guest_output_paint_root_parameter_effect_constants.ShaderVisibility =
      D3D12_SHADER_VISIBILITY_PIXEL;
  // Bilinear sampler.
  D3D12_STATIC_SAMPLER_DESC guest_output_paint_root_sampler;
  guest_output_paint_root_sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
  guest_output_paint_root_sampler.AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  guest_output_paint_root_sampler.AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  guest_output_paint_root_sampler.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  guest_output_paint_root_sampler.MipLODBias = 0.0f;
  guest_output_paint_root_sampler.MaxAnisotropy = 1;
  guest_output_paint_root_sampler.ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  guest_output_paint_root_sampler.BorderColor =
      D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  guest_output_paint_root_sampler.MinLOD = 0.0f;
  guest_output_paint_root_sampler.MaxLOD = 0.0f;
  guest_output_paint_root_sampler.ShaderRegister = 0;
  guest_output_paint_root_sampler.RegisterSpace = 0;
  guest_output_paint_root_sampler.ShaderVisibility =
      D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_ROOT_SIGNATURE_DESC guest_output_paint_root_signature_desc;
  guest_output_paint_root_signature_desc.NumParameters =
      UINT(GuestOutputPaintRootParameter::kCount);
  guest_output_paint_root_signature_desc.pParameters =
      guest_output_paint_root_parameters;
  guest_output_paint_root_signature_desc.NumStaticSamplers = 1;
  guest_output_paint_root_signature_desc.pStaticSamplers =
      &guest_output_paint_root_sampler;
  guest_output_paint_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  // Bilinear filtering (needs the sampler).
  guest_output_paint_root_parameter_effect_constants.Constants.Num32BitValues =
      sizeof(BilinearConstants) / sizeof(uint32_t);
  {
    ID3D12RootSignature* guest_output_paint_root_signature =
        util::CreateRootSignature(provider_,
                                  guest_output_paint_root_signature_desc);
    if (!guest_output_paint_root_signature) {
      XELOGE(
          "D3D12Presenter: Failed to create the guest output bilinear "
          "filtering presentation root signature");
      return false;
    }
    *(guest_output_paint_root_signatures_
          [kGuestOutputPaintRootSignatureIndexBilinear]
              .ReleaseAndGetAddressOf()) = guest_output_paint_root_signature;
  }
  // EASU (needs the sampler).
  guest_output_paint_root_parameter_effect_constants.Constants.Num32BitValues =
      sizeof(FsrEasuConstants) / sizeof(uint32_t);
  {
    ID3D12RootSignature* guest_output_paint_root_signature =
        util::CreateRootSignature(provider_,
                                  guest_output_paint_root_signature_desc);
    if (!guest_output_paint_root_signature) {
      XELOGE(
          "D3D12Presenter: Failed to create the guest output AMD FidelityFX "
          "FSR EASU presentation root signature");
      return false;
    }
    *(guest_output_paint_root_signatures_
          [kGuestOutputPaintRootSignatureIndexFsrEasu]
              .ReleaseAndGetAddressOf()) = guest_output_paint_root_signature;
  }
  // SGSR (needs the sampler - bilinear for the base color, and Gather ignores
  // the filter).
  guest_output_paint_root_parameter_effect_constants.Constants.Num32BitValues =
      sizeof(SgsrConstants) / sizeof(uint32_t);
  {
    ID3D12RootSignature* guest_output_paint_root_signature =
        util::CreateRootSignature(provider_,
                                  guest_output_paint_root_signature_desc);
    if (!guest_output_paint_root_signature) {
      XELOGE(
          "D3D12Presenter: Failed to create the guest output Snapdragon Game "
          "Super Resolution presentation root signature");
      return false;
    }
    *(guest_output_paint_root_signatures_
          [kGuestOutputPaintRootSignatureIndexSgsr]
              .ReleaseAndGetAddressOf()) = guest_output_paint_root_signature;
  }
  // RCAS and CAS don't need the sampler.
  guest_output_paint_root_signature_desc.NumStaticSamplers = 0;
  // RCAS.
  guest_output_paint_root_parameter_effect_constants.Constants.Num32BitValues =
      sizeof(FsrRcasConstants) / sizeof(uint32_t);
  {
    ID3D12RootSignature* guest_output_paint_root_signature =
        util::CreateRootSignature(provider_,
                                  guest_output_paint_root_signature_desc);
    if (!guest_output_paint_root_signature) {
      XELOGE(
          "D3D12Presenter: Failed to create the guest output AMD FidelityFX "
          "FSR RCAS presentation root signature");
      return false;
    }
    *(guest_output_paint_root_signatures_
          [kGuestOutputPaintRootSignatureIndexFsrRcas]
              .ReleaseAndGetAddressOf()) = guest_output_paint_root_signature;
  }
  // CAS, sharpening only.
  guest_output_paint_root_parameter_effect_constants.Constants.Num32BitValues =
      sizeof(CasSharpenConstants) / sizeof(uint32_t);
  {
    ID3D12RootSignature* guest_output_paint_root_signature =
        util::CreateRootSignature(provider_,
                                  guest_output_paint_root_signature_desc);
    if (!guest_output_paint_root_signature) {
      XELOGE(
          "D3D12Presenter: Failed to create the guest output AMD FidelityFX "
          "CAS presentation root signature");
      return false;
    }
    *(guest_output_paint_root_signatures_
          [kGuestOutputPaintRootSignatureIndexCasSharpen]
              .ReleaseAndGetAddressOf()) = guest_output_paint_root_signature;
  }
  // CAS, resampling.
  guest_output_paint_root_parameter_effect_constants.Constants.Num32BitValues =
      sizeof(CasResampleConstants) / sizeof(uint32_t);
  {
    ID3D12RootSignature* guest_output_paint_root_signature =
        util::CreateRootSignature(provider_,
                                  guest_output_paint_root_signature_desc);
    if (!guest_output_paint_root_signature) {
      XELOGE(
          "D3D12Presenter: Failed to create the guest output resampling AMD "
          "FidelityFX CAS presentation root signature");
      return false;
    }
    *(guest_output_paint_root_signatures_
          [kGuestOutputPaintRootSignatureIndexCasResample]
              .ReleaseAndGetAddressOf()) = guest_output_paint_root_signature;
  }

  // Guest output painting pipelines.
  D3D12_GRAPHICS_PIPELINE_STATE_DESC guest_output_paint_pipeline_desc = {};
  guest_output_paint_pipeline_desc.VS.pShaderBytecode =
      shaders::guest_output_triangle_strip_rect_vs;
  guest_output_paint_pipeline_desc.VS.BytecodeLength =
      sizeof(shaders::guest_output_triangle_strip_rect_vs);
  guest_output_paint_pipeline_desc.BlendState.RenderTarget[0]
      .RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
  guest_output_paint_pipeline_desc.SampleMask = UINT_MAX;
  guest_output_paint_pipeline_desc.RasterizerState.FillMode =
      D3D12_FILL_MODE_SOLID;
  guest_output_paint_pipeline_desc.RasterizerState.CullMode =
      D3D12_CULL_MODE_NONE;
  guest_output_paint_pipeline_desc.RasterizerState.DepthClipEnable = true;
  guest_output_paint_pipeline_desc.PrimitiveTopologyType =
      D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  guest_output_paint_pipeline_desc.NumRenderTargets = 1;
  guest_output_paint_pipeline_desc.SampleDesc.Count = 1;
  for (size_t i = 0; i < size_t(GuestOutputPaintEffect::kCount); ++i) {
    GuestOutputPaintEffect guest_output_paint_effect =
        GuestOutputPaintEffect(i);
    switch (guest_output_paint_effect) {
      case GuestOutputPaintEffect::kBilinear:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_bilinear_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_bilinear_ps);
        break;
      case GuestOutputPaintEffect::kBilinearDither:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_bilinear_dither_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_bilinear_dither_ps);
        break;
      case GuestOutputPaintEffect::kCasSharpen:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_ffx_cas_sharpen_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_ffx_cas_sharpen_ps);
        break;
      case GuestOutputPaintEffect::kCasSharpenDither:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_ffx_cas_sharpen_dither_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_ffx_cas_sharpen_dither_ps);
        break;
      case GuestOutputPaintEffect::kCasResample:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_ffx_cas_resample_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_ffx_cas_resample_ps);
        break;
      case GuestOutputPaintEffect::kCasResampleDither:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_ffx_cas_resample_dither_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_ffx_cas_resample_dither_ps);
        break;
      case GuestOutputPaintEffect::kFsrEasu:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_ffx_fsr_easu_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_ffx_fsr_easu_ps);
        break;
      case GuestOutputPaintEffect::kFsrRcas:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_ffx_fsr_rcas_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_ffx_fsr_rcas_ps);
        break;
      case GuestOutputPaintEffect::kFsrRcasDither:
        guest_output_paint_pipeline_desc.PS.pShaderBytecode =
            shaders::guest_output_ffx_fsr_rcas_dither_ps;
        guest_output_paint_pipeline_desc.PS.BytecodeLength =
            sizeof(shaders::guest_output_ffx_fsr_rcas_dither_ps);
        break;
      case GuestOutputPaintEffect::kSgsr:
        if (cvars::postprocess_sgsr_base == "catrom") {
          guest_output_paint_pipeline_desc.PS.pShaderBytecode =
              shaders::guest_output_sgsr_catrom_ps;
          guest_output_paint_pipeline_desc.PS.BytecodeLength =
              sizeof(shaders::guest_output_sgsr_catrom_ps);
        } else {
          guest_output_paint_pipeline_desc.PS.pShaderBytecode =
              shaders::guest_output_sgsr_ps;
          guest_output_paint_pipeline_desc.PS.BytecodeLength =
              sizeof(shaders::guest_output_sgsr_ps);
        }
        break;
      default:
        // Not supported by this implementation.
        continue;
    }
    guest_output_paint_pipeline_desc.pRootSignature =
        guest_output_paint_root_signatures_
            [GetGuestOutputPaintRootSignatureIndex(guest_output_paint_effect)]
                .Get();
    // The creation is guarded against driver shader compiler crashes (the
    // Xbox UWP compiler dies with an access violation on some valid shaders) -
    // a crashed compilation only makes the effect unavailable.
    DWORD pipeline_creation_exception = 0;
    if (CanGuestOutputPaintEffectBeIntermediate(guest_output_paint_effect)) {
      guest_output_paint_pipeline_desc.RTVFormats[0] =
          kGuestOutputIntermediateFormat;
      if (FAILED(util::CreateGraphicsPipelineStateGuarded(
              device, &guest_output_paint_pipeline_desc,
              IID_PPV_ARGS(&guest_output_paint_intermediate_pipelines_[i]),
              &pipeline_creation_exception))) {
        if (pipeline_creation_exception) {
          XELOGE(
              "D3D12Presenter: The driver's shader compiler CRASHED "
              "(exception 0x{:08X}) creating the guest output painting "
              "pipeline for effect {} (intermediate) - the effect will be "
              "unavailable",
              uint32_t(pipeline_creation_exception), i);
          guest_output_paint_intermediate_pipelines_[i].Reset();
          guest_output_paint_final_pipelines_[i].Reset();
          continue;
        }
        XELOGE(
            "D3D12Presenter: Failed to create the guest output painting "
            "pipeline for effect {} writing to an intermediate texture",
            i);
        return false;
      }
    }
    if (CanGuestOutputPaintEffectBeFinal(guest_output_paint_effect)) {
      guest_output_paint_pipeline_desc.RTVFormats[0] = kSwapChainFormat;
      if (FAILED(util::CreateGraphicsPipelineStateGuarded(
              device, &guest_output_paint_pipeline_desc,
              IID_PPV_ARGS(&guest_output_paint_final_pipelines_[i]),
              &pipeline_creation_exception))) {
        if (pipeline_creation_exception) {
          XELOGE(
              "D3D12Presenter: The driver's shader compiler CRASHED "
              "(exception 0x{:08X}) creating the guest output painting "
              "pipeline for effect {} (final) - the effect will be "
              "unavailable",
              uint32_t(pipeline_creation_exception), i);
          guest_output_paint_intermediate_pipelines_[i].Reset();
          guest_output_paint_final_pipelines_[i].Reset();
          continue;
        }
        XELOGE(
            "D3D12Presenter: Failed to create the guest output painting "
            "pipeline for effect {} writing to a swap chain buffer",
            i);
        return false;
      }
    }
  }

  // Initialize connection-independent parts of the painting context.

  ID3D12CommandQueue* direct_queue = provider_.GetDirectQueue();

  // Paint completion timelines.
  paint_context_.paint_completion_timeline =
      D3D12GPUCompletionTimeline::Create(device);
  paint_context_.present_completion_timeline =
      D3D12GPUCompletionTimeline::Create(device);
  if (!paint_context_.paint_completion_timeline ||
      !paint_context_.present_completion_timeline) {
    return false;
  }

  // Paint command allocators and command list.
  for (Microsoft::WRL::ComPtr<ID3D12CommandAllocator>&
           paint_command_allocator_ref : paint_context_.command_allocators) {
    if (FAILED(device->CreateCommandAllocator(
            D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&paint_command_allocator_ref)))) {
      XELOGE(
          "D3D12Presenter: Failed to create a command allocator for drawing to "
          "a swap chain");
      return false;
    }
  }
  if (FAILED(device->CreateCommandList(
          0, D3D12_COMMAND_LIST_TYPE_DIRECT,
          paint_context_.command_allocators[0].Get(), nullptr,
          IID_PPV_ARGS(&paint_context_.command_list)))) {
    XELOGE(
        "D3D12Presenter: Failed to create the command list for drawing to a "
        "swap chain");
    return false;
  }
  // Command lists are created in an open state.
  paint_context_.command_list->Close();

  // RTV descriptor heap.
  D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc;
  rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
  rtv_heap_desc.NumDescriptors = PaintContext::kRTVCount;
  rtv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
  rtv_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(
          &rtv_heap_desc, IID_PPV_ARGS(&paint_context_.rtv_heap)))) {
    XELOGE(
        "D3D12Presenter: Failed to create an RTV descriptor heap with {} "
        "descriptors",
        rtv_heap_desc.NumDescriptors);
    return false;
  }

  // CBV/SRV/UAV descriptor heap.
  D3D12_DESCRIPTOR_HEAP_DESC view_heap_desc;
  view_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
  view_heap_desc.NumDescriptors = PaintContext::kViewCount;
  view_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
  view_heap_desc.NodeMask = 0;
  if (FAILED(device->CreateDescriptorHeap(
          &view_heap_desc, IID_PPV_ARGS(&paint_context_.view_heap)))) {
    XELOGE(
        "D3D12Presenter: Failed to create a shader-visible CBV/SRV/UAV "
        "descriptor heap with {} descriptors",
        view_heap_desc.NumDescriptors);
    return false;
  }

  guest_output_resource_refresher_completion_timeline_ =
      D3D12GPUCompletionTimeline::Create(device);
  if (!guest_output_resource_refresher_completion_timeline_) {
    return false;
  }

  ui_completion_timeline_ = D3D12GPUCompletionTimeline::Create(device);
  if (!ui_completion_timeline_) {
    return false;
  }

  // SMAA is optional - a failure only disables it (logged inside).
  InitializeSmaaStaticObjects();

  return InitializeCommonSurfaceIndependent();
}

size_t D3D12Presenter::GetSmaaQualityFromCvar() {
  const std::string& value = cvars::postprocess_smaa;
  if (value == "low") {
    return kSmaaQualityLow;
  }
  if (value == "medium") {
    return kSmaaQualityMedium;
  }
  if (value == "high") {
    return kSmaaQualityHigh;
  }
  if (value == "ultra") {
    return kSmaaQualityUltra;
  }
  return SIZE_MAX;
}

bool D3D12Presenter::InitializeSmaaStaticObjects() {
  ID3D12Device* device = provider_.GetDevice();

  // Root signatures: b0 root constants (float4 RT metrics, both stages), SRV
  // table t0.. (pixel), static samplers s0 = linear clamp, s1 = point clamp.
  D3D12_ROOT_PARAMETER smaa_root_parameters[2];
  smaa_root_parameters[0].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS;
  smaa_root_parameters[0].Constants.ShaderRegister = 0;
  smaa_root_parameters[0].Constants.RegisterSpace = 0;
  smaa_root_parameters[0].Constants.Num32BitValues = 4;
  smaa_root_parameters[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_ALL;
  D3D12_DESCRIPTOR_RANGE smaa_srv_range;
  smaa_srv_range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
  smaa_srv_range.BaseShaderRegister = 0;
  smaa_srv_range.RegisterSpace = 0;
  smaa_srv_range.OffsetInDescriptorsFromTableStart = 0;
  smaa_root_parameters[1].ParameterType =
      D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
  smaa_root_parameters[1].DescriptorTable.NumDescriptorRanges = 1;
  smaa_root_parameters[1].DescriptorTable.pDescriptorRanges = &smaa_srv_range;
  smaa_root_parameters[1].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  D3D12_STATIC_SAMPLER_DESC smaa_samplers[2] = {};
  smaa_samplers[0].Filter = D3D12_FILTER_MIN_MAG_LINEAR_MIP_POINT;
  smaa_samplers[0].AddressU = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  smaa_samplers[0].AddressV = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  smaa_samplers[0].AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
  smaa_samplers[0].MaxAnisotropy = 1;
  smaa_samplers[0].ComparisonFunc = D3D12_COMPARISON_FUNC_NEVER;
  smaa_samplers[0].BorderColor = D3D12_STATIC_BORDER_COLOR_OPAQUE_BLACK;
  smaa_samplers[0].MinLOD = 0.0f;
  smaa_samplers[0].MaxLOD = 0.0f;
  smaa_samplers[0].ShaderRegister = 0;
  smaa_samplers[0].RegisterSpace = 0;
  smaa_samplers[0].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
  smaa_samplers[1] = smaa_samplers[0];
  smaa_samplers[1].Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
  smaa_samplers[1].ShaderRegister = 1;
  D3D12_ROOT_SIGNATURE_DESC smaa_root_signature_desc;
  smaa_root_signature_desc.NumParameters = UINT(xe::countof(smaa_root_parameters));
  smaa_root_signature_desc.pParameters = smaa_root_parameters;
  smaa_root_signature_desc.NumStaticSamplers = UINT(xe::countof(smaa_samplers));
  smaa_root_signature_desc.pStaticSamplers = smaa_samplers;
  smaa_root_signature_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_NONE;
  smaa_srv_range.NumDescriptors = 2;
  {
    ID3D12RootSignature* smaa_root_signature =
        util::CreateRootSignature(provider_, smaa_root_signature_desc);
    if (!smaa_root_signature) {
      XELOGE(
          "D3D12Presenter: Failed to create the SMAA 2-SRV root signature - "
          "SMAA will be unavailable");
      return false;
    }
    *(smaa_root_signature_2_srvs_.ReleaseAndGetAddressOf()) =
        smaa_root_signature;
  }
  smaa_srv_range.NumDescriptors = 3;
  {
    ID3D12RootSignature* smaa_root_signature =
        util::CreateRootSignature(provider_, smaa_root_signature_desc);
    if (!smaa_root_signature) {
      XELOGE(
          "D3D12Presenter: Failed to create the SMAA 3-SRV root signature - "
          "SMAA will be unavailable");
      smaa_root_signature_2_srvs_.Reset();
      return false;
    }
    *(smaa_root_signature_3_srvs_.ReleaseAndGetAddressOf()) =
        smaa_root_signature;
  }

  // Pipelines for each quality preset and pass.
  struct SmaaPassShaders {
    const void* vs;
    size_t vs_size;
    const void* ps;
    size_t ps_size;
    // Progressively simpler pixel shaders to retry with if the driver's
    // shader compiler crashes on the full one (seen on the Xbox UWP driver
    // with the high/ultra blend weight shaders). ps_flat is the same shader
    // fully predicated ([flatten] everywhere - no branches around the
    // unrolled searches, same quality); ps_nodiag drops the diagonal pattern
    // detection entirely (which the working low/medium presets lack anyway).
    const void* ps_flat = nullptr;
    size_t ps_flat_size = 0;
    const void* ps_nodiag = nullptr;
    size_t ps_nodiag_size = 0;
  };
  // [quality][pass].
  const SmaaPassShaders smaa_pass_shaders[kSmaaQualityCount][kSmaaPassCount] = {
      {{shaders::smaa_edge_luma_vs, sizeof(shaders::smaa_edge_luma_vs),
        shaders::smaa_edge_luma_low_ps, sizeof(shaders::smaa_edge_luma_low_ps)},
       {shaders::smaa_blend_weight_low_vs,
        sizeof(shaders::smaa_blend_weight_low_vs),
        shaders::smaa_blend_weight_low_ps,
        sizeof(shaders::smaa_blend_weight_low_ps)},
       {shaders::smaa_neighborhood_blend_vs,
        sizeof(shaders::smaa_neighborhood_blend_vs),
        shaders::smaa_neighborhood_blend_ps,
        sizeof(shaders::smaa_neighborhood_blend_ps)}},
      {{shaders::smaa_edge_luma_vs, sizeof(shaders::smaa_edge_luma_vs),
        shaders::smaa_edge_luma_medium_ps,
        sizeof(shaders::smaa_edge_luma_medium_ps)},
       {shaders::smaa_blend_weight_medium_vs,
        sizeof(shaders::smaa_blend_weight_medium_vs),
        shaders::smaa_blend_weight_medium_ps,
        sizeof(shaders::smaa_blend_weight_medium_ps)},
       {shaders::smaa_neighborhood_blend_vs,
        sizeof(shaders::smaa_neighborhood_blend_vs),
        shaders::smaa_neighborhood_blend_ps,
        sizeof(shaders::smaa_neighborhood_blend_ps)}},
      {{shaders::smaa_edge_luma_vs, sizeof(shaders::smaa_edge_luma_vs),
        shaders::smaa_edge_luma_high_ps,
        sizeof(shaders::smaa_edge_luma_high_ps)},
       {shaders::smaa_blend_weight_high_vs,
        sizeof(shaders::smaa_blend_weight_high_vs),
        shaders::smaa_blend_weight_high_ps,
        sizeof(shaders::smaa_blend_weight_high_ps),
        shaders::smaa_blend_weight_high_flat_ps,
        sizeof(shaders::smaa_blend_weight_high_flat_ps),
        shaders::smaa_blend_weight_high_nodiag_ps,
        sizeof(shaders::smaa_blend_weight_high_nodiag_ps)},
       {shaders::smaa_neighborhood_blend_vs,
        sizeof(shaders::smaa_neighborhood_blend_vs),
        shaders::smaa_neighborhood_blend_ps,
        sizeof(shaders::smaa_neighborhood_blend_ps)}},
      {{shaders::smaa_edge_luma_vs, sizeof(shaders::smaa_edge_luma_vs),
        shaders::smaa_edge_luma_ultra_ps,
        sizeof(shaders::smaa_edge_luma_ultra_ps)},
       {shaders::smaa_blend_weight_ultra_vs,
        sizeof(shaders::smaa_blend_weight_ultra_vs),
        shaders::smaa_blend_weight_ultra_ps,
        sizeof(shaders::smaa_blend_weight_ultra_ps),
        shaders::smaa_blend_weight_ultra_flat_ps,
        sizeof(shaders::smaa_blend_weight_ultra_flat_ps),
        shaders::smaa_blend_weight_ultra_nodiag_ps,
        sizeof(shaders::smaa_blend_weight_ultra_nodiag_ps)},
       {shaders::smaa_neighborhood_blend_vs,
        sizeof(shaders::smaa_neighborhood_blend_vs),
        shaders::smaa_neighborhood_blend_ps,
        sizeof(shaders::smaa_neighborhood_blend_ps)}},
  };
  const DXGI_FORMAT smaa_pass_formats[kSmaaPassCount] = {
      DXGI_FORMAT_R8G8_UNORM,      // Edges.
      DXGI_FORMAT_R8G8B8A8_UNORM,  // Blend weights.
      kGuestOutputFormat,          // Anti-aliased output.
  };

  D3D12_GRAPHICS_PIPELINE_STATE_DESC smaa_pipeline_desc = {};
  smaa_pipeline_desc.BlendState.RenderTarget[0].RenderTargetWriteMask =
      D3D12_COLOR_WRITE_ENABLE_ALL;
  smaa_pipeline_desc.SampleMask = UINT_MAX;
  smaa_pipeline_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
  smaa_pipeline_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
  smaa_pipeline_desc.RasterizerState.DepthClipEnable = true;
  smaa_pipeline_desc.PrimitiveTopologyType =
      D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
  smaa_pipeline_desc.NumRenderTargets = 1;
  smaa_pipeline_desc.SampleDesc.Count = 1;
  // A driver shader compiler crash on one quality preset must only make that
  // preset unavailable, with the runtime falling back to the closest preset
  // that did compile - not disable SMAA entirely (on the Xbox UWP driver the
  // biggest unrolled blend weight shaders may still be rejected while
  // low/medium compile fine).
  size_t smaa_qualities_available = 0;
  for (size_t quality = 0; quality < kSmaaQualityCount; ++quality) {
    bool quality_available = true;
    for (size_t pass = 0; pass < kSmaaPassCount; ++pass) {
      const SmaaPassShaders& pass_shaders = smaa_pass_shaders[quality][pass];
      smaa_pipeline_desc.pRootSignature =
          (pass == kSmaaPassWeights ? smaa_root_signature_3_srvs_
                                    : smaa_root_signature_2_srvs_)
              .Get();
      smaa_pipeline_desc.VS.pShaderBytecode = pass_shaders.vs;
      smaa_pipeline_desc.VS.BytecodeLength = pass_shaders.vs_size;
      smaa_pipeline_desc.RTVFormats[0] = smaa_pass_formats[pass];
      // Ladder of PS variants, most capable first. Guarded: the Xbox UWP
      // driver's shader compiler may crash on shaders that are valid
      // elsewhere - that must only degrade or disable SMAA, not kill the
      // emulator at startup. Each step is logged so a report shows exactly
      // which shader construct the driver rejects.
      const struct {
        const void* ps;
        size_t ps_size;
        const char* name;
      } ps_variants[] = {
          {pass_shaders.ps, pass_shaders.ps_size, "full"},
          {pass_shaders.ps_flat, pass_shaders.ps_flat_size,
           "branch-free (flattened)"},
          {pass_shaders.ps_nodiag, pass_shaders.ps_nodiag_size,
           "no diagonal detection"},
      };
      HRESULT smaa_pipeline_hr = E_FAIL;
      for (const auto& ps_variant : ps_variants) {
        if (!ps_variant.ps) {
          continue;
        }
#if XE_PLATFORM_WINRT
        // Established on the Xbox UWP driver (newbe_xs.dll): the "full"
        // variant with [branch] regions around the unrolled diagonal searches
        // ALWAYS crashes its shader compiler, and the branch-free variant is
        // the known-good replacement - go straight to it instead of
        // provoking a contained-but-real access violation in the driver on
        // every launch.
        if (ps_variant.ps == pass_shaders.ps && pass_shaders.ps_flat) {
          continue;
        }
#endif  // XE_PLATFORM_WINRT
        smaa_pipeline_desc.PS.pShaderBytecode = ps_variant.ps;
        smaa_pipeline_desc.PS.BytecodeLength = ps_variant.ps_size;
        DWORD smaa_pipeline_exception = 0;
        smaa_pipeline_hr = util::CreateGraphicsPipelineStateGuarded(
            device, &smaa_pipeline_desc,
            IID_PPV_ARGS(
                smaa_pipelines_[quality][pass].ReleaseAndGetAddressOf()),
            &smaa_pipeline_exception);
        if (SUCCEEDED(smaa_pipeline_hr)) {
          if (ps_variant.ps != pass_shaders.ps) {
            XELOGI(
                "D3D12Presenter: SMAA pipeline (quality {}, pass {}) created "
                "with the \"{}\" shader variant",
                quality, pass, ps_variant.name);
          }
          break;
        }
        if (smaa_pipeline_exception) {
          XELOGW(
              "D3D12Presenter: The driver's shader compiler CRASHED "
              "(exception 0x{:08X}) on the \"{}\" SMAA shader variant "
              "(quality {}, pass {})",
              uint32_t(smaa_pipeline_exception), ps_variant.name, quality,
              pass);
        } else {
          XELOGW(
              "D3D12Presenter: Failed to create the SMAA pipeline with the "
              "\"{}\" shader variant (quality {}, pass {})",
              ps_variant.name, quality, pass);
        }
      }
      if (FAILED(smaa_pipeline_hr)) {
        XELOGE(
            "D3D12Presenter: No SMAA shader variant works for quality {}, "
            "pass {} - this quality preset will be unavailable",
            quality, pass);
        quality_available = false;
        break;
      }
    }
    if (quality_available) {
      ++smaa_qualities_available;
    } else {
      for (auto& pipeline : smaa_pipelines_[quality]) {
        pipeline.Reset();
      }
    }
  }
  if (!smaa_qualities_available) {
    XELOGE(
        "D3D12Presenter: No SMAA quality preset could be created - SMAA will "
        "be unavailable");
    smaa_root_signature_2_srvs_.Reset();
    smaa_root_signature_3_srvs_.Reset();
    return false;
  }

  // Optional color (max per-channel delta) edge detection variants - a
  // superset of the luma edges, selected via
  // cvars::postprocess_smaa_edge_detection. A creation failure only keeps the
  // luma edge pass for that quality.
  {
    const struct {
      const void* ps;
      size_t ps_size;
    } smaa_edge_color_shaders[kSmaaQualityCount] = {
        {shaders::smaa_edge_color_low_ps,
         sizeof(shaders::smaa_edge_color_low_ps)},
        {shaders::smaa_edge_color_medium_ps,
         sizeof(shaders::smaa_edge_color_medium_ps)},
        {shaders::smaa_edge_color_high_ps,
         sizeof(shaders::smaa_edge_color_high_ps)},
        {shaders::smaa_edge_color_ultra_ps,
         sizeof(shaders::smaa_edge_color_ultra_ps)},
    };
    smaa_pipeline_desc.pRootSignature = smaa_root_signature_2_srvs_.Get();
    smaa_pipeline_desc.VS.pShaderBytecode = shaders::smaa_edge_luma_vs;
    smaa_pipeline_desc.VS.BytecodeLength = sizeof(shaders::smaa_edge_luma_vs);
    smaa_pipeline_desc.RTVFormats[0] = smaa_pass_formats[kSmaaPassEdges];
    for (size_t quality = 0; quality < kSmaaQualityCount; ++quality) {
      if (!smaa_pipelines_[quality][kSmaaPassEdges]) {
        // The whole quality preset is unavailable.
        continue;
      }
      smaa_pipeline_desc.PS.pShaderBytecode =
          smaa_edge_color_shaders[quality].ps;
      smaa_pipeline_desc.PS.BytecodeLength =
          smaa_edge_color_shaders[quality].ps_size;
      DWORD smaa_pipeline_exception = 0;
      if (FAILED(util::CreateGraphicsPipelineStateGuarded(
              device, &smaa_pipeline_desc,
              IID_PPV_ARGS(
                  smaa_edge_color_pipelines_[quality].ReleaseAndGetAddressOf()),
              &smaa_pipeline_exception))) {
        XELOGW(
            "D3D12Presenter: Failed to create the SMAA color edge detection "
            "pipeline (quality {}){} - luma edge detection will be used for "
            "this preset",
            quality,
            smaa_pipeline_exception ? " because the driver's shader compiler "
                                      "crashed"
                                    : "");
        smaa_edge_color_pipelines_[quality].Reset();
      }
    }
  }

  // The lookup textures (uploaded on first use, on the paint command list).
  D3D12_RESOURCE_DESC smaa_lut_desc = {};
  smaa_lut_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
  smaa_lut_desc.DepthOrArraySize = 1;
  smaa_lut_desc.MipLevels = 1;
  smaa_lut_desc.SampleDesc.Count = 1;
  smaa_lut_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
  smaa_lut_desc.Width = AREATEX_WIDTH;
  smaa_lut_desc.Height = AREATEX_HEIGHT;
  smaa_lut_desc.Format = DXGI_FORMAT_R8G8_UNORM;
  if (FAILED(device->CreateCommittedResource(
          &util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &smaa_lut_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(smaa_area_texture_.ReleaseAndGetAddressOf())))) {
    XELOGE(
        "D3D12Presenter: Failed to create the SMAA area lookup texture - SMAA "
        "will be unavailable");
    return false;
  }
  smaa_lut_desc.Width = SEARCHTEX_WIDTH;
  smaa_lut_desc.Height = SEARCHTEX_HEIGHT;
  smaa_lut_desc.Format = DXGI_FORMAT_R8_UNORM;
  if (FAILED(device->CreateCommittedResource(
          &util::kHeapPropertiesDefault, D3D12_HEAP_FLAG_NONE, &smaa_lut_desc,
          D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
          IID_PPV_ARGS(smaa_search_texture_.ReleaseAndGetAddressOf())))) {
    XELOGE(
        "D3D12Presenter: Failed to create the SMAA search lookup texture - "
        "SMAA will be unavailable");
    smaa_area_texture_.Reset();
    return false;
  }

  // The upload buffer with both lookup textures, already filled - rows placed
  // at D3D12_TEXTURE_DATA_PITCH_ALIGNMENT.
  constexpr uint32_t kAreaRowPitch =
      (AREATEX_PITCH + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
      ~uint32_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  constexpr uint32_t kAreaUploadSize = kAreaRowPitch * AREATEX_HEIGHT;
  constexpr uint32_t kSearchUploadOffset =
      (kAreaUploadSize + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
      ~uint32_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
  constexpr uint32_t kSearchRowPitch =
      (SEARCHTEX_PITCH + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
      ~uint32_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
  constexpr uint32_t kSmaaLutUploadBufferSize =
      kSearchUploadOffset + kSearchRowPitch * SEARCHTEX_HEIGHT;
  D3D12_RESOURCE_DESC smaa_lut_upload_buffer_desc;
  util::FillBufferResourceDesc(smaa_lut_upload_buffer_desc,
                               kSmaaLutUploadBufferSize,
                               D3D12_RESOURCE_FLAG_NONE);
  if (FAILED(device->CreateCommittedResource(
          &util::kHeapPropertiesUpload, D3D12_HEAP_FLAG_NONE,
          &smaa_lut_upload_buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ,
          nullptr,
          IID_PPV_ARGS(smaa_lut_upload_buffer_.ReleaseAndGetAddressOf())))) {
    XELOGE(
        "D3D12Presenter: Failed to create the SMAA lookup upload buffer - "
        "SMAA will be unavailable");
    smaa_area_texture_.Reset();
    smaa_search_texture_.Reset();
    return false;
  }
  {
    void* smaa_lut_upload_mapping;
    D3D12_RANGE smaa_lut_upload_read_range = {};
    if (FAILED(smaa_lut_upload_buffer_->Map(0, &smaa_lut_upload_read_range,
                                            &smaa_lut_upload_mapping))) {
      XELOGE(
          "D3D12Presenter: Failed to map the SMAA lookup upload buffer - SMAA "
          "will be unavailable");
      smaa_area_texture_.Reset();
      smaa_search_texture_.Reset();
      smaa_lut_upload_buffer_.Reset();
      return false;
    }
    uint8_t* upload_bytes = static_cast<uint8_t*>(smaa_lut_upload_mapping);
    for (uint32_t y = 0; y < AREATEX_HEIGHT; ++y) {
      std::memcpy(upload_bytes + size_t(kAreaRowPitch) * y,
                  smaa_textures::areaTexBytes + size_t(AREATEX_PITCH) * y,
                  AREATEX_PITCH);
    }
    for (uint32_t y = 0; y < SEARCHTEX_HEIGHT; ++y) {
      std::memcpy(
          upload_bytes + kSearchUploadOffset + size_t(kSearchRowPitch) * y,
          smaa_textures::searchTexBytes + size_t(SEARCHTEX_PITCH) * y,
          SEARCHTEX_PITCH);
    }
    smaa_lut_upload_buffer_->Unmap(0, nullptr);
  }
  smaa_luts_uploaded_ = false;

  return true;
}

bool D3D12Presenter::PaintSmaaPasses(ID3D12GraphicsCommandList* command_list,
                                     ID3D12Resource* guest_output_resource,
                                     uint32_t frontbuffer_width,
                                     uint32_t frontbuffer_height,
                                     size_t quality,
                                     uint64_t current_paint_submission) {
  if (!smaa_root_signature_2_srvs_ || !smaa_area_texture_) {
    return false;
  }
  // The requested preset may have been unavailable at initialization (driver
  // shader compiler crash) - substitute the closest one that did compile,
  // preferring lower (cheaper, known-simpler shaders).
  if (!smaa_pipelines_[quality][0]) {
    size_t substitute = SIZE_MAX;
    for (size_t below = quality; below != SIZE_MAX; --below) {
      if (smaa_pipelines_[below][0]) {
        substitute = below;
        break;
      }
    }
    if (substitute == SIZE_MAX) {
      for (size_t above = quality + 1; above < kSmaaQualityCount; ++above) {
        if (smaa_pipelines_[above][0]) {
          substitute = above;
          break;
        }
      }
    }
    if (substitute == SIZE_MAX) {
      return false;
    }
    static bool smaa_substitute_logged = false;
    if (!smaa_substitute_logged) {
      smaa_substitute_logged = true;
      XELOGW(
          "D3D12Presenter: SMAA quality preset {} is unavailable on this "
          "driver, using preset {} instead",
          quality, substitute);
    }
    quality = substitute;
  }
  ID3D12Device* device = provider_.GetDevice();

  // (Re)create the working textures for the current guest output size.
  {
    std::pair<uint32_t, uint32_t> smaa_current_size(0, 0);
    if (paint_context_.smaa_output_texture) {
      D3D12_RESOURCE_DESC smaa_output_desc =
          paint_context_.smaa_output_texture->GetDesc();
      smaa_current_size.first = uint32_t(smaa_output_desc.Width);
      smaa_current_size.second = smaa_output_desc.Height;
    }
    if (smaa_current_size !=
        std::make_pair(frontbuffer_width, frontbuffer_height)) {
      if (paint_context_.smaa_output_texture ||
          paint_context_.smaa_edges_texture) {
        paint_context_.paint_completion_timeline
            ->AwaitSubmissionAndUpdateCompleted(
                paint_context_.smaa_texture_last_usage);
        paint_context_.smaa_edges_texture.Reset();
        paint_context_.smaa_weights_texture.Reset();
        paint_context_.smaa_output_texture.Reset();
      }
      D3D12_RESOURCE_DESC smaa_texture_desc = {};
      smaa_texture_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
      smaa_texture_desc.Width = frontbuffer_width;
      smaa_texture_desc.Height = frontbuffer_height;
      smaa_texture_desc.DepthOrArraySize = 1;
      smaa_texture_desc.MipLevels = 1;
      smaa_texture_desc.SampleDesc.Count = 1;
      smaa_texture_desc.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
      smaa_texture_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
      const DXGI_FORMAT smaa_texture_formats[3] = {
          DXGI_FORMAT_R8G8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM,
          kGuestOutputFormat};
      Microsoft::WRL::ComPtr<ID3D12Resource>* const smaa_textures[3] = {
          &paint_context_.smaa_edges_texture,
          &paint_context_.smaa_weights_texture,
          &paint_context_.smaa_output_texture};
      const UINT smaa_rtv_indices[3] = {PaintContext::kRTVIndexSmaaEdges,
                                        PaintContext::kRTVIndexSmaaWeights,
                                        PaintContext::kRTVIndexSmaaOutput};
      const UINT smaa_srv_indices[3] = {PaintContext::kViewIndexSmaaEdges,
                                        PaintContext::kViewIndexSmaaWeights,
                                        PaintContext::kViewIndexSmaaOutput};
      for (size_t i = 0; i < 3; ++i) {
        smaa_texture_desc.Format = smaa_texture_formats[i];
        if (FAILED(device->CreateCommittedResource(
                &util::kHeapPropertiesDefault,
                provider_.GetHeapFlagCreateNotZeroed(), &smaa_texture_desc,
                D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE, nullptr,
                IID_PPV_ARGS(smaa_textures[i]->ReleaseAndGetAddressOf())))) {
          XELOGE(
              "D3D12Presenter: Failed to create a {}x{} SMAA working texture",
              frontbuffer_width, frontbuffer_height);
          paint_context_.smaa_edges_texture.Reset();
          paint_context_.smaa_weights_texture.Reset();
          paint_context_.smaa_output_texture.Reset();
          return false;
        }
        // RTV.
        D3D12_RENDER_TARGET_VIEW_DESC smaa_rtv_desc;
        smaa_rtv_desc.Format = smaa_texture_desc.Format;
        smaa_rtv_desc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        smaa_rtv_desc.Texture2D.MipSlice = 0;
        smaa_rtv_desc.Texture2D.PlaneSlice = 0;
        device->CreateRenderTargetView(
            smaa_textures[i]->Get(), &smaa_rtv_desc,
            provider_.OffsetRTVDescriptor(
                paint_context_.rtv_heap->GetCPUDescriptorHandleForHeapStart(),
                smaa_rtv_indices[i]));
        // SRV.
        D3D12_SHADER_RESOURCE_VIEW_DESC smaa_srv_desc;
        smaa_srv_desc.Format = smaa_texture_desc.Format;
        smaa_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
        smaa_srv_desc.Shader4ComponentMapping =
            D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        smaa_srv_desc.Texture2D.MostDetailedMip = 0;
        smaa_srv_desc.Texture2D.MipLevels = 1;
        smaa_srv_desc.Texture2D.PlaneSlice = 0;
        smaa_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
        device->CreateShaderResourceView(
            smaa_textures[i]->Get(), &smaa_srv_desc,
            provider_.OffsetViewDescriptor(
                paint_context_.view_heap->GetCPUDescriptorHandleForHeapStart(),
                smaa_srv_indices[i]));
      }
      // The LUT SRVs live in the same heap - (re)create them together with the
      // working texture views (also handles the very first use).
      D3D12_SHADER_RESOURCE_VIEW_DESC smaa_lut_srv_desc;
      smaa_lut_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
      smaa_lut_srv_desc.Shader4ComponentMapping =
          D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
      smaa_lut_srv_desc.Texture2D.MostDetailedMip = 0;
      smaa_lut_srv_desc.Texture2D.MipLevels = 1;
      smaa_lut_srv_desc.Texture2D.PlaneSlice = 0;
      smaa_lut_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
      smaa_lut_srv_desc.Format = DXGI_FORMAT_R8G8_UNORM;
      device->CreateShaderResourceView(
          smaa_area_texture_.Get(), &smaa_lut_srv_desc,
          provider_.OffsetViewDescriptor(
              paint_context_.view_heap->GetCPUDescriptorHandleForHeapStart(),
              PaintContext::kViewIndexSmaaArea));
      smaa_lut_srv_desc.Format = DXGI_FORMAT_R8_UNORM;
      device->CreateShaderResourceView(
          smaa_search_texture_.Get(), &smaa_lut_srv_desc,
          provider_.OffsetViewDescriptor(
              paint_context_.view_heap->GetCPUDescriptorHandleForHeapStart(),
              PaintContext::kViewIndexSmaaSearch));
      // The guest output color SRV must be recreated for the new... actually
      // for the resource identity - handled below.
      paint_context_.smaa_color_srv_resource = nullptr;
    }
  }

  // SRV of the current guest output texture at kViewIndexSmaaColor.
  if (paint_context_.smaa_color_srv_resource != guest_output_resource) {
    D3D12_SHADER_RESOURCE_VIEW_DESC smaa_color_srv_desc;
    smaa_color_srv_desc.Format = kGuestOutputFormat;
    smaa_color_srv_desc.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    smaa_color_srv_desc.Shader4ComponentMapping =
        D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    smaa_color_srv_desc.Texture2D.MostDetailedMip = 0;
    smaa_color_srv_desc.Texture2D.MipLevels = 1;
    smaa_color_srv_desc.Texture2D.PlaneSlice = 0;
    smaa_color_srv_desc.Texture2D.ResourceMinLODClamp = 0.0f;
    device->CreateShaderResourceView(
        guest_output_resource, &smaa_color_srv_desc,
        provider_.OffsetViewDescriptor(
            paint_context_.view_heap->GetCPUDescriptorHandleForHeapStart(),
            PaintContext::kViewIndexSmaaColor));
    paint_context_.smaa_color_srv_resource = guest_output_resource;
  }

  paint_context_.smaa_texture_last_usage = current_paint_submission;

  // Upload the lookup textures on first use.
  if (!smaa_luts_uploaded_) {
    constexpr uint32_t kAreaRowPitch =
        (AREATEX_PITCH + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
        ~uint32_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    constexpr uint32_t kAreaUploadSize = kAreaRowPitch * AREATEX_HEIGHT;
    constexpr uint32_t kSearchUploadOffset =
        (kAreaUploadSize + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
        ~uint32_t(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
    constexpr uint32_t kSearchRowPitch =
        (SEARCHTEX_PITCH + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) &
        ~uint32_t(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
    D3D12_TEXTURE_COPY_LOCATION copy_dest, copy_source;
    copy_dest.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    copy_dest.SubresourceIndex = 0;
    copy_source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    copy_source.pResource = smaa_lut_upload_buffer_.Get();
    copy_dest.pResource = smaa_area_texture_.Get();
    copy_source.PlacedFootprint.Offset = 0;
    copy_source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8G8_UNORM;
    copy_source.PlacedFootprint.Footprint.Width = AREATEX_WIDTH;
    copy_source.PlacedFootprint.Footprint.Height = AREATEX_HEIGHT;
    copy_source.PlacedFootprint.Footprint.Depth = 1;
    copy_source.PlacedFootprint.Footprint.RowPitch = kAreaRowPitch;
    command_list->CopyTextureRegion(&copy_dest, 0, 0, 0, &copy_source,
                                    nullptr);
    copy_dest.pResource = smaa_search_texture_.Get();
    copy_source.PlacedFootprint.Offset = kSearchUploadOffset;
    copy_source.PlacedFootprint.Footprint.Format = DXGI_FORMAT_R8_UNORM;
    copy_source.PlacedFootprint.Footprint.Width = SEARCHTEX_WIDTH;
    copy_source.PlacedFootprint.Footprint.Height = SEARCHTEX_HEIGHT;
    copy_source.PlacedFootprint.Footprint.RowPitch = kSearchRowPitch;
    command_list->CopyTextureRegion(&copy_dest, 0, 0, 0, &copy_source,
                                    nullptr);
    D3D12_RESOURCE_BARRIER lut_barriers[2];
    for (size_t i = 0; i < 2; ++i) {
      lut_barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
      lut_barriers[i].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
      lut_barriers[i].Transition.Subresource =
          D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
      lut_barriers[i].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
      lut_barriers[i].Transition.StateAfter =
          D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
    }
    lut_barriers[0].Transition.pResource = smaa_area_texture_.Get();
    lut_barriers[1].Transition.pResource = smaa_search_texture_.Get();
    command_list->ResourceBarrier(2, lut_barriers);
    smaa_luts_uploaded_ = true;
  }

  // Common state for the passes.
  D3D12_VIEWPORT smaa_viewport;
  smaa_viewport.TopLeftX = 0.0f;
  smaa_viewport.TopLeftY = 0.0f;
  smaa_viewport.Width = float(frontbuffer_width);
  smaa_viewport.Height = float(frontbuffer_height);
  smaa_viewport.MinDepth = 0.0f;
  smaa_viewport.MaxDepth = 1.0f;
  command_list->RSSetViewports(1, &smaa_viewport);
  D3D12_RECT smaa_scissor;
  smaa_scissor.left = 0;
  smaa_scissor.top = 0;
  smaa_scissor.right = LONG(frontbuffer_width);
  smaa_scissor.bottom = LONG(frontbuffer_height);
  command_list->RSSetScissorRects(1, &smaa_scissor);
  command_list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
  // (1/w, 1/h, w, h) - SMAA_RT_METRICS.
  float smaa_rt_metrics[4] = {
      1.0f / float(frontbuffer_width), 1.0f / float(frontbuffer_height),
      float(frontbuffer_width), float(frontbuffer_height)};
  D3D12_CPU_DESCRIPTOR_HANDLE rtv_heap_start =
      paint_context_.rtv_heap->GetCPUDescriptorHandleForHeapStart();
  D3D12_GPU_DESCRIPTOR_HANDLE view_heap_gpu_start =
      paint_context_.view_heap->GetGPUDescriptorHandleForHeapStart();
  const float kSmaaClearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};

  D3D12_RESOURCE_BARRIER smaa_barriers[2];
  for (size_t i = 0; i < 2; ++i) {
    smaa_barriers[i].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    smaa_barriers[i].Flags = D3D12_RESOURCE_BARRIER_FLAG_NONE;
    smaa_barriers[i].Transition.Subresource =
        D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
  }

  // Pass 1: edge detection (edges -> RT, draw, then edges -> SRV together
  // with weights -> RT).
  smaa_barriers[0].Transition.pResource =
      paint_context_.smaa_edges_texture.Get();
  smaa_barriers[0].Transition.StateBefore =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  smaa_barriers[0].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  command_list->ResourceBarrier(1, smaa_barriers);
  {
    D3D12_CPU_DESCRIPTOR_HANDLE edges_rtv = provider_.OffsetRTVDescriptor(
        rtv_heap_start, PaintContext::kRTVIndexSmaaEdges);
    command_list->OMSetRenderTargets(1, &edges_rtv, true, nullptr);
    command_list->ClearRenderTargetView(edges_rtv, kSmaaClearColor, 0,
                                        nullptr);
    command_list->SetGraphicsRootSignature(smaa_root_signature_2_srvs_.Get());
    command_list->SetGraphicsRoot32BitConstants(0, 4, smaa_rt_metrics, 0);
    command_list->SetGraphicsRootDescriptorTable(
        1, provider_.OffsetViewDescriptor(view_heap_gpu_start,
                                          PaintContext::kViewIndexSmaaColor));
    // Color (max per-channel delta) edge detection is a superset of luma -
    // "both" is accepted as an alias since it would be identical output.
    ID3D12PipelineState* smaa_edge_pipeline =
        smaa_pipelines_[quality][kSmaaPassEdges].Get();
    if ((cvars::postprocess_smaa_edge_detection == "color" ||
         cvars::postprocess_smaa_edge_detection == "both") &&
        smaa_edge_color_pipelines_[quality]) {
      smaa_edge_pipeline = smaa_edge_color_pipelines_[quality].Get();
    }
    command_list->SetPipelineState(smaa_edge_pipeline);
    command_list->DrawInstanced(3, 1, 0, 0);
  }

  // Pass 2: blend weight calculation.
  smaa_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  smaa_barriers[0].Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  smaa_barriers[1].Transition.pResource =
      paint_context_.smaa_weights_texture.Get();
  smaa_barriers[1].Transition.StateBefore =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  smaa_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  command_list->ResourceBarrier(2, smaa_barriers);
  {
    D3D12_CPU_DESCRIPTOR_HANDLE weights_rtv = provider_.OffsetRTVDescriptor(
        rtv_heap_start, PaintContext::kRTVIndexSmaaWeights);
    command_list->OMSetRenderTargets(1, &weights_rtv, true, nullptr);
    command_list->ClearRenderTargetView(weights_rtv, kSmaaClearColor, 0,
                                        nullptr);
    command_list->SetGraphicsRootSignature(smaa_root_signature_3_srvs_.Get());
    command_list->SetGraphicsRoot32BitConstants(0, 4, smaa_rt_metrics, 0);
    command_list->SetGraphicsRootDescriptorTable(
        1, provider_.OffsetViewDescriptor(view_heap_gpu_start,
                                          PaintContext::kViewIndexSmaaEdges));
    command_list->SetPipelineState(
        smaa_pipelines_[quality][kSmaaPassWeights].Get());
    command_list->DrawInstanced(3, 1, 0, 0);
  }

  // Pass 3: neighborhood blending into the SMAA output.
  smaa_barriers[0].Transition.pResource =
      paint_context_.smaa_weights_texture.Get();
  smaa_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  smaa_barriers[0].Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  smaa_barriers[1].Transition.pResource =
      paint_context_.smaa_output_texture.Get();
  smaa_barriers[1].Transition.StateBefore =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  smaa_barriers[1].Transition.StateAfter = D3D12_RESOURCE_STATE_RENDER_TARGET;
  command_list->ResourceBarrier(2, smaa_barriers);
  {
    D3D12_CPU_DESCRIPTOR_HANDLE output_rtv = provider_.OffsetRTVDescriptor(
        rtv_heap_start, PaintContext::kRTVIndexSmaaOutput);
    command_list->OMSetRenderTargets(1, &output_rtv, true, nullptr);
    command_list->DiscardResource(paint_context_.smaa_output_texture.Get(),
                                  nullptr);
    command_list->SetGraphicsRootSignature(smaa_root_signature_2_srvs_.Get());
    command_list->SetGraphicsRoot32BitConstants(0, 4, smaa_rt_metrics, 0);
    command_list->SetGraphicsRootDescriptorTable(
        1, provider_.OffsetViewDescriptor(view_heap_gpu_start,
                                          PaintContext::kViewIndexSmaaColor));
    command_list->SetPipelineState(
        smaa_pipelines_[quality][kSmaaPassBlend].Get());
    command_list->DrawInstanced(3, 1, 0, 0);
  }

  // Back to the steady state for use as the source of the scaling chain.
  smaa_barriers[0].Transition.pResource =
      paint_context_.smaa_output_texture.Get();
  smaa_barriers[0].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
  smaa_barriers[0].Transition.StateAfter =
      D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
  command_list->ResourceBarrier(1, smaa_barriers);

  // One-time activity marker for diagnosing the configuration.
  static bool smaa_logged = false;
  if (!smaa_logged) {
    smaa_logged = true;
    XELOGI(
        "D3D12Presenter: SMAA active (quality preset {}, {} edge detection), "
        "{}x{}",
        quality,
        (cvars::postprocess_smaa_edge_detection == "color" ||
         cvars::postprocess_smaa_edge_detection == "both") &&
                smaa_edge_color_pipelines_[quality]
            ? "color"
            : "luma",
        frontbuffer_width, frontbuffer_height);
  }

  return true;
}

}  // namespace d3d12
}  // namespace ui
}  // namespace xe
