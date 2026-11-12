# Build do mod cb-mp (UE4SS + núcleo do cliente + portas Win32 + casca do mod). Projeto próprio, separado do build
# nativo da raiz: configurado por tools/build.ps1 -Target mod com o vcpkg em modo manifesto (client/vcpkg.json,
# triplet x64-windows-static-md, pacotes em C:/cbmp-vi-mod). Sem testes aqui: o núcleo e as portas são testados no
# build da raiz.
cmake_minimum_required(VERSION 3.25)
project(CbMp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/deps/RE-UE4SS/deps/first/Unreal/include)
    message(FATAL_ERROR "UE4SS não encontrado em client/deps/RE-UE4SS: rode tools/fetch-ue4ss.ps1")
endif()

add_subdirectory(deps/RE-UE4SS)

# C também neste diretório: o alvo estático do GNS pede c_std_99 (INTERFACE_COMPILE_FEATURES) e as features de C
# precisam estar conhecidas aqui (o UE4SS habilita C só no escopo dele). Depois do UE4SS, para não mudar a configuração
# dele.
enable_language(C)

# Os pacotes do vcpkg exportam Release/Debug; nos nossos alvos (criados daqui para baixo) a configuração do UE4SS
# Game__Shipping__Win64 usa o Release deles. Depois do UE4SS: os alvos importados dele (patternsleuth, via corrosion) têm
# a própria Game__Shipping__Win64 e não podem ser remapeados; o segundo item cobre um desses que chegue até nós.
set(CMAKE_MAP_IMPORTED_CONFIG_GAME__SHIPPING__WIN64 Release Game__Shipping__Win64)

find_package(GameNetworkingSockets CONFIG REQUIRED)
# Mesma escolha de alvo do build da raiz: triplets dinâmicos exportam GameNetworkingSockets::GameNetworkingSockets,
# estáticos (x64-windows-static-md, aqui) exportam GameNetworkingSockets::GameNetworkingSockets_s.
if(TARGET GameNetworkingSockets::GameNetworkingSockets)
    set(CBMP_GNS_TARGET GameNetworkingSockets::GameNetworkingSockets)
elseif(TARGET GameNetworkingSockets::GameNetworkingSockets_s)
    set(CBMP_GNS_TARGET GameNetworkingSockets::GameNetworkingSockets_s)
else()
    message(FATAL_ERROR "GameNetworkingSockets: nenhum target conhecido exportado pelo pacote")
endif()

# Daqui para baixo (só os nossos alvos; o UE4SS já foi configurado acima com as opções dele).
add_compile_options(/utf-8)

# Codec do protocolo (o proto/CMakeLists.txt não entra: ele pede libsodium e Catch2).
add_library(cbmp_codec STATIC ../proto/src/bitstream.cpp ../proto/src/messages.cpp)
target_include_directories(cbmp_codec PUBLIC ../proto/include)
target_compile_features(cbmp_codec PUBLIC cxx_std_23)

add_subdirectory(core)
add_subdirectory(src/platform)
add_subdirectory(src)
