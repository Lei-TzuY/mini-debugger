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


def verify_member(records, member, expected_name, expected_encoding):
    name = clean_name(member["attrs"].get("name", ""))
    if name != expected_name:
        raise RuntimeError(
            f"caller_union member mismatch: expected {expected_name}, got {name!r}"
        )
    location = member["attrs"].get("data_member_location")
    if location is not None and scalar_number(
        location, f"caller_union {expected_name} offset"
    ) != 0:
        raise RuntimeError(
            f"caller_union member {expected_name} does not overlap at offset zero"
        )

    type_attr = member["attrs"].get("type")
    if not type_attr:
        raise RuntimeError(f"caller_union member {expected_name} has no DW_AT_type")
    base = unwrap_type(
        records,
        type_reference(type_attr, f"caller_union {expected_name}"),
        f"caller_union {expected_name}",
    )
    if base["tag"] != "DW_TAG_base_type":
        raise RuntimeError(
            f"caller_union member {expected_name} is not a compiler base type"
        )
    size = base["attrs"].get("byte_size")
    encoding = base["attrs"].get("encoding")
    if not size or scalar_number(
        size, f"caller_union {expected_name} byte size"
    ) != 4:
        raise RuntimeError(
            f"caller_union member {expected_name} is not exactly four bytes"
        )
    if encoding is None or scalar_number(
        encoding, f"caller_union {expected_name} encoding"
    ) != expected_encoding:
        raise RuntimeError(
            f"caller_union member {expected_name} has the wrong integer encoding"
        )


def verify_layout(records):
    variable = find_variable(records, "caller_with_stack_local", "caller_union")
    type_attr = variable["attrs"].get("type")
    if not type_attr:
        raise RuntimeError("caller_union has no DW_AT_type")
    union = unwrap_type(
        records,
        type_reference(type_attr, "caller_union"),
        "caller_union",
    )
    if union["tag"] != "DW_TAG_union_type":
        raise RuntimeError("caller_union is not a compiler union type")
    size = union["attrs"].get("byte_size")
    if not size or scalar_number(size, "caller_union byte size") != 4:
        raise RuntimeError("caller_union is not exactly four bytes")

    members = [
        child for child in direct_children(records, union)
        if child["tag"] == "DW_TAG_member"
    ]
    if len(members) != 2:
        raise RuntimeError(
            f"caller_union requires exactly two direct members, found {len(members)}"
        )
    verify_member(records, members[0], "signed_value", 5)
    verify_member(records, members[1], "unsigned_value", 7)


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_physical_union_dwarf_oracle.py <fixture>")
    path = sys.argv[1]
    info = run("readelf", "--debug-dump=info", path)
    loc = run("readelf", "--debug-dump=loc", path)
    records = parse_records(info)
    basic_records = parse_basic_dies(info)
    symbols = symbol_addresses(path)

    verify_stack_local(
        "caller_with_stack_local",
        "caller_union",
        "snapshot_caller_resume_probe",
        loc,
        basic_records,
        symbols,
        probe_adjust=-1,
    )
    verify_layout(records)
    print("physical union DWARF oracle passed")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"physical union DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
