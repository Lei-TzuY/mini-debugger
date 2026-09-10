#!/usr/bin/env python3
import re
import subprocess
import sys


def run(*args):
    return subprocess.check_output(args, text=True, stderr=subprocess.STDOUT)


def clean_name(value):
    marker = "): "
    if marker in value:
        return value.rsplit(marker, 1)[1].strip()
    return value.strip()


def parse_dies(text):
    records = []
    current = None
    header_re = re.compile(
        r"^\s*<(\d+)><([0-9a-fA-F]+)>:\s+Abbrev Number:\s+\d+\s+\((DW_TAG_[^)]+)\)"
    )
    attr_re = re.compile(r"^\s*<[0-9a-fA-F]+>\s+DW_AT_([^\s]+)\s*:\s*(.*)$")
    for line in text.splitlines():
        header = header_re.match(line)
        if header:
            if current is not None:
                records.append(current)
            current = {
                "depth": int(header.group(1)),
                "offset": int(header.group(2), 16),
                "tag": header.group(3),
                "attrs": {},
            }
            continue
        if current is not None:
            attr = attr_re.match(line)
            if attr:
                current["attrs"][attr.group(1)] = attr.group(2).strip()
    if current is not None:
        records.append(current)
    return records


def ref_offset(value, context):
    match = re.search(r"<0x([0-9a-fA-F]+)>", value)
    if not match:
        raise RuntimeError(f"{context}: DIE reference is unavailable: {value}")
    return int(match.group(1), 16)


def resolved_attr(record, by_offset, name):
    current = record
    for _ in range(8):
        value = current["attrs"].get(name)
        if value:
            return value
        origin_text = current["attrs"].get("abstract_origin")
        if not origin_text:
            return None
        current = by_offset.get(ref_offset(origin_text, "abstract_origin"))
        if current is None:
            return None
    raise RuntimeError("abstract-origin attribute chain is too deep")


def resolved_name(record, by_offset):
    value = resolved_attr(record, by_offset, "name")
    return clean_name(value) if value else None


def origin_name(record, by_offset):
    origin_text = record["attrs"].get("abstract_origin")
    if not origin_text:
        return None
    origin = by_offset.get(ref_offset(origin_text, "inline abstract_origin"))
    if origin is None or origin["tag"] != "DW_TAG_subprogram":
        raise RuntimeError("inline abstract origin does not reference a subprogram")
    return resolved_name(origin, by_offset)


def symbol_address(path, name):
    for line in run("nm", "-n", path).splitlines():
        fields = line.split()
        if len(fields) >= 3 and fields[2] == name:
            return int(fields[0], 16)
    raise RuntimeError(f"missing probe symbol: {name}")


def addr2line_contexts(path, address):
    lines = run("addr2line", "-i", "-f", "-e", path, hex(address)).splitlines()
    if len(lines) < 2 or len(lines) % 2 != 0:
        raise RuntimeError("addr2line emitted an incomplete caller-inline chain")
    return [(lines[i].strip(), lines[i + 1].strip()) for i in range(0, len(lines), 2)]


def unwrap_type(record, by_offset, context):
    current = record
    wrappers = {"DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type", "DW_TAG_restrict_type"}
    for _ in range(16):
        if current["tag"] not in wrappers:
            return current
        wrapped = current["attrs"].get("type")
        if not wrapped:
            raise RuntimeError(f"{context}: type wrapper has no DW_AT_type")
        current = by_offset.get(ref_offset(wrapped, context))
        if current is None:
            raise RuntimeError(f"{context}: type wrapper references an unknown DIE")
    raise RuntimeError(f"{context}: type wrapper chain is too deep")


def referenced_type(record, by_offset, context):
    type_text = resolved_attr(record, by_offset, "type")
    if not type_text:
        raise RuntimeError(f"{context}: no resolved DW_AT_type")
    result = by_offset.get(ref_offset(type_text, context))
    if result is None:
        raise RuntimeError(f"{context}: references an unknown type DIE")
    return unwrap_type(result, by_offset, context)


def selected_inner_bindings(records, by_offset):
    result = {}
    wanted = {"caller_shadow", "caller_pointer", "caller_aggregate_pointer"}
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
            name = resolved_name(child, by_offset)
            if name not in wanted:
                continue
            location = child["attrs"].get("location")
            if not location:
                continue
            type_die = referenced_type(child, by_offset, f"{name} type")
            result.setdefault(name, []).append(
                (child["offset"], child["depth"], location, type_die["tag"], type_die["offset"])
            )
    return result


def validate_aggregate_pointer(records, by_offset, bindings):
    entries = bindings.get("caller_aggregate_pointer", [])
    if not entries:
        raise RuntimeError(
            "caller_aggregate_pointer has no compiler-produced concrete DW_AT_location"
        )

    candidates = []
    binding_offsets = {entry[0] for entry in entries}
    for record in records:
        if record["offset"] not in binding_offsets:
            continue
        pointer = referenced_type(record, by_offset, "caller_aggregate_pointer type")
        if pointer["tag"] != "DW_TAG_pointer_type":
            continue
        pointee_text = pointer["attrs"].get("type")
        if not pointee_text:
            raise RuntimeError("caller_aggregate_pointer pointer type has no pointee")
        pointee = by_offset.get(ref_offset(pointee_text, "aggregate pointee"))
        if pointee is None:
            raise RuntimeError("caller_aggregate_pointer pointee DIE is unavailable")
        pointee = unwrap_type(pointee, by_offset, "aggregate pointee")
        if pointee["tag"] == "DW_TAG_structure_type":
            candidates.append(pointee)

    if not candidates:
        raise RuntimeError(
            "caller_aggregate_pointer does not resolve to a compiler-owned structure type"
        )

    structure = candidates[0]
    try:
        position = next(i for i, record in enumerate(records) if record["offset"] == structure["offset"])
    except StopIteration as error:
        raise RuntimeError("aggregate structure DIE disappeared from the parsed stream") from error

    members = {}
    for child in records[position + 1 :]:
        if child["depth"] <= structure["depth"]:
            break
        if child["depth"] != structure["depth"] + 1 or child["tag"] != "DW_TAG_member":
            continue
        name = resolved_name(child, by_offset)
        if name in {"direct", "linked"}:
            members[name] = child

    if set(members) != {"direct", "linked"}:
        raise RuntimeError(f"aggregate direct-member DIEs are incomplete: {sorted(members)}")
    for name, member in members.items():
        if "data_member_location" not in member["attrs"]:
            raise RuntimeError(f"aggregate member {name} has no DW_AT_data_member_location")

    direct_type = referenced_type(members["direct"], by_offset, "direct member type")
    if direct_type["tag"] != "DW_TAG_base_type":
        raise RuntimeError("aggregate direct member is not a compiler base type")

    linked_type = referenced_type(members["linked"], by_offset, "linked member type")
    if linked_type["tag"] != "DW_TAG_pointer_type":
        raise RuntimeError("aggregate linked member is not a compiler pointer type")
    linked_pointee_text = linked_type["attrs"].get("type")
    if not linked_pointee_text:
        raise RuntimeError("aggregate linked pointer has no pointee type")
    linked_pointee = by_offset.get(ref_offset(linked_pointee_text, "linked member pointee"))
    if linked_pointee is None:
        raise RuntimeError("aggregate linked pointer references an unknown pointee")
    linked_pointee = unwrap_type(linked_pointee, by_offset, "linked member pointee")
    if linked_pointee["tag"] != "DW_TAG_base_type":
        raise RuntimeError("aggregate linked pointer does not target a compiler base type")

    return structure, members


def main():
    if len(sys.argv) != 2:
        raise SystemExit("usage: core_caller_inline_dwarf_oracle.py <fixture>")
    path = sys.argv[1]
    records = parse_dies(run("readelf", "--debug-dump=info", path))
    by_offset = {record["offset"]: record for record in records}
    resume = symbol_address(path, "snapshot_caller_inline_resume_probe")
    contexts = addr2line_contexts(path, resume)
    chain = [name for name, _ in contexts]
    wanted = ["caller_inline_inner", "caller_inline_outer", "caller_physical_frame"]
    if chain[: len(wanted)] != wanted:
        raise RuntimeError(f"unexpected caller-inline addr2line chain: {chain}")

    bindings = selected_inner_bindings(records, by_offset)
    shadow = bindings.get("caller_shadow", [])
    pointer = bindings.get("caller_pointer", [])
    aggregate_pointer = bindings.get("caller_aggregate_pointer", [])
    if not shadow:
        raise RuntimeError("caller_shadow has no compiler-produced concrete DW_AT_location")
    if not pointer:
        raise RuntimeError("caller_pointer has no compiler-produced concrete DW_AT_location")
    if not aggregate_pointer:
        raise RuntimeError(
            "caller_aggregate_pointer has no compiler-produced concrete DW_AT_location"
        )
    if not any(entry[3] == "DW_TAG_pointer_type" for entry in pointer):
        raise RuntimeError(
            "caller_pointer compiler binding does not resolve to a DW_TAG_pointer_type"
        )

    structure, members = validate_aggregate_pointer(records, by_offset, bindings)

    print(f"caller-inline resume probe: 0x{resume:x}")
    print("caller-inline addr2line chain: " + " -> ".join(
        f"{name}@{location}" for name, location in contexts[: len(wanted)]
    ))
    print("caller_inline_inner concrete DWARF bindings:")
    for name in ("caller_shadow", "caller_pointer", "caller_aggregate_pointer"):
        for offset, depth, location, type_tag, type_offset in bindings[name]:
            print(
                f"  {name}: die=0x{offset:x} depth={depth} location={location} "
                f"type={type_tag}@0x{type_offset:x}"
            )
    print(
        "caller_aggregate_pointer structure: "
        f"die=0x{structure['offset']:x} direct={members['direct']['attrs']['data_member_location']} "
        f"linked={members['linked']['attrs']['data_member_location']}"
    )

    try:
        loc_dump = run("readelf", "--debug-dump=loc", path)
    except subprocess.CalledProcessError as error:
        loc_dump = error.output
    print("caller-inline location-list dump (bounded):")
    print(loc_dump[:12000])


if __name__ == "__main__":
    try:
        main()
    except (RuntimeError, subprocess.CalledProcessError) as error:
        print(f"caller-inline DWARF oracle failure: {error}", file=sys.stderr)
        raise SystemExit(1)
