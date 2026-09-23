# Phase 72 Roadmap — compiler-proven historical live pointer identity and one-hop dereference

Status: complete for the current Linux x86-64 optimized live-debugger historical-frame milestone.

Phase 72 extends CFI-owned historical live-frame source inspection from scalar/enum identity into one bounded pointer reachability operation. A caller-owned pointer recovered in frame 1 now preserves canonical pointer/pointee metadata and can perform exactly one checked read from the stopped tracee.

## Completed acceptance

- A dedicated optimized GCC/Clang fixture retains a genuine caller-owned `int32_t *historical_pointer` targeting `historical_pointer_target == 0x13579bdf`.
- Independent readelf evidence proves the real x86-64 pointer DIE, exact eight-byte width, signed four-byte integer pointee type, caller return-PC ownership, active location range, and compiler-produced historical location.
- Both permanent compiler lanes converge on the same new historical machine-state form: exact one-op `DW_OP_reg3 (rbx)` at the caller return PC.
- Test-first heads `14917244aa62f9e0b32f0ab949ea2bd2ed36ae2a`, `7319c84bb67ce4597745175186de9d66ae916d4f`, and `89e9c1b54795348c5d4acaa27f95da11751e5d4c` progressively establish the genuine formal-parameter/type/location evidence while normal CI remains red only at the intended historical pointer ownership boundary.
- Production head `7733984a019c47c46105ff40adfc3644e1b27959` reuses the already-canonical `BoundedRootTypeResolution` for historical live lookup rather than adding a historical-only pointer parser. Pointer roots preserve the existing `LocalValueType` pointee descriptor.
- Exact historical `DW_OP_reg3` pointer ownership requires `InspectionFrameContext::registers.rbx`; no arbitrary historical `regN` evaluator or fallback to the current callee register file is added.
- The callee deliberately clobbers live RBX to `0x1122334455667788`. The integration requires frame 1's CFI-recovered RBX to equal the genuine target runtime address and to differ from current callee RBX, proving cross-frame ownership rather than accidental current-register reuse.
- `materialize_live_scalar()` now optionally carries canonical pointer pointee metadata while leaving existing integer/enum behavior unchanged.
- A bounded live `dereference_local_pointer(Debugger, ElfFile, InspectionFrameContext, name)` operation performs exactly one stopped-tracee memory read. It rejects non-pointers, missing/unsupported pointee metadata, null pointers, host-width overflow, and short reads, and returns a terminal bounded integer value.
- API evidence proves `historical_pointer` preserves pointer kind, x86-64 width, exact target address, signed-int32 pointee metadata, and one-hop dereference to `0x13579bdf`.
- Historical inspection-frame stop identity remains authoritative: after execution advances, reusing the old caller frame is rejected.
- Exact production head `7733984a019c47c46105ff40adfc3644e1b27959` passes full GCC / Clang-large CI plus both dedicated GCC / Clang compiler/core/session evidence lanes.
- The branch is based directly on main `d6dfa68d6cc800f665b41867d47a63eb5cc2f3ad`, ahead by four commits and behind by zero.

Phase 72 is sealed here. More historical registers, pointer aliases, pointer arithmetic, chains, casts, implicit dereference, mutation, or alternate integer pointee widths are not new milestones.

## Phase 73 promotion — compiler-proven historical live pointer to bounded structure

The next meaningful executable frontier is historical object reachability rather than another register or scalar-pointee variant. Immutable snapshot inspection already supports one-hop pointer-to-bounded-structure materialization, and the canonical root descriptor already carries bounded structure pointee metadata. Live historical dereference currently stops at a terminal integer pointee.

The first coherent Phase 73 slice must:

1. Start from a genuine optimized GCC/Clang live artifact where one caller-owned pointer to a bounded flat structure remains recoverable in frame 1 at a callee probe. Do not choose its historical location form before compiler evidence proves it.
2. Independently prove the pointer DIE/width, exact bounded structure pointee byte extent, direct member names/offsets/scalar widths/signedness, caller lookup PC, active historical location, target runtime address, and target member values before product assertions run.
3. Reuse only machine state explicitly owned by `InspectionFrameContext` or compiler-proven CFA/stack recovery. Current callee state must remain observably distinct whenever practical.
4. Reuse canonical `resolve_bounded_root_value_type()` and the already-supported bounded structure pointee descriptor; no historical-only structure layout parser or pointer descriptor is allowed.
5. Extend the live one-hop dereference operation only enough to materialize the already-supported flat bounded structure from stopped-tracee memory using the existing canonical structure decoder/member metadata. Preserve the integer-pointee path.
6. Keep traversal depth fixed at one dereference. No recursive graphs, pointer-valued nested traversal, arrays, arbitrary member paths, pointer arithmetic, casts, implicit dereference, or mutation.
7. Preserve stale-frame rejection after execution advances and deterministic null/unreadable/invalid-layout failures.
8. Prove the real API and existing live debugger surface where applicable without inventing a second historical-session model solely for presentation.
9. Require full GCC / Clang-large CI plus permanent compiler/oracle/live inspection evidence on the exact candidate before integration.

This promotion expands historical live object reachability through an already-canonical bounded type surface rather than enumerating more register/value variants.
