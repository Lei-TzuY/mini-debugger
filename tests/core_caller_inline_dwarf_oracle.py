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


def selected_inner_bindings(records, by_offset):
    result = {}
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
            if name not in {"caller_shadow", "caller_pointer"}:
                continue
            location = child["attrs"].get("location")
            if not location:
                continue
            type_text = resolved_attr(child, by_offset, "type")
            if not type_text:
                raise RuntimeError(f"{name} has no resolved DW_AT_type")
            type_die = by_offset.get(ref_offset(type_text, f"{name} type"))
            if type_die is None:
                raise RuntimeError(f"{name} references an unknown type DIE")
            result.setdefault(name, []).append(
                (child["offset"], child["depth"], location, type_die["tag"], type_die["offset"])
            )
    return result


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
    if not shadow:
        raise RuntimeError("caller_shadow has no compiler-produced concrete DW_AT_location")
    if not pointer:
        raise RuntimeError("caller_pointer has no compiler-produced concrete DW_AT_location")
    if not any(entry[3] == "DW_TAG_pointer_type" for entry in pointer):
        raise RuntimeError(
            "caller_pointer compiler binding does not resolve to a DW_TAG_pointer_type"
        )

    print(f"caller-inline resume probe: 0x{resume:x}")
    print("caller-inline addr2line chain: " + " -> ".join(
        f"{name}@{location}" for name, location in contexts[: len(wanted)]
    ))
    print("caller_inline_inner concrete DWARF bindings:")
    for name in ("caller_shadow", "caller_pointer"):
        for offset, depth, location, type_tag, type_offset in bindings[name]:
            print(
                f"  {name}: die=0x{offset:x} depth={depth} location={location} "
                f"type={type_tag}@0x{type_offset:x}"
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
