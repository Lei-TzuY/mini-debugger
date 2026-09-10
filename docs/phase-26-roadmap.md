# Phase 26 roadmap — signal-restored composite register-piece ownership

## Status

Phase 26 is complete for the current evidence-bounded Linux x86-64 genuine-core milestone.

The phase began from the signal-restored scalar and XMM ownership established by Phases 24–25. It did not widen historical register ownership by enumerating saved registers. Instead, a dedicated genuine signal-core workflow first proved one stable compiler-produced aggregate location under both permanent compiler lanes: a 16-byte `struct SignalRegisterPair` whose interrupted location at the exact saved application PC is `DW_OP_reg5 (rdi); DW_OP_piece: 8; DW_OP_reg4 (rsi); DW_OP_piece: 8`.

## Completed capability

- the dedicated signal-piece fixture keeps one 16-byte aggregate formal live across a synchronous `ud2` interruption while constraining its two 8-byte pieces through RDI and RSI without hand-authoring DWARF;
- the integration requires `readelf` to show the exact compiler-produced two-piece location and requires that location range to cover the exported interrupted probe under GCC and Clang, PIE and non-PIE;
- the real SA_SIGINFO handler publishes the kernel-provided `ucontext_t` address before deliberately crashing with different RDI/RSI marker values, so crash-time `NT_PRSTATUS` cannot accidentally satisfy the interrupted-frame ownership test;
- the integration independently reads the Linux x86-64 ucontext GPR slots for RDI, RSI, RSP, and RIP and requires the saved RDI/RSI values to equal the aggregate members while the crashed handler registers remain different;
- `recover_linux_x86_signal_frame()` transports only the newly compiler/kernel-proven RDI and RSI ownership together with the previously established signal-restored cursor, R12, and XMM0 state;
- `InspectionRegisterState` owns optional RDI/RSI only on the exact application frame restored across that signal boundary; later ordinary `.eh_frame` caller frames are explicitly required not to inherit either register;
- the existing bounded live-debugger register-piece decoder remains the single parser for the compiler-proven 16-byte `RDI(8) + RSI(8)` piece layout; it was generalized only at the register-source boundary so snapshot inspection can supply frame-owned restored registers without duplicating `DW_OP_piece` parsing;
- snapshot source-value evaluation accepts that register-piece aggregate only when both restored RDI and RSI are explicitly owned by the selected historical frame; a missing piece is an explicit failure and there is no fallback to crash-time `NT_PRSTATUS`;
- the reconstructed value keeps `SnapshotCoreRegister` provenance and preserves the original module ownership used by snapshot inspection;
- `CoreInspectionSession::inspect_value("interrupted_pair")` reconstructs both aggregate members with their expected 64-bit values;
- the genuine-core integration launches the real sibling `mdbg-core`, selects the restored frame, executes `print interrupted_pair`, and requires the complete aggregate value in CLI output;
- the dedicated handler/crash boundary is deliberately kept non-tail-callable so both GCC and Clang preserve the already evidence-proven signal-handler → libc trampoline → ucontext unwind shape rather than relying on compiler-specific tail-call behavior;
- GCC and Clang-large, PIE and non-PIE genuine-core workflows cover the same compiler DWARF → kernel ucontext → core snapshot → signal-frame restoration → source-value materialization path.

## Explicit bounds

Phase 26 does **not** generalize signal-restored register ownership into an arbitrary DWARF register map, arbitrary `DW_OP_piece` sequences, mixed GPR/SIMD pieces, bit pieces, pieces wider or narrower than the compiler-proven 8-byte pair, or register-piece values on ordinary non-signal historical callers.

The existing register-piece decoder remains fail-closed: the current evidence requires exactly a 16-byte structure, `DW_OP_reg5`, an 8-byte `DW_OP_piece`, `DW_OP_reg4`, a second 8-byte `DW_OP_piece`, and no trailing operations. Unsupported layouts remain explicit failures rather than being interpreted heuristically.

RDI and RSI provenance belongs only to the exact signal-restored interrupted frame. Ordinary CFI does not propagate those values, and historical source-value lookup never substitutes the crashed handler's `NT_PRSTATUS` register file for missing restored ownership.

## Phase 27 promotion — signal-restored pointer provenance and object reachability

The next architectural boundary is not another register number or another aggregate shape. Snapshot inspection already has bounded pointer type metadata and pointer dereference support, while Phase 26 now proves that historical signal restoration can own multiple compiler-saved GPR values with frame-local provenance. The next useful question is whether a pointer value restored from a genuine interrupted register can retain that provenance while crossing into snapshot memory/object inspection.

Phase 27 must remain evidence-gated:

1. Start from a genuine GCC/Clang signal-handler core and demonstrate one stable interrupted pointer/formal whose active DWARF location at the exact saved application RIP is a register location backed by kernel `ucontext_t` state.
2. Independently validate the pointer register from the real signal context and make the crash-time handler register value deliberately different; do not infer the historical pointer from top-frame `NT_PRSTATUS`.
3. Reuse existing pointer type metadata and snapshot dereference machinery. Do not create a signal-specific pointer renderer or a raw-address shortcut.
4. Prove the selected historical frame can inspect the pointer value and dereference it into bounded core-backed or runtime-artifact-backed object bytes with explicit provenance.
5. Prove `CoreInspectionSession` and real `mdbg-core frame <index>` / `print <name>` / `deref <name>` follow the same ownership path under both permanent compiler lanes and PIE/non-PIE where deterministic.
6. Missing register ownership, null/invalid pointers, unsupported pointer locations, absent bytes, ambiguous module provenance, malformed signal contexts, and compiler-shape drift remain explicit failures.

If no stable compiler-produced signal-interrupted pointer location and dereference workflow can be demonstrated under both permanent compiler lanes, Phase 27 must no-op and the next architectural frontier must be audited instead of enumerating restored registers or inventing synthetic DWARF.

## Selection rule

Phase 27 is the next frontier after Phase 26 integration. Work begins with compiler/kernel/core evidence and a real failing `CoreInspectionSession`/`mdbg-core` workflow. A bounded implementation may proceed only after the exact pointer location, restored register ownership, and target object bytes are independently demonstrated.
