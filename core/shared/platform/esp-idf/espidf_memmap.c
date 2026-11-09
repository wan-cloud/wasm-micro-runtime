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
#endif /* CONFIG_IDF_TARGET_ESP32S3 */

void *
os_mmap(void *hint, size_t size, int prot, int flags, os_file_handle file)
{
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    /* XUANSI: EXEC＝真實 MMU 映射（見上注）；非 EXEC 照舊 8BIT 對齊配置。 */
    if (prot & MMAP_PROT_EXEC) {
        return xuansi_mmap_exec(size);
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
    /* XUANSI：執行映射走表收帳（esp_mmu_unmap＋釋放 PSRAM 背襯）。
     * os_mmap(EXEC) 返回 **dbus** 指針（修補 #3）——收帳查找須涵蓋
     * dbus 域（追收 #9，見 xuansi_munmap_exec 注記）。 */
    if (xuansi_munmap_exec(ptr)) {
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
    /* 接受 dbus 或 exec 指針（載入器兩種都會傳）。 */
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
     * 執行視窗槽位）。表外位址＝非本機制記帳的指針，原樣返回。 */
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
