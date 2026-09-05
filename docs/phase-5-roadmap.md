# Phase 5 roadmap

Phase 5 begins after Phase 4 establishes bounded source-value ownership across real DWARF4 and DWARF5 compiler output. The architectural objective is not broader metadata parsing for its own sake; it is to evaluate richer compiler-produced location expressions while preserving the same current-PC, module, process/TID, register, memory, CFI, type, and lexical-scope ownership model.

## Priority 0: compiler-produced entry-value ownership — complete

The first Phase 5 slice is grounded in active GCC and Clang optimized DWARF5 output rather than an unselected opcode occurrence. A dedicated `-O1 -g -gdwarf-5` formal-parameter fixture produces a one-byte current-PC range whose selected location description is `DW_OP_entry_value(DW_OP_reg5); DW_OP_stack_value` in both permanent compiler lanes.

Completed Priority 0 contract:

- the integration derives the active entry-value PC from the real ELF function extent (`st_value + st_size - 1`) instead of relying on a source-level asm label whose address is not guaranteed to match an optimizer location-list transition;
- the existing source-value expression path accepts only the compiler-proven bounded nested form: one `DW_OP_entry_value`, a one-op `DW_OP_reg5` nested expression, `DW_OP_stack_value`, and no trailing operations;
- `Debugger` records the complete selected-TID register state when a managed breakpoint is actually observed at a function-entry runtime address, and source-value evaluation resolves the subprogram's DWARF low PC back to that observed breakpoint snapshot;
- the historical entry value is never substituted with the current register. A probe-only session that reaches the entry-value range without first observing the function-entry breakpoint fails explicitly because the required historical snapshot does not exist;
- repeated managed hits replace the observation for the same TID/runtime entry, while process-domain switching, exec/image replacement, detach, and task teardown discard snapshots with the same ownership boundaries as the rest of debugger state;
- malformed nested expressions, unsupported registers/forms, missing historical state, and expressions outside the compiler-proven surface remain deterministic failures;
- direct API and real CLI coverage exercise PIE and non-PIE under both GCC and Clang-large. The test deliberately clobbers current RDI to a sentinel before the entry-value range and proves the recovered parameter remains the original function-entry RDI with module/type ownership intact and clean tracee exit.

Priority 0 does not create a general DWARF stack machine. Historical function-entry state remains bounded to debugger-observed managed breakpoint stops; the implementation does not claim arbitrary frame reconstruction, recursion-aware entry histories, or call-site parameter recovery.

## Current frontier: next compiler-produced expression failure

Do not select the next opcode family from the DWARF specification by enumeration. First find a real GCC or Clang current-PC location expression that the existing evaluator rejects in an otherwise valid source-value workflow, preserve the exact compiler evidence, and then add only the minimum semantics and ownership state required by that failure.

Potential surfaces such as arithmetic/composite expressions, pieces, dereference chains, inlined-scope ownership, richer stack-value combinations, or additional entry-value registers remain candidates only when real compiler output makes one independently necessary. A candidate that only appears in an inactive location-list range is not sufficient evidence.

## Selection rule

Choose the smallest compiler-generated current-PC failure that advances source-value capability without weakening existing process/TID/module/type/lexical ownership. If evaluation needs historical or cross-frame state the debugger has not captured, treat that as an architectural blocker and design the ownership explicitly rather than substituting current state. Unsupported or malformed compiler output remains fail-closed until executable evidence justifies a bounded extension.
