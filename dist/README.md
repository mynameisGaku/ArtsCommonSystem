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
│  ├─ Debug/acs.lib          ← merged static lib  (/MDd, debug CRT)
│  └─ Release/acs.lib        ← merged static lib  (/MD,  release CRT)
├─ examples/
│  ├─ check.cpp              ← minimal consumer (compiles & links only acs.h + acs.lib)
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
`ole32`, `dbghelp`, `winmm`, `user32`, `gdi32` (all part of the Windows SDK).

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
*only* `<acs.h>`, uses the container / math / easy modules, and links *only*
`acs.lib`.

---

## What's included

`acs.h` exposes the full **public** runtime API: `foundation`, `math`,
`container`, `memory`, `threading`, `platform`, `ecs`, `event`, `collision`,
`mvvm`, `asset` / `assetpack`, `audio`, `render` (the backend-agnostic `IRhi*`
interfaces + `FRenderer` / `FSpriteBatch` / `FStandardShader` / `FPbrShader` …),
`ui`, the `gameframework` (`FGame` / `FScene` / `FNode2D` / components / systems),
and the **`easy`** beginner layer (`acs::easy::…`). The graphics (DirectX 12),
audio (XAudio2) and other heavy SDKs are compiled **into** `acs.lib` and hidden
behind interfaces — they never leak into `acs.h`.

### Optional integrations

Steamworks, ONNX (ML), OpenXR and Lua scripting are present as **interfaces** in
`acs.h`, but using them at runtime requires their own SDK / redistributable
DLLs. The core graphics / audio / gameplay / `easy` stack needs nothing beyond
`acs.lib` + the Windows system libs above.

---

## Regenerating the distribution

After changing the engine, rebuild and re-amalgamate:

```powershell
# 1) build the engine (Debug and/or Release)
cmake --build acs/Intermediate/vs --config Debug   -j
cmake --build acs/Intermediate/vs --config Release -j
# 2) regenerate acs.h + merge libs into dist/
powershell -ExecutionPolicy Bypass -File acs/scripts/build_single_header.ps1
```

`acs/scripts/amalgamate.py` produces `acs.h` (inlines every public
`#include "..."`, hoists nothing — external `<...>` includes stay in place;
strips `#pragma once`; adds the link/ABI pragmas).
`acs/scripts/build_single_header.ps1` runs it and merges the per-module
`acs_*.lib` (+ bundled `imgui`/`lua`/`ufbx`) into one `acs.lib` per config.
