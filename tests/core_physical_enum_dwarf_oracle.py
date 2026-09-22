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


def integral_representation(records, enum_type):
    encoding = enum_type["attrs"].get("encoding")
    if encoding is not None:
        code = scalar_number(encoding, "caller_mode direct encoding")
        if code not in {5, 7}:
            raise RuntimeError(
                "caller_mode direct compiler encoding is not signed/unsigned integer"
            )
        return ("direct-encoding", code == 5)

    underlying_attr = enum_type["attrs"].get("type")
    if not underlying_attr:
        raise RuntimeError(
            "caller_mode has neither direct encoding nor compiler underlying type"
        )
    underlying = unwrap_type(
        records,
        type_reference(underlying_attr, "caller_mode underlying type"),
        "caller_mode underlying type",
    )
    if underlying["tag"] != "DW_TAG_base_type":
        raise RuntimeError("caller_mode underlying type is not a compiler base type")
    size = underlying["attrs"].get("byte_size")
    encoding = underlying["attrs"].get("encoding")
    if not size or scalar_number(size, "caller_mode underlying size") != 4:
        raise RuntimeError("caller_mode underlying type is not exactly four bytes")
    if encoding is None:
        raise RuntimeError("caller_mode underlying type has no integer encoding")
    code = scalar_number(encoding, "caller_mode underlying encoding")
    if code not in {5, 7}:
        raise RuntimeError("caller_mode underlying type is not signed/unsigned integer")
    return ("underlying-type", code == 5)


def verify_layout(records):
    variable = find_variable(records, "caller_with_stack_local", "caller_mode")
    type_attr = variable["attrs"].get("type")
    if not type_attr:
        raise RuntimeError("caller_mode has no DW_AT_type")
    enum_type = unwrap_type(
        records,
        type_reference(type_attr, "caller_mode"),
        "caller_mode",
    )
    if enum_type["tag"] != "DW_TAG_enumeration_type":
        raise RuntimeError("caller_mode is not a compiler enumeration type")

    name = clean_name(enum_type["attrs"].get("name", ""))
    if name != "CallerPhysicalMode":
        raise RuntimeError(f"caller_mode enum name changed: {name!r}")
    size = enum_type["attrs"].get("byte_size")
    if not size or scalar_number(size, "caller_mode byte size") != 4:
        raise RuntimeError("caller_mode compiler enum type is not exactly four bytes")
    representation, is_signed = integral_representation(records, enum_type)
    if is_signed:
        raise RuntimeError("caller_mode compiler enum representation is unexpectedly signed")

    enumerators = [
        child for child in direct_children(records, enum_type)
        if child["tag"] == "DW_TAG_enumerator"
    ]
    expected = {
        "CallerPhysicalIdle": 3,
        "CallerPhysicalReady": 7,
        "CallerPhysicalBusy": 42,
    }
    actual = {}
    for enumerator in enumerators:
        entry_name = clean_name(enumerator["attrs"].get("name", ""))
        value = enumerator["attrs"].get("const_value")
        if not entry_name or value is None:
            raise RuntimeError("caller_mode enumerator lost compiler name/value")
        actual[entry_name] = scalar_number(value, f"{entry_name} const_value")
    if actual != expected:
        raise RuntimeError(f"caller_mode compiler enumerator table mismatch: {actual}")
    return representation


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_physical_enum_dwarf_oracle.py <fixture>")
    path = sys.argv[1]
    info = run("readelf", "--debug-dump=info", path)
    loc = run("readelf", "--debug-dump=loc", path)
    records = parse_records(info)
    basic_records = parse_basic_dies(info)
    symbols = symbol_addresses(path)

    verify_stack_local(
        "caller_with_stack_local",
        "caller_mode",
        "snapshot_caller_resume_probe",
        loc,
        basic_records,
        symbols,
        probe_adjust=-1,
    )
    representation = verify_layout(records)
    print(
        "physical enum DWARF oracle passed: "
        f"type=CallerPhysicalMode byte-size=4 unsigned representation={representation} "
        "CallerPhysicalIdle=3 CallerPhysicalReady=7 CallerPhysicalBusy=42"
    )


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"physical enum DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
