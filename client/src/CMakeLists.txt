set(TARGET cb-mp)
add_library(${TARGET} SHARED
    dllmain.cpp
    CbMpMod.cpp
    FileHash.cpp
    CrashDump.cpp
    game/Ue4ssGame.cpp
)
target_include_directories(${TARGET} PRIVATE .)
target_link_libraries(${TARGET} PUBLIC UE4SS)
target_link_libraries(${TARGET} PRIVATE cbmp_core bcrypt dbghelp)
target_compile_options(${TARGET} PRIVATE /utf-8 /EHa)
