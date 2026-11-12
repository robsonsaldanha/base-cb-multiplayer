# Portas Win32 do cliente (sem UE4SS): WinConnectFs (resgate do connect.json) e WinProcessGate (época + dono).
# Serve ao build nativo da raiz (com testes, client/tests/platform) e ao build do mod (client/CMakeLists.txt).
add_library(cbmp_client_platform STATIC
    WinConnectFs.cpp
    WinProcessGate.cpp
)
target_include_directories(cbmp_client_platform PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_features(cbmp_client_platform PUBLIC cxx_std_23)
target_compile_definitions(cbmp_client_platform PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX UNICODE _UNICODE)
target_link_libraries(cbmp_client_platform PUBLIC cbmp_client_core PRIVATE advapi32)
