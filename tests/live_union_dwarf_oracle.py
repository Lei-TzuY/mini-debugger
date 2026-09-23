#!/usr/bin/env python3
import subprocess
import sys

from core_physical_aggregate_dwarf_oracle import (
    clean_name,
    direct_children,
    find_variable,
    parse_records,
    run,
    scalar_number,
    symbol_addresses,
    type_reference,
    unwrap_type,
)
from core_xmm_dwarf_oracle import parse_dies as parse_basic_dies, verify_stack_local


def verify_integer_member(records, member, expected_name, expected_encoding):
    name = clean_name(member["attrs"].get("name", ""))
    if name != expected_name:
        raise RuntimeError(
            f"live_union member mismatch: expected {expected_name}, got {name!r}"
        )
    location = member["attrs"].get("data_member_location")
    if location is not None and scalar_number(
        location, f"live_union {expected_name} offset"
    ) != 0:
        raise RuntimeError(
            f"live_union member {expected_name} does not overlap at offset zero"
        )
    type_attr = member["attrs"].get("type")
    if not type_attr:
        raise RuntimeError(f"live_union member {expected_name} has no type")
    base = unwrap_type(
        records,
        type_reference(type_attr, f"live_union {expected_name}"),
        f"live_union {expected_name}",
    )
    if base["tag"] != "DW_TAG_base_type":
        raise RuntimeError(
            f"live_union member {expected_name} is not a compiler base type"
        )
    size = base["attrs"].get("byte_size")
    encoding = base["attrs"].get("encoding")
    if not size or scalar_number(
        size, f"live_union {expected_name} byte size"
    ) != 4:
        raise RuntimeError(
            f"live_union member {expected_name} is not exactly four bytes"
        )
    if encoding is None or scalar_number(
        encoding, f"live_union {expected_name} encoding"
    ) != expected_encoding:
        raise RuntimeError(
            f"live_union member {expected_name} has unexpected integer encoding"
        )


def verify_layout(records):
    variable = find_variable(records, "inspect_live_union", "live_union")
    type_attr = variable["attrs"].get("type")
    if not type_attr:
        raise RuntimeError("live_union has no DW_AT_type")
    union_type = unwrap_type(
        records, type_reference(type_attr, "live_union"), "live_union"
    )
    if union_type["tag"] != "DW_TAG_union_type":
        raise RuntimeError("live_union is not a compiler union type")
    size = union_type["attrs"].get("byte_size")
    if not size or scalar_number(size, "live_union byte size") != 4:
        raise RuntimeError("live_union is not exactly four bytes")
    members = [
        child for child in direct_children(records, union_type)
        if child["tag"] == "DW_TAG_member"
    ]
    if len(members) != 2:
        raise RuntimeError(
            f"live_union requires exactly two direct members, found {len(members)}"
        )
    verify_integer_member(records, members[0], "signed_value", 5)
    verify_integer_member(records, members[1], "unsigned_value", 7)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: live_union_dwarf_oracle.py <fixture>")
    path = sys.argv[1]
    info = run("readelf", "--debug-dump=info", path)
    loc = run("readelf", "--debug-dump=loc", path)
    records = parse_records(info)
    basic_records = parse_basic_dies(info)
    symbols = symbol_addresses(path)

    verify_stack_local(
        "inspect_live_union",
        "live_union",
        "live_union_probe",
        loc,
        basic_records,
        symbols,
    )
    verify_layout(records)
    print(
        "live union DWARF oracle passed: "
        "type=LiveUnion bytes=4 members=signed_value,unsigned_value "
        "location=DW_OP_fbreg"
    )


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"live union DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
