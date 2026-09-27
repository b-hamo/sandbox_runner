cmake_minimum_required(VERSION 3.20)
project(sandbox_runner LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "sandbox_runner requires a Windows toolchain (MSYS2 UCRT64).")
endif()

# Host-only verification executable; never part of the deployed Runner.
option(RUNNER_BUILD_TESTS "Build Windows Output watcher integration tests" OFF)
if(RUNNER_BUILD_TESTS)
    enable_testing()
    add_executable(output_watcher_tests tests/output_watcher_tests.cpp src/artifact/output_watcher.cpp)
    target_include_directories(output_watcher_tests PRIVATE src)
    if(MINGW)
        target_link_options(output_watcher_tests PRIVATE -static -municode)
    endif()
    add_test(NAME output_watcher COMMAND output_watcher_tests $<TARGET_FILE:sandbox_runner>)
    set_tests_properties(output_watcher PROPERTIES TIMEOUT 30)
endif()

# Single product executable. Connect future Control sources to this target.
set(ARTIFACT_CANDIDATE_SOURCES src/artifact/candidate_detector.cpp src/artifact/file_stability.cpp)
add_executable(sandbox_runner src/main.cpp src/artifact/output_watcher.cpp ${ARTIFACT_CANDIDATE_SOURCES})

if(RUNNER_BUILD_TESTS)
    add_executable(candidate_tests tests/candidate_tests.cpp src/artifact/output_watcher.cpp ${ARTIFACT_CANDIDATE_SOURCES})
    target_include_directories(candidate_tests PRIVATE src)
    if(MINGW)
        target_link_options(candidate_tests PRIVATE -static)
    endif()
    add_test(NAME artifact_candidate COMMAND candidate_tests)
    set_tests_properties(artifact_candidate PROPERTIES TIMEOUT 45)
endif()

# Keep the initial Runner executable independent of the host's MinGW DLLs.
if(MINGW)
    target_link_options(sandbox_runner PRIVATE -static -municode)
endif()
