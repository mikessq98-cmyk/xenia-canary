/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2026 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 */

// UWP / Xbox entry point. This is the CoreApplication / IFrameworkView parallel
// of windowed_app_main_win.cc's wWinMain. It deliberately reuses the same
// cross-platform plumbing (GetWindowedAppCreator, InitializeWin32App,
// WindowedApp::OnInitialize) so the only UWP-specific part is the view provider
// and the CoreDispatcher run loop.
//
// References:
//   - IFrameworkView / IFrameworkViewSource lifecycle:
//     https://learn.microsoft.com/uwp/api/windows.applicationmodel.core.iframeworkview
//   - CoreApplication.Run:
//     https://learn.microsoft.com/uwp/api/windows.applicationmodel.core.coreapplication.run

#include "xenia/base/platform.h"

#if XE_PLATFORM_WINRT

#include <memory>

#include "xenia/base/platform_win.h"

#include <winrt/Windows.ApplicationModel.Activation.h>
#include <winrt/Windows.ApplicationModel.Core.h>
#include <winrt/Windows.ApplicationModel.h>
#include <winrt/Windows.Foundation.h>
#include <winrt/Windows.UI.Core.h>

#include "xenia/base/logging.h"
#include "xenia/base/main_win.h"
#include "xenia/ui/windowed_app.h"
#include "xenia/ui/windowed_app_context_uwp.h"

namespace {

namespace wac = winrt::Windows::ApplicationModel::Core;
namespace wam = winrt::Windows::ApplicationModel;
namespace waa = winrt::Windows::ApplicationModel::Activation;
namespace wf = winrt::Windows::Foundation;
namespace wuc = winrt::Windows::UI::Core;

// The single app view provider. There is one CoreWindow / one UI thread; all of
// Initialize/SetWindow/Load/Run/Uninitialize execute on it, in that order.
struct XeniaFrameworkView
    : winrt::implements<XeniaFrameworkView, wac::IFrameworkViewSource,
                        wac::IFrameworkView> {
  wac::IFrameworkView CreateView() { return *this; }

  void Initialize(const wac::CoreApplicationView& application_view) {
    application_view.Activated({this, &XeniaFrameworkView::OnActivated});
    // Suspend/resume must be handled or the OS terminates the app on Xbox.
    // Real emulator pause/resume is a seam left for the GPU/CPU lifecycle work;
    // here we only keep the app alive across the transition.
    wac::CoreApplication::Suspending({this, &XeniaFrameworkView::OnSuspending});
    wac::CoreApplication::Resuming({this, &XeniaFrameworkView::OnResuming});
  }

  void SetWindow(const wuc::CoreWindow& /*window*/) {
    // On Xbox the B button raises a system back request that suspends the app
    // if left unhandled; mark it handled. (Documented Xbox UWP behavior.) Must
    // run on a thread that has a view, i.e. here, not in wWinMain.
    try {
      auto navigation = wuc::SystemNavigationManager::GetForCurrentView();
      navigation.BackRequested(
          [](const wf::IInspectable&,
             const wuc::BackRequestedEventArgs& args) { args.Handled(true); });
    } catch (...) {
    }
  }

  void Load(const winrt::hstring& /*entry_point*/) {}

  void Run() {
    // Constructed here so it records THIS (the UI) thread as the UI thread and
    // captures its dispatcher.
    xe::ui::UWPWindowedAppContext app_context;

    std::unique_ptr<xe::ui::WindowedApp> app =
        xe::ui::GetWindowedAppCreator()(app_context);

    // Initializes cvars/config (no real command line on UWP - this just loads
    // the config file and applies defaults). Must run before InitializeWin32App.
    xe::ParseWin32LaunchArguments(false, app->GetPositionalOptionsUsage(),
                                  app->GetPositionalOptions(), nullptr);

    xe::InitializeWin32App(app->GetName());

    bool initialized = false;
    try {
      initialized = app->OnInitialize();
    } catch (const winrt::hresult_error& e) {
      XELOGE("UWP OnInitialize hresult 0x{:08X}: {}", uint32_t(e.code().value),
             winrt::to_string(e.message()));
    } catch (const std::exception& e) {
      XELOGE("UWP OnInitialize exception: {}", e.what());
    }

    if (initialized) {
      // Activate the CoreWindow explicitly here, before pumping. Relying solely
      // on the CoreApplicationView::Activated event is racy on Xbox: if it
      // doesn't fire (or fires too early), the window is never shown, so the
      // CoreWindow swap chain presents to a window that isn't visible -> black
      // screen even though rendering and Present() both succeed. This is the
      // documented IFrameworkView pattern (Activate() in Run() before the
      // dispatch loop). Idempotent with OnActivated.
      try {
        wuc::CoreWindow::GetForCurrentThread().Activate();
      } catch (...) {
      }

      // Blocks, processing input and our dispatched pending-function/paint
      // pumps, until CoreApplication::Exit() is called from
      // UWPWindowedAppContext::PlatformQuitFromUIThread.
      wuc::CoreWindow::GetForCurrentThread().Dispatcher().ProcessEvents(
          wuc::CoreProcessEventsOption::ProcessUntilQuit);
    }

    app->InvokeOnDestroy();
    xe::ShutdownWin32App();
  }

  void Uninitialize() {}

  void OnActivated(const wac::CoreApplicationView& /*application_view*/,
                   const waa::IActivatedEventArgs& /*args*/) {
    // Game-launch-from-activation (protocol/file args) is a seam for later; for
    // now just bring the window up so the emulator UI is interactive.
    wuc::CoreWindow::GetForCurrentThread().Activate();
  }

  void OnSuspending(const wf::IInspectable& /*sender*/,
                    const wam::SuspendingEventArgs& args) {
    // Take and immediately complete a deferral. Persisting emulator state on
    // suspend is intentionally not done here (seam for lifecycle work).
    auto deferral = args.SuspendingOperation().GetDeferral();
    deferral.Complete();
  }

  void OnResuming(const wf::IInspectable& /*sender*/,
                  const wf::IInspectable& /*args*/) {}
};

}  // namespace

int __stdcall wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
  winrt::init_apartment();
  wac::CoreApplication::Run(winrt::make<XeniaFrameworkView>());
  winrt::uninit_apartment();
  return 0;
}

#endif  // XE_PLATFORM_WINRT
