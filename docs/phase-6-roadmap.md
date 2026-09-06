# Phase 6 roadmap

Phase 6 begins after Phase 5 establishes bounded compiler-produced source-value evaluation for the live stopped frame. Its architectural goal is to separate **execution ownership** from **inspection-frame ownership**.

Execution ownership remains the live active process/TID that ptrace may resume, single-step, deliver signals to, mutate registers/memory for, or program hardware watchpoints on. Inspection-frame ownership may instead refer to frame 0 or a caller frame whose PC, module, unwind cursor, recovered registers, memory, type, and lexical scope are used only for source inspection.

A caller-frame value must never be faked by reading the active callee TID register file. Registers required by a non-current frame must come from explicit unwind recovery. Inferior-memory reads remain owned by the correct process domain/TID. Unsupported or unavailable state fails explicitly.

## Priority 0: caller-frame source-value ownership — complete

The first Phase 6 slice establishes an executable caller-frame inspection boundary rather than a presentation-only frame selector.

Completed bounded capability:

- `InspectionFrameContext` carries the owning process/TID, monotonic debugger-stop identity, origin live-register fingerprint, frame PC/module identity, stack/frame cursors, and bounded recovered register state;
- frame 0 is constructed from the actual active stopped TID and preserves the existing live-frame source-value path;
- the existing CFI machinery derives a real caller frame and PIE/non-PIE GCC/Clang-large integration recovers a compiler-produced caller local from `DW_OP_fbreg` stack storage without substituting callee registers;
- caller-frame source lookup preserves module, type, lexical-scope, and process/TID ownership and fails explicitly when the caller expression requires unavailable historical state;
- inspection contexts are bound to a monotonic stop sequence as well as the live RIP/RSP/RBP fingerprint, so a new stop invalidates an older frame even when the tracee returns to an identical machine state;
- process-domain selection advances the same inspection generation, preventing an old frame from becoming valid again after an A→B→A process-selection round trip;
- execution ownership remains independent: constructing or using an inspection frame does not redirect resume, step, signal delivery, register/memory mutation, breakpoint displacement, or hardware-watchpoint ownership.

This milestone deliberately does not claim an arbitrary historical register file, generic frame UI, or post-mortem debugging.

## Priority 1: compiler-proven caller register recovery — complete

The second Phase 6 slice extends caller inspection from stack-owned values to one real compiler-produced historical-register case without inventing a generic saved-register database.

Completed bounded capability:

- the optimized formal-parameter fixture creates a deterministic caller value whose active caller-frame DWARF location is the already compiler-proven `DW_OP_breg3` + `DW_OP_constu` + `DW_OP_xor` + `DW_OP_stack_value` shape while the callee deliberately clobbers live RBX to a sentinel;
- the owning callee's real `.eh_frame` rule recovers the caller RBX value from its CFI-defined saved state and carries it in `EhFrameCursor`; the live callee register file is never substituted for historical state;
- recovered RBX is propagated through `InspectionRegisterState`, while initial unwind cursors retain the actually known live RBX so `same_value` semantics have an explicit source;
- caller-frame source-value evaluation reuses the same strict compiler-proven `DW_OP_breg3` expression parser, but it is driven only by `frame.registers.rbx`; missing historical RBX remains an explicit failure;
- the previous frame-0 evaluator and caller `DW_OP_fbreg` path remain intact, and unsupported caller register expressions still fail rather than being guessed;
- PIE and non-PIE integration proves the recovered `transformed` value under both permanent GCC and Clang-large CI lanes while also proving the live callee RBX remains the sentinel throughout inspection.

Priority 1 deliberately does not generalize to arbitrary GPR recovery, arbitrary `DW_CFA_*` rules, or arbitrary `DW_OP_breg*` caller expressions. Any future historical-register extension must start from a new compiler-produced source-value failure.

## Phase 6 status — complete for the compiler-proven caller-inspection milestone

Phase 6 now has an explicit ownership model for live-frame values, caller stack values, and one compiler-proven caller register-resident value. Continuing by enumerating more GPRs or DWARF opcodes without a concrete compiler failure would be variant farming rather than an architectural advance.

The next architectural frontier moves from live ptrace-owned inspection to an **immutable post-mortem inspection domain**. A core file has no resumable TID and no ptrace register file, so reusing live execution ownership would be incorrect. Phase 7 therefore begins with bounded ELF64 core-dump ownership: explicit snapshot registers, mapped-memory segments, and module identity that can feed existing symbol/source/unwind inspection without pretending the snapshot can execute.

See `docs/phase-7-roadmap.md`.

## Selection rule

Reopen Phase 6 only for a new real compiler-produced caller-frame source value that demonstrably requires historical state not represented by the completed ownership model. Do not implement generic frame UI, register guessing, or unrelated DWARF expression opcodes.
