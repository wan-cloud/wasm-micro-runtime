/*
 * Copyright (C) 2026 xuansi project contributors.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/*
 * XUANSI (P3-T10): host-side unit tests for the surgery container
 * format and the loader's dual-path placement contracts (spec 5.3.2 /
 * 5.3.3).  Everything here runs on the HOST build of the runtime; the
 * board-side PSRAM/I-cache specifics belong to fw T11/T15.
 *
 * Usage: xuansi_aot_tests <stock_host.aot> [xtensa_stock.aot ...]
 *
 *   arg 1 (required)  : a stock container compiled by THIS fork's wamrc
 *                       for the host target (x86-64).  It is transformed
 *                       in-memory into a surgery container (literal pool
 *                       split into the xuansi.literal CUSTOM section
 *                       placed between EXPORT and RELOCATION) and then
 *                       loaded, instantiated, CALLED, and unloaded —
 *                       the full load path with the relaxed section
 *                       order, the pool placement and the instance slot
 *                       fill asserted along the way.
 *   arg 2.. (optional): xtensa containers (e.g. the five driver module
 *                       products) for classify-only assertions.
 *
 * All MALFORMED vectors are handcrafted bytes: api rejection must be
 * honest (error message) and must never crash.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wasm_export.h"
#include "bh_platform.h"
#include "aot_runtime.h"
#include "xuansi_aot.h"

static int g_failures;
static int g_checks;

#define CHECK(cond, msg)                                             \
    do {                                                             \
        g_checks++;                                                  \
        if (!(cond)) {                                               \
            g_failures++;                                            \
            printf("FAIL: %s (line %d)\n", msg, __LINE__);           \
        }                                                            \
    } while (0)

/* ------------------------------------------------------------------ */
/* tiny container helpers (little-endian host builds only)             */
/* ------------------------------------------------------------------ */

static uint32
rd32(const uint8 *p)
{
    return (uint32)p[0] | ((uint32)p[1] << 8) | ((uint32)p[2] << 16)
           | ((uint32)p[3] << 24);
}

static void
wr32(uint8 *p, uint32 v)
{
    p[0] = (uint8)v;
    p[1] = (uint8)(v >> 8);
    p[2] = (uint8)(v >> 16);
    p[3] = (uint8)(v >> 24);
}

/* append a section header + body to buf at *p_off (4-aligned start,
 * matching the emitter discipline) */
static void
append_section(uint8 *buf, uint32 *p_off, uint32 type, const uint8 *body,
               uint32 size)
{
    uint32 off = (*p_off + 3) & ~(uint32)3;
    memset(buf + *p_off, 0, off - *p_off);
    wr32(buf + off, type);
    wr32(buf + off + 4, size);
    if (size)
        memcpy(buf + off + 8, body, size);
    *p_off = off + 8 + size;
}

/* a minimal xtensa-flavoured container for the classifier/loader gate:
 * header + TARGET_INFO(arch "xtensa") + TEXT(literal_size == 0) [+ pool
 * CUSTOM when with_pool] + empty RELOCATION.  Bodies beyond what the
 * classifier and the pre-placement gate read are zero padding. */
#define XMINI_TARGET_INFO_SIZE (32 + 16)
#define XMINI_TEXT_SIZE 24
#define XMINI_POOL_CONTENT 8

static uint32
build_mini(uint8 *buf, uint32 buf_size, bool with_pool)
{
    uint32 off = 0;
    uint8 body[64];

    wr32(buf, 0x746f6100); /* AOT_MAGIC_NUMBER ("\0aot") */
    wr32(buf + 4, 4);      /* container version v4 — NOT bumped */
    off = 8;

    memset(body, 0, sizeof(body));
    strcpy((char *)body + 32, "xtensa"); /* arch string at offset 32 */
    append_section(buf, &off, 0 /* TARGET_INFO */, body,
                   XMINI_TARGET_INFO_SIZE);

    wr32(body, 0); /* text literal_size == 0, no code */
    append_section(buf, &off, 2 /* TEXT */, body, XMINI_TEXT_SIZE);

    if (with_pool) {
        /* CUSTOM RAW "xuansi.literal" + XMINI_POOL_CONTENT bytes,
         * emitter discipline: the u16 name length counts the NUL and
         * the pool starts 4-aligned inside the section */
        uint8 *sec = body;
        uint32 p = 0;
        uint32 nl = (uint32)strlen(XUANSI_LITERAL_SECTION_NAME);
        wr32(sec + p, 0 /* AOT_CUSTOM_SECTION_RAW */);
        p += 4;
        sec[p++] = (uint8)(nl + 1);
        sec[p++] = 0;
        memcpy(sec + p, XUANSI_LITERAL_SECTION_NAME, nl);
        p += nl;
        sec[p++] = '\0';
        while (p & 3)
            sec[p++] = 0; /* name padding */
        memset(sec + p, 0xA5, XMINI_POOL_CONTENT);
        p += XMINI_POOL_CONTENT;
        append_section(buf, &off, 100 /* CUSTOM */, body, p);
    }

    wr32(body, 0);
    append_section(buf, &off, 5 /* RELOCATION */, body, 4);

    return off;
}

/* ------------------------------------------------------------------ */
/* stock -> surgery transformer (host fixtures)                        */
/* ------------------------------------------------------------------ */

static const uint8 pool_pattern[16] = { 0xC0, 0xFF, 0xEE, 0x01, 0x02, 0x03,
                                        0x04, 0x05, 0x06, 0x07, 0x08, 0x09,
                                        0x0A, 0x0B, 0x0C, 0x0D };

/*
 * Rewrite a stock host container (whose text section already carries
 * literal_size == 0 — host codegen has no literal pool) into the
 * surgery shape: insert a CUSTOM xuansi.literal section right before
 * the RELOCATION section.  Code bytes, function offsets and relocation
 * records are untouched, so the transformed container still runs.
 * (An xtensa stock fixture would additionally move its literal block
 * out of the text section; the five-module CI leg covers that shape
 * at the container level — running xtensa code is board work.)
 */
static uint8 *
transform_to_surgery(const uint8 *buf, uint32 size, uint32 *p_out_size)
{
    const uint8 *p = buf + 8, *p_end = buf + size;
    const uint8 *reloc_at = NULL;
    static const char name[] = XUANSI_LITERAL_SECTION_NAME;
    uint32 name_len = (uint32)strlen(name);
    /* EMIT_STR convention: u16 length counts the terminating NUL; the
     * emitter pads the name so the pool starts 4-aligned in the file —
     * the transformer must reproduce that layout byte-for-byte */
    uint32 name_region = 2 + (name_len + 1);
    uint32 name_pad = (4 - (name_region & 3)) & 3;
    uint32 custom_body = 4 + name_region + name_pad + sizeof(pool_pattern);
    uint32 pad = (4 - (custom_body & 3)) & 3; /* keep next header aligned */
    uint32 custom_total = 8 + custom_body + pad;
    uint8 *out;
    const uint8 *copy_from;
    uint32 prefix_len, off;

    while (p + 8 <= p_end) {
        uint32 type, sec_size;
        p = (const uint8 *)(((uintptr_t)p + 3) & ~(uintptr_t)3);
        if (p + 8 > p_end)
            break;
        type = rd32(p);
        sec_size = rd32(p + 4);
        if (type == 5 /* RELOCATION */ && !reloc_at) {
            reloc_at = p;
        }
        p += 8 + sec_size;
    }
    if (!reloc_at) {
        printf("transform: no RELOCATION section found\n");
        return NULL;
    }

    prefix_len = (uint32)(reloc_at - buf); /* already 4-aligned */
    out = malloc(size + custom_total);
    if (!out)
        return NULL;
    memset(out, 0, size + custom_total);
    memcpy(out, buf, prefix_len);

    off = prefix_len;
    wr32(out + off, 100 /* CUSTOM */);
    wr32(out + off + 4, custom_body);
    off += 8;
    wr32(out + off, 0 /* AOT_CUSTOM_SECTION_RAW */);
    off += 4;
    out[off++] = (uint8)(name_len + 1);
    out[off++] = 0;
    memcpy(out + off, name, name_len);
    off += name_len;
    out[off++] = '\0';
    off += name_pad; /* name padding: pool starts 4-aligned */
    memcpy(out + off, pool_pattern, sizeof(pool_pattern));
    off += sizeof(pool_pattern);
    off += pad; /* zero padding keeps the next header 4-aligned */

    copy_from = reloc_at;
    memcpy(out + off, copy_from, size - prefix_len);
    off += size - prefix_len;

    *p_out_size = off;
    return out;
}

/* ------------------------------------------------------------------ */
/* runtime helpers                                                     */
/* ------------------------------------------------------------------ */

static uint8 *
read_file(const char *path, uint32 *p_size)
{
    FILE *f = fopen(path, "rb");
    uint8 *buf;
    long sz;
    if (!f)
        return NULL;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    buf = malloc((uint32)sz);
    if (buf && fread(buf, 1, (uint32)sz, f) != (uint32)sz) {
        free(buf);
        buf = NULL;
    }
    fclose(f);
    *p_size = (uint32)sz;
    return buf;
}

static AOTModule *
load_aot(uint8 *buf, uint32 size, char *err, uint32 errsz, bool expect_ok)
{
    wasm_module_t module = wasm_runtime_load(buf, size, err, errsz);
    g_checks++;
    if (expect_ok) {
        if (!module) {
            g_failures++;
            printf("FAIL: expected load success, got: %s\n", err);
            return NULL;
        }
        return (AOTModule *)module;
    }
    if (module) {
        g_failures++;
        printf("FAIL: expected load rejection, it loaded\n");
        wasm_runtime_unload(module);
    }
    return NULL;
}

static bool
call_f(wasm_module_inst_t inst, uint32 *p_result)
{
    wasm_exec_env_t exec_env = NULL;
    wasm_function_inst_t func = wasm_runtime_lookup_function(inst, "f");
    uint32 argv[1] = { 0 };
    bool ok = false;

    if (!func)
        return false;
    exec_env = wasm_runtime_create_exec_env(inst, 8192);
    if (!exec_env)
        return false;
    /* i32 f(): one result returned in argv[0] */
    if (wasm_runtime_call_wasm(exec_env, func, 0, argv)) {
        *p_result = argv[0];
        ok = true;
    }
    wasm_runtime_destroy_exec_env(exec_env);
    return ok;
}

/* ------------------------------------------------------------------ */

int
main(int argc, char *argv[])
{
    char err[256];
    RuntimeInitArgs init_args;
    uint8 mini[256];
    uint32 mini_size;
    uint8 *stock = NULL, *surgery = NULL;
    uint32 stock_size = 0, surgery_size = 0;

    if (argc < 2) {
        printf("usage: %s <stock_host.aot> [more_xtensa.aot ...]\n",
               argv[0]);
        return 2;
    }

    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_System_Allocator;
    if (!wasm_runtime_full_init(&init_args)) {
        printf("runtime init failed\n");
        return 2;
    }

    /* slot offset contract: this must equal the LLVM fork constant
     * (the CI constant gate cross-asserts the two definitions). */
    bh_static_assert(offsetof(AOTModuleInstance, xuansi_literal_base)
                     == XUANSI_LITERAL_BASE_SLOT);
    bh_static_assert(offsetof(AOTModuleInstance, cur_exception) == 104);
    bh_static_assert(offsetof(AOTModuleInstance, global_table_data) == 344);

    /* ---- 1. handcrafted classifier + loader-gate vectors ---- */

    /* bad magic */
    {
        uint8 buf[16] = { 0 };
        CHECK(aot_xuansi_kind(buf, sizeof(buf)) == AOT_XUANSI_KIND_MALFORMED,
              "bad magic classifies MALFORMED");
        wasm_runtime_load(buf, sizeof(buf), err, sizeof(err));
        CHECK(strstr(err, "magic") != NULL, "bad magic rejected honestly");
    }

    /* truncated real-looking buffer */
    mini_size = build_mini(mini, sizeof(mini), true);
    {
        uint8 trunc[20];
        memcpy(trunc, mini, sizeof(trunc));
        CHECK(aot_xuansi_kind(trunc, sizeof(trunc))
                  == AOT_XUANSI_KIND_MALFORMED,
              "truncated classifies MALFORMED");
    }

    /* xtensa, literal_size == 0, no pool -> MALFORMED (spec 5.3.3 row 3)
     * — both via the classifier and via the loader's api gate */
    mini_size = build_mini(mini, sizeof(mini), false);
    CHECK(aot_xuansi_kind(mini, mini_size) == AOT_XUANSI_KIND_MALFORMED,
          "xtensa lit0/no-pool classifies MALFORMED");
    load_aot(mini, mini_size, err, sizeof(err), false);
    CHECK(strstr(err, "malformed xuansi") != NULL,
          "xtensa lit0/no-pool rejected with the malformed-xuansi message");

    /* xtensa, literal_size == 0, pool section present -> SURGERY
     * (classifier only: a full handcrafted load would need complete
     * sections; the surgery LOAD runs on the transformed fixture below) */
    mini_size = build_mini(mini, sizeof(mini), true);
    CHECK(aot_xuansi_kind(mini, mini_size) == AOT_XUANSI_KIND_SURGERY,
          "xtensa lit0/with-pool classifies SURGERY");

    /* stock marker: literal_size > 0 -> STOCK (fake a text body; the
     * pool section must NOT be present — both markers at once would be
     * an emit-side invariant violation) */
    mini_size = build_mini(mini, sizeof(mini), false);
    wr32(mini + 8 + 8 + XMINI_TARGET_INFO_SIZE + 8, 16); /* literal_size */
    CHECK(aot_xuansi_kind(mini, mini_size) == AOT_XUANSI_KIND_STOCK,
          "literal_size>0 classifies STOCK");
    /* ...and the loader must reject it before parsing further
     * (handcrafted bodies are not loadable) */
    err[0] = '\0';
    load_aot(mini, mini_size, err, sizeof(err), false);
    CHECK(err[0] != '\0', "unparseable stock mini rejected");

    /* non-xtensa, literal_size == 0, no pool -> STOCK (host shape) */
    mini_size = build_mini(mini, sizeof(mini), false);
    strcpy((char *)mini + 8 + 8 + 32, "x86_64");
    CHECK(aot_xuansi_kind(mini, mini_size) == AOT_XUANSI_KIND_STOCK,
          "non-xtensa lit0/no-pool classifies STOCK");

    /* ---- 2. real fixture: stock load + run (non-regression) ---- */

    stock = read_file(argv[1], &stock_size);
    CHECK(stock != NULL, "stock fixture readable");
    if (stock) {
        AOTModule *aot;
        wasm_module_t module = NULL;
        wasm_module_inst_t inst = NULL;
        uint32 result = 0;

        CHECK(aot_xuansi_kind(stock, stock_size) == AOT_XUANSI_KIND_STOCK,
              "host stock fixture classifies STOCK");

        module = wasm_runtime_load(stock, stock_size, err, sizeof(err));
        CHECK(module != NULL, "stock fixture loads");
        if (module) {
            aot = (AOTModule *)module;
            CHECK(aot->xuansi_kind == (uint8)AOT_XUANSI_KIND_STOCK,
                  "module kind recorded as STOCK");
            CHECK(aot->xuansi_literal_owned == 0,
                  "stock literal not owned separately");
            inst = wasm_runtime_instantiate(module, 8192, 8192, err,
                                            sizeof(err));
            CHECK(inst != NULL, "stock fixture instantiates");
            if (inst) {
                CHECK(call_f(inst, &result) && result == 77,
                      "stock fixture f() == 77");
                CHECK(*(uint8 **)((uint8 *)inst + XUANSI_LITERAL_BASE_SLOT)
                          == NULL,
                      "stock instance slot stays NULL");
                wasm_runtime_deinstantiate(inst);
            }
            wasm_runtime_unload(module);
        }
    }

    /* ---- 3. transformed fixture: surgery load + place + slot + run ---- */

    if (stock) {
        AOTModule *aot;
        wasm_module_t module = NULL;
        wasm_module_inst_t inst = NULL;
        uint32 result = 0;
        uint8 *slot_ptr;

        surgery = transform_to_surgery(stock, stock_size, &surgery_size);
        CHECK(surgery != NULL, "transform produced a surgery container");
        if (surgery) {
            CHECK(aot_xuansi_kind(surgery, surgery_size)
                      == AOT_XUANSI_KIND_SURGERY,
                  "transformed fixture classifies SURGERY");

            module =
                wasm_runtime_load(surgery, surgery_size, err, sizeof(err));
            CHECK(module != NULL, "surgery fixture loads");
            if (module) {
                aot = (AOTModule *)module;
                CHECK(aot->xuansi_kind == (uint8)AOT_XUANSI_KIND_SURGERY,
                      "module kind recorded as SURGERY");
                CHECK(aot->xuansi_literal_owned == 1,
                      "surgery literal pool owned separately");
                CHECK(aot->literal != NULL
                          && aot->literal_size
                                 >= sizeof(pool_pattern),
                      "surgery pool placed, >= pattern size");
                if (aot->literal
                    && aot->literal_size >= sizeof(pool_pattern)) {
                    CHECK(memcmp(aot->literal, pool_pattern,
                                 sizeof(pool_pattern))
                              == 0,
                          "pool content survived placement");
                }
                /* pool must NOT alias the text mapping */
                CHECK(aot->literal < (uint8 *)aot->code
                          || aot->literal >= (uint8 *)aot->code
                                 + aot->code_size,
                      "pool outside the text mapping");

                inst = wasm_runtime_instantiate(module, 8192, 8192, err,
                                                sizeof(err));
                CHECK(inst != NULL, "surgery fixture instantiates");
                if (inst) {
                    slot_ptr = *(uint8 **)((uint8 *)inst
                                           + XUANSI_LITERAL_BASE_SLOT);
                    CHECK(slot_ptr == aot->literal,
                          "instance slot filled with the pool address");
                    CHECK(call_f(inst, &result) && result == 77,
                          "surgery fixture f() == 77 (code untouched)");
                    wasm_runtime_deinstantiate(inst);
                }
                wasm_runtime_unload(module); /* also frees the pool */
            }

            /* MALFORMED on real bytes: strip the pool section -> the
             * host arch makes it merely STOCK again (host codegen has
             * no pool), so for the MALFORMED leg on real bytes we
             * truncate instead: api reject, no crash. */
            {
                wasm_module_t m;
                m = wasm_runtime_load(surgery, surgery_size / 3, err,
                                      sizeof(err));
                CHECK(m == NULL, "truncated surgery rejected");
            }
        }
    }

    /* ---- 4. classify-only legs on extra fixtures (real products) ---- */

    for (int i = 2; i < argc; i++) {
        uint8 *buf;
        uint32 size = 0;
        buf = read_file(argv[i], &size);
        CHECK(buf != NULL, "extra fixture readable");
        if (buf) {
            AOTXuansiKind k = aot_xuansi_kind(buf, size);
            /* real products must classify decisively — stock products
             * (xtensa, literal embedded) or surgery products; a
             * MALFORMED verdict on a toolchain product is a bug */
            CHECK(k != AOT_XUANSI_KIND_MALFORMED,
                  "extra fixture classifies decisively (not MALFORMED)");
            printf("  %s: kind=%s\n", argv[i],
                   k == AOT_XUANSI_KIND_STOCK ? "STOCK" : "SURGERY");
            free(buf);
        }
    }

    printf("\nxuansi_aot_tests: %d checks, %d failures\n", g_checks,
           g_failures);
    return g_failures ? 1 : 0;
}
