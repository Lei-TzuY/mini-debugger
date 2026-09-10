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


def selected_union_bindings(records, by_offset):
    result = []
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
            if resolved_name(child, by_offset) != "caller_union":
                continue
            location = child["attrs"].get("location")
            if not location:
                continue
            result.append(child)
    return result


def direct_members(records, union):
    try:
        position = next(
            index for index, record in enumerate(records)
            if record["offset"] == union["offset"]
        )
    except StopIteration as error:
        raise RuntimeError("caller_union type DIE disappeared from parsed DWARF") from error

    members = []
    for child in records[position + 1 :]:
        if child["depth"] <= union["depth"]:
            break
        if child["depth"] == union["depth"] + 1 and child["tag"] == "DW_TAG_member":
            members.append(child)
    return members


def validate_member(member, by_offset, expected_name, expected_encoding):
    if resolved_name(member, by_offset) != expected_name:
        raise RuntimeError(
            f"caller_union member order/name mismatch: expected {expected_name}"
        )
    location = member["attrs"].get("data_member_location")
    if location and numeric_attr(location, f"caller_union {expected_name} offset") != 0:
        raise RuntimeError(
            f"caller_union member {expected_name} does not overlap storage at offset zero"
        )

    value_type = referenced_type(member, by_offset, f"caller_union {expected_name} type")
    if value_type["tag"] != "DW_TAG_base_type":
        raise RuntimeError(
            f"caller_union member {expected_name} is not a compiler base type"
        )
    size = value_type["attrs"].get("byte_size")
    encoding = value_type["attrs"].get("encoding")
    if not size or numeric_attr(size, f"caller_union {expected_name} byte size") != 4:
        raise RuntimeError(
            f"caller_union member {expected_name} is not four bytes"
        )
    if not encoding or numeric_attr(
        encoding, f"caller_union {expected_name} encoding"
    ) != expected_encoding:
        raise RuntimeError(
            f"caller_union member {expected_name} has unexpected scalar encoding"
        )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_caller_inline_union_dwarf_oracle.py <fixture>")

    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    bindings = selected_union_bindings(records, by_offset)
    if not bindings:
        raise RuntimeError(
            "caller_union has no compiler-produced concrete selected-inline DW_AT_location"
        )

    union_types = []
    for binding in bindings:
        value_type = referenced_type(binding, by_offset, "caller_union type")
        if value_type["tag"] != "DW_TAG_union_type":
            raise RuntimeError(
                "caller_union concrete binding does not resolve to DW_TAG_union_type"
            )
        union_types.append(value_type)

    first = union_types[0]
    if any(candidate["offset"] != first["offset"] for candidate in union_types):
        raise RuntimeError("caller_union bindings disagree on compiler union type ownership")
    size = first["attrs"].get("byte_size")
    if not size or numeric_attr(size, "caller_union byte size") != 4:
        raise RuntimeError("caller_union compiler union type is not exactly four bytes")

    members = direct_members(records, first)
    if len(members) != 2:
        raise RuntimeError(
            f"caller_union requires exactly two direct members, found {len(members)}"
        )
    validate_member(members[0], by_offset, "signed_value", 5)
    validate_member(members[1], by_offset, "unsigned_value", 7)

    print(
        "caller_union compiler evidence: "
        f"binding-count={len(bindings)} type=DW_TAG_union_type byte-size=4 "
        "members=signed_value:int32,unsigned_value:uint32 shared-offset=0"
    )


if __name__ == "__main__":
    main()
