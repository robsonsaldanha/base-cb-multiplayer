add_library(cbmp_core STATIC
    src/Transform.cpp
    src/Log.cpp
    src/Store.cpp
    src/SpikeController.cpp
)
target_include_directories(cbmp_core PUBLIC include)
target_compile_features(cbmp_core PUBLIC cxx_std_23)
if(MSVC)
    target_compile_options(cbmp_core PRIVATE /W4 /utf-8)
endif()
