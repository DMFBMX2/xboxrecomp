cmake_minimum_required(VERSION 3.20)
project(apu_current_voice_test C)
if(NOT WIN32)
    message(FATAL_ERROR "The current-voice check uses Windows threads")
endif()
set(CMAKE_C_STANDARD 11)
add_subdirectory(../.. runtime)
# test_main.c compiles apu_vp.c itself, to reach its static helpers, and
# stands in for the little it uses from the rest of the APU.
add_executable(apu_current_voice_test test_main.c)
target_include_directories(apu_current_voice_test PRIVATE ../../src ../../src/apu)
target_compile_definitions(apu_current_voice_test PRIVATE _USE_MATH_DEFINES)
enable_testing()
add_test(NAME apu_current_voice COMMAND apu_current_voice_test)
