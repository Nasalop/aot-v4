# x86_64 Windows PE -> native (AOT static recompiler)

Recompiles a Windows x64 `.exe` **ahead of time** into C, which you compile natively (x86_64 host, or ARM64/Android with the NDK clang).
No interpreter, no JIT: guest registers live in C locals (so the compiler register-allocates them), guest memory is the host address space
(the PE is mapped at its preferred ImageBase, so a guest pointer *is* a host pointer: loads/stores are plain `mov`/`ldr`).
Flags are computed only where a later instruction in the block reads them (liveness pass using Capstone's eflags info).

## Use
```bash
pip install -r src/transpiler/requirements.txt          # capstone, pefile
./aot.sh game.exe mygame                                # transpile + compile -> output/mygame
./output/mygame game.exe [args]                         # needs the original exe next to it (for data/resources)
CC=aarch64-linux-android30-clang ./aot.sh game.exe      # Android build (run on device)
tests/run.sh                                            # regression tests (needs clang + lld-link)
```
The transpiler requires Python 3 with `capstone` and `pefile`; native builds
require a C compiler. The PE-fixture regression suite additionally needs
`clang` and `lld-link` and reports a clear skip when those tools are absent.
`AOT_VERBOSE=1` lists imports that have no shim, warns when an import uses an
existing placeholder handler, and records dynamic
`LoadLibrary`/`GetProcAddress` successes and failures. Unimplemented imports
and instructions stop with a clear message naming them.

## What works (verified by tests/run.sh against native builds, -O0 and -O2 code)
Integer ALU/shift/rotate/mul/div (incl. 128-bit), SSE/SSE2 scalar+common packed ops, string ops with rep, lock-prefixed atomics, jump tables
(MSVC/gcc patterns), function pointers/vtables (found via relocations, data scan, `.pdata`, `lea`/`mov` of code addresses), TLS, a fake TEB/PEB (`gs:`),
Win64 calling convention for shims, calling guest code from shims (`aot_call`, used by `_initterm`), ~200 kernel32/CRT shims in `src/runtime/shims.c`.

## What does NOT work yet (be realistic about real games)
- **Full graphics/audio/input/windowing**: D3D11 and D3D10 are constrained compatibility subsets, not general Direct3D-to-Vulkan support. D3D10 supports a core single-device vtable subset; geometry shaders, instancing, depth/stencil, predication, stream output, and many getters/resource methods remain unsupported. XInput, DirectSound, and most user32/gdi32 behavior are also missing.
- x87 FPU, AVX/AVX2, most packed-integer SSE, SEH/C++ exceptions, `syscall`, self-modifying/packed/DRM code (Denuvo, VMProtect, Themida...), JIT code, loading arbitrary guest DLLs, and exes with non-default ImageBase collisions. `LoadLibrary`/`GetProcAddress` only resolve the runtime's explicitly registered DLL/API subset; unsupported modules and exports fail rather than receiving a success-shaped fallback.
- Indirect jumps through tables the heuristic doesn't recognise stop with "undiscovered indirect jump target".
- ARM64 weak memory model: only `lock`/`xchg`/`mfence` get barriers; multithreaded code needs more care.
- Threads (`CreateThread`) are not implemented.
Typical workflow: run, read the first missing-import/instruction message, add the shim or instruction, repeat.

## PE dependency inspection and runtime resolution
`src/tools/pe_dependency_inspect.py` inspects PE32+ normal and delay imports,
including symbol names and ordinals. It recursively follows application dependency files found beside the executable
and in `--search-dir` directories. Host system DLLs are inspected for exports
and forwarded exports, while their large OS-wide import graph is skipped by
default; `--scan-system-imports` opts into that full scan.
The report compares imports against the DLL-scoped runtime shim tables and
lists missing DLLs, functions, dependency files, and unresolved exports.
Known placeholder handlers are listed separately from complete shim matches.

```powershell
py -3.12 src/tools/pe_dependency_inspect.py "C:\Games\AC1\AssassinsCreed_Dx10.exe" `
  --search-dir "C:\Games\AC1" `
  --json output\ac1-dependencies.json `
  --markdown output\ac1-dependencies.md
```

Recursive inspection is enabled by default; `--no-recursive` limits the scan
to the specified executable. To include observed dynamic loads and symbol
lookups, run the recompiled program with `AOT_VERBOSE=1` and pass its captured
log with `--runtime-log`. Runtime resolution is DLL-scoped; an API with the
same name exported from an unrelated shim DLL is not treated as a match.
Arbitrary guest DLL loading is not implemented. AC1-specific dependency
priorities remain unverified until its executable or an import report is
available; the repository's stress-test executable is not AC1.

Nested guest calls now restore the complete virtual CPU state (including XMM
registers and flags), so CRT constructors cannot leak state into their caller.

## Win32 game-folder file APIs
`src/runtime/win_file.c` isolates synchronous file and directory shims from the
general CRT/kernel32 shims. It implements `CreateFileA/W`, `ReadFile`,
`WriteFile`, `CloseHandle`, `FlushFileBuffers`, `GetFileType`,
`SetFilePointer[Ex]`, `GetFileSize[Ex]`, `GetFileAttributes[A/W/Ex]`,
`CreateDirectoryA/W`, `DeleteFileA/W`, `RemoveDirectoryA/W`,
`GetCurrentDirectoryA/W`, and `GetModuleFileNameA/W`. It converts UTF-16
paths, approximates Windows case-insensitive path lookup for ASCII names,
maintains generation-checked guest handles, and reports failures via
`GetLastError`. `win_file_async.c` separately implements overlapped `ReadFile`
and `WriteFile` requests on worker threads, event objects, waits, and
`GetOverlappedResult`; offsets are read from the Windows `OVERLAPPED` layout.
Open the file with `FILE_FLAG_OVERLAPPED`; requests complete through an
optional event and transfer count, with `ERROR_IO_PENDING` reported when
queued. Up to 128 requests can be pending at once. Cancellation and I/O
completion ports are not implemented. Regressions cover asynchronous file
writes and reads, event signaling, waits, and completion result retrieval.
`FindFirstFileA/W`,
`FindNextFileA/W`, and `FindClose` provide non-recursive wildcard enumeration
(`*` and `?`) with Windows-compatible
`WIN32_FIND_DATAA/W` layouts and end-of-search errors. Regressions cover
case-insensitive paths, wildcard filters, ANSI/UTF-16 results, exhaustion,
handle closure, and traversal rejection. File mappings support
`CreateFileMappingA/W`, `MapViewOfFile[Ex]`, `UnmapViewOfFile`, and
`FlushViewOfFile` for read-only, read/write, and copy-on-write views. Anonymous
mappings use Linux `memfd`; file-backed writes are shared and flushable, and
view mappings remain valid after closing their mapping handle. Named mappings
and executable mappings are not supported, and `MapViewOfFileEx` rejects a
requested fixed address. The mapping regression checks file-backed persistence,
anonymous shared views, offset-alignment errors, flushing, and view lifetime
after closing the mapping handle.

On Android, `GameActivity` requests a game folder with the Storage Access
Framework and copies its contents into app-specific storage; no broad storage
permission is requested. Chaquopy runs the existing Python PE translator on
the device, using vendored Capstone Python bindings and an NDK-built, x86-only
Capstone library. The app translates the selected x86-64 PE32+ executable into
C sources and writes a recursive dependency report under its app-specific
`translated` directory. Unsupported instructions and missing APIs are reported.
The default Android build is this translation-only mode; it no longer links
the sample game's pre-generated code. The app does not yet compile the emitted
C or dynamically load the resulting game runtime, so translation is not the
same as booting the game. Executables other than x86-64 PE32+ are rejected.
File paths in the native game runtime resolve inside the imported game folder;
traversal above it and symbolic-link traversal are rejected. Named events,
cancellation, and I/O completion ports remain unsupported.

## WinMM PCM audio
`src/runtime/win_audio.c` provides a separate `waveOut` compatibility module.
On Android it sends queued PCM audio through OpenSL ES; it supports mono/stereo
8-bit and 16-bit PCM, prepare/write/reset/pause/restart/close, and volume
queries. The host backend completes buffers immediately for deterministic API
regressions and does not play sound. Only `CALLBACK_NULL` is accepted; guest
function/window callbacks, compressed formats, capture, DirectSound, and
OpenAL are not implemented. Regressions cover format query, header lifecycle,
device state, and volume calls.

## Graphics foundation
The graphics compatibility path is split by responsibility:
`src/runtime/win_gfx.c` implements D3D11/DXGI calls and state;
`src/runtime/gfx_draw_batch.c` snapshots each draw and retains its resources;
`src/runtime/gfx_backend.c` translates those snapshots into Vulkan pipelines,
bindings, and frame submission. `src/runtime/gfx.c` remains the lower-level
graphics resource foundation. The D3D11 surface is still constrained rather
than a general Direct3D implementation. Input layouts validate semantic
elements, calculate append offsets and strides, map common DXGI formats to
Vulkan formats, and bind D3D11 primitive topologies before draws.
Constant buffers are exposed as 16-byte-aligned CBVs with D3D11-style slot
binding and range validation; `aot_uniform_buffer_info` provides the offset
and range needed to populate a Vulkan uniform-buffer descriptor.
The runtime now exposes guest-callable COM-shaped `ID3D11Device` and
`ID3D11DeviceContext` objects. Their vtables contain recompiler thunk
addresses, `D3D11CreateDevice` is bound as an import shim, and the initial
methods include reference counting, immediate-context retrieval, buffer
creation, input-layout/topology binding, constant-buffer binding, and Draw.
Texture2D creation, SRV mip/range validation, sampler filter/address-mode
mapping, and pixel-shader resource/sampler slots are also exposed through the
device/context vtables.
Render-target and depth-stencil views now have separate validated descriptors,
OM render-target/depth binding, clear operations, and corresponding Vulkan
attachment metadata paths.

The separate `src/runtime/win_d3d10.c` module binds
`D3D10CreateDevice` and `D3D10CreateDeviceAndSwapChain`; the D3D10 device facade
maps common resource creation and immediate draw/state methods into the existing
graphics path. The multi-cube regression exercises D3D10 creation, shared device
vtable methods, and submits 3,000
independent D3D11 compatibility-path draws per frame for 600 frames (1.8 million
requested draws total). Draw batches support up to 4,096 draws per frame.

## On-device (Termux) quick start
`./termux_setup.sh` once, then `./device_check.sh`. Unimplemented COM vtable slots now abort with the exact interface/slot number instead of silently returning success.

## Build an APK with GitHub (no Android Studio)
1. Create a **private** GitHub repo, push this folder to it, put your exe at `game/game.exe` (<100 MB).
2. The `build-apk` workflow transpiles it, builds with the NDK and uploads `aotgame-apk` (Actions tab -> run -> Artifacts).
3. Install the APK. Run it once. Read `Android/data/com.aot.game/files/log.txt` (shows stdout/stderr, missing imports, etc.).
   Put the game's data files in `Android/data/com.aot.game/files/game/` next to the extracted exe.

## Vulkan triangle self-test
`src/triangle/` is a standalone Android Vulkan bring-up (surface -> device -> swapchain -> render pass -> pipeline -> rotating triangle).
Build it from the GitHub **Actions -> build-apk -> Run workflow -> mode: triangle** (or when `game/game.exe` is absent). Log: `Android/data/com.aot.game/files/triangle_log.txt`.
Failure to initialise Vulkan fills the screen red and writes the reason to that log.

## D3D11 / DXGI minimal layer (v7)
`src/runtime/win_gfx.c`: `D3D11CreateDevice[AndSwapChain]`, `CreateDXGIFactory[1]`, device/context/swapchain/texture/RTV COM objects with real vtable layouts
(unimplemented slots abort naming `Interface::vtbl[N]`), plus ~60 user32/winmm shims (fake window, empty message loop).
`src/runtime/gfx_backend.c`: Android = Vulkan swapchain with a basic clear and graphics-pipeline path; host = logging stub. `tests/t3.exe` checks clear+present end to end.
Android hook: `android_main.c` must call `aot_android_set_window(app->window)` on APP_CMD_INIT_WINDOW and `(NULL)` on TERM_WINDOW, and must NOT draw on the window itself (Vulkan owns it).

## DXBC shader translator dependency
Game-mode Android builds fetch and compile the MIT-licensed
[dxbc-spirv](https://github.com/doitsujin/dxbc-spirv) library at the pinned
commit `76236b827e95381aa1b37bccfb31642896c427f8`; CMake verifies the source
archive SHA-256 before building it with the NDK. Its pinned SPIR-V-Headers
submodule is fetched separately and placed at the expected source path. The
runtime uses the translator for supported D3D11 vertex and pixel shaders.
The source archive is fetched during CMake configuration, so game-mode Android
builds need network access. Set `-DAOT_ENABLE_DXBC_SPIRV=OFF` to skip it.
The translator's MIT notice is packaged in the APK at
`assets/third_party_licenses/dxbc-spirv.txt`; the SPIR-V-Headers notice is
packaged alongside it.

The Android game path supports a constrained draw slice:
`CreateBuffer`, `CreateInputLayout`, `CreateVertexShader`, and
`CreatePixelShader`, plus `IASetVertexBuffers`, `IASetInputLayout`,
`IASetPrimitiveTopology`, `VSSetShader`, `PSSetShader`, `VSSetConstantBuffers`,
`UpdateSubresource`, pixel-shader resource/sampler binding, and up to 1024
triangle-list `Draw` calls per `Present`. Each D3D11 `Draw` snapshots its
vertex streams, constant-buffer contents, shader state, and resource bindings;
the Vulkan backend records those individual draws into one command buffer and
submits one frame. It accepts four vertex-buffer slots, up to 16 input
attributes, four vertex constant-buffer slots (`b0`-`b3`), and four pixel
texture/sampler slots (`t0`-`t3` / `s0`-`s3`). Input semantics are mapped by
input-element order. Default-usage constant buffers can be updated in full
with `UpdateSubresource`; dynamic buffers support `Map`/`Unmap` with
`WRITE_DISCARD` and `WRITE_NO_OVERWRITE`. The shared texture path supports
RGBA8 single-mip textures and shader-resource views. `gfx_draw_batch.c` owns
the per-draw snapshots and retains bound resources through presentation.
Indexed drawing is supported for R16_UINT and R32_UINT index buffers. The
supported blend state is standard source-alpha / inverse-source-alpha color
blending with ONE / inverse-source-alpha alpha blending. One viewport and one
scissor rectangle are supported; scissoring is applied when enabled by a
rasterizer state. Rasterizer states support solid fill, front/back/no culling,
and depth clipping, without depth bias, multisampling, or antialiased lines.
Drawing requires Vulkan 1.3 and the Vulkan memory model; older devices keep the
clear/present path but report shader drawing as unsupported. This remains a
constrained compatibility path, not a general D3D11 renderer.

## D3D11 2D sprite test
Run **build-apk** in GitHub Actions with mode `sprite` to build the
`gfx_2d_sprite.exe` test and an APK that runs it. The test uses two textured,
overlapping quads, dynamic vertex/index buffers mapped through D3D11, a
source-alpha blend state, and a constant-buffer update. It configures a
viewport and enabled scissor rectangle, then submits two independent
`DrawIndexed` calls per frame for 600 frames. It reports shader setup,
presented frames, draw totals, errors, and resource cleanup. The renderer logs
uploaded index-buffer bytes alongside vertex and constant-buffer usage. This
is a focused baseline test, not proof that an arbitrary 2D game is supported.

## D3D11 textured-cube test
Pushes build the textured-cube test by default; it can also be selected by
running **build-apk** in GitHub Actions with mode `cube`. The Windows job
compiles the test's HLSL shaders with the Windows SDK `fxc.exe`, builds
`gfx_textured_cube.exe`, and publishes it as the `d3d11-textured-cube-exe`
artifact. The Android job transpiles that program and builds an installable
APK. The app creates a six-face RGBA8 four-colour checker texture, rotates the
cube around both axes, moves its camera on an orbit path, and updates a
perspective world-view-projection matrix every frame through a D3D11 vertex
constant buffer. The D3D11 context update is translated into a Vulkan uniform
buffer descriptor; depth testing handles face occlusion. It runs 600 frames
and reports shader translation, presented-frame count, rendering failures and
COM resource cleanup. The single-cube test remains a regression test for the
same shared drawing path.

## 100-object D3D11 draw-batch test
Pushes also build the dedicated `gfx_multi_cube.exe` test and select it for the
APK; run **build-apk** with mode `multi` to build it on demand. The test
submits 100 independent D3D11 `Draw` calls per frame for 600 frames, with
independently rotating/translating cubes, two split vertex streams and
alternating input layouts, four per-object constant-buffer slots, and four
distinct checker textures/samplers selected by each object's material. The
test reports presented frames, draw totals, elapsed time/FPS, errors, and
resource cleanup. The runtime logs successful Vulkan draw/descriptor counts
and bytes uploaded for vertex and constant buffers; verify those values from
the Android device log after running the test. The Windows workflow also
builds the triangle-grid stress executable as a regression program.
These tests cover only the constrained features above, not general D3D11.
Vulkan renderer errors are also copied into the app's `log.txt` to help
diagnose failures without Android logcat access.

## D3D11 graphics stress executable
In GitHub Actions, run **build-apk** with mode `stress`. Its Windows job
compiles the HLSL shaders with the Windows SDK `fxc.exe`, embeds their DXBC,
and produces `gfx_stress.exe` as the `d3d11-graphics-stress-exe` artifact. The
Android job also recompiles that test and produces an installable APK. The test
draws a 24x16 colored triangle grid (768 triangles per frame) for 60 presents
and reports elapsed time or the first failed D3D11 operation. The built test
is also checked in as `game/stresst_test.exe`; future default APK workflow runs
will use it when present. Its 60-frame result is a short graphics-load check,
not a sustained thermal or maximum-performance benchmark.

## 32-bit (PE32 / x86) support
`aot_transpiler.py` accepts x86 PE32 images (Machine 0x14c): 32-bit registers/stack (`PUSH32/POP32`, 32-bit-wrapped addressing, `fs:` TEB at 0x18/0x2C/0x30), stdcall/cdecl,
x87 (shared with x64), `pushad/popad/pushfd/popfd/lahf/sahf`. The runtime maps the image at its 32-bit ImageBase, keeps heap/stack/TEB below 4 GB (`win_x86.c`),
and adapts guest stack frames to the existing shims. **Only imports listed in `win_x86.c:aot_x86_apis` are bound** (kernel32/msvcrt subset with verified arg sizes);
any other import is unresolved and aborts with "call to unimplemented import" - nothing is silently stubbed. D3D9/D3D10/user32/printf-style varargs are not available for PE32 yet.
Report: `python3 src/tools/aot_compat_report.py game.exe [--compile] --out-dir out/` (architecture, protection hints, unsupported instructions, unresolved APIs, build status).
Tests: `python3 tests/test_x86_pe32.py`.

## Running from a selected game folder (Android)
Game-mode APKs (game.exe bundled by CI) no longer need the game copied into `Android/data`. On first launch the app asks for
"All files access", then lets you pick the game folder with the system folder picker (the one that holds the .exe and data). The game
runs in place with that folder as its working directory, and writes `aot_log.txt` into it (first failing import / unsupported instruction
included). To pick a different folder, clear the app's data (or delete `Android/data/com.aot.game/files/game_dir.txt`).
