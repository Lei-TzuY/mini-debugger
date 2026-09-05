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

## Priority 1: GCC register-relative stack values — complete

The second Phase 5 slice is again selected from a real active compiler location rather than from the opcode table. The `-O1 -g -gdwarf-5` fixture keeps `arithmetic_local = parameter + 0x25` live at an exported probe. GCC 13.3 selects the current-PC location expression `DW_OP_breg5 (rdi): 37; DW_OP_stack_value` for that probe, while Clang 18.1.3 materializes the same source local in `DW_OP_reg0 (rax)` and therefore remains covered by the pre-existing register path.

Completed Priority 1 contract:

- test-first GCC PIE and non-PIE sessions reach the real arithmetic-local probe with module, lexical-scope, type, and location-list selection already resolved, then fail specifically because the active `DW_OP_breg5 ...; DW_OP_stack_value` expression is unsupported; the Clang-large lane stays green on the same source workflow;
- the evaluator accepts only the compiler-proven `DW_OP_breg5`, one signed LEB128 offset, one trailing `DW_OP_stack_value`, and no additional operations;
- evaluation reads RDI from the currently selected stopped TID, performs checked signed 64-bit addition, then truncates through the already-resolved scalar type width before returning the value with existing module/type ownership;
- direct API and real CLI coverage prove `arithmetic_local == 0x10203040506070a5` for PIE and non-PIE, and the permanent GCC/Clang-large matrix remains green;
- malformed offsets, missing `DW_OP_stack_value`, trailing operations, and other `DW_OP_bregN` registers remain fail-closed.

The same GCC debug output also contains more complex register-relative arithmetic such as constant/XOR/shift expressions in other location ranges. Priority 1 deliberately does not interpret those expressions: they remain candidates only if a future current-PC source-value workflow independently requires them.

## Current frontier: next compiler-produced expression failure

Do not select the next opcode family from the DWARF specification by enumeration. First find a real GCC or Clang current-PC location expression that the existing evaluator rejects in an otherwise valid source-value workflow, preserve the exact compiler evidence, and then add only the minimum semantics and ownership state required by that failure.

Potential surfaces such as arithmetic/composite expressions, pieces, dereference chains, inlined-scope ownership, richer stack-value combinations, or additional entry-value registers remain candidates only when real compiler output makes one independently necessary. A candidate that only appears in an inactive location-list range is not sufficient evidence.

## Selection rule

Choose the smallest compiler-generated current-PC failure that advances source-value capability without weakening existing process/TID/module/type/lexical ownership. If evaluation needs historical or cross-frame state the debugger has not captured, treat that as an architectural blocker and design the ownership explicitly rather than substituting current state. Unsupported or malformed compiler output remains fail-closed until executable evidence justifies a bounded extension.
