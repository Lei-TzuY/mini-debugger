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

## Priority 1: compiler-proven caller register recovery — current frontier

The next Phase 6 slice should extend caller inspection only when real compiler output demonstrates a caller local or parameter whose selected location needs historical register state that Priority 0 does not recover.

Acceptance criteria:

- first capture a deterministic GCC or Clang caller-frame source-value failure whose active location is register-resident or otherwise requires a specific historical GPR; do not choose a register or DWARF expression speculatively;
- recover only the required caller register from the owning frame's actual `.eh_frame`/CFI rule and propagate it through `InspectionRegisterState`; never copy the currently active callee register as a substitute;
- keep current-frame inspection and the proven `DW_OP_fbreg` caller path unchanged;
- preserve stop-sequence, process-domain, TID, module, type, and lexical-scope ownership checks for the extended caller value;
- unsupported CFI register rules or DWARF register expressions remain deterministic failures rather than guessed values;
- prove the selected compiler-produced workflow in PIE and non-PIE and preserve both permanent compiler CI lanes, extending cross-compiler evidence only when the emitted location/rule is actually present.

Do not broaden this milestone into a generic saved-register database or enumerate DW_CFA/DW_OP register forms without a failing compiler-produced caller-value scenario.

## Selection rule

Choose a real compiler-produced caller-frame value whose recovery advances inspection semantics and whose required state can be owned explicitly. Do not implement generic frame UI, register guessing, or unrelated DWARF expression opcodes.
