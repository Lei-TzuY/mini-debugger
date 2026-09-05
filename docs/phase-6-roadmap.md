# Phase 6 roadmap

Phase 6 begins after Phase 5 establishes bounded compiler-produced source-value evaluation for the live stopped frame. Its architectural goal is to separate **execution ownership** from **inspection-frame ownership**.

Execution ownership remains the live active process/TID that ptrace may resume, single-step, deliver signals to, mutate registers/memory for, or program hardware watchpoints on. Inspection-frame ownership may instead refer to frame 0 or a caller frame whose PC, module, unwind cursor, recovered registers, memory, type, and lexical scope are used only for source inspection.

A caller-frame value must never be faked by reading the active callee TID register file. Registers required by a non-current frame must come from explicit unwind recovery. Inferior-memory reads remain owned by the correct process domain/TID. Unsupported or unavailable state fails explicitly.

## Priority 0: caller-frame source-value ownership — current frontier

The first Phase 6 slice must be an executable frame-inspection workflow, not a presentation-only `frame` command. Select a real two-or-more-frame compiler fixture with a caller local or parameter that is visible at a stable callee stop and prove caller inspection uses an explicit frame context.

Acceptance criteria:

- introduce an explicit inspection-frame context containing at least the owning process domain/TID, frame PC/module identity, and the bounded unwind register state required by the selected compiler-produced location;
- frame 0 derives from the actual active stopped TID and remains behaviorally identical to the existing current-frame source-value path;
- derive at least one real caller frame through the existing bounded unwind/CFI machinery and recover a compiler-produced caller local or parameter with correct module/type/lexical ownership in PIE and non-PIE under both permanent compiler lanes;
- if the selected caller expression requires a register that the current CFI cursor does not recover, extend register recovery only for the compiler/CFI evidence required by that fixture; never substitute the callee's current register;
- real CLI coverage may expose `frame <index>` or frame selection only in the same slice that can successfully inspect a caller value; a shell-only selector is not sufficient;
- changing the inspection frame must not redirect `continue`, `step`, breakpoint displacement, signal delivery, register mutation, memory mutation, or hardware-watchpoint ownership away from the live active TID;
- any resume, new stop, process-domain switch, exec/image replacement, detach, or equivalent execution-state transition invalidates stale non-current frame contexts before further inspection;
- invalid frame indices, unavailable unwind state, unsupported caller expressions, and cross-domain ambiguity remain deterministic failures.

Priority 0 is not a promise of an arbitrary historical register file or post-mortem debugger. It establishes the ownership boundary required before deeper frame-aware locals, arguments, inline-frame inspection, or richer historical value reconstruction can be added safely.

## Selection rule

Choose a real compiler-produced caller-frame value whose recovery advances inspection semantics and whose required state can be owned explicitly. Do not implement generic frame UI, register guessing, or unrelated DWARF expression opcodes.
