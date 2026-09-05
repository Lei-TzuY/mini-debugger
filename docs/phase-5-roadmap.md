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

## Priority 2: compiler-preserved RBX XOR stack values — complete

The third Phase 5 slice promotes one of the previously deferred arithmetic expressions only after a real current-PC workflow requires it. `inspect_entry_parameter()` computes `transformed = entry_parameter ^ 0x55aa00ff33cc6699`, then calls a function that deliberately clobbers current RDI before an exported `transformed_local_probe`. GCC 13.3 and Clang 18.1.3 both select the active post-call location expression `DW_OP_breg3 (rbx): 0; DW_OP_constu 6172747335350380185; DW_OP_xor; DW_OP_stack_value` at that probe.

Completed Priority 2 contract:

- test-first PIE and non-PIE sessions under both permanent compiler lanes reach the real post-call probe, verify current RDI has already become the sentinel `0x777788889999aaaa`, and then fail specifically because the selected RBX/XOR expression is unsupported;
- the fixture keeps `transformed` unqualified so this slice isolates expression semantics instead of implicitly adding a separate `DW_TAG_const_type` capability; independent compiler probes confirm GCC and Clang still select the same post-call RBX/XOR expression;
- the evaluator accepts only `DW_OP_breg3`, one signed LEB128 offset, `DW_OP_constu`, one unsigned LEB128 constant, `DW_OP_xor`, one trailing `DW_OP_stack_value`, and no additional operations;
- evaluation reads RBX from the currently selected stopped TID, applies checked signed base adjustment, XORs the compiler-provided constant, and truncates through the already-resolved scalar type width;
- direct API and real CLI coverage prove `transformed == 0x458a30bf63ac1619` while current RDI remains the clobbered sentinel, so recovery cannot accidentally substitute the original argument register or historical entry state;
- existing entry-value, `DW_OP_breg5`, register, frame-base, pointer, structure, process-domain, and debugger execution coverage stays green under GCC and Clang-large;
- other `DW_OP_bregN` registers, shift/piece/dereference forms, alternative arithmetic sequences, malformed LEB128 operands, missing `DW_OP_stack_value`, and trailing operations remain fail-closed.

Priority 2 still does not create a general DWARF expression stack machine. The larger multi-register XOR/shift expressions visible elsewhere in compiler output remain unsupported until a separate active current-PC workflow independently requires them.

## Priority 3: compiler-composed multi-register stack values — complete

The fourth Phase 5 slice promotes exactly such a larger expression only after it becomes the active location of an existing executable workflow. The optimized arithmetic fixture now computes the same expected `arithmetic_local == 0x10203040506070a5` from six live arguments. At the exported `arithmetic_local_probe`, GCC 13.3 selects a current-PC expression that combines R8, R9, RCX, RDX, RSI, and RDI through zero-offset `DW_OP_bregN`, shift constants (`DW_OP_const1u`, exactly the emitted `DW_OP_lit8/16/24`), repeated `DW_OP_shl`/`DW_OP_xor`, and a final `DW_OP_stack_value`. Clang 18.1.3 materializes the same source value in `DW_OP_reg0 (rax)` and remains covered by the existing register path.

Completed Priority 3 contract:

- test-first GCC PIE and non-PIE sessions use the pre-existing direct API and CLI `print arithmetic_local` workflow and fail only because this newly selected current-PC composite expression is unsupported; all other GCC tests stay green while the complete Clang-large lane remains green on the same source fixture;
- the evaluator is a deliberately bounded stack interpreter for the compiler-proven expression surface only: zero-offset `DW_OP_breg1/2/4/5/8/9`, `DW_OP_const1u`, exactly the emitted `DW_OP_lit8/16/24`, `DW_OP_shl`, `DW_OP_xor`, one trailing `DW_OP_stack_value`, and no other operations;
- each register value comes from the currently selected stopped TID; nonzero register-relative offsets remain rejected in this composite path, binary operations require two stack operands, shift counts must be below 64, and completion requires exactly one final stack value;
- the recovered value is truncated through the already-resolved scalar type width and returned with the existing module/type/lexical ownership rather than introducing a parallel value representation;
- the same existing direct API and real CLI assertions prove `arithmetic_local == 0x10203040506070a5` for PIE and non-PIE after the GCC path becomes supported, while Clang continues through its pre-existing `DW_OP_reg0` path;
- unsupported registers, constants, arithmetic operations, malformed operands, stack underflow, invalid shifts, missing/floating `DW_OP_stack_value`, and trailing operations remain fail-closed.

Priority 3 still does not claim a general DWARF virtual machine. The bounded stack exists only because a real current-PC GCC expression required composition; pieces, dereference, arbitrary literals/registers, alternate arithmetic, and historical/cross-frame operations still require independent executable compiler evidence.

## Priority 4: compiler register-piece aggregate ownership — complete

The fifth Phase 5 slice is selected from a real by-value aggregate ABI case rather than by enumerating `DW_OP_piece`. A dedicated `-O1 -g -gdwarf-5` fixture passes a 16-byte `struct RegisterPair { uint64_t first; uint64_t second; }` by value and pins the debugger stop to the callee function entry. GCC 13.3 and Clang 18.1.3 both select the same active current-PC ownership expression there: `DW_OP_reg5 (rdi); DW_OP_piece 8; DW_OP_reg4 (rsi); DW_OP_piece 8`.

Completed Priority 4 contract:

- test-first PIE and non-PIE sessions under both permanent compiler lanes reach the real function-entry probe and fail only because structures were previously restricted to `DW_OP_fbreg` memory ownership; every pre-existing test remains green;
- the evaluator accepts only the compiler-proven 16-byte structure shape composed of exactly two 8-byte pieces, first from RDI and second from RSI, with no additional pieces or trailing operations;
- both registers are read from the currently selected stopped TID and serialized as little-endian owned aggregate bytes, then decoded through the same existing structure-member/type metadata used by memory-backed structures rather than introducing a second aggregate representation;
- direct API coverage verifies structure kind, 16-byte size, module identity, the exact `first == 0x1122334455667788` and `second == 0x99aabbccddeeff00` member values, and continued rejection by the integer-only API;
- real CLI coverage renders the same reconstructed `pair` through the existing structure printer, and PIE/non-PIE sessions exit cleanly under both GCC and Clang-large;
- the fixture exports the probe as an alias of the callee entry so the tested current PC is stable across compiler register-motion choices. A post-entry GCC range that moves the first piece from RDI to RAX is deliberately not used to broaden the supported family;
- arbitrary piece counts, alternate registers, non-8-byte pieces, `DW_OP_bit_piece`, mixed memory/register pieces, malformed ULEB sizes, and trailing operations remain fail-closed.

Priority 4 does not create a general piece assembler. It extends the existing typed aggregate ownership model only for the exact System V x86-64 register-pair shape independently emitted by both permanent compilers at the selected current PC.

## Priority 5: compiler register-relative memory local ownership — complete

The sixth Phase 5 slice is selected from a real optimized scalar local whose storage ownership crosses from the selected TID register file into inferior memory. The existing `-O1 -g -gdwarf-5` formal-parameter fixture now adds `indirect_local = *ptr` and stops at an exported probe before GCC materializes the load. GCC 13.3 selects the active current-PC location expression `DW_OP_breg5 (rdi): 0` with no `DW_OP_stack_value`, meaning the local resides in memory at the selected TID's RDI-relative address. Clang 18.1.3 has already materialized the same source value in `DW_OP_reg0 (rax)` and therefore remains covered by the existing direct-register path.

Completed Priority 5 contract:

- test-first GCC PIE and non-PIE sessions reach the real `indirect_local_probe` through the pre-existing source-value API and CLI workflow and fail specifically because the prior evaluator interprets every `DW_OP_breg5` expression as a stack value requiring a trailing `DW_OP_stack_value`; the same fixture remains fully green under Clang-large;
- the evaluator now distinguishes the two compiler-proven meanings without generalizing the expression machine: `DW_OP_breg5 + signed offset + DW_OP_stack_value` keeps the existing Priority 1 value semantics, while a bare `DW_OP_breg5 + signed offset` is treated as an inferior-memory location;
- the bare memory form accepts exactly one signed LEB128 offset and no trailing operations, reads RDI from the currently selected stopped TID, computes the runtime address with checked signed arithmetic, then reads exactly the already-resolved scalar type width through `Debugger::read_memory`;
- the recovered bytes reuse the existing integer decoder and preserve module, current-PC, lexical-scope, process/TID, and signed/unsigned type ownership rather than introducing a raw dereference or parallel memory reader;
- direct API and real CLI coverage prove `indirect_local == 0x8877665544332211` for PIE and non-PIE, while all pre-existing GCC tests and the complete Clang-large lane remain green;
- malformed signed offsets, trailing operations, unsupported register-relative memory registers, arbitrary dereference chains, and other expression forms remain fail-closed.

Priority 5 does not add `DW_OP_deref` or a general location-expression VM. It adds only the compiler-proven distinction between a register-relative value expression terminated by `DW_OP_stack_value` and the same bounded register-relative address used as owned memory storage.

## Current frontier: next compiler-produced expression failure

Do not select the next opcode family from the DWARF specification by enumeration. First find a real GCC or Clang current-PC location expression that the existing evaluator rejects in an otherwise valid source-value workflow, preserve the exact compiler evidence, and then add only the minimum semantics and ownership state required by that failure.

Potential surfaces such as dereference chains, inlined-scope ownership, richer stack-value combinations, additional piece layouts, or additional entry-value registers remain candidates only when real compiler output makes one independently necessary. A candidate that only appears in an inactive location-list range is not sufficient evidence.

## Selection rule

Choose the smallest compiler-generated current-PC failure that advances source-value capability without weakening existing process/TID/module/type/lexical ownership. If evaluation needs historical or cross-frame state the debugger has not captured, treat that as an architectural blocker and design the ownership explicitly rather than substituting current state. Unsupported or malformed compiler output remains fail-closed until executable evidence justifies a bounded extension.
