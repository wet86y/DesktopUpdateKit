cmake_minimum_required(VERSION 3.25)
project(DesktopUpdateKitNative VERSION 0.1.0 LANGUAGES CXX)

if(NOT WIN32 OR NOT MSVC)
    message(FATAL_ERROR "DesktopUpdateKit native candidate requires Windows and MSVC.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

add_library(desktop_update_kit_native STATIC
    src/Json.cpp
    src/UpdateKit.cpp
)
target_include_directories(desktop_update_kit_native PUBLIC include)
target_compile_definitions(desktop_update_kit_native PUBLIC UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(desktop_update_kit_native PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(desktop_update_kit_native PRIVATE bcrypt winhttp)

add_executable(DesktopUpdateKitUpdaterStub updater/main.cpp)
target_compile_definitions(DesktopUpdateKitUpdaterStub PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
target_compile_options(DesktopUpdateKitUpdaterStub PRIVATE /W4 /permissive- /utf-8 /EHsc)
target_link_libraries(DesktopUpdateKitUpdaterStub PRIVATE desktop_update_kit_native)

include(CTest)
if(BUILD_TESTING)
    add_executable(desktop_update_kit_native_test_host tests/TestHost.cpp)
    target_compile_definitions(desktop_update_kit_native_test_host PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(desktop_update_kit_native_test_host PRIVATE /W4 /permissive- /utf-8 /EHsc)
    add_executable(desktop_update_kit_native_tests tests/NativeTests.cpp)
    target_compile_definitions(desktop_update_kit_native_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(desktop_update_kit_native_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_libraries(desktop_update_kit_native_tests PRIVATE desktop_update_kit_native)
    add_test(NAME desktop_update_kit_native COMMAND desktop_update_kit_native_tests $<TARGET_FILE:DesktopUpdateKitUpdaterStub> $<TARGET_FILE:desktop_update_kit_native_test_host>)

    add_executable(desktop_update_kit_native_transport_tests tests/TransportTests.cpp)
    target_compile_definitions(desktop_update_kit_native_transport_tests PRIVATE UNICODE _UNICODE NOMINMAX WIN32_LEAN_AND_MEAN)
    target_compile_options(desktop_update_kit_native_transport_tests PRIVATE /W4 /permissive- /utf-8 /EHsc)
    target_link_libraries(desktop_update_kit_native_transport_tests PRIVATE desktop_update_kit_native)
    add_test(NAME desktop_update_kit_native_transport COMMAND desktop_update_kit_native_transport_tests)
endif()
