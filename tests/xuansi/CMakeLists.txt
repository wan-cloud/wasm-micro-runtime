# Copyright (C) 2026 xuansi project contributors.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#
# XUANSI (P3-T10): host-side unit tests for the surgery container
# format and the loader dual-path placement contracts.
#
#   cmake -B build -S . && cmake --build build && ./build/xuansi_aot_tests <fixture.aot> ...
#
# The fixture argument is a stock container compiled by this fork's
# wamrc for the HOST target (x86-64) from tests/xuansi/mini.wat:
#   (module (func (export "f") (result i32) i32.const 77))
# Extra arguments (e.g. xtensa products) are classify-only legs.

cmake_minimum_required (VERSION 3.14)
project (xuansi-aot-tests)

set (WAMR_BUILD_PLATFORM "linux")
set (WAMR_BUILD_TARGET ${CMAKE_HOST_SYSTEM_PROCESSOR})
if (WAMR_BUILD_TARGET STREQUAL "aarch64" OR WAMR_BUILD_TARGET STREQUAL "arm64")
  set (WAMR_BUILD_TARGET "AARCH64")
elseif (WAMR_BUILD_TARGET MATCHES "riscv64")
  set (WAMR_BUILD_TARGET "RISCV64_LP64D")
else ()
  set (WAMR_BUILD_TARGET "X86_64")
endif ()

set (WAMR_BUILD_INTERP 1)
set (WAMR_BUILD_FAST_INTERP 1)
set (WAMR_BUILD_AOT 1)
set (WAMR_BUILD_LIBC_BUILTIN 0)
set (WAMR_BUILD_LIBC_WASI 0)
set (WAMR_BUILD_APP_FRAMEWORK 0)

include (${CMAKE_CURRENT_SOURCE_DIR}/../../build-scripts/runtime_lib.cmake)

add_library (vmlib STATIC ${WAMR_RUNTIME_LIB_SOURCE})
target_include_directories (vmlib INTERFACE
  ${WAMR_RUNTIME_INCLUDE_DIRECTORIES}
)
target_link_libraries (vmlib -lm -ldl -lpthread)

add_executable (xuansi_aot_tests test_xuansi_aot.c)
target_link_libraries (xuansi_aot_tests vmlib)

add_test (NAME xuansi_aot_tests_help
          COMMAND xuansi_aot_tests)
