# ACS — Single-Header Distribution

Use the whole ACS engine from a **completely separate solution** with just:

```cpp
#include <acs.h>
```

This folder is self-contained. You do **not** need the ACS source tree.

```
dist/
├─ acs.h                     ← single amalgamated header (all public API)
├─ lib/x64/
│  ├─ Debug/                 ← acs.lib + adjacent Diligent/xxhash libs (/MDd)
│  └─ Release/               ← acs.lib + adjacent Diligent/xxhash libs (/MD)
├─ examples/
│  ├─ check.cpp              ← minimal single-header consumer smoke test
│  └─ build_example.cmd      ← build it: `build_example.cmd Debug|Release`
└─ README.md
```

`acs.h` auto-links `acs.lib` and the required Windows system libraries via
`#pragma comment(lib, …)`, so you only set two project paths and write `#include <acs.h>`.

---

## Quick start (Visual Studio project)

1. **Include path** → add the `dist` folder (so `<acs.h>` resolves).
2. **Library path** → add `dist\lib\x64\$(Configuration)` (so `acs.lib` resolves).
3. Set the **required compiler options** below (they must match how the engine was built).
4. `#include <acs.h>` and build (x64).

### Required compiler options (ABI must match)

ACS is a **no-STL, no-exceptions, no-RTTI** engine. A consumer translation unit
**must** be built with the same settings or layouts/mangling diverge. `acs.h`
contains `#error` guards that stop the build immediately if exceptions or RTTI
are left enabled, so you get a clear message instead of cryptic link errors.

| Setting | Value | VS / cl flag |
|---|---|---|
| C++ standard | C++20 | `/std:c++20` |
| Exceptions | disabled | `/EHs-c-` + `/D_HAS_EXCEPTIONS=0` |
| RTTI | disabled | `/GR-` |
| Runtime (Debug) | Multi-threaded Debug DLL | `/MDd` → link `lib\x64\Debug` |
| Runtime (Release) | Multi-threaded DLL | `/MD` → link `lib\x64\Release` |
| Source charset | UTF-8 | `/utf-8` |
| Conformance | recommended | `/permissive- /Zc:__cplusplus /Zc:preprocessor` |
| Platform | x64 | — |

> Debug and Release have **separate** `acs.lib` files (different CRT). Point the
> library path at the one matching your configuration.

### Auto-linked libraries

`acs.h` emits these via pragma — you don't list them manually:

`acs.lib`, `d3d12`, `dxgi`, `d3dcompiler`, `dxguid`, `xaudio2`, `ws2_32`,
`ole32`, `dbghelp`, `winmm`, `user32`, `gdi32`, `Shlwapi`. Every entry after
`acs.lib` in this paragraph is a Windows SDK library.

The Diligent backend and its xxHash dependency are separate static libraries
shipped **next to `acs.lib`** in each configuration directory. The same pragma
mechanism auto-links all twelve Diligent libraries
(`Diligent-GraphicsEngineD3D12-static`, `Diligent-GraphicsEngineD3DBase`,
`Diligent-GraphicsEngineNextGenBase`, `Diligent-GraphicsEngine`,
`Diligent-GraphicsTools`, `Diligent-Archiver-static`, `Diligent-ShaderTools`,
`Diligent-GraphicsAccessories`, `Diligent-Common`, `Diligent-Win32Platform`,
`Diligent-BasicPlatform`, `Diligent-Primitives`) plus `xxhash`. Keep the whole
configuration directory together; only its directory needs to be added to the
library search path.

---

## Try the example

```bat
cd dist\examples
build_example.cmd Debug      ::  or: build_example.cmd Release
check.exe
```

Expected output:

```
acs.h OK | sum=42 dist=5.0 clamp=100.0 len=5.0
```

`examples\check.cpp` is the canonical "is my setup correct?" test: it includes
*only* `<acs.h>` and uses the container / math / easy modules. The consumer
does not list individual libraries on its command line; `acs.h` supplies the
`acs.lib`, Windows SDK, Diligent and xxHash link directives above.

---

## What's included

`acs.h` exposes the full **public** runtime API: `foundation`, `math`,
`container`, `memory`, `threading`, `platform`, `ecs`, `event`, `collision`,
`mvvm`, `asset` / `assetpack`, `audio`, `render` (the backend-agnostic `IRhi*`
interfaces + `FRenderer` / `FSpriteBatch` / `FStandardShader` / `FPbrShader` …),
`ui`, the `gameframework` (`FGame` / `FScene` / `FScene2D` / `ANode` /
`AComponent` / systems), and the **`easy`** beginner layer (`acs::easy::…`).
ACS module implementations, including the raw DirectX 12 and XAudio2 paths,
are merged into `acs.lib`; the Diligent implementation remains in the adjacent
static libraries listed above. Backend implementation headers do not leak into
`acs.h`.

### Checked container and storage contracts

`TArray` provides `TryReserve`, `TryResize`, `TryPushBack`, and
`TryEmplaceBack`; `FString` provides `TryReserve` and `TryAppend`. These
operations return `false` or `nullptr` without changing the existing value when
allocation fails. Growing `TArray` while the appended value or constructor
arguments refer to an element in that same array is supported: the new element
is constructed while the old storage is still alive, then the existing
elements are transferred and the growth is committed.

Use `ReleaseStorage` when a long-lived array or string must return retained
capacity. `FStorage` accepts an explicit `FAllocator`, provides the atomic
`TrySetString` entry point, and releases entry capacity in `Clear`.
`FAllocator::AllocationCount` and `LifetimeGeneration` expose allocation
liveness and allocator-lifetime identity to diagnostics and ownership guards.

### Optional integrations

Steamworks, ONNX (ML), OpenXR and Lua scripting are present as **interfaces** in
`acs.h`, but using them at runtime requires their own SDK / redistributable
DLLs. The core graphics / audio / gameplay / `easy` stack needs nothing beyond
the complete configuration directory + the Windows SDK libraries above.

---

## Regenerating the distribution

After changing the engine, rebuild and re-amalgamate:

```powershell
# 1) build the engine (Debug and/or Release)
cmake --build acs/Intermediate/vs --config Debug   -j
cmake --build acs/Intermediate/vs --config Release -j
# 2) regenerate acs.h + merge libs into dist/
powershell -ExecutionPolicy Bypass -File acs/scripts/build_single_header.ps1 -SelfTest
powershell -ExecutionPolicy Bypass -File acs/scripts/build_single_header.ps1
# 3) verify tracked header drift and naming/node conventions
python acs/scripts/amalgamate.py --check
python acs/scripts/audit_cpp_conventions.py --root dist --scope .
# 4) in an x64 Visual Studio developer shell, syntax-check the consumer only
cl /nologo /Zs /std:c++20 /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHs-c- /GR- /D_HAS_EXCEPTIONS=0 /I dist dist/examples/check.cpp
```

`acs/scripts/amalgamate.py` produces `acs.h` (inlines every public
`#include "..."`, hoists nothing — external `<...>` includes stay in place;
strips `#pragma once`; adds the link/ABI pragmas). `--check` renders the same
header in memory and byte-compares it with tracked `dist/acs.h`.
`acs/scripts/build_single_header.ps1` runs it and merges the per-module
`acs_*.lib` (+ bundled `imgui`/`lua`/`ufbx`/`mimalloc`) into one `acs.lib` per
config, then requires and copies the adjacent Diligent/xxHash libraries.
The merge uses a unique response file and a same-directory temporary library,
publishing `acs.lib` only after `lib.exe` succeeds and the output is non-empty.
`-Deploy` rejects drive roots, reparse points, and any destination overlapping
the source/build/dist trees before invoking `robocopy /MIR`. After the copy it
fails closed unless the destination has the exact same relative file set,
sizes, and SHA-256 hashes as `dist/`; a successful `配置完了` therefore means
the consumer directory is a complete byte-for-byte mirror rather than a
partially updated mix of SDK generations.

With `ACS_BUILD_TESTS=ON`, a top-level ACS CMake configure that has this sibling
`dist` directory also registers drift, convention-audit and `/Zs` consumer
tests. An ACS source tree consumed through `add_subdirectory` does not register
those distribution-only checks.
