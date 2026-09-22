#!/usr/bin/env python3
import sys

from core_caller_inline_dwarf_oracle import (
    addr2line_contexts,
    numeric_attr,
    origin_name,
    parse_dies,
    referenced_type,
    resolved_name,
    run,
    symbol_address,
)


def selected_bindings(records, by_offset):
    result = []
    for pos, record in enumerate(records):
        if record["tag"] != "DW_TAG_inlined_subroutine":
            continue
        if origin_name(record, by_offset) != "caller_nested_inline_inner":
            continue
        for child in records[pos + 1 :]:
            if child["depth"] <= record["depth"]:
                break
            if child["tag"] not in {"DW_TAG_variable", "DW_TAG_formal_parameter"}:
                continue
            if resolved_name(child, by_offset) != "caller_nested_aggregate":
                continue
            if child["attrs"].get("location"):
                result.append(child)
    if not result:
        raise RuntimeError(
            "caller_nested_aggregate has no compiler-produced selected-inline DW_AT_location"
        )
    return result


def direct_members(records, by_offset, structure):
    try:
        position = next(
            index
            for index, record in enumerate(records)
            if record["offset"] == structure["offset"]
        )
    except StopIteration as error:
        raise RuntimeError("nested aggregate DIE disappeared from parsed DWARF") from error

    result = {}
    for child in records[position + 1 :]:
        if child["depth"] <= structure["depth"]:
            break
        if child["depth"] != structure["depth"] + 1 or child["tag"] != "DW_TAG_member":
            continue
        name = resolved_name(child, by_offset)
        if name:
            result[name] = child
    return result


def member_offset(member, context):
    location = member["attrs"].get("data_member_location")
    if location is None:
        raise RuntimeError(f"{context} has no DW_AT_data_member_location")
    return numeric_attr(location, f"{context} offset")


def validate_signed_int32(member, by_offset, context):
    value_type = referenced_type(member, by_offset, f"{context} type")
    if value_type["tag"] != "DW_TAG_base_type":
        raise RuntimeError(f"{context} is not a compiler base type")
    size = value_type["attrs"].get("byte_size")
    encoding = value_type["attrs"].get("encoding")
    if not size or numeric_attr(size, f"{context} byte size") != 4:
        raise RuntimeError(f"{context} is not exactly four bytes")
    if not encoding or numeric_attr(encoding, f"{context} encoding") != 5:
        raise RuntimeError(f"{context} is not a signed integer")


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_caller_inline_nested_aggregate_dwarf_oracle.py <fixture>")

    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}

    resume = symbol_address(path, "snapshot_caller_nested_inline_resume_probe")
    contexts = addr2line_contexts(path, resume)
    chain = [name for name, _ in contexts]
    wanted = [
        "caller_nested_inline_inner",
        "caller_nested_inline_outer",
        "caller_nested_physical_frame",
    ]
    if chain[: len(wanted)] != wanted:
        raise RuntimeError(f"unexpected nested-aggregate addr2line chain: {chain}")

    bindings = selected_bindings(records, by_offset)
    outer_structures = []
    for binding in bindings:
        location = binding["attrs"].get("location", "")
        if "DW_OP_fbreg" not in location:
            raise RuntimeError(
                "caller_nested_aggregate does not use the supported caller-frame DW_OP_fbreg location: "
                + location
            )
        value_type = referenced_type(binding, by_offset, "caller_nested_aggregate type")
        if value_type["tag"] != "DW_TAG_structure_type":
            raise RuntimeError("caller_nested_aggregate binding is not a structure type")
        outer_structures.append(value_type)

    outer = outer_structures[0]
    if any(candidate["offset"] != outer["offset"] for candidate in outer_structures):
        raise RuntimeError("caller_nested_aggregate bindings disagree on outer structure ownership")
    if resolved_name(outer, by_offset) != "CallerInlineNestedOuter":
        raise RuntimeError("outer nested aggregate type name is not compiler-preserved")
    outer_size = outer["attrs"].get("byte_size")
    if not outer_size or numeric_attr(outer_size, "nested outer byte size") != 8:
        raise RuntimeError("CallerInlineNestedOuter compiler layout is not exactly eight bytes")

    outer_members = direct_members(records, by_offset, outer)
    if set(outer_members) != {"prefix", "inner"}:
        raise RuntimeError(f"nested outer direct members are incomplete: {sorted(outer_members)}")
    offsets = {name: member_offset(member, f"nested outer {name}")
               for name, member in outer_members.items()}
    if offsets != {"prefix": 0, "inner": 4}:
        raise RuntimeError(f"nested outer compiler member offsets changed: {offsets}")
    validate_signed_int32(outer_members["prefix"], by_offset, "nested outer prefix")

    inner = referenced_type(outer_members["inner"], by_offset, "nested inner structure type")
    if inner["tag"] != "DW_TAG_structure_type":
        raise RuntimeError("nested outer inner member is not a compiler structure type")
    if resolved_name(inner, by_offset) != "CallerInlineNestedInner":
        raise RuntimeError("inner nested aggregate type name is not compiler-preserved")
    inner_size = inner["attrs"].get("byte_size")
    if not inner_size or numeric_attr(inner_size, "nested inner byte size") != 4:
        raise RuntimeError("CallerInlineNestedInner compiler layout is not exactly four bytes")

    inner_members = direct_members(records, by_offset, inner)
    if set(inner_members) != {"terminal"}:
        raise RuntimeError(f"nested inner direct members are incomplete: {sorted(inner_members)}")
    if member_offset(inner_members["terminal"], "nested inner terminal") != 0:
        raise RuntimeError("nested inner terminal member is not at offset zero")
    validate_signed_int32(inner_members["terminal"], by_offset, "nested inner terminal")

    print(f"caller-nested-inline resume probe: 0x{resume:x}")
    print(
        "caller-nested-inline addr2line chain: "
        + " -> ".join(
            f"{name}@{location}" for name, location in contexts[: len(wanted)]
        )
    )
    print(
        "caller_nested_aggregate compiler evidence: "
        f"binding-count={len(bindings)} location=DW_OP_fbreg "
        "outer=CallerInlineNestedOuter:8 prefix@0:signed-int32 "
        "inner@4=CallerInlineNestedInner:4 terminal@0:signed-int32"
    )


if __name__ == "__main__":
    main()
