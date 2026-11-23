cmake_minimum_required(VERSION 3.22...4.3)

project(emberfall
  VERSION 0.2.0
  DESCRIPTION "Realms of Emberfall game server and tools"
  LANGUAGES C CXX)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  message(FATAL_ERROR "The Emberfall server targets Linux only (GCC 12+/Clang 15+).")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(EF_WERROR "Treat compiler warnings as errors" ON)
option(EF_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)
option(EF_BUILD_TESTS "Build unit tests" ON)
option(EF_FUZZ "Build the libFuzzer targets (Clang only)" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(Dependencies)

# Warning/sanitizer flags applied to our own targets only (not third-party code).
add_library(ef_build_flags INTERFACE)
target_compile_options(ef_build_flags INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
  -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Woverloaded-virtual
  -Wnull-dereference -Wdouble-promotion -Wformat=2 -Wimplicit-fallthrough)
# Catch2's TEST_CASE expands __COUNTER__, which Clang 22+ flags under -Wpedantic.
include(CheckCXXCompilerFlag)
check_cxx_compiler_flag(-Wc2y-extensions EF_HAS_WC2Y_EXTENSIONS)
if(EF_HAS_WC2Y_EXTENSIONS)
  target_compile_options(ef_build_flags INTERFACE -Wno-c2y-extensions)
endif()
if(EF_WERROR)
  target_compile_options(ef_build_flags INTERFACE -Werror)
endif()
if(EF_SANITIZE OR EF_FUZZ)
  target_compile_options(ef_build_flags INTERFACE -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  target_link_options(ef_build_flags INTERFACE -fsanitize=address,undefined)
endif()
if(EF_FUZZ)
  # Coverage feedback from all of our code, not just the fuzz entry points.
  target_compile_options(ef_build_flags INTERFACE -fsanitize=fuzzer-no-link)
endif()

add_subdirectory(server)
add_subdirectory(tools/bot)
add_subdirectory(tools/balance_sim)
if(EF_FUZZ)
  add_subdirectory(tools/fuzz)
endif()

if(EF_BUILD_TESTS)
  enable_testing()
  add_subdirectory(server/tests)
endif()
