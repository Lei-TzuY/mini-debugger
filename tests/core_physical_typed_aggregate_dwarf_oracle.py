#!/usr/bin/env python3
import subprocess
import sys

from core_physical_aggregate_dwarf_oracle import (
    clean_name,
    direct_children,
    find_variable,
    parse_records,
    scalar_number,
    type_reference,
    unwrap_type,
)
from core_xmm_dwarf_oracle import (
    parse_dies as parse_basic_dies,
    run,
    symbol_addresses,
    verify_stack_local,
)


def require_unsigned_u64_type(records, offset, context):
    base = unwrap_type(records, offset, context)
    if base["tag"] != "DW_TAG_base_type":
        raise RuntimeError(f"{context}: pointee is not a base type")
    size_attr = base["attrs"].get("byte_size")
    encoding = base["attrs"].get("encoding", "")
    if not size_attr or scalar_number(size_attr, context) != 8:
        raise RuntimeError(f"{context}: pointee is not exactly eight bytes")
    if "unsigned" not in encoding.lower():
        raise RuntimeError(f"{context}: pointee is not compiler-described unsigned")


def require_pointer_to_unsigned_u64(records, member, context):
    type_attr = member["attrs"].get("type")
    if not type_attr:
        raise RuntimeError(f"{context}: member has no DW_AT_type")
    pointer = unwrap_type(records, type_reference(type_attr, context), context)
    if pointer["tag"] != "DW_TAG_pointer_type":
        raise RuntimeError(f"{context}: member is not a pointer type")
    size_attr = pointer["attrs"].get("byte_size")
    if size_attr and scalar_number(size_attr, context) != 8:
        raise RuntimeError(f"{context}: pointer is not x86-64 width")
    pointee_attr = pointer["attrs"].get("type")
    if not pointee_attr:
        raise RuntimeError(f"{context}: pointer has no DW_AT_type pointee")
    require_unsigned_u64_type(
        records, type_reference(pointee_attr, context), context + " pointee"
    )


def require_unsigned_u64_member(records, member, context):
    type_attr = member["attrs"].get("type")
    if not type_attr:
        raise RuntimeError(f"{context}: member has no DW_AT_type")
    require_unsigned_u64_type(
        records, type_reference(type_attr, context), context
    )


def verify_typed_layout(records):
    variable = find_variable(
        records, "caller_with_stack_local", "caller_typed_aggregate"
    )
    type_attr = variable["attrs"].get("type")
    if not type_attr:
        raise RuntimeError("caller_typed_aggregate has no DW_AT_type")
    structure = unwrap_type(
        records,
        type_reference(type_attr, "caller_typed_aggregate"),
        "caller_typed_aggregate",
    )
    if structure["tag"] != "DW_TAG_structure_type":
        raise RuntimeError("caller_typed_aggregate is not a compiler structure type")
    size_attr = structure["attrs"].get("byte_size")
    if not size_attr or scalar_number(size_attr, "caller_typed_aggregate size") != 16:
        raise RuntimeError("caller_typed_aggregate is not exactly sixteen bytes")

    members = [
        child
        for child in direct_children(records, structure)
        if child["tag"] == "DW_TAG_member"
    ]
    if len(members) != 2:
        raise RuntimeError(
            "caller_typed_aggregate does not have exactly two direct members"
        )

    expected = {"payload": 0, "marker": 8}
    actual = {}
    for member in members:
        name = clean_name(member["attrs"].get("name", ""))
        if name not in expected:
            raise RuntimeError(f"unexpected caller typed member: {name}")
        location = member["attrs"].get("data_member_location")
        if location is None:
            raise RuntimeError(f"{name}: missing DW_AT_data_member_location")
        offset = scalar_number(location, f"{name} offset")
        if name == "payload":
            require_pointer_to_unsigned_u64(records, member, name)
        else:
            require_unsigned_u64_member(records, member, name)
        actual[name] = offset

    if actual != expected:
        raise RuntimeError(f"caller typed aggregate layout mismatch: {actual}")


def main():
    if len(sys.argv) != 2:
        raise SystemExit(
            "usage: core_physical_typed_aggregate_dwarf_oracle.py <fixture>"
        )
    path = sys.argv[1]
    info = run("readelf", "--debug-dump=info", path)
    loc = run("readelf", "--debug-dump=loc", path)
    records = parse_records(info)
    basic_records = parse_basic_dies(info)
    symbols = symbol_addresses(path)

    verify_stack_local(
        "caller_with_stack_local",
        "caller_typed_aggregate",
        "snapshot_caller_resume_probe",
        loc,
        basic_records,
        symbols,
        probe_adjust=-1,
    )
    verify_typed_layout(records)
    print("physical typed aggregate DWARF oracle passed")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"physical typed aggregate DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
