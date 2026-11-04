# Fuzz testing support.
#
# Two build flavours share the same harness sources (each defines LLVMFuzzerTestOneInput):
#
#   SLIC3R_FUZZ=ON  (Clang only): every project target gets -fsanitize=fuzzer-no-link coverage
#                   instrumentation and each harness links the libFuzzer engine. Combine with
#                   SLIC3R_ASAN / SLIC3R_UBSAN. If $LIB_FUZZING_ENGINE is set (OSS-Fuzz /
#                   ClusterFuzzLite convention) it is linked instead of -fsanitize=fuzzer.
#
#   SLIC3R_FUZZ=OFF (any compiler, default): with SLIC3R_BUILD_TESTS each harness is linked
#                   against a small replay main and registered with ctest, so the checked-in
#                   corpus (seeds + minimised past crashers) runs as a regression test in the
#                   normal test suite.
#
# Usage (from a module's CMakeLists.txt):
#   slic3r_add_fuzzer(<name>
#       SOURCES fuzz/<Name>Fuzzer.cpp
#       LIBS    <targets to link>
#       [CORPUS fuzz/corpus/<name>]       # directory of seed inputs, replayed by ctest
#       [DICT   fuzz/<name>.dict])        # libFuzzer dictionary, copied next to the binary

# Shared UBSan suppressions for the corpus replays. Resolved from this file's location, not
# PROJECT_SOURCE_DIR, which points at the calling module when a module declares its own project().
get_filename_component(SLIC3R_FUZZ_UBSAN_SUPPRESSIONS
    "${CMAKE_CURRENT_LIST_DIR}/../../src/slic3r-shared/fuzz/ubsan.supp" ABSOLUTE)

option(SLIC3R_FUZZ "Build libFuzzer harnesses (Clang only; pair with SLIC3R_ASAN/UBSAN)" OFF)

if (SLIC3R_FUZZ)
    if (NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
        message(FATAL_ERROR "SLIC3R_FUZZ requires Clang (libFuzzer). On macOS AppleClang ships no libFuzzer runtime; use Homebrew LLVM.")
    endif ()
    # Coverage instrumentation for all project code. Dependencies in deps/ are not instrumented,
    # which is fine: we fuzz PrusaSlicer's own parsers, not zlib/expat/etc.
    add_compile_options(-fsanitize=fuzzer-no-link)
    # Lets harnesses / code under test skip slow or non-deterministic paths when fuzzing.
    add_compile_definitions(FUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION)
    # Bounds-checked std::vector/string/span operator[] etc. ASan alone misses out-of-range reads
    # that stay inside an allocation (std::vector<bool> words, size < i < capacity), and on Apple
    # Sanitizers.cmake disables libc++'s container-overflow annotations. Header-only, ABI-stable.
    add_compile_definitions(_LIBCPP_HARDENING_MODE=_LIBCPP_HARDENING_MODE_EXTENSIVE _GLIBCXX_ASSERTIONS)
    # SLIC3R_UBSAN adds Clang's -fsanitize=integer, which reports well-defined unsigned
    # wrap-around and shifts (e.g. inside libc++'s vector<bool>) on every input. Keep real UB only.
    if (SLIC3R_UBSAN)
        add_compile_options(-fno-sanitize=unsigned-integer-overflow,unsigned-shift-base,implicit-conversion)
    endif ()
    if (DEFINED ENV{LIB_FUZZING_ENGINE})
        set(SLIC3R_FUZZ_ENGINE "$ENV{LIB_FUZZING_ENGINE}")
    else ()
        set(SLIC3R_FUZZ_ENGINE "-fsanitize=fuzzer")
    endif ()
endif ()

function(slic3r_add_fuzzer name)
    cmake_parse_arguments(FZ "" "CORPUS;DICT" "SOURCES;LIBS" ${ARGN})

    if (NOT SLIC3R_FUZZ AND NOT SLIC3R_BUILD_TESTS)
        return()
    endif ()

    set(_target fuzz-${name})
    add_executable(${_target} ${FZ_SOURCES})
    target_link_libraries(${_target} PRIVATE ${FZ_LIBS})
    set_target_properties(${_target} PROPERTIES FOLDER fuzz)

    if (SLIC3R_FUZZ)
        target_link_options(${_target} PRIVATE ${SLIC3R_FUZZ_ENGINE})
        if (FZ_DICT)
            configure_file(${FZ_DICT} ${CMAKE_CURRENT_BINARY_DIR}/${name}.dict COPYONLY)
        endif ()
    else ()
        target_link_libraries(${_target} PRIVATE slic3r-fuzz-replay)
    endif ()

    # Replay the corpus in both flavours (in the libFuzzer flavour: -runs=0 just executes inputs).
    if (FZ_CORPUS AND SLIC3R_BUILD_TESTS)
        get_filename_component(_corpus ${FZ_CORPUS} ABSOLUTE)
        if (SLIC3R_FUZZ)
            add_test(NAME ${_target} COMMAND ${_target} -runs=0 ${_corpus})
        else ()
            add_test(NAME ${_target} COMMAND ${_target} ${_corpus})
        endif ()
        # UBSan is recoverable by default: halt so its findings fail the replay. Known benign reports
        # are suppressed by the shared suppression file.
        set_tests_properties(${_target} PROPERTIES LABELS fuzz ENVIRONMENT
            "UBSAN_OPTIONS=halt_on_error=1:print_stacktrace=1:suppressions=${SLIC3R_FUZZ_UBSAN_SUPPRESSIONS}")
    endif ()
endfunction()
