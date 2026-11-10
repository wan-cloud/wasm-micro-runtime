/*
 * Copyright (C) 2019 Intel Corporation.  All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "platform_api_vmcore.h"
#include "platform_api_extension.h"
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
#include "soc/mmu.h"
#include "rom/cache.h"

#define MEM_DUAL_BUS_OFFSET (SOC_IROM_LOW - SOC_DROM_LOW)

#define in_ibus_ext(addr) \
    (((uint32)addr >= SOC_IROM_LOW) && ((uint32)addr < SOC_IROM_HIGH))

static portMUX_TYPE s_spinlock = portMUX_INITIALIZER_UNLOCKED;
#endif

/*
 * XUANSI VENDORED PATCH #3 (B10-T2, 2026-11-08) — executable AOT text via a
 * REAL esp_mmu_map PSRAM mapping on ESP32-S3.
 *
 * Why (both upstream 2.4.0~1 paths measured broken on S3; see
 * VENDORED-NOTES.md for evidence):
 *   (a) DUAL_BUS on: MEM_DUAL_BUS_OFFSET regression lands text in a dead
 *       window (load fails "invalid relocation offset"); with the constant
 *       fixed to +0x6000000 the I-bus alias still has NO MMU entry for heap
 *       PSRAM pages → first fetch faults (panic reboot);
 *   (b) DUAL_BUS off: os_mmap(EXEC)→MALLOC_CAP_EXEC, whose heap is empty in
 *       this app (measured free=0B) → "allocate memory failed".
 *
 * This patch (CONFIG_IDF_TARGET_ESP32S3 only) gives os_mmap(MMAP_PROT_EXEC)
 * a self-contained implementation:
 *   - page-aligned PSRAM backing allocation;
 *   - esp_mmu_map(paddr, MMU_TARGET_PSRAM0, EXEC|READ|WRITE,
 *     PADDR_SHARED) into the executable external virtual window — the D-bus
 *     heap mapping of the same physical page coexists, so writes through the
 *     plain heap pointer remain valid;
 *   - the returned pointer is directly writable AND executable: the loader
 *     memcpy/relocations and the I-fetches hit the same physical page; on
 *     S3 the external I/D caches are separate, so os_icache_flush() now
 *     performs Cache_WriteBack_All + ICache re-arm (the load path calls it
 *     once after all text writes, before the module is returned);
 *   - os_munmap() consults a small table to unmap + free the backing.
 *
 * Table bound: 16 concurrent executable mappings (instance texts + merged
 * data/text buffers; the app co-loads at most 4 WASM instances).
 */
#if defined(CONFIG_IDF_TARGET_ESP32S3)
#include "esp_heap_caps.h"
#include "esp_cache.h"
#include "esp_mmu_map.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/mmu_types.h"
#include "rom/cache.h"
#include "sdkconfig.h"

#define XUANSI_PAGE_SIZE CONFIG_MMU_PAGE_SIZE
#define XUANSI_MIRROR_MAX 16

typedef struct {
    void *exec;   /* executable virtual address (esp_mmu_map), page-aligned */
    void *dbus;   /* heap PSRAM pointer of the same physical pages */
    size_t map_size;
} xuansi_exec_map_t;

static xuansi_exec_map_t s_xuansi_maps[XUANSI_MIRROR_MAX];

static xuansi_exec_map_t *xuansi_map_find_by_exec(void *ptr)
{
    for (int i = 0; i < XUANSI_MIRROR_MAX; i++) {
        uint8_t *b = s_xuansi_maps[i].exec;
        if (b != NULL && (uint8_t *)ptr >= b
            && (uint8_t *)ptr < b + s_xuansi_maps[i].map_size) {
            return &s_xuansi_maps[i];
        }
    }
    return NULL;
}

/* XUANSI (H1 finding): mirror lookup by the D-bus (data) pointer.
 * os_mmap(EXEC) returns the D-side heap pointer (patch #3), and
 * aot_load_from_aot_file() ends with
 * os_icache_flush(module->code, module->code_size) where module->code is
 * exactly that D-side pointer - so the post-relocation sync used to miss
 * the exec-only table lookup and return without syncing at all.
 * Upstreamable candidate (deferred until A0-A3 verified). */
static xuansi_exec_map_t *xuansi_map_find_by_dbus(void *ptr)
{
    for (int i = 0; i < XUANSI_MIRROR_MAX; i++) {
        uint8_t *b = s_xuansi_maps[i].dbus;
        if (b != NULL && (uint8_t *)ptr >= b
            && (uint8_t *)ptr < b + s_xuansi_maps[i].map_size) {
            return &s_xuansi_maps[i];
        }
    }
    return NULL;
}

/* XUANSI (P3-T6, 2026-11-10): A1 internal-SRAM AOT text placement.
 *
 * Board measurement (P3-T5, canonical load states) proved the
 * EXEC|INTERNAL heap is structurally 0 B on this app's sdkconfig - every
 * internal heap region carries DRAM caps only - so a
 * heap_caps_malloc(EXEC) placement can never succeed. The A1 placement is
 * a static IRAM reserved region instead:
 *   - one .iram1 carve-out (inside .iram0.text, size tunable via
 *     XUANSI_AOT_EXEC_RESERVE_BYTES; 24 KB default covers every stock
 *     .aot text measured so far, the largest being st7789 at 20,684 B);
 *   - single occupancy: one module's text at a time (co-residency waits
 *     for the A3 PSRAM placement), os_mmap(EXEC) refuses when occupied;
 *   - zero-on-alloc (same discipline as patch #8: never hand out stale
 *     bytes from a previously unloaded module);
 *   - the region pointer is the plain IRAM address: readable, writable
 *     and executable through the same address (internal SRAM is
 *     single-alias and uncached on S3, so the loader's data writes and
 *     the I-fetches cohere without any bus translation).
 * SECTION CHOICE IS LOAD-BEARING (first board attempt): the S3 memory
 * protection (CONFIG_ESP_SYSTEM_MEMPROT_FEATURE, locked after boot)
 * splits IRAM at _iram_text_end - .iram0.text and below = R+X, while
 * .iram0.data/.iram0.bss above the split = R+W with NO execute. A
 * .iram.bss reserve executes into an int-wdt wedge on the first fetch;
 * the reserve must live inside the executable span, hence .iram1
 * (collected into .iram0.text). PROGBITS cost: the reserve occupies the
 * flash image (~24 KB of zeros copied at boot) - accepted, the app
 * partition has headroom.
 * Stock .aot never goes to the PSRAM exec path (patch #3 machinery) in
 * A1: the container-gate dispatch lands with the A3 surgery-version
 * modules (T10/T11). */
#ifndef XUANSI_AOT_EXEC_RESERVE_BYTES
#define XUANSI_AOT_EXEC_RESERVE_BYTES (24 * 1024)
#endif

static uint8_t s_xuansi_aot_exec[XUANSI_AOT_EXEC_RESERVE_BYTES]
    __attribute__((section(".iram1"), aligned(16)));
static uint8_t s_xuansi_aot_exec_used;

/* XUANSI VENDORED PATCH #13 (P3-T6w, 2026-11-10) — survive PMS
 * write-protection of the IRAM0 text span.
 *
 * The reserve lives in .iram1 (inside .iram0.text, below _iram_text_end)
 * because that is the only internal-SRAM span the S3 memory protection
 * leaves executable. Whenever the restrictive PMS configuration is active
 * that span is also WRITE-DENIED (W^X). The loader writing the reserve
 * then raises an IRAM0 PMS violation whose interrupt source is routed to
 * ETS_MEMACCESS_ERR_INUM - the SAME inum as the cache-error sources - so
 * xt_highint4 classifies it as PANIC_RSN_CACHEERR. A PMS violation leaves
 * the cache access-error statuses empty, esp_cache_err_acs_save_and_clr()
 * returns -1, and the panic arbitration parks this core (APP) in
 * busy_wait() at INTLEVEL 5 waiting for a core that never panics: a
 * whole-board freeze, int-wdt reboot on release builds. (Board evidence
 * 2026-11-10: CORE_1_IRAM0_PMS_MONITOR_2 = {VIOLATE_INTR, WR, LOADSTORE,
 * ADDR 0x4037bf50 = the reserve base}; the store itself is CANCELLED -
 * pattern readback read zero.)
 * Two layers keep the board alive and the failure honest:
 *   (a) write-probe the requested span up front (a write-denied store
 *       silently cancels) - refuse the mapping, no interrupt ever fires;
 *   (b) across the whole load window, unroute the four per-core PMS
 *       violation sources (to the invalid inum) and clear latched
 *       violations on exit, so a mid-load state flip degrades to a
 *       corrupt-text load failure instead of the panic-path deadlock.
 * The enforcement state varies across boots ("wedge phases" vs "green
 * phases", hours-scale, root not identified - it is not any fw/fork
 * code); the probe makes both phases safe and reports which one is live. */
#include "soc/interrupts.h"
#include "soc/sensitive_reg.h"

static uint8_t s_xuansi_pms_guard_depth;
static uint8_t s_xuansi_pms_unrouted;

/* Unroute the four per-core PMS violation sources to the invalid inum.
 * Once per boot (the unrouted state is kept for the whole boot: a latched
 * violation keeps its line asserted, and re-arming the routing while hot
 * would deliver the very interrupt we are defending against; a board
 * session with the sources unrouted was verified stable - the stores
 * stay DENIED either way, only the (broken) panic reporting is muted). */
static void
xuansi_pms_unroute_once(void)
{
    extern void esp_rom_route_intr_matrix(int cpu_core,
                                          uint32_t periph_intr_id,
                                          uint32_t cpu_intr_num);
    if (s_xuansi_pms_unrouted) {
        return;
    }
    s_xuansi_pms_unrouted = 1;
    esp_rom_route_intr_matrix(1, ETS_CORE1_IRAM0_PMS_INTR_SOURCE,
                              ETS_INVALID_INUM);
    esp_rom_route_intr_matrix(1, ETS_CORE1_DRAM0_PMS_INTR_SOURCE,
                              ETS_INVALID_INUM);
    esp_rom_route_intr_matrix(0, ETS_CORE0_IRAM0_PMS_INTR_SOURCE,
                              ETS_INVALID_INUM);
    esp_rom_route_intr_matrix(0, ETS_CORE0_DRAM0_PMS_INTR_SOURCE,
                              ETS_INVALID_INUM);
}

static void
xuansi_pms_guard_enter(void)
{
    if (s_xuansi_pms_guard_depth++ != 0) {
        return;
    }
    xuansi_pms_unroute_once();
}

/* Close the window: swallow anything latched (write-1-to-clear). The
 * routing stays unrouted for the rest of the boot (see unroute_once). */
static void
xuansi_pms_guard_exit(void)
{
    if (s_xuansi_pms_guard_depth == 0 || --s_xuansi_pms_guard_depth != 0) {
        return;
    }
    REG_SET_BIT(SENSITIVE_CORE_0_IRAM0_PMS_MONITOR_1_REG,
                SENSITIVE_CORE_0_IRAM0_PMS_MONITOR_VIOLATE_CLR);
    REG_SET_BIT(SENSITIVE_CORE_1_IRAM0_PMS_MONITOR_1_REG,
                SENSITIVE_CORE_1_IRAM0_PMS_MONITOR_VIOLATE_CLR);
    REG_SET_BIT(SENSITIVE_CORE_0_DRAM0_PMS_MONITOR_1_REG,
                SENSITIVE_CORE_0_DRAM0_PMS_MONITOR_VIOLATE_CLR);
    REG_SET_BIT(SENSITIVE_CORE_1_DRAM0_PMS_MONITOR_1_REG,
                SENSITIVE_CORE_1_DRAM0_PMS_MONITOR_VIOLATE_CLR);
}

/* Pattern-write/readback probe over the first/last words of the span
 * (permission granularity is the PMS slot/category, far coarser than the
 * span, so two words cover it). Returns 1 when the stores land. */
static int
xuansi_region_writable(size_t size)
{
    volatile uint32_t *b = (volatile uint32_t *)(void *)s_xuansi_aot_exec;
    size_t words = size / sizeof(uint32_t);
    if (words == 0) {
        words = 1;
    }
    uint32_t f = b[0], l = b[words - 1];
    b[0] = 0xA5A5F00Fu;
    b[words - 1] = 0x0F5AA5A5u;
    int ok = (b[0] == 0xA5A5F00Fu) && (b[words - 1] == 0x0F5AA5A5u);
    b[0] = f;
    b[words - 1] = l;
    return ok;
}

/* Single source for the fw-side budget gate and the caps advertisement
 * (aot_exec_budget_bytes): the fw must not hand-copy the reserve size. */
uint32_t
xuansi_aot_exec_budget(void)
{
    return (uint32_t)XUANSI_AOT_EXEC_RESERVE_BYTES;
}

/* Single source for the fw-side pre-gate (patch #13): probe whether the
 * reserve span is writable this boot, without disturbing resident text
 * (a resident module implies the probe passed for it; do not touch it). */
int
xuansi_aot_exec_writable(uint32_t size)
{
    if (size == 0 || size > XUANSI_AOT_EXEC_RESERVE_BYTES) {
        return 0;
    }
    if (s_xuansi_aot_exec_used) {
        return 1;
    }
    xuansi_pms_guard_enter();
    int ok = xuansi_region_writable(size);
    xuansi_pms_guard_exit();
    return ok;
}

static int
xuansi_ptr_in_aot_exec(const void *ptr)
{
    return (const uint8_t *)ptr >= s_xuansi_aot_exec
           && (const uint8_t *)ptr
                  < s_xuansi_aot_exec + XUANSI_AOT_EXEC_RESERVE_BYTES;
}

static void *
xuansi_mmap_exec_region(size_t size)
{
    if (size == 0 || size > XUANSI_AOT_EXEC_RESERVE_BYTES) {
        return NULL;
    }
    if (s_xuansi_aot_exec_used) {
        return NULL;   /* single-occupancy slot is taken (unload first) */
    }
    /* patch #13: guard first (the probe's own denied write would raise
     * the very interrupt we are defending against), then probe, then
     * zero-on-alloc. Probe failure closes the guard before returning. */
    xuansi_pms_guard_enter();
    if (!xuansi_region_writable(size)) {
        xuansi_pms_guard_exit();
        return NULL;   /* PMS write-denies the span this boot */
    }
    s_xuansi_aot_exec_used = 1;
    memset(s_xuansi_aot_exec, 0, size);
    return s_xuansi_aot_exec;
}

/* Third munmap branch (P3-T6): settle pointers inside the reserved region
 * by releasing the slot - NEVER by the generic free()/esp_mmu_unmap
 * paths below (the region is static storage). Range match on purpose:
 * the loader may hand an interior pointer (module->literal - 4 etc.),
 * any pointer within [base, base + size) releases the one slot (this
 * also closes the T4-review note about exact-match munmap semantics). */
static int
xuansi_munmap_exec_region(void *ptr)
{
    if (!xuansi_ptr_in_aot_exec(ptr)) {
        return 0;
    }
    s_xuansi_aot_exec_used = 0;
    xuansi_pms_guard_exit();   /* patch #13: close the write window */
    return 1;
}

/* Text-sync for the reserved region (P3-T6 ruling): one esp_cache_msync
 * M2C|INST over the line-rounded range clamped to the region, on the
 * direct pointer - internal SRAM has no dual-bus alias and is uncached
 * on S3, so this is defensive (same helper shape as xuansi_msync_pair,
 * which needs the two aliases only for PSRAM mappings). */
static void
xuansi_sync_exec_region(const void *ptr, size_t len)
{
    uint32_t dline = Cache_Get_DCache_Line_Size();
    uint32_t iline = Cache_Get_ICache_Line_Size();
    uint32_t line = (dline > iline) ? dline : iline;
    uint32_t off, start, end;

    if (line == 0) {
        line = 32;
    }
    off = (uint32_t)((const uint8_t *)ptr - s_xuansi_aot_exec);
    if (off >= XUANSI_AOT_EXEC_RESERVE_BYTES || len == 0) {
        return;
    }
    if (len > XUANSI_AOT_EXEC_RESERVE_BYTES - off) {
        len = XUANSI_AOT_EXEC_RESERVE_BYTES - off;
    }
    start = off & ~(line - 1);
    end = (off + (uint32_t)len + line - 1) & ~(line - 1);
    if (end > XUANSI_AOT_EXEC_RESERVE_BYTES) {
        end = XUANSI_AOT_EXEC_RESERVE_BYTES;
    }
    if (end <= start) {
        return;
    }
    esp_cache_msync(s_xuansi_aot_exec + start, end - start,
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C
                        | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
}


/* XUANSI (P3-T6): parked for A3 — os_mmap(EXEC) now serves the internal
 * IRAM reserved region above (single occupancy). This PSRAM mapper is
 * the placement machinery the A3 surgery-version dispatch (T10/T11)
 * revives; keep it compiled (marked unused until then) so the revival
 * is a routing change, not a resurrection from history.
 * P3-T10: revived — os_mmap(EXEC | MMAP_PROT_XUANSI_PSRAM) (surgery
 * containers only, routed by the loader's container-kind dispatch)
 * lands here. Stock containers never set the bit. */
static void *xuansi_mmap_exec(size_t size)
{
    size_t map_size =
        (size + XUANSI_PAGE_SIZE - 1) & ~(size_t)(XUANSI_PAGE_SIZE - 1);
    uint8_t *dbus = heap_caps_aligned_alloc(XUANSI_PAGE_SIZE, map_size,
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (dbus == NULL) {
            return NULL;
    }
    esp_paddr_t paddr = 0;
    mmu_target_t target = 0;
    if (esp_mmu_vaddr_to_paddr(dbus, &paddr, &target) != ESP_OK
        || target != MMU_TARGET_PSRAM0) {
            heap_caps_free(dbus);
        return NULL;
    }
    void *exec = NULL;
    esp_err_t rc = esp_mmu_map(paddr, map_size, MMU_TARGET_PSRAM0,
                               MMU_MEM_CAP_EXEC | MMU_MEM_CAP_READ,
                               ESP_MMU_MMAP_FLAG_PADDR_SHARED, &exec);
    /* XUANSI (follow-up #11): esp_mmu_map failure must fail honestly —
     * free the backing and return NULL. The old path entered the table
     * and returned dbus anyway, reporting success with a dead exec
     * window (xuansi_exec_alias would hand out an unusable pointer and
     * the first fetch through it would fault). */
    if (rc != ESP_OK) {
        heap_caps_free(dbus);
        return NULL;
    }
    for (int i = 0; i < XUANSI_MIRROR_MAX; i++) {
        if (s_xuansi_maps[i].exec == NULL) {
            s_xuansi_maps[i].exec = exec;
            s_xuansi_maps[i].dbus = dbus;
            s_xuansi_maps[i].map_size = map_size;
            /* 回 **dbus**：載入器的一切讀寫（literal/code 讀、relocation
             * 寫）走 D 窗＝cache 同步資料面；取指入口（func_ptrs）經
             * xuansi_exec_alias() 換 I 窗別名（aot_loader 修補 #5）。 */
            return dbus;
        }
    }
    esp_mmu_unmap(exec);
    heap_caps_free(dbus);
    return NULL;
}

/* D 面寫回＋I 面作廢（供 loader 在 text 複製後、讀取前呼叫——
 * 見 os_icache_flush 注記）。 */
void xuansi_sync_exec(void *exec, size_t len);
/* dbus→exec 別名（func_ptrs 用——資料面走 D 窗、取指走 I 窗）。 */
void *xuansi_exec_alias(void *dbus_ptr);

/* XUANSI (follow-up #9): os_mmap(EXEC) returns the D-bus pointer (patch
 * #3), but os_munmap() used to consult only the exec-window lookup — the
 * dbus pointer never hit, so esp_mmu_unmap() was never called and the
 * 16-slot table leaked one entry per AOT unload (table exhaustion after
 * 16 load/unload cycles even with PSRAM to spare). Natural companion of
 * the dbus lookup added in patch #7: exec domain first, dbus domain on
 * miss, hit = unmap the exec window (needs the mapping base, so pass
 * m->exec, not the caller's possibly-interior pointer) + free the PSRAM
 * backing + release the slot. Returns 1 when handled by this mechanism. */
static int
xuansi_munmap_exec(void *ptr)
{
    xuansi_exec_map_t *m = xuansi_map_find_by_exec(ptr);
    if (m == NULL)
        m = xuansi_map_find_by_dbus(ptr);
    if (m == NULL)
        return 0;
    esp_mmu_unmap(m->exec);
    heap_caps_free(m->dbus);
    m->exec = NULL;
    m->dbus = NULL;
    m->map_size = 0;
    return 1;
}

/* XUANSI VENDORED PATCH #15 (P3-T11, 2026-11-10) — release path for
 * the surgery literal pool (the P3-T11 board unload defect).
 *
 * The T10 surgery pool allocation (os_mmap(PROT_XUANSI_PSRAM,
 * non-EXEC), below) hands out a RAW heap_caps_malloc(SPIRAM|8BIT)
 * pointer with NO origin header — deliberately so: the pool is pure
 * data and never goes near the I-window. But every other non-EXEC
 * os_mmap() product on this platform uses the upstream origin-header
 * layout (size + 4 + sizeof(ptr), origin stored at ptr-4), and
 * os_free() (espidf_malloc.c) unwraps that header unconditionally.
 * aot_unload() releases the pool with os_munmap(literal), whose three
 * S3 branches (IRAM reserve, exec window table, generic) all miss the
 * raw pointer, so it fell through to os_free(): *(pool-4) reads the
 * heap block's own size field (0xF4 = 244 = the ledwatch pool size)
 * and free(0xF4) dies in a heap assert -> board panic (board
 * evidence: gdb backtrace aot_unload -> os_munmap -> os_free with
 * ptr=0xf4, captured at the consumer's instruction-level
 * root-cause session). The same fallthrough fires
 * on the load-failure cleanup path (hello import refusal), which is
 * why every unload-class matrix leg hung.
 *
 * Fix (root-cause option (b), narrowest face): a small exact-match
 * registry of live pool pointers. os_mmap() registers what it handed
 * out (exhaustion = honest NULL, the patch-#11 discipline — a pool we
 * cannot account for would be a guaranteed unload panic later);
 * os_munmap() gains a fourth branch that releases a registered pool
 * with heap_caps_free() and NEVER reaches the header-unwrap path.
 * STOCK semantics are untouched: stock containers never set
 * MMAP_PROT_XUANSI_PSRAM, so neither the registry nor the branch can
 * see a stock pointer. Host builds cannot catch this class (posix
 * os_munmap frees directly, no header convention) — the proof lives
 * in the board unload/reload cycles. */
#define XUANSI_POOL_MAX 4 /* concurrent surgery modules; the fw loads one at a time */
static void *s_xuansi_pools[XUANSI_POOL_MAX];

/* returns 1 when the pointer was a registered pool and is now freed */
static int
xuansi_munmap_pool(void *ptr)
{
    for (int i = 0; i < XUANSI_POOL_MAX; i++) {
        if (s_xuansi_pools[i] == ptr) {
            heap_caps_free(ptr);
            s_xuansi_pools[i] = NULL;
            return 1;
        }
    }
    return 0;
}

/* returns 0 on success; -1 when the registry is full (caller must
 * free the allocation and fail the mapping honestly) */
static int
xuansi_pool_register(void *ptr)
{
    for (int i = 0; i < XUANSI_POOL_MAX; i++) {
        if (s_xuansi_pools[i] == NULL) {
            s_xuansi_pools[i] = ptr;
            return 0;
        }
    }
    return -1;
}
#endif /* CONFIG_IDF_TARGET_ESP32S3 */

void *
os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    /* XUANSI: EXEC＝A1 內部 SRAM 保留區（P3-T6，單佔用槽；見上方注記）；
     * PSRAM esp_mmu_map 路（修補 #3）停等 A3 手術版派送（T10/T11）。
     * 非 EXEC 照舊 8BIT 對齊配置。
     * P3-T10: MMAP_PROT_XUANSI_PSRAM 復活 #3 映射器——手術版容器
     * （判別＝SURGERY）的 text 走 PSRAM 執行窗；stock 容器不帶此位，
     * 保留區路徑位元級不變。 */
    if (prot & MMAP_PROT_EXEC) {
        if (prot & MMAP_PROT_XUANSI_PSRAM) {
            return xuansi_mmap_exec(size);
        }
        return xuansi_mmap_exec_region(size);
    }
    if (prot & MMAP_PROT_XUANSI_PSRAM) {
        /* XUANSI (P3-T10): 手術版 literal 池＝純資料，PSRAM 一般資料堆
         * （heap_caps SPIRAM；主人裁定 2026-11-10）。池不經 esp_mmu_map
         * ——資料讀取走 D 窗本就安全。分配歸零（修補 #8 紀律）。
         * XUANSI (patch #15): the RAW pointer (no origin header) must be
         * registered so os_munmap() releases it with heap_caps_free()
         * instead of the header-unwrap fallthrough (see the registry
         * note above); registry exhaustion fails the load honestly. */
        void *buf = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!buf) {
            return NULL;
        }
        if (xuansi_pool_register(buf) != 0) {
            heap_caps_free(buf);
            return NULL;
        }
        memset(buf, 0, size);
        return buf;
    }
    void *buf_origin_8 =
        heap_caps_malloc(size + 4 + sizeof(uintptr_t), MALLOC_CAP_8BIT);
    if (!buf_origin_8) {
        return NULL;
    }
    void *buf_fixed_8 = buf_origin_8 + sizeof(void *);
    if ((uintptr_t)buf_fixed_8 & (uintptr_t)0x7) {
        buf_fixed_8 = (void *)((uintptr_t)(buf_fixed_8 + 4) & (~(uintptr_t)7));
    }
    uintptr_t *addr_field_8 = buf_fixed_8 - sizeof(uintptr_t);
    *addr_field_8 = (uintptr_t)buf_origin_8;
    /* XUANSI: zero-initialize the mapping. os_mmap() backs WASM linear
     * memory, which the spec requires to start zeroed (fresh .bss and
     * yet-unwritten data pages must read as 0). heap_caps_malloc() does
     * not zero, and unlike the non-S3 branches below this S3 fast path
     * never memset, so a fresh instance's linear memory carried heap
     * garbage (observed as a JPEG-encoder module reading a bogus default
     * DHT table from unzeroed memory). One memset over the mapped size
     * at allocation time. */
    memset(buf_fixed_8, 0, size);
    return buf_fixed_8;
#else
    if (prot & MMAP_PROT_EXEC) {
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
        uint32_t mem_caps = MALLOC_CAP_SPIRAM;
#else
        uint32_t mem_caps = MALLOC_CAP_EXEC;
#endif

        // Memory allocation with MALLOC_CAP_EXEC will return 4-byte aligned
        // Reserve extra 4 byte to fixup alignment and size for the pointer to
        // the originally allocated address
        void *buf_origin =
            heap_caps_malloc(size + 4 + sizeof(uintptr_t), mem_caps);
        if (!buf_origin) {
            return NULL;
        }
        void *buf_fixed = buf_origin + sizeof(void *);
        if ((uintptr_t)buf_fixed & (uintptr_t)0x7) {
            buf_fixed = (void *)((uintptr_t)(buf_fixed + 4) & (~(uintptr_t)7));
        }

        uintptr_t *addr_field = buf_fixed - sizeof(uintptr_t);
        *addr_field = (uintptr_t)buf_origin;
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
        memset(buf_fixed + MEM_DUAL_BUS_OFFSET, 0, size);
        return buf_fixed + MEM_DUAL_BUS_OFFSET;
#else
        memset(buf_fixed, 0, size);
        return buf_fixed;
#endif
    }
    else {
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
        uint32_t mem_caps = MALLOC_CAP_SPIRAM;
#else
        uint32_t mem_caps = MALLOC_CAP_8BIT;
#endif
        void *buf_origin =
            heap_caps_malloc(size + 4 + sizeof(uintptr_t), mem_caps);
        if (!buf_origin) {
            return NULL;
        }

        // Memory allocation with MALLOC_CAP_SPIRAM or MALLOC_CAP_8BIT will
        // return 4-byte aligned Reserve extra 4 byte to fixup alignment and
        // size for the pointer to the originally allocated address
        void *buf_fixed = buf_origin + sizeof(void *);
        if ((uintptr_t)buf_fixed & (uintptr_t)0x7) {
            buf_fixed = (void *)((uintptr_t)(buf_fixed + 4) & (~(uintptr_t)7));
        }

        uintptr_t *addr_field = buf_fixed - sizeof(uintptr_t);
        *addr_field = (uintptr_t)buf_origin;

        memset(buf_fixed, 0, size);
        return buf_fixed;
    }
#endif
}

void *
os_mremap(void *old_addr, size_t old_size, size_t new_size)
{
    return os_mremap_slow(old_addr, old_size, new_size);
}

void
os_munmap(void *addr, size_t size)
{
    char *ptr = (char *)addr;

#if defined(CONFIG_IDF_TARGET_ESP32S3)
    /* XUANSI（P3-T6 第三分支——先行）：保留區內指針＝釋放單佔用槽。
     * **絕不**落到通用 free()/esp_mmu_unmap——保留區是靜態儲存。
     * 範圍查找（起訖界內非精確匹配）。 */
    if (xuansi_munmap_exec_region(ptr)) {
        return;
    }
    /* XUANSI：PSRAM 執行映射走表收帳（esp_mmu_unmap＋釋放 PSRAM 背襯）。
     * os_mmap(EXEC) 返回 **dbus** 指針（修補 #3）——收帳查找須涵蓋
     * dbus 域（追收 #9，見 xuansi_munmap_exec 注記）。 */
    if (xuansi_munmap_exec(ptr)) {
        return;
    }
    /* XUANSI (patch #15): 手術版 literal 池＝裸 heap_caps 指針（無 origin
     * header）——精確命中登記表即 heap_caps_free 直釋，**絕不**落到
     * os_free 的 header 解包路（讀池前 4B＝heap 塊自身 size 欄位＝
     * free(0xF4) 崩潰；見上方登記表注記）。 */
    if (xuansi_munmap_pool(ptr)) {
        return;
    }
#endif

#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
    if (in_ibus_ext(ptr)) {
        ptr -= MEM_DUAL_BUS_OFFSET;
    }
#endif
    // We don't need special handling for the executable allocations
    // here, free() of esp-idf handles it properly
    return os_free(ptr);
}

int
os_mprotect(void *addr, size_t size, int prot)
{
    return 0;
}

void
#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
    IRAM_ATTR
#endif
    os_dcache_flush()
{
#if (WASM_MEM_DUAL_BUS_MIRROR != 0) && !defined(CONFIG_IDF_TARGET_ESP32S3)
    uint32_t preload;
    extern void Cache_WriteBack_All(void);

    portENTER_CRITICAL(&s_spinlock);

    Cache_WriteBack_All();
    preload = Cache_Disable_ICache();
    Cache_Enable_ICache(preload);

    portEXIT_CRITICAL(&s_spinlock);
#else
    /* XUANSI (B10-T2): ROM Cache_Disable/Enable_ICache 全域輪舞在 S3 上
     * 實測**殺死 systimer**（首次 AOT load 後全 fw esp_timer/uptime 凍結
     * ——ping uptime_us 三連同值實證）。S3 的精確同步改在
     * os_icache_flush(start,len) 以 esp_cache_msync 完成（cache-safe/
     * thread-safe API），本全域函數退為 no-op。 */
#endif
}

void *xuansi_exec_alias(void *dbus_ptr)
{
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    /* XUANSI (P3-T6): reserved-region pointers are their own exec alias
     * (internal SRAM is single-alias) - identity, not a table lookup. */
    if (xuansi_ptr_in_aot_exec(dbus_ptr)) {
        return dbus_ptr;
    }
    for (int i = 0; i < XUANSI_MIRROR_MAX; i++) {
        xuansi_exec_map_t *m = &s_xuansi_maps[i];
        if (m->exec == NULL) continue;
        if ((uint8_t *)dbus_ptr >= (uint8_t *)m->dbus
            && (uint8_t *)dbus_ptr < (uint8_t *)m->dbus + m->map_size) {
            return (uint8_t *)m->exec
                   + ((uint8_t *)dbus_ptr - (uint8_t *)m->dbus);
        }
    }
#endif
    return dbus_ptr;
}

/* XUANSI (H1 finding, verified on board 2026-11-08): esp_cache_msync()
 * requires addr % line == 0 && size % line == 0 and returns
 * ESP_ERR_INVALID_ARG (258) otherwise, and the M2C direction rejects
 * ESP_CACHE_MSYNC_FLAG_UNALIGNED outright. The loader's length is only
 * 4-byte aligned (total_size = (n + 3) & ~3), so both raw msync pairs
 * below used to return 258 and the sync silently did nothing.
 *
 * Fix: round [off, off + len) outward to cache-line boundaries (line size
 * queried at runtime, max of I/D; 32 B for both on ESP32-S3 external
 * memory), clamped to the mapping - map_size is page-aligned, so the
 * clamp keeps line alignment. With this helper both msyncs return ESP_OK
 * (measured) and AOT load proceeds past the point where it used to wedge.
 * Upstreamable candidate (deferred until A0-A3 verified). */
#if defined(CONFIG_IDF_TARGET_ESP32S3)
static void
xuansi_msync_pair(const xuansi_exec_map_t *m, uint32_t off, uint32_t len)
{
    uint32_t line, start, end;

    if (m == NULL || len == 0)
        return;
    /* XUANSI (first-compile fix): patch #6 assumed
     * esp_cache_get_{d,i}cache_line_size(&int) helpers that do not
     * exist in IDF v5.5.5 — the ESP32-S3 ROM accessors (rom/cache.h,
     * already included) return the line size directly. */
    uint32_t dline = Cache_Get_DCache_Line_Size();
    uint32_t iline = Cache_Get_ICache_Line_Size();
    line = (dline > iline ? dline : iline);
    if (line == 0)
        line = 32;

    start = off & ~(line - 1);
    end = (off + len + line - 1) & ~(line - 1);
    if (end > m->map_size)
        end = m->map_size;
    if (end <= start)
        return;

    /* write back the D alias, invalidate (and refill) the I alias */
    esp_cache_msync((uint8_t *)m->dbus + start, end - start,
                    ESP_CACHE_MSYNC_FLAG_DIR_C2M
                        | ESP_CACHE_MSYNC_FLAG_TYPE_DATA);
    esp_cache_msync((uint8_t *)m->exec + start, end - start,
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C
                        | ESP_CACHE_MSYNC_FLAG_TYPE_INST);
}
#endif /* CONFIG_IDF_TARGET_ESP32S3 */

static void xuansi_sync_dbus_impl(void *dbus_base, size_t len)
{
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    if (dbus_base == NULL || len == 0) return;
    xuansi_exec_map_t *m = xuansi_map_find_by_dbus(dbus_base);
    if (m == NULL) return;
    uint32_t off =
        (uint32_t)((uint8_t *)dbus_base - (uint8_t *)m->dbus);
    uint32_t rest = (uint32_t)m->map_size - off;
    if (len > rest) len = rest;
    xuansi_msync_pair(m, off, len);
#endif
}

void xuansi_sync_exec(void *ptr, size_t len)
{
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    /* 接受 dbus 或 exec 指針（載入器兩種都會傳）。
     * P3-T6：保留區指針先行——單別名同步（見 xuansi_sync_exec_region）。 */
    if (xuansi_ptr_in_aot_exec(ptr)) {
        xuansi_sync_exec_region(ptr, len);
        return;
    }
    xuansi_exec_map_t *m = xuansi_map_find_by_exec(ptr);
    if (m != NULL) {
        uint8_t *base = (uint8_t *)ptr;
        uint32_t off = (uint32_t)(base - (uint8_t *)m->exec);
        uint32_t rest = (uint32_t)m->map_size - off;
        if (len > rest) len = rest;
        xuansi_msync_pair(m, off, len);
        return;
    }
    xuansi_sync_dbus_impl(ptr, len);
#else
    /* XUANSI (follow-up #10): the symbol is declared outside the S3
     * guard (aot_loader's DUAL_BUS call site carries no target guard)
     * but the table and msync machinery exist only for S3 — guard the
     * body like xuansi_sync_dbus_impl, so non-S3 targets compile and
     * take the upstream no-sync path. */
    (void)ptr;
    (void)len;
#endif
}

void
os_icache_flush(void *start, size_t len)
{
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    /* XUANSI：S3 外部記憶體 I/D cache 分立——text 寫入走 D 面（鏡像），
     * 取指走 I 面：寫回 D 面該範圍（C2M|DATA）＋作廢 I 面該範圍（M2C|
     * INST，miss 時自 PSRAM 重取——D 面寫回已使其為最新）。aot_load_
     * from_aot_file 在 load 收尾統一呼叫（載入期所有 memcpy/relocation
     * 已完成）。**不得**改用 ROM Cache_Disable/Enable_ICache 全域輪舞
     * ——S3 實測殺 systimer（見 os_dcache_flush 注記）。 */
    if (start == NULL || len == 0) return;
    /* XUANSI（P3-T6）：保留區＝單別名同步（內部 SRAM——M2C|INST 一發，
     * 無雙匯流排鏡像）。 */
    if (xuansi_ptr_in_aot_exec(start)) {
        xuansi_sync_exec_region(start, len);
        /* patch #13: aot_load_from_aot_file ends the load here (this is
         * the last touch of the region); close the PMS write window. */
        xuansi_pms_guard_exit();
        return;
    }
    xuansi_exec_map_t *m = xuansi_map_find_by_exec(start);
    uint8_t *base = (uint8_t *)start;
    uint32_t off;
    if (m != NULL) {
        off = (uint32_t)(base - (uint8_t *)m->exec);
    }
    else if ((m = xuansi_map_find_by_dbus(start)) != NULL) {
        /* XUANSI (H1 finding): the loader passes module->code, which is
         * the D-side pointer since os_mmap(EXEC) returns the dbus
         * address (patch #3) - accept it here, not just the exec alias. */
        off = (uint32_t)(base - (uint8_t *)m->dbus);
    }
    else {
        return;
    }
    uint32_t rest = (uint32_t)m->map_size - off;
    if (len > rest) len = rest;
    xuansi_msync_pair(m, off, len);
#else
    (void)start;
    (void)len;
#endif
}

#if (WASM_MEM_DUAL_BUS_MIRROR != 0)
void *
os_get_dbus_mirror(void *ibus)
{
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    /* XUANSI: 表查（本檔的 esp_mmu_map 映射位址≠dbus+常數——esp_mm 自選
     * 執行視窗槽位）。表外位址＝非本機制記帳的指針，原樣返回。
     * P3-T6：保留區指針＝自身鏡像（內部 SRAM 單別名——顯式短路，不依賴
     * in_ibus_ext 對內部位址的否定判定）。 */
    if (xuansi_ptr_in_aot_exec(ibus)) {
        return ibus;
    }
    xuansi_exec_map_t *m = xuansi_map_find_by_exec(ibus);
    if (m != NULL) {
        return m->dbus + ((uint8_t *)ibus - (uint8_t *)m->exec);
    }
#endif
    if (in_ibus_ext(ibus)) {
        return (void *)((char *)ibus - MEM_DUAL_BUS_OFFSET);
    }
    else {
        return ibus;
    }
}
#endif
