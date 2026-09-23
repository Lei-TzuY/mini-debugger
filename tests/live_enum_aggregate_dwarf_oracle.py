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
from core_physical_enum_dwarf_oracle import integral_representation
from core_xmm_dwarf_oracle import parse_dies as parse_basic_dies, verify_stack_local

EXPECTED_ENUMERATORS = {"LiveIdle": 3, "LiveReady": 7, "LiveBusy": 42}


def require_signed_i32(records, member):
    type_attr = member["attrs"].get("type")
    if not type_attr:
        raise RuntimeError("live_enum_aggregate.direct has no DW_AT_type")
    base = unwrap_type(
        records,
        type_reference(type_attr, "live_enum_aggregate.direct type"),
        "live_enum_aggregate.direct type",
    )
    if base["tag"] != "DW_TAG_base_type":
        raise RuntimeError("live_enum_aggregate.direct is not a base type")
    size = base["attrs"].get("byte_size")
    encoding = base["attrs"].get("encoding")
    if not size or scalar_number(size, "direct byte size") != 4:
        raise RuntimeError("live_enum_aggregate.direct is not four bytes")
    if encoding is None or scalar_number(encoding, "direct encoding") != 5:
        raise RuntimeError("live_enum_aggregate.direct is not compiler-described signed")


def require_enum_member(records, member):
    type_attr = member["attrs"].get("type")
    if not type_attr:
        raise RuntimeError("live_enum_aggregate.mode has no DW_AT_type")
    enum_type = unwrap_type(
        records,
        type_reference(type_attr, "live_enum_aggregate.mode type"),
        "live_enum_aggregate.mode type",
    )
    if enum_type["tag"] != "DW_TAG_enumeration_type":
        raise RuntimeError("live_enum_aggregate.mode is not a compiler enum type")
    if clean_name(enum_type["attrs"].get("name", "")) != "LiveMode":
        raise RuntimeError("live enum aggregate member type name changed")
    size = enum_type["attrs"].get("byte_size")
    if not size or scalar_number(size, "enum member byte size") != 4:
        raise RuntimeError("live enum aggregate member is not four bytes")
    representation, is_signed = integral_representation(records, enum_type)
    if is_signed:
        raise RuntimeError("live enum aggregate representation is unexpectedly signed")

    actual = {}
    for child in direct_children(records, enum_type):
        if child["tag"] != "DW_TAG_enumerator":
            continue
        name = clean_name(child["attrs"].get("name", ""))
        value = child["attrs"].get("const_value")
        if not name or value is None:
            raise RuntimeError("live enum aggregate enumerator lost name/value")
        actual[name] = scalar_number(value, f"{name} const_value")
    if actual != EXPECTED_ENUMERATORS:
        raise RuntimeError(
            f"live enum aggregate enumerator table changed: {actual}"
        )
    return representation


def verify_layout(records):
    variable = find_variable(
        records, "inspect_live_enum_aggregate", "live_enum_aggregate"
    )
    type_attr = variable["attrs"].get("type")
    if not type_attr:
        raise RuntimeError("live_enum_aggregate has no DW_AT_type")
    structure = unwrap_type(
        records,
        type_reference(type_attr, "live_enum_aggregate"),
        "live_enum_aggregate",
    )
    if structure["tag"] != "DW_TAG_structure_type":
        raise RuntimeError("live_enum_aggregate is not a compiler structure type")
    size = structure["attrs"].get("byte_size")
    if not size or scalar_number(size, "live_enum_aggregate size") != 8:
        raise RuntimeError("live_enum_aggregate is not exactly eight bytes")

    members = [
        child for child in direct_children(records, structure)
        if child["tag"] == "DW_TAG_member"
    ]
    if len(members) != 2:
        raise RuntimeError(
            f"live_enum_aggregate requires exactly two members, found {len(members)}"
        )

    expected_offsets = {"direct": 0, "mode": 4}
    actual_offsets = {}
    representation = None
    for member in members:
        name = clean_name(member["attrs"].get("name", ""))
        if name not in expected_offsets:
            raise RuntimeError(f"unexpected live_enum_aggregate member: {name}")
        location = member["attrs"].get("data_member_location")
        if location is None:
            raise RuntimeError(f"{name}: missing DW_AT_data_member_location")
        actual_offsets[name] = scalar_number(location, f"{name} offset")
        if name == "direct":
            require_signed_i32(records, member)
        else:
            representation = require_enum_member(records, member)
    if actual_offsets != expected_offsets:
        raise RuntimeError(
            f"live_enum_aggregate member offsets changed: {actual_offsets}"
        )
    return representation


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: live_enum_aggregate_dwarf_oracle.py <fixture>")
    path = sys.argv[1]
    info = run("readelf", "--debug-dump=info", path)
    loc = run("readelf", "--debug-dump=loc", path)
    records = parse_records(info)
    basic_records = parse_basic_dies(info)
    symbols = symbol_addresses(path)

    verify_stack_local(
        "inspect_live_enum_aggregate",
        "live_enum_aggregate",
        "live_enum_aggregate_probe",
        loc,
        basic_records,
        symbols,
    )
    representation = verify_layout(records)
    print(
        "live enum aggregate DWARF oracle passed: "
        f"size=8 direct@0 mode@4 enum=LiveMode representation={representation} "
        "location=DW_OP_fbreg"
    )


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"live enum aggregate DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
