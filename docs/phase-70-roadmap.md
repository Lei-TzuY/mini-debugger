# Phase 70 Roadmap — compiler-proven live enum identity

Status: complete for the current Linux x86-64 optimized live-debugger compiler-evidence milestone.

Phase 70 closes the current-frame live enum gap without widening the DWARF location VM. A genuine optimized enum local now retains canonical enum identity while being materialized through the same live scalar register path already used by integer values.

## Completed acceptance

- The permanent `-O1 -g -gdwarf-4` formal-parameter fixture now retains a genuine `LiveMode live_mode` with `LiveIdle = 3`, `LiveReady = 7`, and `LiveBusy = 42`.
- An independent readelf oracle proves the real `DW_TAG_enumeration_type`, exact four-byte unsigned representation, finite enumerator table, exported `live_enum_probe`, and the probe-active compiler location before product assertions run.
- Both permanent GCC and Clang-large lanes independently emit the same already-supported live location form at the probe: `DW_OP_reg5 (rdi)`. No new register opcode, location-list form, or expression operation was added.
- Initial evidence head `4d107b7ddc161819c7c14685c0e2705d21cf4def` deliberately failed both oracle and product checks because the first oracle assumption required RAX. Live compiler output instead proved RDI under both lanes.
- Evidence head `21316fb242994b165866a406d9a78fe3cbb1011e` pins the oracle to the genuine `DW_OP_reg5 (rdi)` form. On that exact head the enum oracle passes under GCC and Clang while only the new API/CLI tests fail at the explicit product guard `live enum materialization is outside current compiler evidence`; the other 54 tests remain green.
- Production head `b12e79f741598aabc29b54220194d7e11f173d4c` removes only the current-frame live enum guard. The historical live-frame enum guard remains fail-closed.
- One `materialize_live_scalar()` helper now preserves canonical root enum metadata for all already-supported current-frame scalar location paths while leaving ordinary integer/pointer behavior unchanged. It rejects aggregate/floating misuse and inconsistent enum metadata deterministically.
- The live CLI now uses the same conservative symbolic lookup semantics as `mdbg-core`: a unique raw match renders `LiveMode::LiveBusy (0x2a)`; unknown values remain numeric and duplicate raw aliases remain ambiguous rather than choosing an arbitrary name.
- Real API evidence proves kind, width/signedness, raw value 42, exact enum name/table, unique-symbol lookup, unknown numeric behavior, and duplicate-alias ambiguity.
- Real `mdbg` subprocess evidence proves `break live_enum_probe -> continue -> print live_mode` renders symbolic + numeric identity.
- Exact production head `b12e79f741598aabc29b54220194d7e11f173d4c` passes full GCC / Clang-large CI plus both dedicated GCC / Clang selected-inline evidence lanes.

Phase 70 is sealed here. More current-frame enumerator values, aliases, alternate names, or extra enum locals are not new milestones.

## Phase 71 promotion — compiler-proven historical live-frame enum identity

The next meaningful boundary is cross-frame machine-state ownership. The live debugger already builds immutable-at-stop `InspectionFrameContext` values from CFI, validates stop identity, and materializes compiler-proven historical caller integers without borrowing current-frame state. Canonical enum identity is now proven in the current live frame, but the historical live-frame evaluator still explicitly rejects enum roots.

The first coherent Phase 71 slice must:

1. Start from a genuine optimized GCC and Clang live artifact where a caller-owned enum local remains recoverable in a non-zero inspection frame at the callee probe. Do not force a register/location form before compiler evidence establishes it.
2. Independently prove the enum DIE/name/width/signedness/enumerator table, exact caller lookup PC, and the active historical location expression before product assertions run.
3. Reuse only machine state actually present in `InspectionFrameContext` or caller stack/CFA recovery. Never substitute current callee registers for unrecovered historical registers.
4. Preserve canonical `LocalEnumType` through the existing historical scalar materialization path and conservative symbolic lookup. No historical-only enum descriptor or renderer is allowed.
5. Prove API behavior on frame 1 plus stale-frame rejection after a new stop. The value must not mutate or redirect live execution state.
6. Prove a real user-visible workflow through the existing live-debugger inspection surface where applicable; do not add a second expression language or debugger session model.
7. Keep enum mutation, arithmetic, flags decomposition, enum-valued aggregate variants, additional historical register recovery, and unsupported DWARF forms out of scope unless genuine compiler evidence makes one of them necessary to the first slice.
8. Require full GCC / Clang-large CI plus permanent compiler/oracle/live inspection evidence on the exact candidate before integration.

This promotion expands enum identity across a distinct CFI-owned execution context rather than farming additional current-frame enum variants.
