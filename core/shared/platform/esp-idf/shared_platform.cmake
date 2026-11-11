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

# XUANSI VENDORED PATCH #16 (2026-11-11): compile-time switch for the
# internal-SRAM AOT lane (patches #12/#13 — static IRAM reserve, PMS
# write-probe, per-boot unroute guard), DEFAULT OFF. The consumer's fw
# 0.4.0 retires the internal lane: every .aot rides the PSRAM exec
# window (the surgery path: patch #3 mapping + #15 pool registry +
# loader container-kind dispatch), which stays unconditional. Turn this
# ON only to rebuild the pre-#16 internal lane bit-identically (rollback
# leg). OFF also drops the xuansi_aot_exec_budget()/
# xuansi_aot_exec_writable() exports — consumers must stop referencing
# them first (see the patch #16 note in espidf_memmap.c).
option(XUANSI_AOT_IRAM_LANE "Compile the internal-SRAM AOT lane (xuansi patches #12/#13)" OFF)
if(XUANSI_AOT_IRAM_LANE)
    add_definitions(-DXUANSI_AOT_IRAM_LANE=1)
endif()
