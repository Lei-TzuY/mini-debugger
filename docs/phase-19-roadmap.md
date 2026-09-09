# Phase 19 Roadmap — bounded aggregate navigation through core pointers

Status: **complete for the current bounded Linux x86-64 genuine-core milestone**.

Phase 19 composes two already-proven post-mortem capabilities rather than creating a new expression engine: Phase 18 one-step typed pointer dereference and the existing bounded flat-structure decoder. The completed slice is driven by genuine compiler output and immutable core/runtime-artifact memory ownership.

## Completed executable slice

- the existing optimized XMM/core fixture now keeps a compiler-owned `struct AggregatePointee *aggregate_pointer` live at the real crash probe; its pointee is a 16-byte flat structure containing two direct unsigned 64-bit members with deterministic values;
- both permanent GCC and Clang-large lanes compile PIE and non-PIE versions, produce real kernel core dumps, and expose the pointer/type graph through compiler-generated DWARF4;
- test-first evidence reached the genuine core successfully and failed only because the snapshot pointer path previously forced every pointee through the integer-only type resolver; all normal CTest coverage remained green;
- snapshot pointer resolution now identifies the 8-byte pointer itself before materializing its pointee, so the snapshot/core path can retain either the previously proven bounded integer pointee or an already-supported bounded structure pointee without broadening live ptrace type semantics;
- `LocalPointeeType` retains only the validated structure metadata required for one dereference: total byte size plus direct member name, offset, scalar width, and signedness;
- structure pointee metadata is derived through the existing `resolve_value_type()` structure parser, preserving the same 256-byte aggregate limit, 32-member limit, constant offsets, named direct members, bounded scalar-member types, and in-storage range checks;
- `dereference_local_pointer()` reconstructs that validated bounded layout and routes the pointee bytes through `read_snapshot_memory()` and the existing `materialize_snapshot_memory_value()` structure decoder rather than adding a pointer-specific aggregate renderer;
- immutable core-memory and explicit runtime-artifact fallback provenance remain visible on the dereferenced value; pointer dereference does not reinterpret mutable live-process ownership;
- the core API proves `aggregate_pointer -> *aggregate_pointer` yields `LocalValueKind::Structure` with exactly `first=0x0123456789abcdef` and `second=0xfedcba9876543210` under both compiler lanes;
- the permanent genuine-core CLI gate now executes `deref aggregate_pointer` and requires the existing structure renderer to produce the same two members, proving the source-value API and `mdbg-core` presentation compose end to end.

The scalar-pointer behavior from Phase 18 remains covered in the same genuine-core workflow, so aggregate support is additive rather than a replacement path.

## Explicit boundary

Phase 19 proves one controlled dereference from a typed pointer to an already-supported flat structure. It does **not** add arrays, nested aggregates, bit-fields, inheritance, pointer-valued structure members, recursive traversal, pointer-to-pointer expansion, `ptr->field`, arbitrary member expressions, or a second dereference. Those surfaces require separate compiler-produced evidence and must not be enumerated as follow-up micro-PRs.

The shared live/local DWARF type resolver is intentionally not broadened merely because snapshot/core inspection now composes pointer and structure metadata. Phase 19 changes only the immutable post-mortem path demonstrated by the genuine-core artifact.

## Phase 20 promotion — genuine-core stack-resident local recovery

The next architectural frontier is ordinary stack-owned source values in immutable snapshots, not another pointer/aggregate shape.

The live debugger already has bounded compiler-proven `DW_OP_fbreg` handling, while the snapshot/core frame-zero evaluator currently accepts only its demonstrated XMM-register or `DW_OP_addr` memory forms (plus the established caller-frame path). A genuine core can therefore contain a perfectly ordinary compiler-produced stack local whose type and frame are known but whose value cannot yet be recovered through post-mortem source inspection.

The first Phase 20 slice should start from a real GCC/Clang core artifact whose selected frame contains a deterministic stack-resident local represented by `DW_OP_fbreg`, then prove all of the following:

1. identify the compiler-produced `DW_OP_fbreg` scenario before changing production code; no hand-written DWARF or synthetic location expression is acceptable;
2. derive the frame-base/CFA ownership from the selected immutable inspection frame and existing unwind/register model rather than borrowing a live ptrace frame base;
3. evaluate only the demonstrated bounded `DW_OP_fbreg` form and compute the stack address with checked signed arithmetic;
4. read the value through `read_snapshot_memory()` so core bytes and explicit runtime-artifact fallback retain their existing provenance and ambiguity semantics;
5. reuse the established integer/structure materializers when the compiler-produced type is already supported instead of creating a stack-local-specific decoder;
6. preserve selected-thread/frame identity and reject stale, missing, ambiguous, or unsupported frame-base/location evidence explicitly;
7. prove the same source value through the core inspection API and `mdbg-core` under both permanent GCC and Clang-large lanes, with PIE/non-PIE coverage where the compiler emits the scenario deterministically.

Do not turn Phase 20 into generic DWARF-expression interpretation. If the permanent compiler lanes do not produce a stable bounded stack-local failure, audit the next post-mortem architectural frontier instead of manufacturing one.

## Selection rule

Choose a genuine core/source scenario whose selected frame, type, and immutable memory are already trustworthy but whose compiler-owned stack location is rejected solely because snapshot inspection lacks the demonstrated `DW_OP_fbreg` ownership path. Once one coherent stack-local slice is proven, reassess the architecture before expanding location-expression variants.
