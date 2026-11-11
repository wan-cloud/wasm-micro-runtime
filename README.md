# WebAssembly Micro Runtime


**A [Bytecode Alliance][BA] project**

[BA]: https://bytecodealliance.org/

**[Guide](https://wamr.gitbook.io/)**&emsp;&emsp;**[Website](https://bytecodealliance.github.io/wamr.dev)**&emsp;&emsp;**[Chat](https://bytecodealliance.zulipchat.com/#narrow/stream/290350-wamr)**

[Build WAMR](./doc/build_wamr.md) | [Build AOT Compiler](./wamr-compiler/README.md) | [Embed WAMR](./doc/embed_wamr.md) | [Export Native API](./doc/export_native_api.md) | [Build Wasm Apps](./doc/build_wasm_app.md) | [Samples](./samples/README.md)

## About this fork (branch `xuansi/base-2.4.0`)

This is a downstream fork of `espressif/wasm-micro-runtime` pinned at the
component-registry release `2.4.0~1` (commit `8f806e0f`, branch
`esp_based_on_v2.4.0`), carrying a series of small ESP32-S3 patches that
the consuming project needs. The patch series is kept as a run of clean
commits on top of that pin so it can be rebased, reviewed, cherry-picked
or upstreamed piecewise. In-code comments mentioning "VENDORED-NOTES.md"
refer to the patch table below.

### Patch table (relative to espressif/wasm-micro-runtime 2.4.0~1)

| # | Commit | Files | Content |
|---|--------|-------|---------|
| 1 | `esp-idf: fix MEM_DUAL_BUS_OFFSET regression` | `core/shared/platform/esp-idf/espidf_memmap.c` | restore the `ada7e3fe` (WAMR #2385) arithmetic: `SOC_IROM_LOW - SOC_DROM_LOW` = +0x6000000; the regressed negative offset landed every heap PSRAM pointer in a dead window and AOT load always failed with `invalid relocation offset` |
| 2 | `esp-idf: keep WASM_MEM_DUAL_BUS_MIRROR on for S3` | `core/shared/platform/esp-idf/shared_platform.cmake` | the S3 dual-bus define stays on, with a note that the ESP32-classic constant dual-bus alias does not hold for heap PSRAM pages on S3 — patch 3 provides the real mapping |
| 3 | `esp-idf: S3 os_mmap(EXEC) via real esp_mmu_map` | `core/shared/platform/esp-idf/espidf_memmap.c` | `os_mmap(EXEC)` = page-aligned PSRAM backing + `esp_mmu_map(paddr, PSRAM0, EXEC\|READ, PADDR_SHARED)`; a 16-entry `{exec, dbus, map_size}` table; returns the D-bus pointer, `xuansi_exec_alias()` serves I-window aliases; `os_dcache_flush()` retires to a no-op on S3 (the ROM global I-cache round-dance measurably kills the systimer) |
| 4 | `aot: hook S3 dual-window exec mapping at the four text write/read sites` | `core/iwasm/aot/aot_loader.c` | PLT init and tail zero-fill write through `os_get_dbus_mirror()`; text copy is followed by `xuansi_sync_exec()` before any I-side read; `func_ptrs` entries switch to the exec alias |
| 5 | `esp-idf: wamr component REQUIRES esp_mm` | `build-scripts/esp-idf/wamr/CMakeLists.txt` | declare the `esp_mm` dependency introduced by patch 3 |
| 6 | `esp-idf: line-aligned clamped msync pair` | `core/shared/platform/esp-idf/espidf_memmap.c` | `xuansi_msync_pair()`: rounds `[off, off+len)` outward to cache-line boundaries and clamps to the mapping before the C2M\|DATA writeback (D alias) and M2C\|INST invalidate (I alias); all three sync sites route through it. Root cause: `esp_cache_msync()` silently returned `ESP_ERR_INVALID_ARG` (258) for the loader's 4-byte-aligned ranges, so the load-time sync was a no-op; M2C rejects `FLAG_UNALIGNED`, so the call sites could not be fixed with flags alone (upstreamable candidate, deferred until A0-A3 verified) |
| 7 | `esp-idf: os_icache_flush must accept the D-side mirror pointer` | `core/shared/platform/esp-idf/espidf_memmap.c` | `xuansi_map_find_by_dbus()`: `aot_load_from_aot_file()` flushes with `module->code`, which is the D-side pointer since patch 3 returns the dbus address — the exec-only lookup missed and the post-relocation sync never ran (upstreamable candidate, deferred until A0-A3 verified) |
| 8 | `esp-idf: zero-initialize os_mmap allocations` | `core/shared/platform/esp-idf/espidf_memmap.c` | the S3 non-EXEC fast path of `os_mmap()` returned unzeroed `heap_caps_malloc()` memory while both non-S3 branches memset; os_mmap backs WASM linear memory, which the spec requires to start zeroed — a JPEG-encoder module once read a bogus default DHT table from the garbage |
| 9 | `esp-idf: os_munmap must settle dbus-domain mappings too` | `core/shared/platform/esp-idf/espidf_memmap.c` | `os_mmap(EXEC)` returns the dbus pointer (patch 3) but `os_munmap()` looked up only the exec window, so the dbus pointer never hit: `esp_mmu_unmap()` never ran and the 16-slot table leaked one entry per AOT unload. `xuansi_munmap_exec()` now checks the exec domain first, dbus on miss (mirror of patch 7) and unmaps the recorded mapping base |
| 10 | `esp-idf: guard xuansi_sync_exec body for non-S3 targets` | `core/shared/platform/esp-idf/espidf_memmap.c` | `xuansi_sync_exec()` lives outside the S3 guard (aot_loader's DUAL_BUS call site has no target guard) but its body referenced S3-guarded types — hard compile error on non-S3. Body now guarded like `xuansi_sync_dbus_impl`; non-S3 takes the upstream no-sync path |
| 11 | `esp-idf: fail xuansi_mmap_exec honestly when esp_mmu_map fails` | `core/shared/platform/esp-idf/espidf_memmap.c` | the `esp_mmu_map()` return code was ignored: on failure the dead exec window was still tabled and the dbus pointer returned as success (broken alias, burnt slot). Now rc≠`ESP_OK` frees the backing and returns NULL |
| 12 | `esp-idf: A1 AOT text placement via static IRAM reserved region` | `core/shared/platform/esp-idf/espidf_memmap.c` | board measurement proved the EXEC\|INTERNAL heap structurally 0 B on this app's sdkconfig, so `os_mmap(EXEC)` now serves a single-occupancy static `.iram.bss` reserve (`XUANSI_AOT_EXEC_RESERVE_BYTES`, 24 KB default, zero-on-alloc like patch 8); `os_munmap` gains a third branch that range-matches region pointers and releases the slot (never the generic free path); the region pointer is its own exec/dbus alias (single-alias internal SRAM) and syncs via one `esp_cache_msync(M2C\|INST)`; `xuansi_aot_exec_budget()` exports the size so the consumer's budget gate and caps stay single-sourced. The patch-3 PSRAM mapper stays compiled but parked until the A3 surgery-version dispatch (T10/T11) |
| 13 | `esp-idf: survive PMS write-protection of the IRAM0 text span (T6w wedge)` | `core/shared/platform/esp-idf/espidf_memmap.c` | the `.iram1` reserve sits in `.iram0.text` — the only executable internal-SRAM span, and under the restrictive S3 PMS configuration also WRITE-DENIED (W^X). Loader writes there raise an IRAM0 PMS violation whose source shares `ETS_MEMACCESS_ERR_INUM` with the cache-error interrupts, so `xt_highint4` misclassifies it as `PANIC_RSN_CACHEERR`; `esp_cache_err_acs_save_and_clr()` finds no cache status, returns -1, and the panic arbitration parks APP CPU in `busy_wait()` at INTLEVEL 5 forever = whole-board freeze (board evidence: `CORE_1_IRAM0_PMS_MONITOR_2 = {WR, LOADSTORE, ADDR = reserve base}`, stores cancelled, readback zero). Fix layers: (a) pattern write/readback probe of the span before mapping — denied ⇒ refuse, no interrupt ever fires (`xuansi_aot_exec_writable()` is the fw-side pre-gate export); (b) the four per-core PMS violation sources are unrouted (invalid inum) across the whole load window and latches cleared on exit, so a mid-load state flip degrades to a corrupt-text load failure instead of the deadlock. The unrouted state is kept for the whole boot (re-arming while a violation line is hot re-delivers it). Enforcement varies across boots ("wedge"/"green" phases, hours-scale, root unidentified — no fw/fork code drives it); both phases are now safe |
| 14 | `aot: xuansi literal-base slot in the instance struct (offset 320)` + 3 follow-ups (`aot: emit the literal pool into the xuansi.literal section`, `aot: loader dual-path placement + container-kind classifier`, `tests: host unit tests`) | `core/iwasm/aot/xuansi_aot.h`, `core/iwasm/interpreter/wasm_runtime.h`, `core/iwasm/aot/aot_runtime.{h,c}`, `core/iwasm/compilation/aot_emit_aot_file.c`, `core/iwasm/compilation/aot_llvm.{h,c}`, `core/iwasm/include/aot_comp_option.h`, `wamr-compiler/main.c`, `core/iwasm/aot/aot_loader.c`, `core/iwasm/aot/arch/aot_reloc_xtensa.c`, `core/shared/platform/include/platform_api_vmcore.h`, `core/shared/platform/esp-idf/espidf_memmap.c`, `tests/xuansi/` | P3-T10 surgery (spec 5.3.2/5.3.3). wamrc gains the surgery container shape (`--enable/--disable-xuansi-surgery`, default ON for xtensa, one switch governs container shape AND the LLVM literal-base codegen so they can never disagree): the literal bytes move to a CUSTOM section `xuansi.literal` emitted between EXPORT and RELOCATION, text `literal_size == 0`, container version stays v4. The loader classifies containers by section structure (`aot_xuansi_kind()` → SURGERY/STOCK/MALFORMED), places surgery text to the revived patch-3 PSRAM exec window and the pool to a separate PSRAM data-heap allocation (`MMAP_PROT_XUANSI_PSRAM` routing bit; stock keeps the A1 path bit-identical), and fills the new instance-struct slot at offset 320 (carved from the reserved tail so every stock-baked offset — cur_exception 104, global_table_data 344 — stays frozen; `~32KB` module pool size limit comes from the LLVM side, see the fork README of wan-cloud/llvm-project). Emitting the pool section before RELOCATION doubles as the old-runtime honest refusal: pre-T10 loaders reject the section order instead of ever running a surgery product. Xtensa `R_XTENSA_SLOT0_OP` relocations inside a surgery container fail loud (pairing mismatch); stock reloc semantics untouched |
| 15 | `esp-idf: surgery pool release via registry (patch #15)` + follow-up (`tests: unload/reload and load-failure cleanup vectors`) | `core/shared/platform/esp-idf/espidf_memmap.c`, `tests/xuansi/` | P3-T11 unload defect (instruction-level board root cause, gdb backtrace `aot_unload -> os_munmap -> os_free` with `ptr=0xf4`): the T10 pool allocation hands out a RAW `heap_caps_malloc(SPIRAM\|8BIT)` pointer with no origin header, but every other non-EXEC `os_mmap()` product on esp-idf uses the upstream origin-header layout and `os_free()` unwraps it unconditionally — `aot_unload()`'s `os_munmap(pool)` missed all three S3 branches, fell through to `os_free()`, read the heap block's own size field at `*(pool-4)` (0xF4 = 244 = the ledwatch pool size) and `free(0xf4)` died in a heap assert (board panic; the same fallthrough fired on the hello import-refusal cleanup). Fix = root-cause option (b): a 4-slot exact-match registry of live pool pointers; `os_mmap(PROT_XUANSI_PSRAM, non-EXEC)` registers what it handed out (exhaustion = honest NULL, the #11 discipline), `os_munmap()` gains a fourth branch releasing a registered pool with `heap_caps_free()` and never reaching the header-unwrap path. STOCK untouched (stock never sets the bit). Host unit vectors pin the loader-side accounting (load→unload→reload ×2 with all placement contracts; post-placement RELOCATION failure driving the orphaned-pool cleanup, ×2, then a healthy cycle); the routing itself is board-only (posix `os_munmap` frees directly — host cannot catch the class) |
| 16 | `xuansi #16: gate the IRAM AOT lane (#12/#13) behind XUANSI_AOT_IRAM_LANE, default off — the fw retires the internal lane in 0.4.0 (all .aot rides PSRAM exec); surgery path (#3/#15/kind gate) untouched` | `core/shared/platform/esp-idf/espidf_memmap.c`, `core/shared/platform/esp-idf/shared_platform.cmake` | the internal-SRAM AOT lane (patches #12/#13: static `.iram1` reserve + `xuansi_aot_exec_budget()`, PMS write-probe + `xuansi_aot_exec_writable()` + per-boot unroute guard, munmap third branch, the reserve short-circuits in exec_alias/sync/icache_flush/dbus_mirror) becomes compile-time optional behind the CMake option `XUANSI_AOT_IRAM_LANE`, **default OFF**. OFF: no reserve is placed, both exports and the whole #13 machinery are not compiled, and `os_mmap(EXEC)` without `MMAP_PROT_XUANSI_PSRAM` refuses honestly (NULL — there is no internal placement; consumers must drop the two symbol references first). ON: bit-identical to the pre-#16 state (rollback leg; verified by preprocessing both states with the real xtensa toolchain — code-identical). UNCONDITIONAL (red line): patch #3 PSRAM exec window, patch #15 pool registry/`os_munmap` fourth branch, and the loader's `aot_xuansi_kind()` dispatch — the surgery lane fw 0.4.0 rides |

### Known residual

The `esp_mmu_map(EXEC|READ)` window is instruction-fetch-capable, but
data reads through it are not coherent/safe at runtime on this chip/IDF
combination; general AOT text that uses literal pools or embedded data
addresses is still gated off downstream until that is resolved. Patches
1-2, 6-8 are independent of that issue and stand on their own.

### Upgrade procedure (component upgrade)

1. Re-pin this branch onto the new base (the registry archive's
   `idf_component.yml` records its `repository_info.commit_sha`).
2. Check upstream for each patch above (the core ones are #3 exec alias
   coherence and #1 the constant). If fixed upstream, drop the patch;
   otherwise replay it — this table plus the commit series is the replay
   checklist.
3. Re-verify on target: AOT load/instantiate, native symbol registration,
   pthread flow, on-board load/call.


WebAssembly Micro Runtime (WAMR) is a lightweight standalone WebAssembly (Wasm) runtime with small footprint, high performance and highly configurable features for applications cross from embedded, IoT, edge to Trusted Execution Environment (TEE), smart contract, cloud native and so on. It includes a few parts as below:
- [**VMcore**](./core/iwasm/): A set of runtime libraries for loading and running Wasm modules. It supports rich running modes including interpreter, Ahead-of-Time compilation(AoT) and Just-in-Time compilation (JIT). WAMR supports two JIT tiers - Fast JIT, LLVM JIT, and dynamic tier-up from Fast JIT to LLVM JIT.
- [**iwasm**](./product-mini/): The executable binary built with WAMR VMcore which supports WASI and command line interface.
- [**wamrc**](./wamr-compiler/): The AOT compiler to compile Wasm file into AOT file
- Useful components and tools for building real solutions with WAMR vmcore:
  - [App-framework](https://github.com/bytecodealliance/wamr-app-framework/blob/main/app-framework/README.md): A framework for supporting APIs for the Wasm applications
  - [App-manager](https://github.com/bytecodealliance/wamr-app-framework/blob/main/app-mgr/README.md): A framework for dynamical loading the Wasm module remotely
  - [WAMR-IDE](./test-tools/wamr-ide): An experimental VSCode extension for developping WebAssembly applications with C/C++


### Key features
- Full compliant to the W3C Wasm MVP
- Small runtime binary size (core vmlib on cortex-m4f with tail-call/bulk memory/shared memory support, text size from bloaty)
  * ~58.9K for fast interpreter
  * ~56.3K for classic interpreter
  * ~29.4K for aot runtime
  * ~21.4K for libc-wasi library
  * ~3.7K for libc-builtin library
- Near to native speed by AOT and JIT
- Self-implemented AOT module loader to enable AOT working on Linux, Windows, MacOS, Android, SGX and MCU systems
- Choices of Wasm application libc support: the built-in libc subset for the embedded environment or [WASI](https://github.com/WebAssembly/WASI) for the standard libc
- [The simple C APIs to embed WAMR into host environment](./doc/embed_wamr.md), see [how to integrate WAMR](./doc/embed_wamr.md) and the [API list](./core/iwasm/include/wasm_export.h)
- [The mechanism to export native APIs to Wasm applications](./doc/export_native_api.md), see [how to register native APIs](./doc/export_native_api.md)
- [Multiple modules as dependencies](./doc/multi_module.md), ref to [document](./doc/multi_module.md) and [sample](samples/multi-module)
- [Multi-thread, pthread APIs and thread management](./doc/pthread_library.md), ref to [document](./doc/pthread_library.md) and [sample](samples/multi-thread)
- [wasi-threads](./doc/pthread_impls.md#wasi-threads-new), ref to [document](./doc/pthread_impls.md#wasi-threads-new) and [sample](samples/wasi-threads)
- [Linux SGX (Intel Software Guard Extension) support](./doc/linux_sgx.md), ref to [document](./doc/linux_sgx.md)
- [Source debugging support](./doc/source_debugging.md), ref to [document](./doc/source_debugging.md)
- [XIP (Execution In Place) support](./doc/xip.md), ref to [document](./doc/xip.md)
- [Berkeley/Posix Socket support](./doc/socket_api.md), ref to [document](./doc/socket_api.md) and [sample](./samples/socket-api)
- [Multi-tier JIT](./product-mini#linux) and [Running mode control](https://bytecodealliance.github.io/wamr.dev/blog/introduction-to-wamr-running-modes/)
- Language bindings: [Go](./language-bindings/go/README.md), [Python](./language-bindings/python/README.md), [Rust](./language-bindings/rust/README.md)

### Wasm post-MVP features
- [wasm-c-api](https://github.com/WebAssembly/wasm-c-api), ref to [document](doc/wasm_c_api.md) and [sample](samples/wasm-c-api)
- [128-bit SIMD](https://github.com/WebAssembly/simd), ref to [samples/workload](samples/workload)
- [Reference Types](https://github.com/WebAssembly/reference-types), ref to [document](doc/ref_types.md) and [sample](samples/ref-types)
- [Bulk memory operations](https://github.com/WebAssembly/bulk-memory-operations), [Shared memory](https://github.com/WebAssembly/threads/blob/main/proposals/threads/Overview.md#shared-linear-memory), [Memory64](https://github.com/WebAssembly/memory64)
- [Tail-call](https://github.com/WebAssembly/tail-call), [Garbage Collection](https://github.com/WebAssembly/gc), [Exception Handling](https://github.com/WebAssembly/exception-handling)
- [Extended Constant Expressions](https://github.com/WebAssembly/extended-const)

### Supported architectures and platforms
The WAMR VMcore supports the following architectures:
- X86-64, X86-32
- ARM, THUMB (ARMV7 Cortex-M7 and Cortex-A15 are tested)
- AArch64 (Cortex-A57 and Cortex-A53 are tested)
- RISCV64, RISCV32 (RISC-V LP64 and RISC-V LP64D are tested)
- XTENSA, MIPS, ARC

The following platforms are supported, click each link below for how to build iwasm on that platform. Refer to [WAMR porting guide](./doc/port_wamr.md) for how to port WAMR to a new platform.
- [Linux](./product-mini/README.md#linux),  [Linux SGX (Intel Software Guard Extension)](./doc/linux_sgx.md),  [MacOS](./product-mini/README.md#macos),  [Android](./product-mini/README.md#android), [Windows](./product-mini/README.md#windows), [Windows (MinGW, MSVC)](./product-mini/README.md#mingw)
- [Zephyr](./product-mini/README.md#zephyr),  [AliOS-Things](./product-mini/README.md#alios-things),  [VxWorks](./product-mini/README.md#vxworks), [NuttX](./product-mini/README.md#nuttx), [RT-Thread](./product-mini/README.md#RT-Thread), [ESP-IDF(FreeRTOS)](./product-mini/README.md#esp-idf)


## Getting started
- [Build VM core](./doc/build_wamr.md) and [Build wamrc AOT compiler](./wamr-compiler/README.md)
- [Build iwasm (mini product)](./product-mini/README.md): [Linux](./product-mini/README.md#linux), [SGX](./doc/linux_sgx.md), [MacOS](./product-mini/README.md#macos) and [Windows](./product-mini/README.md#windows)
- [Embed into C/C++](./doc/embed_wamr.md), [Embed into Python](./language-bindings/python), [Embed into Go](./language-bindings/go), [Embed in Rust](./language-bindings/rust)
- [Register native APIs for Wasm applications](./doc/export_native_api.md)
- [Build wamrc AOT compiler](./wamr-compiler/README.md)
- [Build Wasm applications](./doc/build_wasm_app.md)
- [Port WAMR to a new platform](./doc/port_wamr.md)
- [VS Code development container](./doc/devcontainer.md)
- [Samples](./samples) and [Benchmarks](./tests/benchmarks)
- [End-user APIs documentation](https://bytecodealliance.github.io/wamr.dev/apis/)


### Performance and memory
- [Blog: The WAMR memory model](https://bytecodealliance.github.io/wamr.dev/blog/the-wamr-memory-model/)
- [Blog: Understand WAMR heaps](https://bytecodealliance.github.io/wamr.dev/blog/understand-the-wamr-heaps/) and [stacks](https://bytecodealliance.github.io/wamr.dev/blog/understand-the-wamr-stacks/)
- [Blog: Introduction to WAMR running modes](https://bytecodealliance.github.io/wamr.dev/blog/introduction-to-wamr-running-modes/)
- [Memory usage tuning](./doc/memory_tune.md): the memory model and how to tune the memory usage
- [Memory usage profiling](./doc/build_wamr.md#enable-memory-profiling-experiment): how to profile the memory usage
- [Performance tuning](./doc/perf_tune.md): how to tune the performance
- [Benchmarks](./tests/benchmarks): checkout these links for how to run the benchmarks: [PolyBench](./tests/benchmarks/polybench), [CoreMark](./tests/benchmarks/coremark), [Sightglass](./tests/benchmarks/sightglass), [JetStream2](./tests/benchmarks/jetstream)
- [Performance and footprint data](https://github.com/bytecodealliance/wasm-micro-runtime/wiki/Performance): the performance and footprint data


Project Technical Steering Committee
====================================
The [WAMR PTSC Charter](./TSC_Charter.md) governs the operations of the project TSC.
The current TSC members:
- [dongsheng28849455](https://github.com/dongsheng28849455) - **Dongsheng Yan**, <dongsheng.yan@sony.com>
- [loganek](https://github.com/loganek) - **Marcin Kolny**, <mkolny@amazon.co.uk>
- [lum1n0us](https://github.com/lum1n0us) - **Liang He**， <liang.he@intel.com>
- [no1wudi](https://github.com/no1wudi) **Qi Huang**, <huangqi3@xiaomi.com>
- [qinxk-inter](https://github.com/qinxk-inter) - **Xiaokang Qin**， <xiaokang.qxk@antgroup.com>
- [ttrenner ](https://github.com/ttrenner) - **Trenner, Thomas**， <trenner.thomas@siemens.com>
- [wei-tang](https://github.com/wei-tang) - **Wei Tang**， <tangwei.tang@antgroup.com>
- [wenyongh](https://github.com/wenyongh) - **Wenyong Huang**， <wenyong.huang@intel.com>
- [woodsmc](https://github.com/woodsmc) - **Woods, Chris**， <chris.woods@siemens.com>
- [xujuntwt95329](https://github.com/xujuntwt95329) - **Jun Xu**， <Jun1.Xu@intel.com>
- [xwang98](https://github.com/xwang98) - **Xin Wang**， <xin.wang@intel.com> (chair)
- [yamt](https://github.com/yamt) - **Takashi Yamamoto**, <yamamoto@midokura.com>


License
=======
WAMR uses the same license as LLVM: the `Apache 2.0 license` with the LLVM
exception. See the LICENSE file for details. This license allows you to freely
use, modify, distribute and sell your own products based on WAMR.
Any contributions you make will be under the same license.

# More resources
- [Who use WAMR?](https://github.com/bytecodealliance/wasm-micro-runtime/wiki)
- [WAMR Blogs](https://bytecodealliance.github.io/wamr.dev/blog/)
- [Community news and events](https://bytecodealliance.github.io/wamr.dev/events/)
- [WAMR TSC meetings](https://github.com/bytecodealliance/wasm-micro-runtime/wiki/TSC-meeting-notes)

