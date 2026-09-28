cmake_minimum_required(VERSION 3.20)
project(sandbox_runner LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "sandbox_runner requires a Windows toolchain (MSYS2 UCRT64).")
endif()

# Single product executable; Control receiver is available for schema integration.
set(ARTIFACT_CANDIDATE_SOURCES src/artifact/candidate_detector.cpp src/artifact/file_stability.cpp)
add_executable(sandbox_runner src/main.cpp src/candidate_telemetry.cpp src/artifact/output_watcher.cpp ${ARTIFACT_CANDIDATE_SOURCES})

# Reusable protocol/transport objects, linked into the single product executable.
# JsonCpp is a host build dependency; link its static target to avoid a new DLL.
find_package(jsoncpp CONFIG REQUIRED)
add_library(runner_telemetry OBJECT
    src/protocol/scrp/envelope.cpp
    src/protocol/scrp/telemetry_messages.cpp
    src/protocol/scrp/artifact_candidate.cpp
    src/transport/telemetry/pending_event_store.cpp
    src/transport/telemetry/telemetry_client.cpp
    src/transport/telemetry/winhttp_websocket.cpp)
target_include_directories(runner_telemetry PUBLIC src)
target_link_libraries(runner_telemetry PUBLIC jsoncpp_static winhttp bcrypt crypt32)
add_library(runner_lifecycle STATIC src/session_context.cpp src/runner_lifecycle.cpp src/artifact_candidate_adapter.cpp)
target_link_libraries(runner_lifecycle PUBLIC runner_telemetry)
add_library(runner_control STATIC src/transport/control/control_receiver.cpp src/protocol/scrp/host_sender_schema.cpp)
target_link_libraries(runner_control PUBLIC runner_telemetry)
target_sources(runner_lifecycle PRIVATE src/control_session.cpp src/gui_product.cpp)
target_link_libraries(runner_lifecycle PUBLIC runner_control)
target_link_libraries(sandbox_runner PRIVATE runner_lifecycle)
target_link_libraries(sandbox_runner PRIVATE runner_control)


# Keep the initial Runner executable independent of the host's MinGW DLLs.
if(MINGW)
    target_link_options(sandbox_runner PRIVATE -static -municode)
endif()

# Imported Windows GUI executor + trusted SCRP adapter, in the same Runner EXE.
add_library(runner_gui STATIC
    src/control/control_types.cpp src/control/control_util.cpp
    src/control/screen_capture.cpp src/control/input_executor.cpp
    src/control/action_scheduler.cpp src/runtime/session_workspace.cpp
    src/runtime/sandbox_runtime.cpp src/gui_session.cpp
    src/transport/observation/https_uploader.cpp)
target_compile_features(runner_gui PUBLIC cxx_std_17)
target_compile_definitions(runner_gui PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX)
target_link_libraries(runner_gui PUBLIC runner_control user32 gdi32 ole32 windowscodecs uuid bcrypt)
target_link_libraries(runner_lifecycle PUBLIC runner_gui)
