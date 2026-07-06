add_library(xenia-gpu-null STATIC)
xe_platform_sources(xenia-gpu-null ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  target_link_libraries(xenia-gpu-null PUBLIC xenia-base xenia-gpu xenia-ui xxhash)
else()
  target_include_directories(xenia-gpu-null PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/Vulkan-Headers/include
  )
  target_link_libraries(xenia-gpu-null PUBLIC xenia-base xenia-gpu xenia-ui xenia-ui-vulkan xxhash)
endif()
xe_target_defaults(xenia-gpu-null)
