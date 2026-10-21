# PORT-MAP — sms-pc-port layout facts

Facts from chasem-dev/sms-pc-port @ e3cdcca, decomp @ 967436b. Line counts exclude third_party/ and tests/ unless stated.

Spot-checked 2026-10-21 against the source (stub count, PORT_GAME_LO/HI, GXInit/GXCopyDisp/GXCopyTex/peekColor/PADRead/port_low_alloc anchors, SMS_ARCH and SMS_COMMON_DEFS lines): all matched.

Method: `git -C <repo> ls-files`, extensions .c/.cc/.cpp/.h/.hpp/.inc/.m/.mm, `wc -l`. Function counts come from a regex over definitions (a name followed by `(` and then a `{` before any `;`) in the module's own files. They are approximate. `__attribute__((weak))` definitions on one line are partly missed, and that is noted where it matters.

Totals: platform/ has 116 non-test, non-third_party source files and 43,348 lines.

## 1. Platform modules

### 1a. Size

| Module | Files (src) | Lines | tests/ (files / lines) | third_party (files / lines) |
|---|---|---|---|---|
| platform/ar | 1 | 85 | – | – |
| platform/audio | 6 | 1,670 | 2 / 655 | – |
| platform/card | 1 | 500 | – | – |
| platform/disc | 2 | 575 | 1 / 214 | – |
| platform/dvd | 1 | 1,026 | – | – |
| platform/endian | 8 | 1,648 | 1 / 778 | – |
| platform/gx | 45 | 24,241 | 4 / 1,406 | 6 / 17,972 (`src/third_party/` bcdec.h, stb_easy_font.h, stb_image.h, stb_image_write.h; `third_party/GL/glcorearb.h`, `KHR/khrplatform.h`) |
| platform/misc | 12 | 1,636 | – | – |
| platform/mods | 13 | 2,701 (2,298 of them in `mods/eclipse/`, which only cmake/eclipse.cmake builds, with SMS_ECLIPSE) | – | – |
| platform/mtx | 3 | 1,278 | – | – |
| platform/os | 7 | 2,227 | 3 / 243 | – |
| platform/pad | 3 | 1,934 | – | – |
| platform/presence | 1 | 34 | – | – |
| platform/savestate | 3 | 1,327 | 1 / 96 | – |
| platform/thp | 2 | 121 | 2 / 309 | – |
| platform/trace | 3 | 533 | – | – |
| platform/vi | 1 | 357 | – | – |
| platform/port_host.cpp | 1 | 43 | – | – |
| platform/port_os.h | 1 | 37 | – | – |
| platform/port_runtime.cpp | 1 | 990 | – | – |
| platform/sdk_stubs.cpp | 1 | 385 | – | – |

### 1b. SDK families implemented, and host libraries used

| Module | SDK prefixes defined (approx. count, examples) | Host libs / OS APIs (from includes or dlopen) | Notes (file header) |
|---|---|---|---|
| ar | AR 8 (ARInit `ar.cpp:37`, ARAlloc, ARStartDMA, ARGetSize); ARQ 2 (ARQInit, ARQPostRequest) | none | Models 16 MiB ARAM as a host buffer. ARQ copies happen at once and complete as interrupts. |
| audio | AI 26 (AIInit, AIInitDMA, AIRegisterDMACallback, AIGetDMAStartAddr…); DSP 6 (DSPInit, DSPSendMailToDSP, DSPCheckMailFromDSP…, `dsp_hle.cpp`); AIS* stream registers in `dtk.cpp` | SDL2 audio loaded with `dlopen` (`ai.cpp`: "libSDL2-2.0.so.0", "@rpath/SDL2.framework/SDL2", "SDL2.dll"…); pthread | Software DSP mixer (`dsp_mixer.cpp`), DSP mail-protocol HLE, AI DMA output at 32 kHz, DTK stream. `PROTOCOL.md` and `README.md` are included. |
| card | CARD 21 (CARDOpen `card.cpp:359`, CARDCreate, CARDCheck, CARDFastOpen…) | none (host files) | Slot A is host files (`$SMS_SAVE_DIR` or the XDG data dir, `<name>.dat`). Slot B is empty. |
| disc | none (not an SDK module) | `<mach-o/dyld.h>`, `<windows.h>` | Reads .iso/.gcm/CISO images and a disc trailer appended to the executable. |
| dvd | DVD 26 (DVDOpen `dvd.cpp:784`, DVDOpenDir `:943`, DVDCancel, DVDChangeDir…) | none | Serves the FST from the disc image or an extracted dir. Reads are synchronous; async completion is delivered as an interrupt. |
| endian | none | none | Byte-order converters for in-place resources (J3D, JAudio, JPA, seq…); hooked from `endian-*` patches. INVENTORY.md and README.md are included. |
| gx | GX ≈297 (excl. GXPC_*), plus ≈64 GXPC_* host extensions; GD comes from the decomp's `libs/dolphin/src/gd/*.c` compiled unchanged (6 files) | SDL2 (`SDL.h`, `SDL_syswm.h`, `SDL_vulkan.h`), OpenGL 3.3 core through its own loader (`GL/glcorearb.h`), EGL headless, Vulkan via volk + VMA + glslang, D3D11/DXGI on Windows (HDR), CoreFoundation on macOS | See section 4. |
| misc | OS 4 (OSInitFont, OSGetFontTexture…); weak DSP* and __DSP_* fake-DSP set plus GDOverflowed in `sdk_data.cpp:45-94` (the regex missed these) | none | `sdk_data.cpp` (SDK globals such as GXNtsc480Int), `msl_math.c`/`msl_rand.c` (MSL maths for the host), language packs, cheats, warp list, window icon, `sdk_stubs_windows.cpp` (MinGW fallbacks). |
| mods | OS 6 (OSCreateAlarm, OSSetAlarm…), IC 3, DC 2, __OSUnhandledException, __CARDSetDiskID, mostly in `mods/eclipse/sdk_extras.cpp` | none | `modhooks.cpp` is the code-mod patch registry. `eclipse/` holds Super Mario Eclipse glue. |
| mtx | PSMTX 16 / C_MTX 23 / PSVEC 10 / C_VEC 10, through `DEF(...)` macros (`mtx.cpp:162-200`, 43 DEF lines; e.g. PSMTXConcat `:164`); 8 `port_J*` paired-single JSystem helpers (`jsys_ps.inc`) | none (CPU feature check `__builtin_cpu_supports("fma")`) | PS and C names forward to one Gekko-rounding implementation (`mtx_ps.inc`). QUAT*: not found. |
| os | OS 72 (OSGetArenaLo `os_core.cpp:56`, OSInitAlloc `:218`, OSCreateHeap `:227`, OSCreateThread…); DC 7; LC 2; IC 1; PPC 2 (PPCSync, PPCMfmsr) | pthread, mach (`mach/mach.h`, `mach_time.h`), dlfcn, execinfo, windows.h | `os_thread.cpp`: each OSThread is a host thread, but one "CPU" mutex lets only one run, with strict priority. Crash reporting (`crash_info.cpp`, `windows_crash.cpp`). |
| pad | PAD 10 (PADRead `pad.cpp:858`, PADInit, PADControlMotor…); PADClamp comes from decomp `libs/dolphin/src/pad/Padclamp.c` (CMakeLists.txt:291) | SDL2 game controller (events through the GX layer's hook); libusb dlopen ("libusb-1.0.so.0", "libusb-1.0.0.dylib"), WinUSB/hid.dll/setupapi.dll for the GC adapter (`gc_adapter.cpp`); pthread | Keyboard, SDL controller and GC adapter on port 1. |
| presence | none | none | Writes `[presence] {...}` lines to stdout when SMS_PRESENCE=1. |
| savestate | none | none | Save states (MEM1, ARAM, game globals); `state_file.cpp` handles compression. |
| thp | none (THPDec.c/THPAudio.c are the decomp's patched files, added by `thp.cmake`) | none | HD movie packs and renderer split (`hd_movies.cpp`). |
| trace | none | Linux only (`trace.cpp` excluded on WIN32/APPLE, CMakeLists.txt:372-374) | Lockstep memory trace against Dolphin .dtm. |
| vi | VI 16 (VIInit `vi.cpp:247`, VIConfigure, VIFlush, VIGetRetraceCount, VISetNextFrameBuffer…) | pthread | A host timer raises retraces at 59.94 Hz (or 119.88). Frame capture is done here. |
| port_runtime.cpp | none (boot sequence standing in for IPL/__start/OSInit; `map_mem1` :565, `port_low_alloc` :621, `port_ptr32_trap` :611, `port_stub_hit/report` :124/:135) | mmap, mach_vm, signal/ucontext, dlfcn, pthread, windows.h | Calls `map_mem1()` at :981 before SMS_main. |
| port_host.cpp | none (`#ifdef _WIN32` only: `port_pread` etc.) | windows.h, io.h, direct.h | |
| port_os.h | none (internal interfaces: interrupt check points, run-lower-priority) | – | |

Families with no definitions anywhere in platform/ outside sdk_stubs.cpp: EXI*, SI*, GBA*, DB* (except the DBInitComm stub), QUAT*.

### 1c. sdk_stubs.cpp

- The file is generated by `tools/gen_stubs.py` from `decomp/docs/progress/port-scope/api-surface.tsv` plus `tools/extra_stubs.txt`. Signatures come from `decomp/libs/dolphin/include/dolphin/**` (header: `platform/sdk_stubs.cpp:1-2`, `tools/gen_stubs.py:2-7`).
- Every stub is `SDK_WEAK` (`__attribute__((weak))`, `src/port_stub.h:16`) with body `SDK_STUB(fn)`, which counts hits via `port_stub_hit` (`src/port_stub.h:17`). Real definitions elsewhere override them at link time. `port_stub_report()` runs at `port_runtime.cpp:349`.
- There are 321 stubs: GX 124, OS 64, GD 24, DVD 19, CARD 14, VI 11, AI 11, PSMTX 9, PAD 8, DSP 7, C_MTX 6, DC 5, PSVEC 4, AR 4, THP 3, PPC 2, LC 2, ARQ 2, MTX 1, DB 1.
- 30 stub names have no strong definition in platform/ or src/: 24 GD* (these come from the decomp GD sources compiled into sms_gx), THPInit/THPVideoDecode/THPAudioDecode (from the patched decomp THP sources via thp.cmake), MTXMultVecSR (a macro name), DSPAddTask and DBInitComm. Only the last two have no real implementation in any of those places.
- On Windows, `platform/misc/sdk_stubs_windows.cpp` gives real fallback definitions, because MinGW COFF weak aliases do not satisfy normal undefined references.

## 2. decomp-patches

The directory holds 278 `.patch` files. They touch 159 unique decomp paths (136 `src/`, 16 `include/`, 3 `libs/THPPlayer/src`, 2 `libs/dolphin/include`, 1 `libs/dolphin/src`, 1 `libs/JSystem`) through 491 `+++ b/` entries. Each patch starts with a `Reason:` line (docs/DEVELOPMENT.md:17,138).

### 2a. Naming groups (by prefix; patches apply in name order)

| Group | Count | Examples / purpose (from DEVELOPMENT.md table where given) |
|---|---|---|
| `0001`–`0022` (numbered) | 22 | host compiler leniency (case scope, goto, const-ref, weak names), endian (0011–0013, 0017), SMS_NO_AUDIO (0014), SMS_SKIP_MOVIES (0016), uintptr fixes (0018,0019,0021,0022) |
| `modhook-NN-*` | 102 | code-mod hook points/exports (`modhook-01-start-and-exports` …) |
| `modhook-zz-*` | 1 | `modhook-zz-macos-data-exports` |
| `framerate-*` | 44 | 60 fps / gameplay rate (`framerate-01-gameplay-60fps` …) |
| `endian-*` | 19 | loader byte-order fixes (JPA, BLO, BMG, …) |
| `port-*` | 10 | port-only features (warp, cheats, …) |
| `fpu-*` | 9 | paired-single routines, MWCC float→unsigned semantics |
| `widescreen-*` | 7 | camera aspect, fader, HUD panes |
| `bounds-*` | 7 | retail out-of-bounds accesses given console results |
| `thp-*` | 6 | host THP decoder, BE audio header, HD row buffers |
| `uninit-*` | 5 | deterministic uninitialised reads |
| `ptr64-*` | 4 | 64-bit one-offs: `01-heap-sizes`, `02-atan-table-wrap`, `03-DSP-parameter-block-ptr32`, `04-J3DAnmLoader-vtx-color-index-offsets` |
| `audio-*` | 4 | JAudio bitfield/LE views, no host overload steal |
| `toolchain-*` | 2 | clang/libc++, JASRegisterParam |
| `ret-*` | 2 | explicit returns (`ret-02`, `ret-03`) |
| `layout-*`, `perf-*`, `windows-*` | 1 each | retail tail padding; hx wiper copy; volume name lowercase |
| `zz-*` | 6 | `zz-hd-movies-01`, `zz-modhook-50/60`, `zz-movie-skip-01/02`, `zz-msl-01` |
| `zzz-*` | 9 | `zzz-camera-01..04`, `zzz-hd-texture-preload`, `zzz-heat-haze-01`, `zzz-presence-01`, `zzz-prompts-01`, `zzz-warp-01` |
| `zzzz-*` | 9 | `zzzz-lang-01..04`, `zzzz-pause-01`, `zzzz-rate-01`, `zzzz-savestate-01/02`, `zzzz-tracker-01` |
| `zzzzz-ext-*` | 7 | `ext/` hook points (`zzzzz-ext-01-hooks`, `-02-demo-camera`, `-03-pollution-clean` …) |

The `z` prefixes put these patches later in the sorted apply order.

### 2b. How they are applied (cmake/patches.cmake, included at CMakeLists.txt:90)

1. `SMS_PATCH_ROOT = ${CMAKE_BINARY_DIR}/patched` and scratch tree `${CMAKE_BINARY_DIR}/patched.new` (patches.cmake:11,17). The scratch tree is `REMOVE_RECURSE`d and recreated on each configure (:21-22).
2. `file(GLOB SMS_PATCHES CONFIGURE_DEPENDS decomp-patches/*.patch)` then `list(SORT)` (:18-19). Each patch is added to `CMAKE_CONFIGURE_DEPENDS` (:34). Progress is printed every 10th patch (:30-33).
3. For each `+++ b/<path>` line in a patch, the original `${SMS_DECOMP}/<path>` is copied into the scratch tree the first time it is touched (:35-43).
4. `execute_process(patch -p1 --quiet --force -d <scratch> -i <patch> TIMEOUT 300)`. A nonzero result is `FATAL_ERROR` (:47-53).
5. Sync step: stale files under `patched/` are removed, except `headers.stamp` (:57-68). Each touched file is copied with `file(COPY_FILE ... ONLY_IF_DIFFERENT)` (:70-74), so unchanged files keep their mtimes. The scratch tree is then deleted (:79).
6. `patched/headers.stamp` lists the patched headers and is rewritten only when that set changes (:81-94). Every game source has it as `OBJECT_DEPENDS` (CMakeLists.txt:224-225).
7. Source replacement: for touched files under `src/` or `libs/*/src/`, the `${SMS_DECOMP}/f` entry in `SMS_DECOMP_CXX_SOURCES`, `SMS_DECOMP_C_SOURCES` and `SMS_DECOMP_PCH_SOURCES` is replaced in place (same index) by `${SMS_PATCH_ROOT}/f` (:96-107).
8. Include precedence for `sms_game` (CMakeLists.txt:298-311), in order: `src/port_include`, then `SMS_PATCHED_INCLUDE_DIRS` (`patched/include`, `patched/libs/<name>/include`), then the decomp include dirs, all `BEFORE PRIVATE`. When sms_gx exists, `SMS_GX_OVERRIDE_INCLUDE_DIR` (`platform/gx/port_include`, `platform/gx/include`) is then added `BEFORE` (it ends up first). In the CP932 mirror case the mirror already merges the patched headers (:300-303).
9. Moving the pin: `tools/update-decomp.sh [--64] [REF]` checks that every patch applies with no fuzz (DEVELOPMENT.md:75).

### 2c. Source enumeration

- At configure time (CMakeLists.txt:77-85), `tools/gen_sources.py <decomp> ${CMAKE_BINARY_DIR}/decomp_sources.cmake` writes the file, which is then `include`d. `decomp/configure.py` is a configure dependency.
- gen_sources.py walks `src/` (its .c files stay C), `libs/JSystem/src` and `libs/THPPlayer/src` (their .c files are treated as C++). It leaves out dolphin SDK, MSL, PowerPC_EABI_Support, TRK and OdemuExi2. It regex-reads `PCHObject(..., "<unit>")` from configure.py to build the PCH list (gen_sources.py:18-36).
- The checked-in `cmake/decomp_sources.cmake` lists 587 CXX, 1 C and 299 PCH units. PCH units get `-include SMS.pch` (CMakeLists.txt:230-236).
- Decomp SDK files compiled separately: `libs/dolphin/src/gd/*.c` (6 files, in sms_gx, with `gd_host_prefix.h` force-included); `pad/Padclamp.c`; patched `thp/THPDec.c` and `THPAudio.c` (via `platform/thp/thp.cmake`).

### 2d. CP932 mirror (tools/darwin_cp932_mirror.py, 123 lines)

- CMakeLists.txt:113 probes `-fexec-charset=CP932`. If the flag is accepted, the game gets `-finput-charset=UTF-8 -fexec-charset=CP932` (:115).
- If it is not accepted (Apple Clang, or GCC without iconv), configure runs `darwin_cp932_mirror.py --decomp --patched --out ${CMAKE_BINARY_DIR}/cp932` (:188-198). The script mirrors `include/`, `src/`, `libs/*/include` and `libs/*/src` (.c/.cc/.cpp/.h/.hpp under src), preferring `patched/` copies. It encodes non-ASCII as CP932, writes any character whose trailing byte is 0x5C as octal escapes, and keeps UTF-8 for characters CP932 lacks.
- CMake then points the game include dirs and PCH dir at the mirror and adds `-Wno-invalid-source-encoding`. Each source list entry is retargeted to the mirror path when one exists (:199-221).

## 3. 32-bit vs 64-bit

### 3a. Selection

| Layer | Fact |
|---|---|
| CMake | `SMS_ARCH` cache var `"auto, 32 or 64"`, default `auto` (CMakeLists.txt:32). `auto` runs `check_cxx_source_compiles` with `-m32` and picks 32 if it links, otherwise 64 with a warning (:33-46). Then `add_compile_options(-m${SMS_ARCH})` and `add_link_options(-m${SMS_ARCH})` (:47-48). |
| macOS in CMake | `CMAKE_OSX_ARCHITECTURES` must be `x86_64` or `arm64` (fatal otherwise). `arm64` sets `SMS_MAC_ARM64=ON` (:140-148), which requires Clang and `llvm-config` beside the compiler (:150-157). |
| build.sh / tools/common.sh | `sms_detect_os`: Linux arches `(32 64)` (default 32); macOS `(64)` plus `arm64` on Apple Silicon (default 64 = x86_64/Rosetta); MSYS2 MINGW64 gives `(64)` (plus 32 with SMS_WINDOWS_32_CROSS=1), MINGW32 gives `(32)` (common.sh:19-48). `SMS_ARCH=32|64` (macOS `64|arm64`) selects, and `sms_select_arch` rejects macos-32 and macos-arm64 on Intel (:52-69). |
| build.sh | `arm64` sets `osx_arch=arm64` (build.sh:61), passes `-DCMAKE_OSX_ARCHITECTURES=$osx_arch` (:132) and `-DSMS_ARCH="${sms_arch/arm64/64}"` (:167). A `-m32` link probe fails with a message (:51-52). Output dirs: `build/linux-32`, `linux-64`, `macos-64`, `macos-arm64`, `windows-64` (build.sh:5-8). |
| 64-bit compile flags | `-fno-pie` on game and platform when SMS_ARCH=64 or Linux (CMakeLists.txt:124-126, 430-435); `-no-pie` link on non-Apple/non-Win. Windows x64: `--image-base,0x400000 --disable-dynamicbase --disable-high-entropy-va` (:419-423). macOS x86_64: `-pagezero_size,0x1000` and `-no_pie` if accepted (:444-453). macOS arm64: `-headerpad,0x10000` (:443). |
| operator new rename | `objcopy --redefine-syms` maps `_Znw{j|m|y}` etc. to `__sms_game_new*`, with the size_t mangling chosen per arch/OS (:315-359). |

### 3b. 32-bit pointer slots on 64-bit hosts

| Mechanism | Location | Fact |
|---|---|---|
| `PTR32(T)` | `src/port_include/dolphin/types.h:94-135` | 8-byte pointers in C++: `port_ptr32<T>`, holding `u32 raw`. It converts back via `port_game_ptr` (or a plain cast under `PORT_GAMEPTR_PLUGIN`). Assignment traps through `port_ptr32_trap` if the address is above 4 GiB and is not a rebased game address. `+=`/`-=` are done in 32 bits. 8-byte pointers in C: `u32`. 4-byte pointers: `T*`. The decomp uses it in 19 files, 118 occurrences (`git grep PTR32`). |
| `port_ptr32_trap` | `platform/port_runtime.cpp:611` | Logs, prints a backtrace and calls `abort()`. |
| `PORT_GAME_LO/HI` | `types.h:81-92` | arm64 macOS: LO=`0x100000000`, HI=`0x10000000000` (1 TiB). Elsewhere both are 0. `PORT_GAME_MIN 0x10000`; `PORT_GAME_REBASE(a)` ORs HI for addresses ≥0x80000000 and LO otherwise. `port_game_ptr(a)` rebases values ≥ MIN. |
| MEM1 mapping | `port_runtime.cpp:565-608` (`map_mem1`, called at :981) | 64 MiB on 64-bit, 24 MiB on 32-bit (`SMS_MEM_MB` overrides). Mapped exactly at `PORT_GAME_HI + 0x80000000` via `map_exact` (:494, MAP_FIXED_NOREPLACE where available) or `VirtualAlloc` on Windows. arm64 exits if it cannot map there; other hosts fall back to a heap block. |
| OS arena | `platform/os/os_core.cpp:33-34,56-70` | Arena lo = `port_mem1_base + 0x4000`, hi = base + size. Bump allocation from lo or hi. OSAlloc heaps at :218-258. |
| `low_map` | `port_runtime.cpp:510-540` | `MAP_32BIT` when defined and `!PORT_GAME_LO` (Linux: below 2 GiB). Otherwise it walks hint addresses from `PORT_GAME_LO+256 MiB` to `PORT_GAME_LO+2 GiB` in 1 MiB steps. |
| `port_low_alloc` | `port_runtime.cpp:621-680` | 32-bit: `malloc`. Win64: reserves a 128 MiB pool below 2 GiB, then VirtualAlloc hint walk. POSIX 64: reserves a pool (128 down to 32 MiB) via `low_map(PROT_NONE)`, carves 64 KiB-grain blocks with a guard grain, then falls back to `low_map`. Callers: thread stacks (`os/os_thread.cpp:178`, `src/port_main.cpp:73`), savestate pictures (`savestate.cpp:787,962`), HD movie pool (`thp/hd_movies.cpp:41`), prompts (`gx/src/gx_prompts.cpp:113,224`, with a weak malloc fallback at :37). |
| Heap sizes | `src/port_compat.h:196-202` | `PORT_HEAP64(n)` is `n*2` on 8-byte pointers and `n` otherwise (used by `ptr64-01-heap-sizes`). |
| pagezero | CMakeLists.txt:445-446; BUILD.md:92 | macOS x86_64 only: `-pagezero_size,0x1000` so low mmap / MAP_32BIT work. |
| GX physical addresses | `platform/gx/src/gx_fifo.cpp:107`, `gx_api.cpp:326` | MEM1 base for physical addresses = `PORT_GAME_HI + 0x80000000`. WG pipe address = `PORT_GAME_HI + 0xCC008000`. sms_gx is built without the pass plugin (MACOS-ARM64.md). |
| tools/gameptr (arm64 macOS) | `tools/gameptr/GamePtrPass.cpp` (241 lines); built at configure by CMakeLists.txt:166-183 into `${CMAKE_BINARY_DIR}/gameptr/libSmsGamePtr.dylib`; flags `-fpass-plugin=… -DPORT_GAMEPTR_PLUGIN=1 -DPORT_GAMEPTR_HASH=<16 hex>` on game (:182) and platform (:438) | Registered at `registerOptimizerLastEPCallback` (:236-241). It rebases: (1) every `inttoptr` whose value is in [0x10000, 4 GiB) by OR-ing kHi/kLo (:128-160), plus constant-expression inttoptr at compile time (:79-85); (2) each `getelementptr` whose base comes from such a cast, re-rebased from the low 32 bits of the result only when the result's high word is in or just past a game half (:175-198); (3) `fptoui`/`fptosi` to ≤32 bits get x86 results (wrap for negative→unsigned, `0x80000000` on overflow) (:134, :216). Test: `tools/gameptr/test.sh`, `test.cpp`. |
| arm64 ASLR | docs/MACOS-ARM64.md | `port_main.cpp` re-execs itself with `_POSIX_SPAWN_DISABLE_ASLR` so the executable loads at `0x100000000`. The post-build step re-signs with `codesign -f -s -` (CMakeLists.txt:467-472). |
| Docs | docs/64-BIT.md (289 lines), docs/MACOS-ARM64.md (83 lines) | 64-BIT.md:10 says "game memory stays at `0x80000000`". :15 covers low stacks and `PTR32` traps. :16 covers heaps doubled and 64 MiB MEM1. :211 covers Windows x64 low stacks and a PE image below 4 GiB. :215-228 covers the pre-work state (417 ptr↔int sites, 191 `JSUConvertOffsetToPtr` sites). MACOS-ARM64.md has the address table (game <0x80000000 at `0x100000000+x`, ≥0x80000000 at `0x10000000000+x`). |

## 4. GX renderer (platform/gx, 45 files, 24,241 lines)

Architecture per `platform/gx/README.md`: GX calls are encoded into BP/CP/XF register writes (`gx_api.cpp`). WG-pipe bytes and GD/J3D display lists go through one parser (`gx_fifo.cpp`). The renderer reads state only from that register file. The backend is OpenGL 3.3 core with GLSL 3.30, with an optional Vulkan path. OpenGL ES / GLES 3.1: not found (no GLES/ES context code in platform/gx or the docs).

| Group | Files (lines) | Group lines |
|---|---|---|
| GX API / register encoding | `src/gx_api.cpp` 1588, `src/gx_internal.h` 326, `src/gx_sdk_math.h` 113, `include/sms_gx/gx_pc.h` 284 | 2,311 |
| Vertex submission, FIFO parser, vertex loader, display-list cache, WG pipe, GD prefix | `src/gx_fifo.cpp` 1469, `src/gx_vert.cpp` 56, `include/sms_gx/gxvert_pc.h` 65, `port_include/dolphin/gx/GXVert.h` 5, `src/gd_host_prefix.h` 24 | 1,619 |
| Batching / draw calls / EFB / copies / peeks / present / post-processing | `src/gx_render.cpp` 3195, `src/gx_glcache.h` 88, `src/gx_nis_coef.h` 159 (NVIDIA Image Scaling coefficients, MIT) | 3,442 |
| TEV / lighting / fog / indirect shader generation | `src/gx_shader.cpp` 1190, `src/gx_shader_keys.inc` 1906 (recorded warm-up keys) | 3,096 |
| Texture decode / upload / cache / HD packs | `src/gx_texture.cpp` 823, `src/gx_hires.cpp` 1682 (Dolphin-format texture packs), `src/gx_bcenc.cpp` 353 (BC1-3/BC7 encoder for pack mips) | 2,858 |
| GL backend plumbing | `src/gl_loader.cpp` 339, `src/gl_funcs.h` 222, `src/gx_glthread.cpp` 234, `src/gx_glthread.h` 110 | 905 |
| Vulkan backend "glvk" (GL subset on Vulkan) | `src/vk/glvk.h` 51, `glvk_impl.h` 486, `glvk_device.cpp` 1416, `glvk_draw.cpp` 1675, `glvk_objects.cpp` 1101, `glvk_shader.cpp` 884 (glslang compiles the GLSL 3.30), `glvk_vma.cpp` 6 | 5,619 |
| Window / SDL / HDR | `src/gx_platform.cpp` 1291, `src/gx_borderless_window.h` 44, `src/gx_window_layout.h` 27, `src/gx_hdr.cpp` 789 (Windows D3D11 HDR swap chain) | 2,151 |
| Overlays / menus | `gx_overlay.cpp` 227, `gx_cheat_menu.cpp/.h` 329+26, `gx_warp_menu.cpp/.h` 463+31, `gx_quit_prompt.cpp/.h` 260+30, `gx_prompts.cpp` 240, `gx_ext_overlay.cpp/.h` 181+18, `gx_savestate_overlay.cpp/.h` 106+17 | 1,928 |
| Debug trace | `src/gx_trace.cpp` 312 | 312 |
| Not counted: `CMakeLists.txt` 291, `README.md` 228, `.gitignore`, 3 PNG screenshots; tests 1,406 (`gx_selftest`, `gx_windowtest`, `gx_borderlesstest`, `gx_hires_test`); third_party 17,972 | | |

### 4a. Where GX API families are defined (anchors)

| Family | Section / anchors |
|---|---|
| GXInit / manage | `gx_api.cpp:141` section. GXInit `:145`, GXSetDrawDone `:240`, GXDrawDone `:245` |
| FIFO objects | `gx_api.cpp:262` section |
| Display lists | `gx_api.cpp:330` section. GXBeginDisplayList `:331`, GXEndDisplayList `:338`, GXCallDisplayList `:347` → `callDisplayList` `gx_fifo.cpp:1242` (DL cache section `:995`) |
| Vertex descriptors / formats / arrays | `gx_api.cpp:357` section. GXSetVtxDesc `:358`, GXSetVtxAttrFmt `:399`, GXSetArray `:463`, GXSetTexCoordGen2 `:478`, GXBegin `:531`. Vertex loader `gx_fifo.cpp:350`, packed formats `:505`, attribute readers `:558`, `decodeVertices` `:852`, command parser `:891`, WG pipe `:1340` |
| Vertex writers | `gx_vert.cpp` (48 functions, e.g. GXPosition3f32 `:19`). The game sees them through `port_include/dolphin/gx/GXVert.h` (the override include dir) |
| Transform | `gx_api.cpp:568` section. GXSetProjection `:569`, GXLoadPosMtxImm `:595`, GXSetCurrentMtx `:611`, GXSetViewport `:659`, GXSetScissor `:664` |
| Lighting | `gx_api.cpp:692` section. GXInitLightPos `:740`, GXLoadLightObjImm `:765`, GXSetChanCtrl `:818`. Shader: `gx_shader.cpp:126-137` (lightColor…, emitLight), vertex shader `:105` |
| TEV | `gx_api.cpp:834` section. GXSetTevColorIn `:863`, GXSetTevKColor `:905`, GXSetZTexture `:930`, GXSetTevOrder `:935`, GXSetNumTevStages `:948`. Fragment shader `gx_shader.cpp:281`, compile/cache `:549`, program cache `:622`, warm-up `:798`, lines `:1033` |
| Indirect | `gx_api.cpp:950` (GXBump section). GXSetTevIndirect `:951`, GXSetIndTexOrder `:985`, GXSetNumIndStages `:989`. Shader key `ind[16]` `gx_shader.cpp:33,95`, `u_indmtx[6]` `:294` |
| Pixel / fog / blend | `gx_api.cpp:1026` section. GXSetFog `:1027`, GXSetFogRangeAdj `:1062`, GXSetBlendMode `:1069`, GXSetZMode `:1082`. Shader fog uniforms `gx_shader.cpp:292-293`, fogType key `:102` |
| EFB peek/poke | GXPokeARGB `gx_api.cpp:1107` (no-op), GXPeekARGB `:1108` → `peekColor` `gx_render.cpp:2864`, GXPeekZ `:1109` → `peekZ` `gx_render.cpp:2870`, GXPokeZ `:1110` (no-op) |
| Framebuffer / EFB→XFB/texture copies | `gx_api.cpp:1114` section. GXSetTexCopySrc `:1119`, GXSetDispCopyGamma `:1170`, `doCopy` `:1172`, GXCopyDisp `:1193`, GXCopyTex `:1194`. Render side: EFB copies section `gx_render.cpp:2194`, `copyEfb` `:2502`, `executeCopy` `:2626`. Write-back encoder `encodeTexture` `gx_texture.cpp:456` |
| Textures / TLUT | `gx_api.cpp:1234` section. GXInitTexObj `:1288`, GXInitTexObjCI `:1291`, GXLoadTexObj `:1340`, GXInitTlutObj `:1345`, GXLoadTlut `:1351`. Decoders `decodeTexture` `gx_texture.cpp:66` (formats per README: I4, I8, IA4, IA8, RGB565, RGB5A3, RGBA8, CMPR, C4, C8, C14X2; TLUTs IA8/RGB565/RGB5A3). Cache section `gx_texture.cpp:190` |
| Perf / helper shapes | `gx_api.cpp:1482` (GXPerf, no counters), `:1508` (GXDraw sphere/cylinder…) |
| Batching | `gx_render.cpp:1200` batching section, `:1295` display-list arena, `flushBatch` `:2024` |
| Host extension API (GXPC_*) | `gx_render.cpp:2650` GXPC_Init, `:2695` GXPC_InvalidateRange, `:2882` GXPC_PresentXFB. `gx_platform.cpp:865` GXPC_InitAuto, `:921` GXPC_Present, `:804` GXPC_ParseArgs. `gx_fifo.cpp:1413-1445` memory window / big-endian ranges / PtrToPhys |

Backend selection and libraries: `SMS_GX_BACKEND=vulkan` picks glvk at run time (docs/VULKAN.md). The `SMS_GX_VULKAN` option defaults ON on Linux and macOS and OFF elsewhere (gx/CMakeLists.txt:133-138). It downloads volk 1.3.296, Vulkan headers, VMA and glslang at configure time, or uses `SMS_GX_VULKAN_DEPS`, and disables itself if they are missing (:139-200). It defines `SMS_GX_HAVE_VULKAN=1 VK_NO_PROTOTYPES=1` (:224). macOS uses MoltenVK, and `SMS_MOLTENVK_DYLIB` is copied into SMS.app (CMakeLists.txt:573-576). SDL2 is optional (`SMS_GX_HAVE_SDL2`, :103-104), as is EGL headless (`SMS_GX_HAVE_EGL`, :117-120). Windows links d3d11 and dxgi (:113-115).

## 5. Build graph

| Target | Defined at | Contents |
|---|---|---|
| `sms_gx` (STATIC) | platform/gx/CMakeLists.txt:50; `add_subdirectory` at CMakeLists.txt:283-285 (option `SMS_WITH_GX`, default ON, :238) | 15 GX sources + the decomp GD .c files (+ window/menu sources when SDL2 is found, + vk when enabled). cxx_std_17, `-ffp-contract=off`, PIC. Includes `src/port_include` (port types.h) |
| `sms_game` (STATIC) | CMakeLists.txt:287 | `SMS_DECOMP_CXX_SOURCES` + `SMS_DECOMP_C_SOURCES` (patched copies substituted), THP decoders (:289), Padclamp.c (:291), `src/savestate_menu.cpp` (:294), `src/pause_tracker.cpp` (:297), `ext/core/port_ext.cpp` and ext game sources (`ext/ext.cmake:6-8`) |
| `sms` (executable) | CMakeLists.txt:375 | `src/port_main.cpp` + `file(GLOB_RECURSE platform/*.c, platform/*.cpp)` (:362-364), minus `/platform/gx/`, `/platform/mods/eclipse/` and `/tests/` (:368-371), and minus `trace/trace.cpp` on WIN32/APPLE (:372-374). This includes sdk_stubs.cpp, port_runtime.cpp, port_host.cpp. Plus ext host sources (`ext/ext.cmake:11`) |
| `sms_standalone` | CMakeLists.txt:581 (macOS SMS.app) / :621 (others) | Only when `-DSMS_BUNDLE_DISC=<iso>`. `tools/bundle_disc.py` appends the disc to the exe, or packs it into SMS.app with SDL2.framework and MoltenVK |
| `gx_selftest` | platform/gx/CMakeLists.txt:255 | when `SMS_GX_BUILD_TESTS` |
| `sms_thread_cancel_test` | CMakeLists.txt:486-496 | `SMS_GX_BUILD_TESTS AND NOT WIN32` |
| `sms_windows_stack_test` | CMakeLists.txt:474-482 | WIN32 and 64-bit, EXCLUDE_FROM_ALL |
| Eclipse mods lib | `cmake/eclipse.cmake` (CMakeLists.txt:525-527) | `SMS_ECLIPSE=ON` only |

Compile definitions and flags:

- `SMS_COMMON_DEFS = GEKKO TARGET_PC VERSION_GMSE01 BUILD_VERSION=2 NDEBUG=1` (CMakeLists.txt:92). These are applied to `sms_game` (:312), `sms` (:409, which adds `SMS_AUDIO_DSP_HLE=1`) and `sms_thread_cancel_test` (:490).
- `SMS_GAME_FLAGS` (:93-103): `-w -fno-strict-aliasing -fwrapv -fno-omit-frame-pointer -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0 -fno-stack-protector -ffp-contract=off`. C++ adds `-std=gnu++03 -fno-gnu-keywords -fpermissive -fno-threadsafe-statics`; C adds `-std=gnu99 -fcommon`. Every unit gets `-include src/port_compat.h`.
- Conditional game flags: x86 adds `-msse2 -mfpmath=sse` (:104-108). Compilers that accept CP932 add `-finput-charset=UTF-8 -fexec-charset=CP932` (:113-116). `-fno-unreachable-traps` is added if supported (:117-120). 64-bit or Linux adds `-fno-pie` (:124-126). Clang adds `-fms-extensions` (:131-133). arm64 macOS adds the gameptr flags (:182).
- `sms` (platform layer): `-fno-strict-aliasing -Wno-unused-parameter -U_FORTIFY_SOURCE -D_FORTIFY_SOURCE=0`, C++ `-std=gnu++11` (:410-411). x86 adds `-msse2 -mfpmath=sse` (also on sms_gx) (:394-399). `platform/misc/msl_math.c` and `platform/mtx/mtx.cpp` get `-ffp-contract=off` (plus SSE on x86) (:379-387). arm64 adds the gameptr flags and `-ffp-contract=off` (:436-439).
- Build type: RelWithDebInfo by default, with `-O2 -g` (:20-26).

Where the platform layer is wired into the game:

- Includes: `sms` sees `platform/`, `platform/mods/include`, `src/`, `src/port_include` and the decomp include dirs (:406-408). `sms_game` sees `src/port_include` first, then the patched includes, then decomp, with the sms_gx override include dir ahead of all (:298-311).
- Linking on Linux/Windows: `-Wl,--whole-archive sms_game -Wl,--no-whole-archive Threads m dl`, plus `--whole-archive sms_gx` (:515-519). On macOS: `-force_load` for sms_game and sms_gx, plus a normal link of sms_gx for SDL2, and `-U` for three optional mod symbols (:497-514). Strong platform/sms_gx definitions replace the weak `SDK_WEAK` stubs at this link.
- `lbl_80004600` is defined as an absolute symbol `0x80004600` via `--defsym` (Linux/Windows) or `src/lbl_80004600_darwin.c` (macOS) (:412-429).
- Post-build: `tools/savestate_ranges.py` writes the game's data ranges, taken from the link map (`sms.map`), into the executable (:455-466).
