# cbmp-headless: núcleo real do cliente (Session + GnsTransport) com um jogo roteirizado; usado pelo tools/client-e2e.py.
add_executable(cbmp-headless main.cpp ScriptedGame.cpp)
target_link_libraries(cbmp-headless PRIVATE cbmp_client_core)
if(WIN32)
  target_link_libraries(cbmp-headless PRIVATE winmm)
endif()
