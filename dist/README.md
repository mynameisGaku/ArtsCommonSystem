# ACS — Single-Header Distribution

Use the whole ACS engine from a **completely separate solution** with just:

```cpp
#include <acs.h>
```

This folder is self-contained. You do **not** need the ACS source tree.

```
dist/
├─ acs.h                     ← single amalgamated header (all public API)
├─ acs-distribution.sha256   ← exact header/library content manifest
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

ACS public ABI remains **free of STL-owned types, no-throw, and no-RTTI**. The official SDK includes
the Diligent static libraries, whose MSVC STL ABI uses exceptions. A consumer
translation unit **must** therefore enable the same compiler exception ABI even
though ACS APIs do not throw. `acs.h` contains `#error` guards for exception and
RTTI mismatches, so you get a clear message instead of cryptic link errors.

| Setting | Value | VS / cl flag |
|---|---|---|
| C++ standard | C++20 | `/std:c++20` |
| Exception ABI | enabled for Diligent; ACS APIs do not throw | `/EHsc` + `/D_HAS_EXCEPTIONS=1` |
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
`ole32`, `dbghelp`, `winmm`, `user32`, `gdi32`, `comdlg32`, `advapi32`, `Shlwapi`. Every entry after
`acs.lib` in this paragraph is a Windows SDK library.

`advapi32` は、同梱した mimalloc が Windows の process token を設定する
`OpenProcessToken` / `AdjustTokenPrivileges` / `LookupPrivilegeValueA` を解決するために必要です。
`comdlg32` は、同梱した Diligent Win32 platform のファイル選択APIを解決するために必要です。

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
acs.h OK | sum=42 dist=5.0 clamp=100.0 len=5.0 hash=2773fad09b34e937 event=1 component=1 scene_timer=1 log_sink=1
```

`examples\check.cpp` is the canonical "is my setup correct?" test: it includes
*only* `<acs.h>` and checks the container, math, easy, event, ECS, scene timer,
log sink and non-inline hash-library paths. The consumer does not list
individual libraries on its command line; `acs.h` supplies the `acs.lib`,
Windows SDK, Diligent and xxHash link directives above.

---

## What's included

`acs.h` exposes the full **public** runtime API: `foundation`, `math`, `timing`,
`container`, `memory`, `threading`, `platform`, `ecs`, `event`, `collision`,
`mvvm`, `asset` / `assetpack`, `audio`, `render` (the backend-agnostic `IRhi*`
interfaces + `CRenderer` / `CSpriteBatch` / `CStandardShader` / `CPbrShader` …),
`ui`, the `gameframework` (`CGame` / `AScene` / `AScene` / `ANode` /
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
python acs/scripts/amalgamate.py --self-test
python acs/scripts/amalgamate.py --check
python acs/scripts/audit_cpp_conventions.py --root dist --scope .
# 4) syntax-check the consumer
cl /nologo /Zs /std:c++20 /utf-8 /permissive- /Zc:__cplusplus /Zc:preprocessor /EHsc /GR- /D_HAS_EXCEPTIONS=1 /I dist dist/examples/check.cpp
# 5) configure, link, and execute a temporary Debug/Release consumer
python acs/scripts/run_distribution_consumer_smoke.py --distribution-root dist --configuration Debug --generator "Visual Studio 18 2026" --generator-platform x64
python acs/scripts/run_distribution_consumer_smoke.py --distribution-root dist --configuration Release --generator "Visual Studio 18 2026" --generator-platform x64
```

`acs/scripts/amalgamate.py` produces `acs.h` (inlines every public
`#include "..."`, hoists nothing — external `<...>` includes stay in place;
strips `#pragma once`; adds the link/ABI pragmas). `--check` renders the same
header in memory and byte-compares it with tracked `dist/acs.h`. Path assembly
normalizes drive roots, and the traversal identifies source files by filesystem
identity, so a `subst` drive or another path alias cannot inline one header
twice. `--self-test` covers this alias contract together with atomic replacement
and symbolic-link/reparse-point rejection.
`acs/scripts/build_single_header.ps1` runs it and merges the per-module
`acs_*.lib` (+ bundled `imgui`/`lua`/`ufbx`/`mimalloc`) into one `acs.lib` per
config, then requires and copies the adjacent Diligent/xxHash libraries.
The merge uses a unique response file and a same-directory temporary library,
publishing `acs.lib` only after `lib.exe` succeeds and the output is non-empty.
その後、`ACS_DIST_SHA256_V1`形式の`acs-distribution.sha256`を、UTF-8 BOMなし、
LF、相対path昇順、uppercase SHA-256で原子的に公開する。対象は`acs.h`と
Debug/Releaseの全libraryであり、consumerは構成directory全体の同一性を検証できる。
単一構成だけを再生成した場合は既存manifestを失効させ、local stagingとして扱う。
Debug/Releaseを同じ実行で再生成するまで`-Deploy`とnamed manifest公開は行えない。
`-Deploy`はsource配布物を変更する前にdrive root、source/build/distとの物理的な重複、
配置先tree内のreparse pointを事前拒否し、`robocopy /MIR /XJ`でmanifest以外のpayloadを
配置する。既存pathはrootからvolume rootまで各ancestorのvolume serial・file ID、
未作成pathは最深既存ancestor identityと正規化した残りcomponentで比較する。このため
SUBST、localhost UNC、8.3短縮名経由でも同一path・ancestor・descendantを拒否し、
descriptorまたはreparse検査に失敗した場合も変更前にfail-closedとなる。file集合、size、
SHA-256の完全一致を確認した後だけmanifestを原子的に置換し、最後にpayloadとmanifestを
再検証する。したがって、同size・同timestampの破損fileをrobocopyがskipした場合や
junctionがある場合は旧manifestを変更せず失敗する。配布tree全体に`.exe`、`.obj`、
`.pdb`、`.ilk`、一時file、正規28件以外の`.lib`など、正規file一覧にないものが
残る場合も拒否する。`-SelfTest`はcanonical形式、改ざん、物理alias overlap、
drive・UNC・extended・volume GUID rootのparent停止点、actual volume root直下、
repositoryがdrive rootまたはその直下にある場合の利用可能なparent chainと
canonical/alias root拒否、欠落、stale file、build成果物、reader lock、skip、
junction、mirrorの拒否契約も確認する。SUBST drive rootのcheckoutではdirectory pinの
物理final pathから実volumeを特定し、actual volume GUID root testを同じく完走させる。
同じsource配布生成と同じdeploy先への並行writerは、親directoryとrootのvolume serial・
file IDに基づくWindows global named mutexで待機せず拒否し、拒否時はpayloadとmanifestを
変更しない。SUBST、localhost UNC、利用可能な8.3短縮名も同じ物理identityへ合流する。
未作成deploy先は既存parent identityと残りpathを先に排他し、作成後のroot identity排他を
重ねてから処理を続ける。mutexはkernel objectなのでlock fileを配布treeへ作らず、
owner異常終了時はabandoned状態を回収する。処理中は配布rootのdirectory handleを
delete共有なしで保持し、sourceとdeploy先が同じ物理directoryならmirror前に拒否する。
lock取得後に外部processが未作成rootを先に作ってもensure後にroot identityを固定する。
移行失敗時は既存payloadとmanifestを変えず、自己作成した空の通常directoryだけを戻す。
未作成部分を持つ場合は既存ancestorごとのidentityと残りpathをすべて排他するため、
複数階層を一度に作成しても作成前後のlockに隙間はなく、異なる物理rootは並行できる。
これは同じscriptを使う協調writerの排他である。切替中も名前空間とidentityの両mutexを
保持するが、このmutexに参加しない外部processが権限の許す範囲で親directoryを
差し替えるTOCTOUまでは完全に防ぐ契約ではない。
配布pathは信頼できるlocal filesystem上で使用すること。

Deployment uses the named manifest as a commit marker. The payload mirror is
not rolled back when `/MIR`, post-copy validation, or manifest publication
fails. Such a tree is unpublished because its old manifest no longer matches;
verified consumers reject it, and the next successful deployment repairs it.

`run_distribution_consumer_smoke.py` creates a verified distribution snapshot,
its CMake source, and its build tree only under the operating-system temporary
directory. Before copying, it rejects reparse points and any file outside the
fixed 33-file mirror. It then pins the root, all directories, and all files
against rename or write, strictly parses the canonical 29-entry named manifest,
and verifies every header/library hash. The selected configuration, `acs.h`,
and the reviewed `examples/check.cpp` are copied into the snapshot. Configure,
link, and execution use only that snapshot; the live SDK root is not read after
snapshot creation. The three commands share one overall deadline. Each command
runs below a Windows Job Object, so a timeout stops CMake/MSBuild and every
descendant before the temporary tree is removed. The temporary tree is removed
on success, failure, or timeout. Pass `C:\acs` as `--distribution-root` to
validate the deployed mirror without creating `.obj` or `.exe` files inside
either SDK directory.

With `ACS_BUILD_TESTS=ON`, a top-level ACS CMake configure that has this sibling
`dist` directory also registers drift, convention-audit and `/Zs` consumer
tests. Set `ACS_ENABLE_DISTRIBUTION_CONSUMER_SMOKE=ON` after generating the
Debug and Release libraries to register
`ACS.DistributionConsumerSmokeDebug` and
`ACS.DistributionConsumerSmokeRelease`. An ACS source tree consumed through
`add_subdirectory` does not register those distribution-only checks.
