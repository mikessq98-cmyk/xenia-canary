/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

#ifndef XENIA_GPU_D3D12_D3D12_SHADER_H_
#define XENIA_GPU_D3D12_D3D12_SHADER_H_

#include <atomic>

#include "xenia/gpu/dxbc_shader.h"
#include "xenia/ui/d3d12/d3d12_provider.h"

namespace xe {
namespace gpu {
namespace d3d12 {

class D3D12Shader : public DxbcShader {
 public:
  class D3D12Translation : public DxbcTranslation {
   public:
    D3D12Translation(D3D12Shader& shader, uint64_t modification)
        : DxbcTranslation(shader, modification) {}

    void DisassembleDxbcAndDxil(const ui::d3d12::D3D12Provider& provider,
                                bool disassemble_dxbc,
                                IDxbcConverter* dxbc_converter = nullptr,
                                IDxcUtils* dxc_utils = nullptr,
                                IDxcCompiler* dxc_compiler = nullptr);

    // For background thread translation: atomically claim the right to
    // translate. Returns true if caller should translate, false if another
    // thread claimed it.
    bool TryClaimTranslation() {
      return !translation_claimed_.test_and_set(std::memory_order_acq_rel);
    }

    // Memory-pressure path: frees the translated DXBC (retranslated from the
    // ucode on demand later) and reopens the translation claim. The caller
    // must guarantee no pipeline creation is concurrently reading the binary.
    void ReleaseTranslationForMemoryPressure() {
      ReleaseTranslatedBinary();
      translation_claimed_.clear(std::memory_order_release);
    }

    // Number of queued or in-flight pipeline creations referencing this
    // translation's binary. Taken when a pipeline is pushed to the creation
    // queue and dropped when its creation attempt ends - both under the
    // pipeline cache's creation_request_lock_, so a holder of that lock sees a
    // stable value and can free every translation that is not referenced.
    // Without this the memory-pressure release had to bail out whenever any
    // creation was in flight, i.e. exactly during a compilation storm, which
    // is when the memory is actually needed.
    void AcquireForCreation() {
      creation_refs_.fetch_add(1, std::memory_order_relaxed);
    }
    void ReleaseFromCreation() {
      creation_refs_.fetch_sub(1, std::memory_order_relaxed);
    }
    bool is_referenced_by_creation() const {
      return creation_refs_.load(std::memory_order_relaxed) != 0;
    }

   private:
    std::atomic_flag translation_claimed_ = ATOMIC_FLAG_INIT;
    std::atomic<uint32_t> creation_refs_{0};
  };

  D3D12Shader(xenos::ShaderType shader_type, uint64_t ucode_data_hash,
              const uint32_t* ucode_dwords, size_t ucode_dword_count,
              std::endian ucode_source_endian = std::endian::big);

  // For owning subsystem like the pipeline cache, accessors for unique
  // identifiers (used instead of hashes to make sure collisions can't happen)
  // of binding layouts used by the shader, for invalidation if a shader with an
  // incompatible layout was bound.
  size_t GetTextureBindingLayoutUserUID() const {
    return texture_binding_layout_user_uid_;
  }
  size_t GetSamplerBindingLayoutUserUID() const {
    return sampler_binding_layout_user_uid_;
  }
  // Modifications of the same shader can be translated on different threads.
  // The "set" function must only be called if "enter" returned true - these are
  // set up only once.
  bool EnterBindingLayoutUserUIDSetup() {
    return !binding_layout_user_uids_set_up_.test_and_set();
  }
  void SetTextureBindingLayoutUserUID(size_t uid) {
    texture_binding_layout_user_uid_ = uid;
  }
  void SetSamplerBindingLayoutUserUID(size_t uid) {
    sampler_binding_layout_user_uid_ = uid;
  }

 protected:
  Translation* CreateTranslationInstance(uint64_t modification) override;

 private:
  std::atomic_flag binding_layout_user_uids_set_up_ = ATOMIC_FLAG_INIT;
  size_t texture_binding_layout_user_uid_ = 0;
  size_t sampler_binding_layout_user_uid_ = 0;
};

}  // namespace d3d12
}  // namespace gpu
}  // namespace xe

#endif  // XENIA_GPU_D3D12_D3D12_SHADER_H_
