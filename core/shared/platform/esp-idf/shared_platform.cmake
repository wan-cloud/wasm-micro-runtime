# Copyright (C) 2019 Intel Corporation.  All rights reserved.
# SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

set (PLATFORM_SHARED_DIR ${CMAKE_CURRENT_LIST_DIR})

add_definitions(-DBH_PLATFORM_ESP_IDF)

include_directories(${PLATFORM_SHARED_DIR})
include_directories(${PLATFORM_SHARED_DIR}/../include)

file (GLOB_RECURSE source_all ${PLATFORM_SHARED_DIR}/*.c)

include (${CMAKE_CURRENT_LIST_DIR}/../common/libc-util/platform_common_libc_util.cmake)
set (source_all ${source_all} ${PLATFORM_COMMON_LIBC_UTIL_SOURCE})

set (PLATFORM_SHARED_SOURCE ${source_all} ${PLATFORM_COMMON_MATH_SOURCE})

# XUANSI VENDORED PATCH #2 (B10-T2, 2026-11-08, rev 2): the ESP32-classic
# constant dual-bus alias is broken on S3 (see VENDORED-NOTES.md) — the define
# stays ON, but espidf_memmap.c now provides the real mapping: esp_mmu_map
# (EXEC|READ, non-writable per esp_mm caps rules) + a table-backed
# os_get_dbus_mirror() so every WAMR write goes through the D-bus pointer
# and every I-fetch through the mapped executable window.
if(CONFIG_ESP32S3_SPIRAM_SUPPORT)
    add_definitions(-DWASM_MEM_DUAL_BUS_MIRROR=1)
endif()
