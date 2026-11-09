find_package(unofficial-sodium CONFIG REQUIRED)

add_executable(cb-bots src/main.cpp src/bot.cpp)
target_link_libraries(cb-bots PRIVATE cbmp_proto unofficial-sodium::sodium
  GameNetworkingSockets::GameNetworkingSockets)
if(WIN32)
  target_link_libraries(cb-bots PRIVATE winmm)
endif()
