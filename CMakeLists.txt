cmake_minimum_required(VERSION 3.25)
project(cbmp_native LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
option(CBMP_BUILD_FUZZ "Alvo de fuzz (clang/libFuzzer)" OFF)

if(MSVC)
  add_compile_options(/W4 /permissive- /utf-8 /EHsc)
else()
  add_compile_options(-Wall -Wextra -Wpedantic)
endif()

enable_testing()
find_package(Catch2 3 CONFIG REQUIRED)
include(Catch)
# Aqui (e não em server/ e bots/): o config do GNS cria ALIAS globais e não pode ser carregado duas vezes.
find_package(GameNetworkingSockets CONFIG REQUIRED)
# Triplets dinâmicos (x64-windows) exportam GameNetworkingSockets::GameNetworkingSockets;
# estáticos (x64-linux) exportam GameNetworkingSockets::GameNetworkingSockets_s.
if(TARGET GameNetworkingSockets::GameNetworkingSockets)
  set(CBMP_GNS_TARGET GameNetworkingSockets::GameNetworkingSockets)
elseif(TARGET GameNetworkingSockets::GameNetworkingSockets_s)
  set(CBMP_GNS_TARGET GameNetworkingSockets::GameNetworkingSockets_s)
else()
  message(FATAL_ERROR "GameNetworkingSockets: nenhum target conhecido exportado pelo pacote")
endif()

find_package(Lua REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(sol2 CONFIG REQUIRED)
find_package(unofficial-libmariadb CONFIG REQUIRED)
set(CBMP_LUA_TARGET ${LUA_LIBRARIES})
set(CBMP_MARIADB_TARGET unofficial::libmariadb)
# Plugins de autenticação da libmariadb (caching_sha2_password do MySQL 8, client_ed25519, sha256_password...): são
# módulos dinâmicos carregados de <pasta do executável>/plugins/libmariadb (MYSQL_PLUGIN_DIR em db_mysql.cpp).
# Copiados depois de cada build dos executáveis que usam o banco; a distribuição precisa levar a pasta junto.
set(CBMP_MARIADB_PLUGIN_ROOT "${VCPKG_INSTALLED_DIR}/${VCPKG_TARGET_TRIPLET}")
function(cbmp_copy_mariadb_plugins tgt)
  if(EXISTS "${CBMP_MARIADB_PLUGIN_ROOT}/plugins/libmariadb")
    add_custom_command(TARGET ${tgt} POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CBMP_MARIADB_PLUGIN_ROOT}/$<$<CONFIG:Debug>:debug/>plugins/libmariadb"
        "$<TARGET_FILE_DIR:${tgt}>/plugins/libmariadb"
      VERBATIM)
  else()
    message(WARNING "plugins da libmariadb não encontrados em ${CBMP_MARIADB_PLUGIN_ROOT}/plugins/libmariadb")
  endif()
endfunction()

add_subdirectory(proto)
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/server/script/CMakeLists.txt)
  add_subdirectory(server/script)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/server/CMakeLists.txt)
  add_subdirectory(server)
endif()
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bots/CMakeLists.txt)
  add_subdirectory(bots)
endif()
