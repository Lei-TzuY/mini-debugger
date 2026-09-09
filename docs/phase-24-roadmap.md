# Phase 24 roadmap — signal-restored register-backed source values

## Status

Phase 24 is complete for the current evidence-bounded Linux x86-64 milestone.

The phase began from a genuine GCC/Clang signal-handler core rather than a synthetic `ucontext_t`. A runtime-derived local named `interrupted_register_local` remains live at the interrupted application probe. Under both permanent compiler lanes at the existing `-O1 -g -gdwarf-4` signal-core build, the compiler emits a genuine register-backed location in **R12** (`DW_OP_reg12` / the equivalent active location-list entry). No register was chosen speculatively.

## Completed capability

- the signal fixture records the kernel-provided `REG_R12` value from the real handler `ucontext_t` as an independent oracle;
- the core integration independently reads the Linux x86-64 saved R12 slot from the core bytes and requires it to equal both the handler oracle and the compiler-proven runtime value `0xb5856a1a93a34cbc`;
- `recover_linux_x86_signal_frame()` now returns an explicit bounded signal-frame recovery result containing the established `EhFrameCursor` plus only the newly proven R12 slot;
- snapshot frame construction attaches that R12 value only to the exact interrupted frame restored across the signal boundary; later ordinary CFI frames are required to have no R12 ownership, so interrupted register state is not silently propagated through historical callers;
- the snapshot source-value evaluator accepts only the exact compiler-proven `DW_OP_reg12` scalar form added by this phase, requires immutable `InspectionRegisterState::r12` ownership, preserves bounded integer/pointer width checks, and marks the result as `SnapshotCoreRegister` provenance;
- `CoreInspectionSession::inspect_value("interrupted_register_local")` recovers the expected value on the restored interrupted frame;
- the genuine-core integration launches the real sibling `mdbg-core`, selects the restored frame, executes `print interrupted_register_local`, and requires the same value in CLI output;
- GCC and Clang-large, PIE and non-PIE signal-core workflows all cover the same kernel/core/restore/source-value path.

## Explicit bounds

Phase 24 does **not** generalize signal recovery to every Linux GPR and does not enumerate `DW_OP_reg0..31`. Only R12 is transported because both permanent compiler lanes produced that real location for the selected interrupted local.

The restored R12 value belongs only to the exact application frame reconstructed from the signal `ucontext_t`. Ordinary `.eh_frame` caller recovery does not inherit it unless a future independent CFI rule explicitly recovers that register.

Malformed or truncated signal contexts, unsupported register-backed locations, missing immutable register ownership, non-signal frames, ambiguous source scope, and unsupported value kinds remain explicit failures.

## Phase 25 promotion — signal-restored FP/SIMD source-value ownership

The next architectural boundary is floating/SIMD state interrupted by a signal. The current core model already proves top-frame NT_FPREGSET/XMM inspection, but that state belongs to the thread at core-dump time inside the crashing handler. It must not be reused as the historical application XMM state that the kernel saved when delivering the earlier signal.

Phase 25 must therefore remain evidence-gated:

1. Start from the same genuine signal-handler core path and produce one stable GCC/Clang interrupted floating value whose DWARF location is demonstrably XMM-register-backed at the saved application RIP.
2. Independently identify and validate the kernel-owned signal FP-state location reachable from the real `ucontext_t`; do not infer interrupted XMM values from the top-thread NT_FPREGSET note.
3. Extend signal-frame recovery with only the exact saved FP/SIMD state required by the compiler-produced case and bind it only to the restored interrupted frame.
4. Prove the existing source-value API and real `mdbg-core frame <index>` / `print <name>` recover that interrupted floating value under both permanent compiler lanes and PIE/non-PIE where deterministic.
5. Prove later ordinary CFI frames do not inherit the signal-restored SIMD state.
6. Truncated/unsupported fpstate layouts, unsupported XMM locations, ambiguous ownership, and non-signal frames must continue to fail explicitly.

If no stable compiler-produced XMM-backed interrupted local and kernel fpstate oracle can be demonstrated, Phase 25 must no-op rather than adding speculative signal-fpstate plumbing.

## Selection rule

Phase 25 is the current frontier. Work begins with compiler/kernel evidence, not production expansion. A bounded PR may proceed only after the real failing scenario is demonstrated; repeated register variants are not a roadmap strategy.
