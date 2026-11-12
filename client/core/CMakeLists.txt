# Núcleo do cliente: C++ puro (sem UE4SS nem cabeçalhos Win32; test_guard.cpp confere). Entra no build nativo da raiz, que
# define as opções de aviso e ${CBMP_GNS_TARGET} (o find_package do GameNetworkingSockets só pode rodar uma vez, lá).
find_package(nlohmann_json CONFIG REQUIRED)

add_library(cbmp_client_core STATIC
    src/Transform.cpp
    src/Log.cpp
    src/Store.cpp
    src/ModConfig.cpp
    src/ConnectFile.cpp
    src/ClaimPolicy.cpp
    src/NetClient.cpp
    src/SnapshotMirror.cpp
    src/RemoteStore.cpp
    src/WorldTracker.cpp
    src/LocalSampler.cpp
    src/SendScheduler.cpp
    src/Interp.cpp
    src/PuppetDirector.cpp
    src/Session.cpp
    src/GnsTransport.cpp
    src/GameGlue.cpp
)
target_include_directories(cbmp_client_core PUBLIC include)
target_compile_features(cbmp_client_core PUBLIC cxx_std_23)
target_link_libraries(cbmp_client_core PUBLIC cbmp_codec nlohmann_json::nlohmann_json ${CBMP_GNS_TARGET})
