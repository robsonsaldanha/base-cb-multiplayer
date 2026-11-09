cmake_minimum_required(VERSION 3.22)
project(CbMp LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(CBMP_WITH_UE4SS "Compilar UE4SS e o mod cb-mp.dll" ON)

if(CBMP_WITH_UE4SS)
    add_subdirectory(deps/RE-UE4SS)
endif()

add_subdirectory(core)

if(CBMP_WITH_UE4SS)
    add_subdirectory(src)
else()
    enable_testing()
    add_subdirectory(tests)
endif()
