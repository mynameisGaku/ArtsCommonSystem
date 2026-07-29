# Engine Foundation Optimization: 80-Task Execution Ledger

## Objective

This programme improves engine-wide runtime efficiency without lowering render,
simulation, editor, or asset quality. Every accepted optimization must preserve
observable behaviour, memory safety, determinism requirements, and public API
contracts.

The work runs as rolling waves. Four workers are kept active (the current
physical concurrency limit), and completed workers immediately receive the next
independent wave. A task is not considered complete merely because it compiles.

## Acceptance contract

Each task must provide the evidence applicable to its risk:

1. A Release build and focused regression tests pass.
2. A deterministic structural metric records the avoided work (allocation,
   wake, poll, copy, lookup, transition, or submission count).
3. A bounded timing benchmark is reported when the host is stable enough for it
   to be meaningful. Timing is diagnostic; deterministic correctness gates are
   authoritative.
4. Template/`constexpr` work reports compilation-time and binary-size impact.
5. No visible quality setting, numerical precision, content fidelity, or frame
   result may be reduced to claim a speedup.
6. Any optimization that cannot demonstrate a benefit, or regresses safety or
   size disproportionately, is reverted rather than retained speculatively.

## Template metaprogramming policy

Template metaprogramming is used only where it removes repeated runtime work:
static traits, fixed dispatch tables, policy specialization, literal hashing,
typed iteration, and scalar/SIMD selection. It must not turn runtime data into
unbounded template instantiations. Explicit instantiation, small policy sets,
and compile-time assertions are required where they keep build cost bounded.

## Task ledger

| ID | Wave | Area | Deliverable | Initial owner | State |
|---:|:---:|---|---|---|---|
| T01 | A | Containers | Reduce `TArray` growth and relocation work with measured policies | A | Complete |
| T02 | A | Containers | Improve hash-table probing and heterogeneous lookup | A | Complete |
| T03 | A | Strings | Remove avoidable `FString` copies through `FStringView` paths | A | Complete |
| T04 | A | Serialization | Reduce JSON parser/writer allocation and token overhead | A | Complete |
| T05 | A | Memory | Improve allocator/object-pool hot paths and reuse | A | Complete |
| T06 | B | Threading | Reduce thread-pool wakeups and contention | B | Complete |
| T07 | B | Jobs | Reduce job-graph scheduling allocations and synchronization | B | Complete |
| T08 | B | Messaging | Add safe message batching and reduce broker contention | B | Complete |
| T09 | B | Timing | Improve timer scheduling and due-item traversal | B | Complete |
| T10 | B | Storage | Reduce filesystem/storage call and buffer overhead | B | Complete |
| T11 | C | Assets | Deduplicate asset-registry keys and accelerate lookup | C | Complete |
| T12 | C | ECS | Improve sparse-set/query locality and iteration overhead | C | Complete |
| T13 | C | ECS | Batch and merge entity command buffers efficiently | C | Complete |
| T14 | C | Scene | Reduce scene/reflection runtime dispatch overhead | C | Complete |
| T15 | C | RHI | Reduce DX12/RHI submission and redundant state work | C | Complete |
| T16 | D | Measurement | Add deterministic foundation performance harness and JSON report | Root | Complete |
| T17 | D | Input | Poll connected pads every frame and stagger disconnected probes | Root | Complete |
| T18 | D | Application | Add bounded minimized/background event wait without visible-frame loss | Root | Complete |
| T19 | D | Logging | Coalesce asynchronous logger wakeups safely | Root | Complete |
| T20 | D | Build | Add measured, reversible Release IPO/LTCG policy | Root | Complete |
| T21 | E | Containers/TMP | Add verified trivially-relocatable trait and `if constexpr` path | A | Complete |
| T22 | E | Hashing/TMP | Add constexpr literal hashing and heterogeneous lookup | A | Complete |
| T23 | E | Parsing/TMP | Generate fixed character/token classification at compile time | A | Complete |
| T24 | E | Memory/TMP | Specialize fixed allocator size/alignment classes | A | Complete |
| T25 | E | Math/TMP | Select scalar/SIMD math policy at compile time | A | Complete |
| T26 | F | Jobs/TMP | Introduce bounded inline callable storage for jobs | B | Complete |
| T27 | F | Events/TMP | Generate typed callback thunks without heap wrappers | B | Complete |
| T28 | F | Queues/TMP | Specialize queue policy for known producer/consumer topology | B | Complete |
| T29 | F | Paths/TMP | Classify common paths/extensions with constexpr tables | B | Complete |
| T30 | F | Timing/TMP | Specialize timer clocks/callback policies without virtual dispatch | B | Complete |
| T31 | G | ECS/TMP | Generate component traits and compact signatures | C | Complete |
| T32 | G | ECS/TMP | Add typed query iteration with static component access | C | Complete |
| T33 | G | Reflection/TMP | Generate bounded constexpr reflection dispatch tables | C | Complete |
| T34 | G | RHI/TMP | Add compile-time RHI format traits and validation | C | Complete |
| T35 | G | RHI/TMP | Generate stable pipeline/descriptor key components | C | Complete |
| T36 | H | Logging/TMP | Add compile-time severity gate with unchanged default semantics | Root | Complete |
| T37 | H | Logging/TMP | Add literal-log fast path that bypasses formatting safely | Root | Complete |
| T38 | H | Measurement | Report TMP dispatch, compile time, and binary size deltas | Root | Complete |
| T39 | H | Verification | Add bounded performance verifier/CTest gate | Root | Complete |
| T40 | H | Governance | Maintain ledger, evidence index, and static-safety audit | Root | Complete |
| T41 | I | Data layout | Measure and remove cache-line false sharing in hot shared state | A | Complete |
| T42 | I | Branching | Split measured hot/cold data and code paths | A | Deferred |
| T43 | I | Containers | Apply bounded inline capacity to a measured transient path | A | Complete |
| T44 | I | Memory | Reuse a measured allocation path with an intrusive/free list | A | Complete |
| T45 | I | Validation/TMP | Generate constexpr enum validation and lookup tables | A | Complete |
| T46 | I | Results | Improve hot/cold result propagation and branch layout | A | Deferred |
| T47 | J | Math/SIMD | Batch AABB and frustum tests with scalar parity fallback | C | Active |
| T48 | J | Transform | Add SoA transform-update batch with cache-local traversal | C | Active |
| T49 | J | ECS | Add measured blocked iteration/prefetch strategy | C | Active |
| T50 | J | Hash/SIMD | Add batch hash/CRC path with scalar parity fallback | C | Active |
| T51 | J | Serialization/TMP | Generate endian-safe typed read/write primitives | A | Complete |
| T52 | J | Memory | Add frame-arena batched suballocation and constant-time reset | A | Complete |
| T53 | K | Async I/O | Coalesce compatible asynchronous file-read requests | B | Complete |
| T54 | K | Storage | Add safe mapped-read path for large immutable assets | B | Complete |
| T55 | K | Compression | Reuse bounded decompression scratch buffers | B | Complete |
| T56 | K | Assets | Batch dependency resolution in stable topological order | B | Complete |
| T57 | K | Assets | Intern asset/DDC paths with bounded lifetime and diagnostics | B | Active |
| T58 | K | Packaging | Add package read-ahead/stream batching without format change | B | Complete |
| T59 | L | Render | Add inline storage for common render-command payloads | C | Active |
| T60 | L | RHI | Batch and safely recycle descriptor allocations | C | Active |
| T61 | L | Shader/TMP | Generate constexpr shader parameter/layout metadata | C | Active |
| T62 | L | Pipeline | Intern stable pipeline-state keys and cache lookups | C | Active |
| T63 | L | Profiling | Shard counters and batch profiler snapshots | B | Active |
| T64 | L | Verification | Aggregate build/performance reports into end-to-end gate | Root | Active |
| T65 | M | Handles/TMP | Add generation-checked static handle-layout traits and parity tests | A | Queued |
| T66 | M | Containers | Add word-batched bitset operations with scalar parity fallback | A | Queued |
| T67 | M | Lookup | Select tiny-table linear/sorted lookup policy from measured thresholds | A | Queued |
| T68 | M | Serialization | Add bounded zero-copy byte-view decoding for immutable input | A | Queued |
| T69 | M | Threading | Batch completion-counter updates without weakening wait semantics | B | Queued |
| T70 | M | Threading | Separate measured hot atomics onto independent cache lines | B | Queued |
| T71 | M | Events | Reuse subscriber dispatch snapshots without callback lifetime races | B | Queued |
| T72 | M | Paths | Normalize and hash canonical paths once across storage boundaries | B | Queued |
| T73 | N | Rendering | Batch transient upload-arena allocation and constant-time retirement | C | Active |
| T74 | N | Rendering/TMP | Generate stable draw-packet sort keys with compile-time field layout | C | Active |
| T75 | N | Render graph | Compute lifetime intervals and safe transient-alias candidates | C | Active |
| T76 | N | Culling | Add hierarchical visibility batches with scalar-result parity | C | Active |
| T77 | O | Startup | Measure module initialization and defer verified cold-only work | Root | Complete |
| T78 | O | ABI | Gate optimized public type layouts with size and alignment budgets | Root | Complete |
| T79 | O | Safety | Add stress gates for optimized ownership, queues, and cancellation | Root | Complete |
| T80 | O | Verification | Run clean Release, enabled backends, full tests, dist drift, and publish | Root | Queued |

## Baseline structural targets

The initial root wave has four deterministic baselines that do not depend on
machine timing:

- Disconnected XInput polling: 4 calls/frame -> at most 1 call/frame, while
  connected controllers remain polled every frame and reconnect within four
  frames.
- Logger wake signals: 1 signal/record -> signal only on a transition that can
  make an idle consumer runnable.
- Minimized application loop: continuous update/render polling -> bounded event
  wait with no update/render work while minimized.
- Release interprocedural optimization: absent/implicit -> explicit,
  user-disableable CMake policy with a recorded link-time and image-size delta.

## Integration discipline

- Work is developed in isolated worktrees based on the same `origin/main`.
- Each wave lands as a focused commit and is cherry-picked into the integration
  branch only after its focused checks pass.
- Before publishing, the integrated tree is rebuilt from a clean Release build
  directory, the complete test suite is run, generated distribution artifacts
  are refreshed, and upstream drift is checked.
- The user's unrelated dirty checkout is never staged, reset, cleaned, or used
  as integration evidence.
