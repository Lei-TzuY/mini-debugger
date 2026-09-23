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


def require_signed_i32(records, type_attr, context):
    base = unwrap_type(
        records,
        type_reference(type_attr, context),
        context,
    )
    if base["tag"] != "DW_TAG_base_type":
        raise RuntimeError(f"{context}: not a compiler base type")
    size = base["attrs"].get("byte_size")
    encoding = base["attrs"].get("encoding")
    if not size or scalar_number(size, f"{context} byte size") != 4:
        raise RuntimeError(f"{context}: not exactly four bytes")
    if encoding is None or scalar_number(encoding, f"{context} encoding") != 5:
        raise RuntimeError(f"{context}: not compiler-described signed int32")


def verify_layout(records):
    variable = find_variable(
        records, "inspect_live_pointer_aggregate", "live_pointer_aggregate"
    )
    type_attr = variable["attrs"].get("type")
    if not type_attr:
        raise RuntimeError("live_pointer_aggregate has no DW_AT_type")
    structure = unwrap_type(
        records,
        type_reference(type_attr, "live_pointer_aggregate"),
        "live_pointer_aggregate",
    )
    if structure["tag"] != "DW_TAG_structure_type":
        raise RuntimeError("live_pointer_aggregate is not a compiler structure type")
    size = structure["attrs"].get("byte_size")
    if not size or scalar_number(size, "live_pointer_aggregate size") != 16:
        raise RuntimeError("live_pointer_aggregate is not exactly sixteen bytes")

    members = [
        child for child in direct_children(records, structure)
        if child["tag"] == "DW_TAG_member"
    ]
    if len(members) != 2:
        raise RuntimeError(
            f"live_pointer_aggregate requires exactly two members, found {len(members)}"
        )

    expected_offsets = {"direct": 0, "linked": 8}
    actual_offsets = {}
    for member in members:
        name = clean_name(member["attrs"].get("name", ""))
        if name not in expected_offsets:
            raise RuntimeError(f"unexpected live pointer member: {name}")
        location = member["attrs"].get("data_member_location")
        if location is None:
            raise RuntimeError(f"{name}: missing DW_AT_data_member_location")
        actual_offsets[name] = scalar_number(location, f"{name} offset")
        member_type = member["attrs"].get("type")
        if not member_type:
            raise RuntimeError(f"{name}: missing DW_AT_type")

        if name == "direct":
            require_signed_i32(
                records, member_type, "live_pointer_aggregate.direct type"
            )
            continue

        pointer = unwrap_type(
            records,
            type_reference(member_type, "live_pointer_aggregate.linked type"),
            "live_pointer_aggregate.linked type",
        )
        if pointer["tag"] != "DW_TAG_pointer_type":
            raise RuntimeError("live_pointer_aggregate.linked is not a pointer")
        pointer_size = pointer["attrs"].get("byte_size")
        if pointer_size and scalar_number(pointer_size, "linked pointer size") != 8:
            raise RuntimeError("live pointer member width is not eight bytes")
        pointee = pointer["attrs"].get("type")
        if not pointee:
            raise RuntimeError("live pointer member has no pointee type")
        require_signed_i32(
            records, pointee, "live_pointer_aggregate.linked pointee type"
        )

    if actual_offsets != expected_offsets:
        raise RuntimeError(
            f"live_pointer_aggregate member offsets changed: {actual_offsets}"
        )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: live_pointer_aggregate_dwarf_oracle.py <fixture>")
    path = sys.argv[1]
    info = run("readelf", "--debug-dump=info", path)
    loc = run("readelf", "--debug-dump=loc", path)
    records = parse_records(info)
    basic_records = parse_basic_dies(info)
    symbols = symbol_addresses(path)

    verify_stack_local(
        "inspect_live_pointer_aggregate",
        "live_pointer_aggregate",
        "live_pointer_aggregate_probe",
        loc,
        basic_records,
        symbols,
    )
    verify_layout(records)
    print(
        "live pointer aggregate DWARF oracle passed: "
        "size=16 direct@0 linked@8 pointer=8 pointee=signed-int32 "
        "location=DW_OP_fbreg"
    )


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"live pointer aggregate DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
