// Build fingerprint for the UWP port. Upstream generates this via
// xenia-build.py; the CMake/WindowsStore build doesn't run that, so this static
// copy is provided on the include path (src/ is added by xe_target_defaults).
#ifndef GENERATED_VERSION_H_
#define GENERATED_VERSION_H_
#define XE_BUILD_BRANCH "canary_experimental_uwp"
#define XE_BUILD_COMMIT "8b948d503f5f7c96f95b6c7016bb2ce165b1f9f6"
#define XE_BUILD_COMMIT_SHORT "8b948d503"
#define XE_BUILD_DATE __DATE__
#endif  // GENERATED_VERSION_H_
