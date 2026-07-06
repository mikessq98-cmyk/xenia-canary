add_library(xenia-app-discord STATIC
  ${CMAKE_CURRENT_SOURCE_DIR}/discord_presence.cc
  ${CMAKE_CURRENT_SOURCE_DIR}/discord_presence.h
)
# Under UWP discord_presence.cc is a no-op stub (Discord RPC's named-pipe IPC is
# unavailable in the App Container), so it does not depend on discord-rpc.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "WindowsStore")
  target_include_directories(xenia-app-discord PRIVATE
    ${PROJECT_SOURCE_DIR}/third_party/discord-rpc/src
  )
  target_link_libraries(xenia-app-discord PUBLIC discord-rpc)
endif()
xe_target_defaults(xenia-app-discord)
