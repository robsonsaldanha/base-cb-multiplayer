# cb-mp.dll: a casca do mod no UE4SS (o pacote o renomeia para main.dll). Só aqui e em game/ há UE4SS/Unreal.
file(STRINGS ${CMAKE_CURRENT_SOURCE_DIR}/../VERSION CBMP_MOD_VERSION LIMIT_COUNT 1)
string(STRIP "${CBMP_MOD_VERSION}" CBMP_MOD_VERSION)
if(NOT CBMP_MOD_VERSION MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
    message(FATAL_ERROR "client/VERSION inválido: '${CBMP_MOD_VERSION}'")
endif()

set(TARGET cb-mp)
add_library(${TARGET} SHARED
    dllmain.cpp
    CbMpMod.cpp
    FileHash.cpp
    CrashDump.cpp
    game/Ue4ssGame.cpp
    game/Reflect.cpp
)
target_include_directories(${TARGET} PRIVATE .)
target_link_libraries(${TARGET} PUBLIC UE4SS)
target_link_libraries(${TARGET} PRIVATE cbmp_client_core cbmp_client_platform bcrypt dbghelp)
# /W4 só nos nossos fontes; cabeçalhos entre <> (UE4SS, vcpkg, núcleo) ficam fora dos avisos.
target_compile_options(${TARGET} PRIVATE /utf-8 /EHa /W4 /external:anglebrackets /external:W0)
target_compile_definitions(${TARGET} PRIVATE CBMP_MOD_VERSION="${CBMP_MOD_VERSION}" WIN32_LEAN_AND_MEAN NOMINMAX)
