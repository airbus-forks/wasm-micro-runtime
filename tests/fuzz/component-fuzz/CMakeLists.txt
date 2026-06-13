# Copyright (C) 2026 Rebecker Specialties. All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

cmake_minimum_required(VERSION 3.14)

project(wamr_component_fuzzing LANGUAGES ASM C CXX)

# libFuzzer requires Clang.
if(NOT CMAKE_C_COMPILER_ID MATCHES ".*Clang")
  message(FATAL_ERROR "Please use Clang as the C compiler for libFuzzer compatibility.")
endif()

set(CMAKE_BUILD_TYPE Debug)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 17)

string(TOLOWER ${CMAKE_HOST_SYSTEM_NAME} WAMR_BUILD_PLATFORM)

set(REPO_ROOT_DIR ${CMAKE_CURRENT_LIST_DIR}/../../..)

# Build target detection (no 32-bit cross here; for a real ILP32 run, see
# the README — point this at an i686 / armv7 toolchain to also catch the
# size_t-is-32-bit overflow class).
if(NOT DEFINED WAMR_BUILD_TARGET)
  if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(arm64|aarch64)")
    set(WAMR_BUILD_TARGET "AARCH64")
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(WAMR_BUILD_TARGET "X86_64")
  elseif(CMAKE_SIZEOF_VOID_P EQUAL 4)
    set(WAMR_BUILD_TARGET "X86_32")
  else()
    message(FATAL_ERROR "Unsupported build target platform!")
  endif()
endif()

if(APPLE)
  add_definitions(-DBH_PLATFORM_DARWIN)
endif()

#
# Interpreter-only, component-model-on, host-layer-off configuration.
# No LLVM / AOT / JIT: this target only exercises the binary parser,
# validator and WAVE helpers. The WASI Preview 2 host layer is excluded
# (LIBC_WASI=0); any unreferenced instantiation / canonical-ABI / host
# code is removed by dead-strip below, so no host symbols are needed.
#
set(WAMR_BUILD_INTERP 1)
set(WAMR_BUILD_FAST_INTERP 1)
set(WAMR_BUILD_AOT 0)
set(WAMR_BUILD_JIT 0)
set(WAMR_BUILD_FAST_JIT 0)
set(WAMR_BUILD_LIBC_BUILTIN 0)
set(WAMR_BUILD_LIBC_WASI 0)
set(WAMR_BUILD_COMPONENT_MODEL 1)
set(WAMR_BUILD_REF_TYPES 1)
set(WAMR_BUILD_SIMD 1)
set(WAMR_BUILD_GC 0)
set(WAMR_BUILD_MULTI_MODULE 0)
set(WAMR_DISABLE_HW_BOUND_CHECK 1)

add_definitions(-DWASM_ENABLE_FUZZ_TEST=1)
add_compile_options(-Wno-unused-command-line-argument)

#
# Sanitizers: AddressSanitizer + UndefinedBehaviorSanitizer + libFuzzer.
# Skip when oss-fuzz drives its own instrumentation
# (-DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION present in CFLAGS).
#
set(CFLAGS_ENV $ENV{CFLAGS})
string(FIND "${CFLAGS_ENV}" "-DFUZZING_BUILD_MODE_UNSAFE_FOR_PRODUCTION" FUZZ_POS)
if(FUZZ_POS GREATER -1)
  set(IN_OSS_FUZZ 1)
else()
  set(IN_OSS_FUZZ 0)
endif()

if(NOT IN_OSS_FUZZ)
  message(STATUS "Enabling ASan + UBSan for the component-model fuzz target")
  add_compile_options(-g -O1 -fno-omit-frame-pointer)
  add_compile_options(-fsanitize=address -fsanitize-address-use-after-scope)
  add_compile_options(
    -fsanitize=array-bounds,bool,builtin,enum,integer-divide-by-zero,null,object-size,return,returns-nonnull-attribute,shift,signed-integer-overflow,unreachable,vla-bound
    -fno-sanitize-recover=array-bounds,bool,builtin,enum,integer-divide-by-zero,null,object-size,return,returns-nonnull-attribute,shift,signed-integer-overflow,unreachable,vla-bound)
  add_link_options(-fsanitize=address,undefined)
endif()

# libFuzzer driver (no vptr: RTTI is off in the runtime build).
add_compile_options(-fsanitize=fuzzer -fno-sanitize=vptr)
add_link_options(-fsanitize=fuzzer -fno-sanitize=vptr)

# Drop unreferenced functions (the instantiation / canonical-ABI / host
# paths the parser fuzzer never calls) so the link needs no host symbols.
if(APPLE)
  add_link_options(-Wl,-dead_strip)
else()
  add_compile_options(-ffunction-sections -fdata-sections)
  add_link_options(-Wl,--gc-sections)
endif()

include(${REPO_ROOT_DIR}/build-scripts/runtime_lib.cmake)
include(${REPO_ROOT_DIR}/core/shared/utils/uncommon/shared_uncommon.cmake)

add_library(vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_include_directories(vmlib PUBLIC ${RUNTIME_LIB_HEADER_LIST})

add_executable(component_parser_fuzz
  component-parser/component_parser_fuzz.cc
  component-parser/host_resolve_stubs.c)
target_link_libraries(component_parser_fuzz PRIVATE vmlib m)
