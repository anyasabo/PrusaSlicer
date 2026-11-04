add_library(slic3r-test-utils STATIC
    include/Slic3r/TestUtils/HwConfigUtils.hpp
    src/Slic3r/TestUtils/HwConfigUtils.cpp
)

target_include_directories(slic3r-test-utils PUBLIC include)
target_include_directories(slic3r-test-utils PRIVATE src)

target_link_libraries(slic3r-test-utils PUBLIC slic3r-domain)

# Replay driver for fuzz harnesses in non-libFuzzer builds (see cmake/modules/Fuzzing.cmake).
if (NOT SLIC3R_FUZZ)
    add_library(slic3r-fuzz-replay STATIC fuzz/FuzzReplayMain.cpp)
endif ()
