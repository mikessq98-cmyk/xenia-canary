add_library(xenia-gpu-null STATIC)
xe_platform_sources(xenia-gpu-null ${CMAKE_CURRENT_SOURCE_DIR})
# The two branches were the wrong way round: the platforms WITHOUT Vulkan were
# the ones linking xenia-ui-vulkan. Because that target does not exist there,
# CMake took it for a plain library name and propagated "xenia-ui-vulkan.lib" -
# PUBLIC, so out through xenia-app - and the executable could not be linked at
# all (LNK1181). The null backend itself needs nothing from Vulkan; only the
# desktop build has the target to link.
if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  target_include_directories(xenia-gpu-null PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
  )
  target_link_libraries(xenia-gpu-null PUBLIC xenia-base xenia-gpu xenia-ui xenia-ui-vulkan xxhash)
else()
  target_link_libraries(xenia-gpu-null PUBLIC xenia-base xenia-gpu xenia-ui xxhash)
endif()
xe_target_defaults(xenia-gpu-null)
