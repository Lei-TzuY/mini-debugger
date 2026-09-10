#!/usr/bin/env python3
import sys

from core_caller_inline_dwarf_oracle import (
    numeric_attr,
    origin_name,
    parse_dies,
    referenced_type,
    resolved_name,
    run,
)
from core_caller_inline_enum_dwarf_oracle import (
    direct_enumerators,
    signed_numeric_attr,
    validate_integral_representation,
)


EXPECTED_ENUMERATORS = {
    "CallerInlineIdle": 3,
    "CallerInlineReady": 7,
    "CallerInlineBusy": 42,
}


def selected_binding(records, by_offset):
    bindings = []
    for pos, record in enumerate(records):
        if record["tag"] != "DW_TAG_inlined_subroutine":
            continue
        if origin_name(record, by_offset) != "caller_inline_inner":
            continue
        for child in records[pos + 1 :]:
            if child["depth"] <= record["depth"]:
                break
            if child["tag"] not in {"DW_TAG_variable", "DW_TAG_formal_parameter"}:
                continue
            if resolved_name(child, by_offset) != "caller_typed_aggregate":
                continue
            if child["attrs"].get("location"):
                bindings.append(child)
    if not bindings:
        raise RuntimeError(
            "caller_typed_aggregate has no compiler-produced concrete selected-inline DW_AT_location"
        )
    return bindings


def direct_members(records, structure):
    try:
        position = next(
            index for index, record in enumerate(records)
            if record["offset"] == structure["offset"]
        )
    except StopIteration as error:
        raise RuntimeError("typed aggregate DIE disappeared from parsed DWARF") from error

    result = {}
    for child in records[position + 1 :]:
        if child["depth"] <= structure["depth"]:
            break
        if child["depth"] != structure["depth"] + 1 or child["tag"] != "DW_TAG_member":
            continue
        name = resolved_name(child, by_offset_global)
        if name in {"direct", "mode"}:
            result[name] = child
    return result


def validate_base_integer(member, by_offset):
    value_type = referenced_type(member, by_offset, "typed aggregate direct member type")
    if value_type["tag"] != "DW_TAG_base_type":
        raise RuntimeError("typed aggregate direct member is not a compiler base type")
    size = value_type["attrs"].get("byte_size")
    encoding = value_type["attrs"].get("encoding")
    if not size or numeric_attr(size, "typed aggregate direct byte size") != 4:
        raise RuntimeError("typed aggregate direct member is not exactly four bytes")
    if not encoding or numeric_attr(encoding, "typed aggregate direct encoding") != 5:
        raise RuntimeError("typed aggregate direct member is not a signed integer")


def validate_enum(member, records, by_offset):
    enum_type = referenced_type(member, by_offset, "typed aggregate mode member type")
    if enum_type["tag"] != "DW_TAG_enumeration_type":
        raise RuntimeError("typed aggregate mode member is not a compiler enumeration type")
    if resolved_name(enum_type, by_offset) != "CallerInlineMode":
        raise RuntimeError("typed aggregate mode member lost CallerInlineMode type identity")
    size = enum_type["attrs"].get("byte_size")
    if not size or numeric_attr(size, "typed aggregate mode byte size") != 4:
        raise RuntimeError("typed aggregate mode enum is not exactly four bytes")
    representation, is_signed = validate_integral_representation(enum_type, by_offset)
    if is_signed:
        raise RuntimeError("typed aggregate mode enum is not compiler-described unsigned")

    actual = {}
    for enumerator in direct_enumerators(records, enum_type):
        name = resolved_name(enumerator, by_offset)
        value = enumerator["attrs"].get("const_value")
        if not name or value is None:
            raise RuntimeError("typed aggregate enum entry lacks compiler name/const_value")
        actual[name] = signed_numeric_attr(value, f"{name} const_value")
    if actual != EXPECTED_ENUMERATORS:
        raise RuntimeError(f"typed aggregate enum table mismatch: {actual}")
    return representation


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_caller_inline_typed_aggregate_dwarf_oracle.py <fixture>")

    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    global by_offset_global
    by_offset_global = by_offset

    bindings = selected_binding(records, by_offset)
    structures = []
    for binding in bindings:
        location = binding["attrs"].get("location", "")
        if "DW_OP_fbreg" not in location:
            raise RuntimeError(
                "caller_typed_aggregate does not use the supported caller-frame DW_OP_fbreg location: "
                + location
            )
        value_type = referenced_type(binding, by_offset, "caller_typed_aggregate type")
        if value_type["tag"] != "DW_TAG_structure_type":
            raise RuntimeError("caller_typed_aggregate binding is not a structure type")
        structures.append(value_type)

    structure = structures[0]
    if any(candidate["offset"] != structure["offset"] for candidate in structures):
        raise RuntimeError("caller_typed_aggregate bindings disagree on structure ownership")
    if resolved_name(structure, by_offset) != "CallerInlineTypedAggregate":
        raise RuntimeError("typed aggregate structure name is not compiler-preserved")
    size = structure["attrs"].get("byte_size")
    if not size or numeric_attr(size, "typed aggregate byte size") != 8:
        raise RuntimeError("CallerInlineTypedAggregate compiler layout is not exactly eight bytes")

    members = direct_members(records, structure)
    if set(members) != {"direct", "mode"}:
        raise RuntimeError(f"typed aggregate direct members are incomplete: {sorted(members)}")
    offsets = {}
    for name, member in members.items():
        location = member["attrs"].get("data_member_location")
        if location is None:
            raise RuntimeError(f"typed aggregate member {name} has no DW_AT_data_member_location")
        offsets[name] = numeric_attr(location, f"typed aggregate {name} offset")
    if offsets != {"direct": 0, "mode": 4}:
        raise RuntimeError(f"typed aggregate compiler member offsets changed: {offsets}")

    validate_base_integer(members["direct"], by_offset)
    representation = validate_enum(members["mode"], records, by_offset)

    print(
        "caller_typed_aggregate compiler evidence: "
        f"binding-count={len(bindings)} location=DW_OP_fbreg "
        "structure=CallerInlineTypedAggregate byte-size=8 direct@0:signed-int32 "
        f"mode@4:CallerInlineMode:uint32 representation={representation} "
        "CallerInlineIdle=3 CallerInlineReady=7 CallerInlineBusy=42"
    )


if __name__ == "__main__":
    main()
