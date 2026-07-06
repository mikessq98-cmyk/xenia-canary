/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// UWP / Xbox file picker. WinRT pickers are async and UI-thread-affine, while
// xe::ui::FilePicker::Show() is synchronous. On UWP we can neither block the UI
// thread (it would deadlock the STA the picker needs) nor pump a nested
// ProcessEvents (illegal under the app's ProcessUntilQuit loop -> 0x8000FFFF
// "Nested calls to ProcessEvents method is not allowed"). So the real flow uses
// ShowFileOpenPickerAsyncUWP(), which starts the async picker and delivers the
// result via a callback on the UI thread; the synchronous FilePicker::Show() is
// a no-op on UWP.
//
// Picked items are added to the FutureAccessList so that, afterwards, the
// *FromApp file APIs in filesystem_win.cc can open files under them by path.

#include "xenia/ui/file_picker.h"
#include "xenia/ui/file_picker_uwp.h"

#if XE_PLATFORM_WINRT

#include <algorithm>
#include <future>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.Foundation.Collections.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.Storage.AccessCache.h>
#include <winrt/Windows.Storage.Pickers.h>
#include <winrt/Windows.Storage.h>
#include <winrt/Windows.UI.Core.h>

#include "xenia/base/logging.h"
#include "xenia/ui/window.h"
#include "xenia/ui/windowed_app_context.h"

namespace xe {
namespace ui {

namespace {

namespace wf = winrt::Windows::Foundation;
namespace ws = winrt::Windows::Storage;
namespace wsa = winrt::Windows::Storage::AccessCache;
namespace wsp = winrt::Windows::Storage::Pickers;

// FileOpenPicker requires at least one FileTypeFilter entry and forbids mixing
// "*" with specific extensions. Since Xenia's lists include "*.*", the wildcard
// wins (full browse); the guest file type is detected by content anyway.
void ApplyFileTypeFilter(
    const wsp::FileOpenPicker& picker,
    const std::vector<std::pair<std::string, std::string>>& extensions) {
  bool wildcard = false;
  std::vector<std::wstring> exts;
  for (const auto& entry : extensions) {
    const std::string& filter = entry.second;
    size_t start = 0;
    while (start <= filter.size()) {
      size_t semi = filter.find(';', start);
      std::string token = filter.substr(
          start, semi == std::string::npos ? std::string::npos : semi - start);
      if (token == "*.*" || token == "*") {
        wildcard = true;
      } else {
        size_t dot = token.find('.');
        if (dot != std::string::npos) {
          std::string ext = token.substr(dot);  // ".iso"
          exts.emplace_back(ext.begin(), ext.end());
        }
      }
      if (semi == std::string::npos) {
        break;
      }
      start = semi + 1;
    }
  }
  if (wildcard || exts.empty()) {
    picker.FileTypeFilter().Append(L"*");
  } else {
    for (const std::wstring& ext : exts) {
      picker.FileTypeFilter().Append(winrt::hstring(ext));
    }
  }
}

// Persist access to the item so it is reachable by path later, and return it.
std::filesystem::path GrantAndPath(const ws::IStorageItem& item) {
  if (item) {
    try {
      wsa::StorageApplicationPermissions::FutureAccessList().Add(item);
    } catch (...) {
    }
    return std::filesystem::path(std::wstring(item.Path().c_str()));
  }
  return {};
}

// The synchronous picker interface works on UWP only when called from a
// non-UI thread (e.g. a guest thread requesting a disc swap): the async picker
// is dispatched to the UI thread and the calling thread blocks on the result.
// Calling it from the UI thread itself would deadlock (and pumping a nested
// ProcessEvents is illegal), so that case stays a no-op - the UI-side flows
// use ShowFileOpenPickerAsyncUWP instead.
class UWPFilePicker final : public FilePicker {
 public:
  bool Show(Window* /*parent_window*/) override {
    if (type() != Type::kFile) {
      XELOGW("UWPFilePicker::Show: only file pickers are supported");
      return false;
    }
    namespace wac = winrt::Windows::ApplicationModel::Core;
    namespace wuc = winrt::Windows::UI::Core;
    wuc::CoreDispatcher dispatcher{nullptr};
    try {
      dispatcher = wac::CoreApplication::MainView().CoreWindow().Dispatcher();
    } catch (...) {
      return false;
    }
    if (dispatcher.HasThreadAccess()) {
      XELOGW(
          "FilePicker::Show() cannot run synchronously on the UI thread on "
          "UWP; use ShowFileOpenPickerAsyncUWP");
      return false;
    }

    auto promise =
        std::make_shared<std::promise<std::vector<std::filesystem::path>>>();
    auto future = promise->get_future();
    auto exts = extensions();
    bool multi = multi_selection();

    dispatcher.RunAsync(
        wuc::CoreDispatcherPriority::Normal, [promise, exts, multi]() {
          try {
            wsp::FileOpenPicker picker;
            picker.SuggestedStartLocation(
                wsp::PickerLocationId::ComputerFolder);
            picker.ViewMode(wsp::PickerViewMode::List);
            ApplyFileTypeFilter(picker, exts);
            if (multi) {
              picker.PickMultipleFilesAsync().Completed(
                  [promise](const auto& op, wf::AsyncStatus status) {
                    std::vector<std::filesystem::path> results;
                    if (status == wf::AsyncStatus::Completed) {
                      auto files = op.GetResults();
                      if (files) {
                        for (const ws::StorageFile& file : files) {
                          auto p = GrantAndPath(file);
                          if (!p.empty()) {
                            results.push_back(std::move(p));
                          }
                        }
                      }
                    }
                    promise->set_value(std::move(results));
                  });
            } else {
              picker.PickSingleFileAsync().Completed(
                  [promise](const auto& op, wf::AsyncStatus status) {
                    std::vector<std::filesystem::path> results;
                    if (status == wf::AsyncStatus::Completed) {
                      ws::StorageFile file = op.GetResults();
                      auto p = GrantAndPath(file);
                      if (!p.empty()) {
                        results.push_back(std::move(p));
                      }
                    }
                    promise->set_value(std::move(results));
                  });
            }
          } catch (const winrt::hresult_error& e) {
            XELOGE("UWPFilePicker::Show failed: 0x{:08X} {}",
                   uint32_t(e.code().value), winrt::to_string(e.message()));
            promise->set_value({});
          } catch (...) {
            promise->set_value({});
          }
        });

    // Block the calling (guest/worker) thread until the user picks a file.
    std::vector<std::filesystem::path> results = future.get();
    if (results.empty()) {
      return false;
    }
    set_selected_files(std::move(results));
    return true;
  }
};

}  // namespace

void ShowFileOpenPickerAsyncUWP(
    Window* window,
    std::vector<std::pair<std::string, std::string>> extensions,
    bool multi_selection,
    std::function<void(std::vector<std::filesystem::path>)> on_picked) {
  // Deliver the result on the UI thread (the emulator interaction happens there;
  // the async Completed handler may run on a background/threadpool thread).
  auto deliver = [window, on_picked = std::move(on_picked)](
                     std::vector<std::filesystem::path> results) {
    window->app_context().CallInUIThread(
        [on_picked, results = std::move(results)]() mutable {
          on_picked(std::move(results));
        });
  };

  try {
    wsp::FileOpenPicker picker;
    picker.SuggestedStartLocation(wsp::PickerLocationId::ComputerFolder);
    picker.ViewMode(wsp::PickerViewMode::List);
    ApplyFileTypeFilter(picker, extensions);

    if (multi_selection) {
      // Capture `picker` to keep it alive until the async completes.
      picker.PickMultipleFilesAsync().Completed(
          [deliver, picker](const auto& op, wf::AsyncStatus status) {
            std::vector<std::filesystem::path> results;
            if (status == wf::AsyncStatus::Completed) {
              auto files = op.GetResults();
              if (files) {
                for (const ws::StorageFile& file : files) {
                  auto p = GrantAndPath(file);
                  if (!p.empty()) {
                    results.push_back(std::move(p));
                  }
                }
              }
            }
            deliver(std::move(results));
          });
    } else {
      picker.PickSingleFileAsync().Completed(
          [deliver, picker](const auto& op, wf::AsyncStatus status) {
            std::vector<std::filesystem::path> results;
            if (status == wf::AsyncStatus::Completed) {
              ws::StorageFile file = op.GetResults();
              auto p = GrantAndPath(file);
              if (!p.empty()) {
                results.push_back(std::move(p));
              }
            }
            deliver(std::move(results));
          });
    }
  } catch (const winrt::hresult_error& e) {
    XELOGE("ShowFileOpenPickerAsyncUWP failed: 0x{:08X} {}",
           uint32_t(e.code().value), winrt::to_string(e.message()));
    deliver({});
  } catch (...) {
    deliver({});
  }
}

std::unique_ptr<FilePicker> FilePicker::Create() {
  return std::make_unique<UWPFilePicker>();
}

}  // namespace ui
}  // namespace xe

#endif  // XE_PLATFORM_WINRT
