cmake_minimum_required(VERSION 3.20)
project(itch_orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)            # -std=c++20, not -std=gnu++20
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)    # compile_commands.json for editors / clangd

# Default to an optimized build. CMake's Release flags for GCC/Clang are "-O3 -DNDEBUG".
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(OB_NATIVE   "Tune for the build machine's CPU (-march=native)" ON)
option(OB_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(OB_TESTS    "Build unit tests" ON)

# Header-only core library. Everything that links `ob` gets these includes and flags.
add_library(ob INTERFACE)
target_include_directories(ob INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_options(ob INTERFACE -Wall -Wextra -Wpedantic -Wconversion)

if(OB_NATIVE)
  include(CheckCXXCompilerFlag)
  check_cxx_compiler_flag(-march=native OB_HAS_MARCH_NATIVE)
  if(OB_HAS_MARCH_NATIVE)
    target_compile_options(ob INTERFACE -march=native)
  endif()
endif()

if(OB_SANITIZE)
  target_compile_options(ob INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(ob INTERFACE -fsanitize=address,undefined)
endif()

add_executable(itch_stats src/main.cpp)
target_link_libraries(itch_stats PRIVATE ob)

if(OB_TESTS)
  include(FetchContent)
  FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.17.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM  # treat gtest headers as system headers: our -W flags don't apply to them
  )
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)

  enable_testing()
  include(GoogleTest)

  foreach(t test_parser test_reference_book)
    add_executable(${t} tests/${t}.cpp)
    target_link_libraries(${t} PRIVATE ob GTest::gtest_main)
    gtest_discover_tests(${t})
  endforeach()
endif()
