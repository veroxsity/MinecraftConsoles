#!/usr/bin/env python3
from __future__ import annotations

import argparse
import shutil
import struct
from dataclasses import dataclass
from pathlib import Path


@dataclass
class Entry:
    raw_name: str
    ptr: int
    size: int
    data: bytes

    @property
    def logical_name(self) -> str:
        return self.raw_name[1:] if self.raw_name.startswith("*") else self.raw_name


def read_utf(data: bytes, offset: int) -> tuple[str, int]:
    (utf_len,) = struct.unpack_from(">H", data, offset)
    offset += 2
    raw = data[offset : offset + utf_len]
    offset += utf_len
    return raw.decode("utf-8"), offset


def write_utf(value: str) -> bytes:
    raw = value.encode("utf-8")
    if len(raw) > 0xFFFF:
        raise ValueError(f"archive entry name is too long: {value}")
    return struct.pack(">H", len(raw)) + raw


def load_archive(path: Path) -> list[Entry]:
    blob = path.read_bytes()
    offset = 0
    (count,) = struct.unpack_from(">i", blob, offset)
    offset += 4

    headers: list[tuple[str, int, int]] = []
    for _ in range(count):
        raw_name, offset = read_utf(blob, offset)
        ptr, size = struct.unpack_from(">ii", blob, offset)
        offset += 8
        headers.append((raw_name, ptr, size))

    entries: list[Entry] = []
    for raw_name, ptr, size in headers:
        entries.append(Entry(raw_name=raw_name, ptr=ptr, size=size, data=blob[ptr : ptr + size]))
    return entries


def build_archive(entries: list[Entry]) -> bytes:
    header_parts: list[bytes] = [struct.pack(">i", len(entries))]
    data_offset = 4

    encoded_names = [write_utf(entry.raw_name) for entry in entries]
    for encoded_name, entry in zip(encoded_names, entries, strict=True):
        data_offset += len(encoded_name) + 8

    payload_parts: list[bytes] = []
    current_ptr = data_offset
    for encoded_name, entry in zip(encoded_names, entries, strict=True):
        header_parts.append(encoded_name)
        header_parts.append(struct.pack(">ii", current_ptr, len(entry.data)))
        payload_parts.append(entry.data)
        current_ptr += len(entry.data)

    return b"".join(header_parts + payload_parts)


def apply_overlays(entries: list[Entry], overlays: dict[str, Path]) -> list[str]:
    replaced: list[str] = []
    by_name = {entry.logical_name: entry for entry in entries}

    for logical_name, overlay_path in overlays.items():
        if not overlay_path.exists():
            raise FileNotFoundError(f"overlay file does not exist: {overlay_path}")

        overlay_data = overlay_path.read_bytes()
        existing = by_name.get(logical_name)
        if existing is None:
            entries.append(Entry(raw_name=logical_name, ptr=0, size=len(overlay_data), data=overlay_data))
        else:
            if existing.raw_name.startswith("*"):
                raise ValueError(
                    f"refusing to replace compressed archive entry '{existing.raw_name}' with raw data"
                )
            existing.data = overlay_data
            existing.size = len(overlay_data)
        replaced.append(logical_name)

    return replaced


def parse_overlay_args(values: list[str]) -> dict[str, Path]:
    overlays: dict[str, Path] = {}
    for value in values:
        name, separator, path = value.partition("=")
        if not separator:
            raise ValueError(f"overlay must be NAME=PATH, got: {value}")
        overlays[name] = Path(path)
    return overlays


def main() -> int:
    parser = argparse.ArgumentParser(description="Overlay selected files into a MediaWindows64.arc archive.")
    parser.add_argument("input", type=Path, help="Existing archive to read")
    parser.add_argument("output", type=Path, help="Archive path to write")
    parser.add_argument(
        "--overlay",
        action="append",
        default=[],
        metavar="NAME=PATH",
        help="Replace or add archive entry NAME with the bytes from PATH",
    )
    parser.add_argument(
        "--backup",
        action="store_true",
        help="Write a .bak copy of the output file before replacing it when the output already exists",
    )
    args = parser.parse_args()

    overlays = parse_overlay_args(args.overlay)
    entries = load_archive(args.input)
    touched = apply_overlays(entries, overlays)
    rebuilt = build_archive(entries)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    if args.backup and args.output.exists():
        backup_path = args.output.with_suffix(args.output.suffix + ".bak")
        shutil.copy2(args.output, backup_path)

    args.output.write_bytes(rebuilt)

    print(f"wrote {args.output}")
    print(f"entries: {len(entries)}")
    if touched:
        print("overlays:")
        for logical_name in touched:
            print(f"  {logical_name}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
