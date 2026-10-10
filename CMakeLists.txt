cmake_minimum_required(VERSION 3.20)
cmake_policy(SET CMP0091 NEW)
project(NetMeter LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "NetMeter is a Windows-only application.")
endif()

add_executable(NetMeter WIN32 netmeter.cpp NetMeter.rc)
set_property(TARGET NetMeter PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
target_compile_features(NetMeter PRIVATE cxx_std_17)
target_compile_definitions(NetMeter PRIVATE UNICODE _UNICODE WIN32_LEAN_AND_MEAN NOMINMAX NTDDI_VERSION=0x06020000)
target_link_libraries(NetMeter PRIVATE iphlpapi shell32 user32 gdi32 advapi32)
