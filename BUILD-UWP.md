# Building & deploying Xenia Canary UWP on Xbox Series (Dev Mode)

Target: **Xbox Series S/X in Developer Mode** (not the Microsoft Store).

## 1. Build

Upstream is CMake-based. Generate Visual Studio projects targeting the UWP
(WindowsStore) toolchain, then build the `xenia-app` target:

```
cd D:\XENIA\main\xenia-canary-uwp-next
cmake -S . -B build-uwp -G "Visual Studio 18 2026" -A x64 -DCMAKE_SYSTEM_NAME=WindowsStore -DCMAKE_SYSTEM_VERSION=10.0 -DCMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION=10.0.26100.0
cmake --build build-uwp --config Release --target xenia-app -- /m /verbosity:detailed
```

> - Generator must match your installed VS (`cmake --help` lists them): VS 2026 =
>   "Visual Studio 18 2026" (toolset v180). UWP C++ AppContainer toolset required
>   (`MSBuild\Microsoft\VC\v180\Microsoft.Cpp.AppContainerApplication.props`).
> - For the **WindowsStore** generator, `CMAKE_SYSTEM_VERSION` is the OS *family*
>   `10.0` (NOT a full SDK version - CMake rejects `10.0.26100.0` there). Pin the
>   actual SDK with `CMAKE_VS_WINDOWS_TARGET_PLATFORM_VERSION`.
> - Build the `xenia-app` target specifically (not ALL_BUILD) to avoid the host
>   tools / trace viewers / tests, which don't build for the App Container.

Setting `CMAKE_SYSTEM_NAME=WindowsStore` makes the MSVC toolset define
`WINAPI_FAMILY=WINAPI_FAMILY_APP`, which auto-defines `XE_PLATFORM_WINRT`
(see `base/platform.h`). The CMake here then:
- compiles the `*_uwp.cc` backends and skips the Win32 menu/window/file-picker
  bodies (they are `#if !XE_PLATFORM_WINRT`-guarded),
- excludes Vulkan, SDL, discord-rpc, winkey,
- links `d3d12 dxgi xaudio2 xinputuap WindowsApp`,
- produces an AppContainer `.appx`/`.msix` with `package/Package.appxmanifest`.

> The old patch-the-vcxproj path (`tools/uwp/patch_vcxproj_winrt.ps1`) is only a
> fallback if you generate desktop projects instead of using the WindowsStore
> toolchain above.

### Signing (Dev Mode)
Dev Mode does not require a Store certificate. Use a self-signed test cert whose
publisher matches `Identity Publisher="CN=XeniaCanary"` in the manifest:
```
New-SelfSignedCertificate -Type Custom -Subject "CN=XeniaCanary" \
  -KeyUsage DigitalSignature -CertStoreLocation "Cert:\CurrentUser\My" \
  -TextExtension @("2.5.29.37={text}1.3.6.1.5.5.7.3.3","2.5.29.19={text}")
```
Export to `.pfx` and point the packaging project's `PackageCertificateKeyFile` at it.

## 2. Run as a GAME, not an App  (CRITICAL for memory)

Per Microsoft (Chuck Walbourn, DirectX team), **Game vs App is NOT an
appxmanifest setting** — the deprecated `expandedResources` capability must not
be used. On a **Dev Mode** console you set it at deploy time:

1. Deploy the package to the console (VS "Remote Machine" deploy, or upload the
   `.appx`/`.msix` via the **Xbox Device Portal**).
2. In **Xbox Dev Home**, open the deployed title, press the controller menu, and
   set its type to **"Game"** (not "App").

| Mode | Memory (foreground) | GPU | CPU |
|---|---|---|---|
| App | ~1 GB | up to 45% | 2-4 shared cores |
| **Game** | **~5 GB** | **full** | **4 exclusive + 2 shared** |

An Xbox 360 emulator needs the Game profile (guest RAM + JIT + GPU caches), so
this step is mandatory. (Inside the VS debugger the limits don't apply, which can
mask the difference — always verify a deployed, non-debug run.)

### Why `codeGeneration` is in the manifest
Xenia JIT-compiles guest PPC into executable host memory. In the App Container,
`VirtualAllocFromApp` with `PAGE_EXECUTE_*` is only allowed with the
`codeGeneration` capability. Without it, **no game runs** (the JIT can't allocate
executable pages). See `base/memory_win.cc` (already upstream-aware of the
`*FromApp` paths).

## 3. File access (game library)

Model: **FilePicker + FutureAccessList** (no `broadFileSystemAccess`).
- `File > Open...` opens the CoreWindow `FileOpenPicker`; the picked file is added
  to the FutureAccessList, after which `filesystem_win.cc`'s `*FromApp` APIs can
  read it by path.
- The manifest's `fileTypeAssociation` + `removableStorage` also let the user
  launch a game (`.xex/.iso/.zar/.con/.pirs/.live`) from the file browser / USB.

## 4. UI on Xbox

CoreWindow has no native menu, so Xenia's main menu is drawn with ImGui
(`menu_item_uwp.cc`, invoked from `imgui_drawer.cc`) and is **gamepad-navigable**
(ImGui `NavEnableGamepad` is already on). It reuses the full existing menu: game
open, install content, CPU/GPU/Display config, profile, and the patch/plugin
flows. Text fields raise the system on-screen keyboard (`InputPane`).

Known follow-ups: the menu bar is currently always-on (a gamepad-button toggle so
it hides during gameplay is a nice next step); a dedicated 10-foot game-grid
launcher could replace the menu later.
