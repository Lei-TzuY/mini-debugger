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


def verify_member(records, member, expected_name, expected_encoding, expected_bits):
    name = clean_name(member["attrs"].get("name", ""))
    if name != expected_name:
        raise RuntimeError(
            f"caller_bit_fields member mismatch: expected {expected_name}, got {name!r}"
        )

    type_attr = member["attrs"].get("type")
    if not type_attr:
        raise RuntimeError(f"{expected_name}: missing DW_AT_type")
    base = unwrap_type(
        records,
        type_reference(type_attr, f"{expected_name} type"),
        f"{expected_name} type",
    )
    if base["tag"] != "DW_TAG_base_type":
        raise RuntimeError(f"{expected_name}: bit field is not a compiler base type")
    size = base["attrs"].get("byte_size")
    encoding = base["attrs"].get("encoding")
    if not size or scalar_number(size, f"{expected_name} byte size") != 4:
        raise RuntimeError(f"{expected_name}: base storage is not four bytes")
    if encoding is None or scalar_number(
        encoding, f"{expected_name} encoding"
    ) != expected_encoding:
        raise RuntimeError(f"{expected_name}: unexpected integer encoding")

    bit_size = member["attrs"].get("bit_size")
    if not bit_size or scalar_number(
        bit_size, f"{expected_name} bit size"
    ) != expected_bits:
        raise RuntimeError(
            f"{expected_name}: missing exact {expected_bits}-bit compiler width"
        )

    bit_offset = member["attrs"].get("bit_offset")
    data_bit_offset = member["attrs"].get("data_bit_offset")
    if (bit_offset is None) == (data_bit_offset is None):
        raise RuntimeError(
            f"{expected_name}: requires exactly one compiler bit-location spelling"
        )

    member_location = member["attrs"].get("data_member_location")
    if data_bit_offset is not None:
        if member_location is not None:
            raise RuntimeError(
                f"{expected_name}: DW_AT_data_bit_offset unexpectedly also has byte offset"
            )
        absolute = scalar_number(
            data_bit_offset, f"{expected_name} data bit offset"
        )
        return ("data_bit_offset", absolute, None)

    if member_location is None:
        raise RuntimeError(
            f"{expected_name}: legacy DW_AT_bit_offset requires byte storage offset"
        )
    byte_offset = scalar_number(
        member_location, f"{expected_name} byte storage offset"
    )
    legacy = scalar_number(bit_offset, f"{expected_name} legacy bit offset")
    storage_bits = 32
    if legacy > storage_bits or expected_bits > storage_bits - legacy:
        raise RuntimeError(f"{expected_name}: legacy bit slice exceeds storage unit")
    absolute = byte_offset * 8 + (storage_bits - legacy - expected_bits)
    return ("bit_offset", legacy, byte_offset, absolute)


def verify_layout(records):
    variable = find_variable(
        records, "caller_with_stack_local", "caller_bit_fields"
    )
    type_attr = variable["attrs"].get("type")
    if not type_attr:
        raise RuntimeError("caller_bit_fields has no DW_AT_type")
    structure = unwrap_type(
        records,
        type_reference(type_attr, "caller_bit_fields"),
        "caller_bit_fields",
    )
    if structure["tag"] != "DW_TAG_structure_type":
        raise RuntimeError("caller_bit_fields is not a compiler structure type")
    size = structure["attrs"].get("byte_size")
    if not size or scalar_number(size, "caller_bit_fields byte size") != 4:
        raise RuntimeError("caller_bit_fields is not exactly four bytes")

    members = [
        child for child in direct_children(records, structure)
        if child["tag"] == "DW_TAG_member"
    ]
    if len(members) != 2:
        raise RuntimeError(
            f"caller_bit_fields requires exactly two direct members, found {len(members)}"
        )

    signed = verify_member(records, members[0], "signed_bits", 5, 5)
    unsigned = verify_member(records, members[1], "unsigned_bits", 7, 6)

    # Both genuine compiler spellings must describe the same little-endian packed layout:
    # signed bits occupy aggregate bits 0..4, unsigned bits occupy bits 5..10.
    signed_absolute = (
        signed[1] if signed[0] == "data_bit_offset" else signed[3]
    )
    unsigned_absolute = (
        unsigned[1] if unsigned[0] == "data_bit_offset" else unsigned[3]
    )
    if signed_absolute != 0 or unsigned_absolute != 5:
        raise RuntimeError(
            "caller_bit_fields compiler bit locations changed: "
            f"signed={signed_absolute} unsigned={unsigned_absolute}"
        )


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_physical_bitfield_dwarf_oracle.py <fixture>")
    path = sys.argv[1]
    info = run("readelf", "--debug-dump=info", path)
    loc = run("readelf", "--debug-dump=loc", path)
    records = parse_records(info)
    basic_records = parse_basic_dies(info)
    symbols = symbol_addresses(path)

    verify_stack_local(
        "caller_with_stack_local",
        "caller_bit_fields",
        "snapshot_caller_resume_probe",
        loc,
        basic_records,
        symbols,
        probe_adjust=-1,
    )
    verify_layout(records)
    print("physical bit-field DWARF oracle passed")


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"physical bit-field DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
