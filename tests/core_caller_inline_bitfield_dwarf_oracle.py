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


def selected_bitfield_bindings(records, by_offset):
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
            if resolved_name(child, by_offset) != "caller_bit_fields":
                continue
            location = child["attrs"].get("location")
            if not location:
                continue
            result.append(child)
    return result


def direct_members(records, structure):
    try:
        position = next(
            index
            for index, record in enumerate(records)
            if record["offset"] == structure["offset"]
        )
    except StopIteration as error:
        raise RuntimeError("caller_bit_fields type DIE disappeared from parsed DWARF") from error

    members = []
    for child in records[position + 1 :]:
        if child["depth"] <= structure["depth"]:
            break
        if child["depth"] == structure["depth"] + 1 and child["tag"] == "DW_TAG_member":
            members.append(child)
    return members


def validate_member(member, by_offset, expected_name, expected_encoding, expected_bits):
    if resolved_name(member, by_offset) != expected_name:
        raise RuntimeError(
            f"caller_bit_fields member order/name mismatch: expected {expected_name}"
        )

    value_type = referenced_type(
        member, by_offset, f"caller_bit_fields {expected_name} type"
    )
    if value_type["tag"] != "DW_TAG_base_type":
        raise RuntimeError(
            f"caller_bit_fields member {expected_name} is not a compiler base type"
        )
    size = value_type["attrs"].get("byte_size")
    encoding = value_type["attrs"].get("encoding")
    if not size or numeric_attr(size, f"{expected_name} byte size") != 4:
        raise RuntimeError(f"caller_bit_fields member {expected_name} is not four bytes")
    if not encoding or numeric_attr(encoding, f"{expected_name} encoding") != expected_encoding:
        raise RuntimeError(
            f"caller_bit_fields member {expected_name} has unexpected scalar encoding"
        )

    bit_size = member["attrs"].get("bit_size")
    if not bit_size or numeric_attr(bit_size, f"{expected_name} bit size") != expected_bits:
        raise RuntimeError(
            f"caller_bit_fields member {expected_name} has no exact {expected_bits}-bit compiler width"
        )

    locations = []
    for attribute in ("data_bit_offset", "bit_offset"):
        value = member["attrs"].get(attribute)
        if value:
            locations.append((attribute, numeric_attr(value, f"{expected_name} {attribute}")))
    if len(locations) != 1:
        raise RuntimeError(
            f"caller_bit_fields member {expected_name} requires exactly one explicit DWARF bit-location attribute"
        )

    member_location = member["attrs"].get("data_member_location")
    byte_offset = (
        numeric_attr(member_location, f"{expected_name} data member location")
        if member_location
        else None
    )
    return locations[0], byte_offset


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_caller_inline_bitfield_dwarf_oracle.py <fixture>")

    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    bindings = selected_bitfield_bindings(records, by_offset)
    if not bindings:
        raise RuntimeError(
            "caller_bit_fields has no compiler-produced concrete selected-inline DW_AT_location"
        )

    structures = []
    for binding in bindings:
        value_type = referenced_type(binding, by_offset, "caller_bit_fields type")
        if value_type["tag"] != "DW_TAG_structure_type":
            raise RuntimeError(
                "caller_bit_fields concrete binding does not resolve to DW_TAG_structure_type"
            )
        structures.append(value_type)

    first = structures[0]
    if any(candidate["offset"] != first["offset"] for candidate in structures):
        raise RuntimeError(
            "caller_bit_fields bindings disagree on compiler structure type ownership"
        )
    size = first["attrs"].get("byte_size")
    if not size or numeric_attr(size, "caller_bit_fields byte size") != 4:
        raise RuntimeError("caller_bit_fields compiler structure type is not exactly four bytes")

    members = direct_members(records, first)
    if len(members) != 2:
        raise RuntimeError(
            f"caller_bit_fields requires exactly two direct members, found {len(members)}"
        )

    signed_location, signed_byte = validate_member(
        members[0], by_offset, "signed_bits", 5, 5
    )
    unsigned_location, unsigned_byte = validate_member(
        members[1], by_offset, "unsigned_bits", 7, 6
    )

    print(
        "caller_bit_fields compiler evidence: "
        f"binding-count={len(bindings)} type=DW_TAG_structure_type byte-size=4 "
        f"signed_bits:int32/5 {signed_location[0]}={signed_location[1]} "
        f"data_member_location={signed_byte}; "
        f"unsigned_bits:uint32/6 {unsigned_location[0]}={unsigned_location[1]} "
        f"data_member_location={unsigned_byte}"
    )


if __name__ == "__main__":
    main()
