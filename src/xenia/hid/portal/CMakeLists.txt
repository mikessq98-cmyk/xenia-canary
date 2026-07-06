add_library(xenia-hid-portal STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/portal.h
  ${CMAKE_CURRENT_SOURCE_DIR}/portal.cc
)

# Hardware (USB) Skylander portal needs libusb, unavailable in the UWP App
# Container; the emulated portal (portal.cc) still works there.
if(WIN32 AND NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  target_sources(xenia-hid-portal PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware_portal.h
    ${CMAKE_CURRENT_SOURCE_DIR}/hardware_portal.cc
  )
  target_link_libraries(xenia-hid-portal PUBLIC libusb)
endif()
xe_target_defaults(xenia-hid-portal)
