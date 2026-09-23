# Phase 71 Roadmap — compiler-proven historical live-frame enum identity

Status: complete for the current Linux x86-64 optimized live-debugger historical-frame milestone.

Phase 71 carries canonical enum identity across one CFI-owned non-zero live inspection frame without borrowing current callee machine state.

## Completed acceptance

- A dedicated optimized GCC/Clang fixture retains a genuine caller-owned `HistoricalLiveMode historical_mode` with `HistoricalIdle = 3`, `HistoricalReady = 7`, and `HistoricalBusy = 42`.
- Independent readelf evidence proves the real enum DIE/name, four-byte unsigned representation, exact finite enumerator table, caller return-PC ownership, and compiler-produced historical location.
- Both permanent compiler lanes converge on the same new machine-state form: exact one-op `DW_OP_reg3 (rbx)` at the historical caller PC.
- Initial evidence heads intentionally remained red while the oracle exposed that `DW_OP_reg3` was not one of the previously supported historical forms and the product still had the explicit historical-enum guard.
- Production head `f95d24b1547668fccbda26c277fe975cd2095b9b` adds only exact `DW_OP_reg3` enum ownership. It requires `InspectionFrameContext::registers.rbx`; it does not read the current callee register file and does not add generic historical `regN` evaluation.
- The callee fixture deliberately writes distinct live RBX sentinel `0x1122334455667788` at the exact probe. This forces GCC/Clang to emit save/restore CFI and makes current callee RBX observably different from caller-owned enum state.
- Final evidence head `5c2c772a1bfa8381ddf50e91504f8982938c199c` requires frame 1 to contain CFI-recovered RBX whose low 32 bits equal 42, while current live RBX remains the distinct sentinel. This proves the value is cross-frame recovered ownership rather than accidental current-register reuse.
- Historical enum materialization reuses the Phase 70 canonical `materialize_live_scalar()` path and therefore preserves `LocalEnumType`, raw value, unique symbolic lookup, unknown numeric behavior, and duplicate-alias ambiguity without a historical-only enum representation.
- Inspection-frame validation still rejects stale frames after execution advances to a new stop.
- Exact final head `5c2c772a1bfa8381ddf50e91504f8982938c199c` passes full GCC / Clang-large CI plus both dedicated GCC / Clang compiler/core/session evidence lanes.
- The branch is based directly on main `016eeb912c2b2d98bb03bbc204597b62e3ae3643`, ahead by five commits and behind by zero.

Phase 71 is sealed here. Additional enum values, aliases, registers, or historical location variants are not new milestones.

## Phase 72 promotion — compiler-proven historical live pointer identity and one-hop dereference

The next meaningful capability gap is typed reachability from a live historical frame. Current-frame live pointers already preserve bounded pointee type information, and immutable core paths already prove one-hop typed dereference. The historical live evaluator can recover scalar bytes from CFI-owned frame state, but it does not yet expose an equivalent historical live pointer identity + dereference workflow.

The first coherent Phase 72 slice must:

1. Start from a genuine optimized GCC/Clang live artifact where one caller-owned pointer remains recoverable in frame 1 at a callee probe. Do not choose a register/location form before compiler evidence establishes it.
2. Independently prove the pointer DIE, exact x86-64 width, bounded integer pointee type, caller lookup PC, active historical location, and pointee target/value before product assertions run.
3. Reuse only machine state explicitly owned by `InspectionFrameContext` or stack/CFA recovery. If a register form is used, current callee state must be observably different whenever practical so historical ownership cannot be satisfied accidentally.
4. Preserve canonical pointer/pointee metadata through the existing live typed-value model; do not introduce a historical-only pointer descriptor.
5. Add exactly one bounded live historical dereference operation that reads the stopped tracee's memory at the recovered pointer, validates null/unreadable targets, and returns the already-supported bounded integer pointee shape. No pointer arithmetic, chains, casts, implicit dereference, or mutation.
6. Preserve inspection-frame stop identity: stale historical frames must remain invalid after execution advances.
7. Prove the real API and existing live debugger user surface where applicable. Do not create a second debugger session model or expression language.
8. Keep arbitrary historical `regN`, pointer-valued aggregates, recursive graphs, and unsupported DWARF forms out of scope unless the first genuine artifact requires one of them.
9. Require full GCC / Clang-large CI plus permanent compiler/oracle/live inspection evidence on the exact candidate before integration.

This promotion expands executable historical object reachability rather than enumerating more enum cases.
