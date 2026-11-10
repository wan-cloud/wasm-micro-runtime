/*
 * Copyright (C) 2026 xuansi project contributors.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/*
 * XUANSI (P3-T10): A3 surgery container contracts, WAMR side.
 *
 * The "surgery" toolchain (LLVM fork branch xuansi/a3-surgery + this
 * fork's wamrc) produces containers whose constant pool is split out of
 * the text section: the text section's literal_size field is 0 and the
 * pool bytes travel in a CUSTOM section named XUANSI_LITERAL_SECTION_NAME.
 * Generated code loads the pool base from a dedicated slot in the module
 * instance struct (filled by the loader at instantiate time) instead of
 * using PC-relative l32r, so the pool can live in non-executable memory.
 *
 * The two constants below are mirrored by name from the LLVM fork
 * (llvm/lib/Target/Xtensa/XtensaSubtarget.h, single source of truth for
 * codegen).  They MUST stay equal on both sides; a CI gate (drivers
 * workflow t10-wamr.yml, "constant gate") cross-asserts the two
 * definitions on every toolchain build and goes red on drift.
 */

#ifndef _XUANSI_AOT_H
#define _XUANSI_AOT_H

#include "wasm_export.h"

/* Byte offset of exec_env->module_inst inside WASMExecEnv.  The AOT
 * calling convention passes exec_env as the first argument; the surgery
 * base chain's first hop loads module_inst from it.  Matches wamrc's
 * own create_basic_func_context (GEP +2 over a pointer = 2 * 4 bytes
 * on the 32-bit target). */
#define XUANSI_EXEC_ENV_MODULE_INST_OFFSET 8

/* Byte offset of the literal pool base slot inside the module instance
 * struct (WASMModuleInstance, of which AOTModuleInstance is an alias).
 * The field is carved from the upstream reserved[] tail so that every
 * offset stock containers bake into generated code (cur_exception at
 * 104, global_table_data at 344, ...) stays frozen; see
 * core/iwasm/interpreter/wasm_runtime.h.  A single l32i (imm8 << 2
 * spans 0..1020) reads it. */
#define XUANSI_LITERAL_BASE_SLOT 320

/* Name of the CUSTOM section (sub type AOT_CUSTOM_SECTION_RAW) that
 * carries the literal pool bytes in a surgery container. */
#define XUANSI_LITERAL_SECTION_NAME "xuansi.literal"

#if WASM_ENABLE_AOT != 0

/* Container kind, derived purely from the section structure (spec
 * 5.3.3 discrimination table; no version bump).  The fw loader gate
 * consumes this before choosing a placement path. */
typedef enum AOTXuansiKind {
    /* literal_size > 0 (literal block embedded in text): v2-era stock
     * container.  Never goes to the PSRAM exec window (bus wedge). */
    AOT_XUANSI_KIND_STOCK = 0,
    /* literal_size == 0 and the xuansi.literal section present:
     * surgery container.  Text may go to the PSRAM exec window, pool
     * to the PSRAM data heap. */
    AOT_XUANSI_KIND_SURGERY,
    /* anything else (bad magic/version, truncated, literal_size == 0
     * without the pool section on an xtensa target): reject. */
    AOT_XUANSI_KIND_MALFORMED,
} AOTXuansiKind;

/*
 * Classify an AOT container buffer by its section structure.  Walks
 * section headers only (plus custom section name prefixes); does not
 * allocate and does not depend on the build target matching the
 * container.  Non-xtensa containers legitimately carry literal_size==0
 * without a pool section (their codegen has no literal pool); those
 * classify as STOCK.
 */
AOTXuansiKind
aot_xuansi_kind(const uint8 *buf, uint32 size);

#endif /* WASM_ENABLE_AOT != 0 */

#endif /* end of _XUANSI_AOT_H */
