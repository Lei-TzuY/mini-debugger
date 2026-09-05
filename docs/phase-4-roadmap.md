# Phase 4 roadmap

Phase 4 starts after the Phase 3 process-domain registry removes the remaining one-image/one-process-topology assumption. The architectural objective is to make source-level debug data executable: the debugger should be able to identify a variable in the current source scope, evaluate the compiler-produced DWARF location that owns its value, read that value from the selected process/TID, and present it without bypassing the existing module, register, memory, CFI, or process-domain ownership models.

Phase 4 is complete for its bounded source-value ownership milestone. Priorities 0–6 now cover real GCC and Clang output from stack-backed integers through optimized DWARF5 location ownership. Broader DWARF expression semantics are promoted to Phase 5 rather than being accumulated here as speculative opcode variants.

## Priority 0: bounded local integer value inspection — complete

The first source-value slice is executable under the permanent GCC and Clang-large gates:

- the existing DWARF4 C fixture contains a noinline `inspect_local_value` function whose `uint64_t local_value` is derived from a volatile runtime seed and remains live at the exported probe;
- `inspect_local_integer` consumes the owning module's real `.debug_info`, `.debug_abbrev`, and `.debug_str`, with DWARF32/version-4/8-byte-address bounds and explicit rejection for forms outside the compiler-proven surface;
- the selected runtime PC is routed through the existing module resolver and ELF load bias before DIE lookup, so debug information from unrelated modules is never searched as if address spaces were interchangeable;
- the bounded DIE walk resolves the active subprogram, one local by name, its typedef chain, and a 1–8 byte signed/unsigned base integer type; missing, ambiguous, malformed, or unsupported structures fail explicitly;
- compiler-produced location evaluation is deliberately narrow: the local must use `DW_OP_fbreg`; GCC's `DW_AT_frame_base = DW_OP_call_frame_cfa` is evaluated through the existing `.eh_frame` CFI cursor, while Clang's `DW_OP_reg6 (rbp)` uses the selected thread's register state;
- the final value read goes through `Debugger::read_memory`, preserving selected process/TID ownership instead of introducing a second ptrace path;
- the CLI exposes `print <name>` and renders the resolved signed or unsigned integer according to the recovered base-type width;
- direct API and real `mdbg` subprocess integration observe the expected runtime value, reject a missing local explicitly, then continue to clean exit in PIE and non-PIE under both GCC and Clang-large.

The supported form/opcode set remains intentionally bounded to what those real compiler fixtures emitted; Priority 0 does not claim a general DIE, expression, location-list, aggregate, or optimized-variable engine.

## Priority 1: register-resident formal parameters — complete

The second source-value slice extends the same evaluator to a real optimized formal parameter without introducing a parallel value engine:

- a dedicated `-O1 -g -gdwarf-4` fixture derives a `uint64_t parameter` from a volatile runtime seed and keeps it live in the System V x86-64 first-argument register at the exported `formal_parameter_probe`;
- test-first PIE/non-PIE integration proved the Priority 0 evaluator rejected that value because it only searched direct-child `DW_TAG_variable` DIEs;
- name resolution also accepts an unambiguous direct-child `DW_TAG_formal_parameter`, while unsupported types and missing values continue to fail explicitly;
- the only new register-location expression accepted is the compiler-proven `DW_OP_reg5 (rdi)`, read from the existing selected-TID register snapshot and truncated through the same 1–8 byte integer type metadata used by Priority 0;
- real optimized GCC 13.3 and Clang 18.1 output places the formal parameter's `DW_AT_location` in a DWARF4 `.debug_loc` section offset; the bounded selector honors the compilation unit's `DW_AT_low_pc`, explicit base-address-selection entries, and the range containing the current virtual PC;
- `DW_OP_GNU_entry_value`, `DW_OP_stack_value`, other register numbers, aggregate values, and broader expression evaluation remain outside scope;
- direct API and real `mdbg` subprocess integration observe the expected parameter value, reject a missing parameter explicitly, and continue to clean exit in PIE and non-PIE under both GCC and Clang-large.

## Priority 2: optimized local location-list ownership — complete

The third source-value slice extends that same current-PC ownership model to one real optimized ordinary local:

- the existing `-O1 -g -gdwarf-4` fixture derives a non-constant `uint64_t optimized_local` from the volatile runtime seed, constrains the same local through RDI and then through RAX, and stops at the exported `optimized_local_probe` while the value is live;
- test-first GCC 13.3 and Clang 18.1 evidence was identical: both PIE and non-PIE builds reached the earlier optimized-local location-list rejection while every other integration test remained green, proving the ordinary `DW_TAG_variable` really owns a `DW_FORM_sec_offset` `.debug_loc` list rather than an inline fixed location;
- ordinary locals reuse the Priority 1 DWARF4 current-PC selector directly, including compilation-unit initial-base semantics, explicit base-address-selection entries, range validation, and deterministic no-location/out-of-range failures; no second location-list parser was introduced;
- the only new expression opcode is the compiler-proven `DW_OP_reg0 (rax)` used by the active probe range; it is read from the selected TID's existing register snapshot and truncated through the same bounded integer type metadata as the earlier `DW_OP_reg5` path;
- `DW_OP_reg5` and `DW_OP_fbreg` retain their existing behavior, while other register numbers, compound stack-machine expressions, entry-value/stack-value operations, pointer/aggregate values, and DWARF5 loclists remain rejected;
- direct API and real `mdbg` subprocess integration observe the expected optimized local, reject a missing optimized local explicitly, preserve module ownership, and continue to clean exit in PIE and non-PIE under both permanent compiler lanes.

Priority 2 therefore closes the first bounded optimized-local ownership gap without turning the source-value evaluator into a speculative general DWARF VM.

## Priority 3: lexical-scope local ownership — complete

The fourth source-value slice fixes name ownership for real nested lexical scopes without broadening the location VM:

- the existing O0 DWARF4 fixture now gives `inspect_local_value` an outer `uint64_t local_value` and an inner lexical block with a shadowing `uint64_t local_value`; both are runtime-derived and deliberately hold different values;
- the first fixture-only run was test-first evidence: GCC 13.3 and Clang 18.1 both configured and built successfully, 40/42 tests remained green, and only `dwarf_line_integration_{pie,nopie}` failed because the evaluator returned the outer value while stopped at the inner `local_value_probe`;
- the DIE parser already preserved parent indexes, so production only adds the compiler-proven `DW_TAG_lexical_block` ownership rule rather than introducing a second DIE model;
- lexical blocks are bounded to the concrete `DW_AT_low_pc` / `DW_AT_high_pc` contiguous range shape emitted by the fixture; a nested value is active only when every lexical ancestor between it and the owning subprogram contains the current virtual PC;
- matching locals are ranked by active lexical depth, so the innermost active scope shadows outer names; same-depth matches remain an explicit ambiguity, unsupported non-lexical nesting is not treated as a lexical scope, and an inactive nested DIE does not hide an outer local;
- value type and location evaluation remain delegated to the already proven integer/type, `DW_OP_fbreg`, register, frame-base, and `.debug_loc` machinery; Priority 3 adds no new expression opcode or location-list format;
- the final fixture exports outer-before, inner, and outer-after probes. Direct API and real `mdbg` subprocess integration stop at all three in one tracee and observe outer → inner → outer values in order, proving both shadowing and ownership restoration after leaving the nested block;
- PIE and non-PIE pass under both GCC and Clang-large, module identity and missing-name failures remain intact, and the tracee exits cleanly after the three managed breakpoint stops.

Priority 3 therefore closes the bounded compiler-produced lexical-shadowing gap. Sibling-block variants, deeper nesting, inlined-subroutine ownership, range lists, and additional expression opcodes are not follow-up micro-PR targets without independent executable evidence.

## Priority 4: bounded pointer scalar inspection — complete

The fifth source-value slice extends the existing typed-value path across one concrete compiler-produced pointer type without broadening the location VM:

- a dedicated `-O0 -g -gdwarf-4` fixture keeps a stack-resident `uint64_t *local_pointer` live at the exported `local_pointer_probe`, pointing at the real runtime object `pointer_target`;
- test-first CI was exact: GCC 13.3 and Clang 18.1 both configured and built successfully, 42/44 existing tests stayed green, and only `pointer_value_integration_{pie,nopie}` failed with `local variable type is not a supported typedef/base-type chain`, proving the breakpoint, module routing, lexical ownership, and existing location evaluator had already succeeded and the rejected boundary was `DW_TAG_pointer_type`;
- the value result grows an explicit `LocalValueKind` and a generic `inspect_local_value` entry point, while `inspect_local_integer` remains as an integer-only compatibility wrapper rather than silently accepting pointer values as integers;
- pointer type resolution follows the same bounded typedef chain, accepts only the compiler-proven `DW_TAG_pointer_type`, requires the x86-64 eight-byte pointer width, and requires a `DW_FORM_ref4` pointee that itself resolves through the already supported integer typedef/base-type chain; malformed links, other widths, aggregates, references, arrays, pointer-to-pointer expansion, and unsupported pointees still fail explicitly;
- no new location expression or location-list rule is added: the pointer uses the existing selected-process/TID `DW_OP_fbreg` path and final memory read, so source-value ownership remains shared with the integer implementation;
- direct API integration compares the recovered raw pointer value with the independently resolved runtime ELF address of `pointer_target`, preserves owning-module identity and pointer width/kind metadata, and continues to clean exit;
- real `mdbg` subprocess integration renders `local_pointer = 0x...` deterministically in hexadecimal; pointer dereference is deliberately not bundled into this scalar milestone;
- PIE and non-PIE pass under both permanent GCC and Clang-large gates while all earlier integer, formal-parameter, optimized-location, and lexical-shadowing coverage remains green.

Priority 4 therefore closes the bounded scalar type-model gap without turning `print` into an aggregate renderer or a general DWARF expression engine.

## Priority 5: bounded struct aggregate presentation — complete

The sixth source-value slice closes the first compiler-produced aggregate shaping gap while preserving the existing ownership model:

- the existing `-O0 -g -gdwarf-4` pointer fixture now also contains a real stack-resident `struct LocalPair local_struct` with an unsigned 32-bit `count` member and a signed 64-bit `delta` member, both independently verifiable and live at `local_struct_probe`;
- test-first CI #609 was exact under both permanent compiler lanes: Configure and Build succeeded, 42/44 tests stayed green, and only `pointer_value_integration_{pie,nopie}` failed with `local variable type is not a supported typedef/base-type/pointer chain`, proving module routing, lexical ownership, location evaluation, breakpoint handling, and the prior scalar paths had already succeeded and the rejected boundary was `DW_TAG_structure_type`;
- the existing typed result grows `LocalValueKind::Structure` plus bounded named member metadata; integer and pointer results keep their prior scalar fields and behavior, and `inspect_local_integer` continues to reject non-integer values rather than silently accepting aggregates;
- type resolution accepts only the concrete compiler-produced `DW_TAG_structure_type` with direct-child `DW_TAG_member` DIEs, a structure byte size bounded to 256 bytes, at most 32 named direct members, `DW_FORM_ref4` member types that resolve through the existing signed/unsigned integer chain, and constant `DW_AT_data_member_location` forms already understood by the parser;
- every member is bounds-checked against the owned aggregate storage; anonymous members, non-member direct children, non-integer member types, expression-based member locations, nested aggregates, unions, arrays, bitfields, inheritance, and value pieces remain explicit failures rather than implied support;
- the aggregate itself must use the already-proven `DW_OP_fbreg` memory-backed location path. Production performs one bounded `Debugger::read_memory` for the complete structure and decodes direct members from that owned byte range, so there is no per-field raw-ptrace path or duplicate location engine;
- direct API integration verifies structure kind, 16-byte aggregate size, module identity, exactly two members, `count = 0x11223344`, signed `delta = -123456789`, integer-only API rejection, and clean exit; the real CLI renders `local_struct = { count = 287454020, delta = -123456789 }` deterministically;
- exact implementation head `a4c8bb4e...` passes all 44 tests under both GCC 13.3 and Clang 18.1 large-model lanes, while all earlier integer, pointer, optimized-location, lexical, process-domain, breakpoint, and mutation coverage remains green.

Priority 5 therefore completes the bounded aggregate milestone without using struct support as a pretext for speculative aggregate recursion or a general DWARF VM.

## Priority 6: DWARF5 optimized source-value location ownership — complete

The seventh source-value slice crosses the existing optimized-value ownership model into real DWARF5 compiler output without introducing a parallel evaluator:

- a separate `-O1 -g -gdwarf-5` build of the already-proven `formal_parameter_fixture.c` supplies independent PIE and non-PIE fixtures, while all earlier DWARF4 artifacts remain unchanged;
- test-first CI #625 was exact: GCC and Clang both configured and built successfully, 44/46 tests stayed green, and only the new `dwarf5_local_value_integration_{pie,nopie}` failed with `only DWARF4 local-value units are supported`, proving the rejection was the format-generation boundary rather than breakpoint, module, type, or location-expression ownership;
- the shared parser now accepts bounded DWARF5 compile-unit headers with 8-byte addresses while preserving the DWARF4 header path and DWARF32-only contract;
- GCC 13.3 evidence requires `DW_FORM_line_strp`, `DW_FORM_implicit_const`, a direct `DW_FORM_sec_offset` into `.debug_loclists`, `DW_LLE_base_address`, and `DW_LLE_offset_pair`; at the exported optimized-local probe the selected location expression remains the already-supported `DW_OP_reg0 (rax)`;
- Clang 18.1 large-model evidence additionally requires `DW_FORM_strx1` with `DW_AT_str_offsets_base` / `.debug_str_offsets`, `DW_FORM_addrx` with `DW_AT_addr_base` / `.debug_addr`, and `DW_FORM_loclistx` with `DW_AT_loclists_base` plus the contribution offset table; the active probe range likewise resolves to the existing `DW_OP_reg0` expression;
- every indexed contribution validates version, address/segment width where applicable, offset-table bounds, resolved section bounds, and overflow before any address, string, or location is consumed;
- the DWARF5 location selector intentionally accepts only the compiler-proven `DW_LLE_base_address` and `DW_LLE_offset_pair` entries. Unknown LLE kinds still fail explicitly, and no new expression opcode is added;
- Clang emits `DW_OP_entry_value` in a different, non-selected formal-parameter range in the same real fixture. Priority 6 deliberately does not interpret it: only the location description selected by the current PC is evaluated, preserving fail-closed expression semantics rather than claiming a general DWARF VM;
- direct API and real `mdbg print optimized_local` subprocess integration recover `0x1e3c1e781e3c1ef0`, preserve module/type ownership, reject a missing optimized local explicitly, and continue to clean exit in PIE and non-PIE under both permanent compiler lanes;
- exact production head `afbf5cee...` passes all 46 tests under both GCC and Clang-large after the test-first failure and one construction-only unused-helper fix.

Priority 6 therefore closes the bounded DWARF4→DWARF5 source-value ownership gap. It does not imply support for arbitrary indexed forms, arbitrary `.debug_loclists` entries, `DW_OP_entry_value`, `DW_OP_stack_value`, inlined-scope ownership, or a general expression machine.

## Phase 4 closure

Phase 4 is sealed at the bounded source-value ownership layer:

- current source-scope name ownership, typed scalar/pointer/struct shaping, selected process/TID state, module/load-bias routing, DWARF4 location lists, and the compiler-proven DWARF5 indexed/location-list metadata now compose through one evaluator;
- GCC and Clang-large both prove the same optimized value through direct API and real CLI under PIE and non-PIE;
- unsupported metadata and expression semantics remain explicit failures instead of being silently skipped;
- further opcode-by-opcode work is not a Phase 4 strategy. A richer expression capability must start from a new exact compiler-produced failure whose current PC selects that expression.

The next architectural frontier is Phase 5: bounded DWARF expression/value semantics. The first candidate is the compiler-produced `DW_OP_entry_value` already observed in Clang DWARF5 output, but it is not implementation-ready until a dedicated probe makes that range active and demonstrates the exact current failure.

## Selection rule

Phase 4 is complete. Do not add more source-value forms, type variants, or location-list opcodes here without a new compiler-produced acceptance scenario. Continue in `docs/phase-5-roadmap.md`, beginning with an exact active-range failure before adding any expression semantics.
