# Phase 5 roadmap

Phase 5 begins after Phase 4 establishes bounded source-value ownership across real DWARF4 and DWARF5 compiler output. The next architectural objective is not broader metadata parsing for its own sake; it is to evaluate richer compiler-produced location expressions while preserving the same current-PC, module, process/TID, register, memory, CFI, type, and lexical-scope ownership model.

## Priority 0: compiler-produced entry-value ownership — current frontier

The first candidate is already grounded in real evidence from the Phase 4 DWARF5 fixture: Clang 18.1 emits `DW_OP_entry_value` in one formal-parameter location-list range. Phase 4 deliberately does not evaluate that expression because its optimized-local probe selects a different `DW_OP_reg0` range.

Priority 0 acceptance:

- add a dedicated real `-O1 -g -gdwarf-5` probe whose current PC selects a compiler-produced entry-value location description; do not infer support merely because an unselected range contains the opcode;
- first demonstrate the exact current evaluator failure under the permanent GCC and Clang-large lanes while all Phase 4 source-value workflows remain green;
- extend the existing expression path rather than creating a second evaluator, and implement only the concrete nested expression/value semantics required by the selected compiler output;
- preserve selected-TID register ownership and distinguish an entry value from the current register value. If correct evaluation requires state that the debugger has not captured at the function-entry boundary, treat that as an architectural blocker and design the required ownership explicitly rather than substituting the current register value;
- malformed nested expressions, unsupported register numbers, unsupported stack/value operations, recursion/size overflow, and expressions outside the compiler-proven surface remain deterministic failures;
- prove the recovered value through direct API and real CLI in PIE and non-PIE, with module/type ownership intact and clean tracee exit.

Priority 0 is not permission to implement a general DWARF stack machine. The first implementation slice must be selected by a concrete active compiler-produced expression and must explain where the historical entry value comes from.

## Later priorities

After entry-value ownership, `DW_OP_stack_value`, arithmetic/composite expressions, pieces, dereference chains, inlined-scope ownership, and richer type presentation remain candidates only when a real compiler-produced current-PC failure makes one independently necessary. They should not be enumerated into opcode micro-PRs.

## Selection rule

Priority 0 is the current frontier. Construct the smallest compiler-generated scenario that makes `DW_OP_entry_value` the active location expression, capture the exact current failure first, then decide whether the missing capability belongs purely in expression evaluation or requires new function-entry state ownership. Do not implement the opcode by reading the current register and calling it an entry value.
