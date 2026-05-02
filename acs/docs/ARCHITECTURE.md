# ACS Architecture (Phase 1)

This document describes the **post-rebuild** architecture of Arts Common System.
Phase 1 covers the **runtime foundation**: types, threading, memory, containers,
math. Phase 2 (rendering, ECS, asset, editor) is planned but not yet implemented.

## Design Principles

1. **No STL.** `std::vector`, `std::string`, `std::atomic`, `std::thread`,
   `std::mutex`, `std::function`, `<algorithm>`, `<type_traits>` etc. are not
   used. Replacements live under `acs::` (`Array<T>`, `String`, `Atomic<T>`,
   `Thread`, `Mutex`, ...). C standard headers (`<cstdint>`, `<cstddef>`,
   `<cstring>`, `<cstdio>`, `<cmath>`), compiler intrinsics (`<intrin.h>`,
   `<immintrin.h>`), and Windows SDK (`<windows.h>`, `<DbgHelp.h>`,
   `<DirectXMath.h>`) are explicitly allowed.
2. **No exceptions, no RTTI.** Errors flow through `Result<T, ErrorCode>`. The
   compiler is configured with `/EHs-c- /GR- /D_HAS_EXCEPTIONS=0`.
3. **Thread-safe by default.** Every public API documents and enforces its
   thread-safety contract. Containers are not internally locked but are safe
   to use behind the engine's RwLock / Mutex / Atomic primitives.
4. **SIMD where it matters.** Math types pad to 16 bytes for SSE friendliness
   and route through DirectXMath (SSE2 baseline; SSE4.1/AVX/AVX2 fast paths
   selected via runtime CPU detection).
5. **Diagnostic-first error reporting.** Every assert, panic, and error
   captures `__FILE__`, `__LINE__`, `__FUNCTION__` via `SourceLoc` (a custom
   `source_location` shim using compiler builtins). Panics dump a symbolicated
   stack trace via DbgHelp.
6. **Modular like Unreal.** Modules are discovered from `src/<mod>/Module.cmake`
   and enabled via the user-editable `modules.cmake`. Each enabled module
   exposes a static library target `ACS::<Name>` and a `WITH_ACS_<NAME>=1`
   compile-time define; per-module features add `WITH_<FEATURE>=1`.

## Module Layout

```
acs/
├── CMakeLists.txt
├── modules.cmake               # USER: which modules + features to build
├── cmake/
│   ├── ACSCompilerOptions.cmake
│   └── ACSModuleSystem.cmake
├── src/
│   ├── foundation/             # Types, SourceLoc, Result, Assert, Panic, Log
│   │   └── Module.cmake
│   ├── threading/              # Atomic, Mutex, RwLock, Thread, ThreadPool
│   │   └── Module.cmake
│   ├── memory/                 # Allocator, System/Linear/Pool/Arena, UniquePtr, Rc
│   │   └── Module.cmake
│   ├── container/              # Array, String, HashMap, Hash, Span
│   │   └── Module.cmake
│   ├── math/                   # Vec, Mat, Quat, CPU detection, dispatch
│   │   └── Module.cmake
│   └── test/                   # Tiny test framework + EXPECT_* macros
│       └── Module.cmake
└── tests/                      # Module unit tests
    └── CMakeLists.txt
```

### Dependency Graph

```
                    Foundation
                  /     |       \
            Threading   |        Math
                  \     |       /
                  Memory       (Math depends on Threading for CPU dispatch)
                     |
                  Container
                     |
                   Test
```

## Key Design Decisions

| Subsystem | Choice | Rationale |
|---|---|---|
| Logger | Per-cell Vyukov bounded MPMC ring + writer thread | Producer hot path is one CAS; writer drains lock-freely. Reference: 1024cores.net Vyukov MPMC, Quill async logger. |
| ThreadPool | Per-worker Chase-Lev SPMC deque + global Mutex submission | Owner Push/Pop is atomic-free in the common case; external submit goes through a Mutex-protected fallback. Steal participates in `Wait()` to avoid deadlock. Reference: Chase & Lev SPAA 2005, enkiTS, Naughty Dog GDC 2015. |
| Atomic | Win32 `_Interlocked*` intrinsics | No `std::atomic`. Suffix variants (`_acq`, `_rel`) on ARM64; full-fence baseline on x64. |
| HashMap | Robin Hood + dense values + 8-bit fingerprint | ankerl::unordered_dense layout — fastest unsuccessful lookup, no tombstones, contiguous iteration. SIMD probing deferred to v2. |
| Allocators | Virtual `Allocator` base + System / Linear / Pool / Arena | Pool uses lock-free Treiber stack with 17-bit ABA tag in the upper bits of pointers (47-bit user space on x64). |
| Math | DirectXMath wrapped behind `Vec3 / Vec4 / Mat4 / Quat` | Microsoft-maintained, SSE2-AVX2 paths included, NEON-on-Windows ready. We expose ergonomic POD types and dispatch batch ops via a function-pointer table. |
| String | 24-byte SSO (22 inline bytes) + heap fallback | Matches absl/folly-style layout sized to a typical x64 cache-line third. |
| Test | Custom `ACS_TEST(Suite, Name)` macro + `EXPECT_*` | Avoids GoogleTest dependency. Mutex-protected registry, per-test failure counter. |

## Adding a New Module

1. Create `src/mymod/Module.cmake`:
    ```cmake
    acs_module(
        NAME    MyMod
        TYPE    Runtime
        SOURCES Foo.cpp Bar.cpp
        HEADERS Foo.h Bar.h
        PUBLIC_DEPS Foundation Container
    )
    acs_module_feature(MODULE MyMod NAME FANCY
        DEFINE MYMOD_FANCY DESCRIPTION "Enable fancy mode" DEFAULT OFF)
    ```
2. Drop the source/header files alongside it.
3. Enable in `modules.cmake`:
    ```cmake
    acs_enable_module(MyMod FEATURES FANCY)
    ```
4. Reconfigure CMake — the new `ACS::MyMod` target appears, with the module
   define `WITH_ACS_MYMOD=1` and the feature define `WITH_MYMOD_FANCY=1`
   visible to PUBLIC consumers.

## Phase 2 Roadmap

| Module | Backend candidates | Notes |
|---|---|---|
| Render | DirectX 12 (raw + DirectX-Headers + D3D12MA), or The-Forge, or bgfx | Pick exactly one; **The-Forge and bgfx cannot coexist** — both are full RHI abstractions. |
| Asset | Custom (DDS/glTF/wav) | Streaming-friendly. |
| ECS | entt or hand-rolled archetype | entt is mature header-only but uses STL. A from-scratch archetype ECS over `Array<T>` keeps the no-STL invariant. |
| Editor | Dear ImGui (DX12 backend) | The-Forge and bgfx both ship ImGui integrations. |
