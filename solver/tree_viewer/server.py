#!/usr/bin/env python3
"""Indexed, viewport-lazy search tree viewer for Solving2048.

The index command streams a solver SGF into fixed-size node records.  The
serve command memory-maps those records and exposes small JSON ranges to the
browser.  The complete tree is logically laid out by DFS order and depth;
the browser never has to parse or retain the complete SGF.
"""

from __future__ import annotations

import argparse
import json
import math
import mmap
import os
from pathlib import Path
import shutil
import struct
import tempfile
from http import HTTPStatus
from http.server import SimpleHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, urlparse


INDEX_VERSION = 1
NODE = struct.Struct("<QQIIQQQIIHBBBBBB")
ROOT_ACTION = 0xFFFF
OVERVIEW_BUCKET = 16_384
MAX_RANGE_NODES = 20_000

PHASE_PLAYER = 0
PHASE_ENVIRONMENT = 1
OUTCOME_UNKNOWN = 0
OUTCOME_WIN = 1
OUTCOME_LOSS = 2
SOLVED_NONE = 0
SOLVED_TERMINAL = 1
SOLVED_RZONE = 2
SOLVED_TT = 3
SOLVED_BACKPROPAGATION = 4
SOLVED_NODE_LIMIT = 5

OUTCOMES = {"UNKNOWN": OUTCOME_UNKNOWN, "WIN": OUTCOME_WIN, "LOSS": OUTCOME_LOSS}
SOLVED = {
    "NONE": SOLVED_NONE,
    "TERMINAL": SOLVED_TERMINAL,
    "RZONE_TT": SOLVED_RZONE,
    "TT": SOLVED_TT,
    "BACKPROPAGATION": SOLVED_BACKPROPAGATION,
    "NODE_LIMIT": SOLVED_NODE_LIMIT,
}
OUTCOME_NAMES = ["UNKNOWN", "WIN", "LOSS"]
SOLVED_NAMES = ["NONE", "TERMINAL", "RZONE_TT", "TT", "BACKPROPAGATION", "NODE_LIMIT"]
PHASE_NAMES = ["player", "environment"]
DIRECTION_NAMES = ["up", "right", "down", "left"]


def parse_comment(text: str) -> dict[str, str]:
    fields: dict[str, str] = {}
    for line in text.splitlines():
        line = line.strip()
        if not line:
            continue
        if "=" in line:
            key, value = line.split("=", 1)
        elif ":" in line:
            key, value = line.split(":", 1)
        else:
            continue
        fields[key.strip()] = value.strip()
    return fields


def unsigned(value: str | None, default: int = 0) -> int:
    if value is None or value == "":
        return default
    return int(value, 0)


def slide_line(values: list[int]) -> list[int]:
    compact = [value for value in values if value]
    output: list[int] = []
    index = 0
    while index < len(compact):
        if index + 1 < len(compact) and compact[index] == compact[index + 1]:
            output.append(compact[index] + 1)
            index += 2
        else:
            output.append(compact[index])
            index += 1
    return output + [0] * (4 - len(output))


def line_positions(direction: int, line: int) -> list[int]:
    if direction == 0:
        return [offset * 4 + line for offset in range(4)]
    if direction == 1:
        return [line * 4 + (3 - offset) for offset in range(4)]
    if direction == 2:
        return [(3 - offset) * 4 + line for offset in range(4)]
    if direction == 3:
        return [line * 4 + offset for offset in range(4)]
    raise ValueError(f"invalid slide opcode {direction}")


def apply_action(board: int, action: int) -> int:
    if action == ROOT_ACTION:
        return board
    if action >= 4:
        position = action - 4 if action < 20 else action - 20
        rank = 1 if action < 20 else 2
        if not 0 <= position < 16:
            raise ValueError(f"invalid placement action {action}")
        if ((board >> (position * 4)) & 0xF) != 0:
            raise ValueError(f"placement {action} targets a non-empty cell")
        return board | (rank << (position * 4))
    result = board
    changed = False
    for line in range(4):
        positions = line_positions(action, line)
        before = [(board >> (position * 4)) & 0xF for position in positions]
        after = slide_line(before)
        if after != before:
            changed = True
        for position, value in zip(positions, after):
            result &= ~(0xF << (position * 4))
            result |= value << (position * 4)
    if not changed:
        raise ValueError(f"illegal unchanged slide {action}")
    return result


def action_label(action: int) -> str:
    if action == ROOT_ACTION:
        return "root"
    if action < 4:
        return DIRECTION_NAMES[action]
    if action < 20:
        return f"2@{action - 4}"
    return f"4@{action - 20}"


class CharacterStream:
    def __init__(self, path: Path, chunk_size: int = 1 << 20):
        self.path = path
        self.file = path.open("r", encoding="utf-8", errors="strict", newline="")
        self.chunk_size = chunk_size
        self.buffer = ""
        self.position = 0
        self.offset = 0
        self.eof = False

    def close(self) -> None:
        self.file.close()

    def _fill(self) -> None:
        if self.position < len(self.buffer) or self.eof:
            return
        self.buffer = self.file.read(self.chunk_size)
        self.position = 0
        if not self.buffer:
            self.eof = True

    def peek(self) -> str:
        self._fill()
        return "" if self.eof else self.buffer[self.position]

    def get(self) -> str:
        value = self.peek()
        if value:
            self.position += 1
            self.offset += 1
        return value

    def skip_space(self) -> None:
        while self.peek() and self.peek().isspace():
            self.get()


def parse_value(stream: CharacterStream) -> str:
    if stream.get() != "[":
        raise ValueError(f"{stream.path}: byte {stream.offset}: expected '['")
    output: list[str] = []
    while True:
        character = stream.get()
        if not character:
            raise ValueError(f"{stream.path}: unterminated SGF property")
        if character == "]":
            return "".join(output)
        if character == "\\":
            escaped = stream.get()
            if not escaped:
                raise ValueError(f"{stream.path}: unterminated SGF escape")
            if escaped == "\r":
                if stream.peek() == "\n":
                    stream.get()
                continue
            if escaped == "\n":
                continue
            output.append(escaped)
        else:
            output.append(character)


def sgf_events(path: Path):
    stream = CharacterStream(path)
    try:
        while True:
            stream.skip_space()
            character = stream.peek()
            if not character:
                return
            if character in "()":
                yield stream.get()
                continue
            if character != ";":
                raise ValueError(f"{path}: byte {stream.offset}: unexpected {character!r}")
            stream.get()
            properties: dict[str, list[str]] = {}
            while True:
                stream.skip_space()
                if not stream.peek().isalpha():
                    break
                name_chars: list[str] = []
                while stream.peek().isalpha():
                    name_chars.append(stream.get())
                name = "".join(name_chars)
                stream.skip_space()
                values: list[str] = []
                while stream.peek() == "[":
                    values.append(parse_value(stream))
                    stream.skip_space()
                if not values:
                    raise ValueError(f"{path}: property {name} has no value")
                properties.setdefault(name, []).extend(values)
            yield properties
    finally:
        stream.close()


class NodeFileWriter:
    def __init__(self, path: Path):
        self.file = path.open("w+b")
        self.count = 0
        self.open_nodes: dict[int, list[int]] = {}

    def close(self) -> None:
        self.file.close()

    def create(self, *, parent: int, board: int, depth: int, action: int,
               phase: int, outcome: int, solved: int, rz_transform: int,
               rz_certificate: int, skipped: int, tt_source: int) -> int:
        self.count += 1
        node_id = self.count
        self.file.seek(0, os.SEEK_END)
        self.file.write(NODE.pack(
            node_id, 0, 0, 0, parent, board, tt_source, depth,
            rz_certificate, action, phase, outcome, solved, 0,
            rz_transform, skipped,
        ))
        self.open_nodes[node_id] = [0, 0]
        if parent:
            parent_state = self.open_nodes.get(parent)
            if parent_state is None:
                raise ValueError(f"node {node_id} refers to closed parent {parent}")
            parent_state[0] += 1
            if parent_state[1] == 0:
                parent_state[1] = node_id
        return node_id

    def finalize(self, node_id: int, subtree_end: int) -> None:
        state = self.open_nodes.pop(node_id, None)
        if state is None:
            return
        offset = (node_id - 1) * NODE.size
        self.file.seek(offset)
        values = list(NODE.unpack(self.file.read(NODE.size)))
        values[0] = subtree_end
        values[1] = state[1]
        values[2] = state[0]
        self.file.seek(offset)
        self.file.write(NODE.pack(*values))
        self.file.seek(0, os.SEEK_END)


def empty_bucket(index: int) -> dict[str, int]:
    return {
        "start": index * OVERVIEW_BUCKET + 1,
        "end": (index + 1) * OVERVIEW_BUCKET,
        "total": 0,
        "win": 0,
        "loss": 0,
        "terminal": 0,
        "rzone": 0,
        "tt": 0,
        "first_terminal": 0,
        "first_rzone": 0,
        "first_tt": 0,
        "max_depth": 0,
    }


def build_index(sgf: Path, output: Path, rz: Path | None, tt: Path | None) -> None:
    if output.exists():
        raise FileExistsError(f"index output already exists: {output}")
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = Path(tempfile.mkdtemp(prefix=output.name + ".tmp.", dir=output.parent))
    writer = NodeFileWriter(temporary / "nodes.bin")
    frames: list[dict[str, int]] = []
    current_id = 0
    current_board = 0
    current_depth = -1
    started = False
    root_fields: dict[str, str] = {}
    buckets: list[dict[str, int]] = []
    maximum_depth = 0

    try:
        for event in sgf_events(sgf):
            if event == "(":
                frames.append({
                    "base_id": current_id,
                    "base_board": current_board,
                    "base_depth": current_depth,
                    "root": 0,
                })
                continue
            if event == ")":
                if not frames:
                    raise ValueError(f"{sgf}: unmatched ')'")
                frame = frames.pop()
                if frame["root"]:
                    writer.finalize(frame["root"], writer.count)
                current_id = frame["base_id"]
                current_board = frame["base_board"]
                current_depth = frame["base_depth"]
                continue

            properties = event
            action = ROOT_ACTION
            player = ""
            if "B" in properties:
                action = unsigned(properties["B"][0])
                player = "B"
            elif "W" in properties:
                action = unsigned(properties["W"][0])
                player = "W"
            if action != ROOT_ACTION:
                current_board = apply_action(current_board, action)

            comment = properties.get("C", [""])[0]
            fields = parse_comment(comment)
            is_search_root = not started and "winning_tile" in fields and "solved_by" in fields
            if is_search_root:
                started = True
                root_fields = fields.copy()

            if not started:
                continue
            if not is_search_root and current_id == 0:
                continue

            parent = 0 if is_search_root else current_id
            depth = 0 if is_search_root else current_depth + 1
            phase = PHASE_PLAYER if is_search_root or player == "W" else PHASE_ENVIRONMENT
            outcome_name = fields.get("solver_status", "UNKNOWN").upper()
            solved_name = fields.get("solved_by", "NONE").upper()
            outcome = OUTCOMES.get(outcome_name, OUTCOME_UNKNOWN)
            solved = SOLVED.get(solved_name, SOLVED_NONE)
            rz_certificate = unsigned(fields.get("rzone_certificate_id"))
            rz_transform = unsigned(fields.get("rzone_transform"))
            tt_source = unsigned(fields.get("tt_source_node_id"))
            skipped = len(fields.get("children_skipped_actions", "").split())
            node_id = writer.create(
                parent=parent,
                board=current_board,
                depth=depth,
                action=ROOT_ACTION if is_search_root else action,
                phase=phase,
                outcome=outcome,
                solved=solved,
                rz_transform=rz_transform,
                rz_certificate=rz_certificate,
                skipped=skipped,
                tt_source=tt_source,
            )
            current_id = node_id
            current_depth = depth
            maximum_depth = max(maximum_depth, depth)
            if not frames:
                raise ValueError(f"{sgf}: node outside game tree")
            if frames[-1]["root"] == 0:
                frames[-1]["root"] = node_id

            bucket_index = (node_id - 1) // OVERVIEW_BUCKET
            while len(buckets) <= bucket_index:
                buckets.append(empty_bucket(len(buckets)))
            bucket = buckets[bucket_index]
            bucket["total"] += 1
            bucket["max_depth"] = max(bucket["max_depth"], depth)
            if outcome == OUTCOME_WIN:
                bucket["win"] += 1
            elif outcome == OUTCOME_LOSS:
                bucket["loss"] += 1
            if solved == SOLVED_TERMINAL:
                bucket["terminal"] += 1
                if not bucket["first_terminal"]:
                    bucket["first_terminal"] = node_id
            if rz_certificate:
                bucket["rzone"] += 1
                if not bucket["first_rzone"]:
                    bucket["first_rzone"] = node_id
            if solved == SOLVED_TT:
                bucket["tt"] += 1
                if not bucket["first_tt"]:
                    bucket["first_tt"] = node_id

            if node_id % 1_000_000 == 0:
                print(f"indexed {node_id:,} search nodes", flush=True)

        if frames:
            raise ValueError(f"{sgf}: unterminated game tree")
        if not started or writer.count == 0:
            raise ValueError(f"{sgf}: solver search root was not found")
        if writer.open_nodes:
            for node_id in sorted(writer.open_nodes, reverse=True):
                writer.finalize(node_id, writer.count)
        writer.file.flush()
        os.fsync(writer.file.fileno())
        writer.close()

        buckets[-1]["end"] = writer.count
        metadata = {
            "schema_version": INDEX_VERSION,
            "node_record_size": NODE.size,
            "node_count": writer.count,
            "maximum_depth": maximum_depth,
            "overview_bucket_size": OVERVIEW_BUCKET,
            "sgf": str(sgf.resolve()),
            "sgf_size": sgf.stat().st_size,
            "rz": str(rz.resolve()) if rz else "",
            "tt": str(tt.resolve()) if tt else "",
            "winning_tile": unsigned(root_fields.get("winning_tile")),
            "outcome": root_fields.get("solver_status", "UNKNOWN"),
            "root_statistics": root_fields,
        }
        (temporary / "metadata.json").write_text(
            json.dumps(metadata, ensure_ascii=False, indent=2), encoding="utf-8")
        (temporary / "overview.json").write_text(
            json.dumps(buckets, separators=(",", ":")), encoding="utf-8")
        os.replace(temporary, output)
        print(f"index complete: {writer.count:,} nodes, depth {maximum_depth}")
        print(f"index directory: {output}")
    except Exception:
        try:
            writer.close()
        except Exception:
            pass
        shutil.rmtree(temporary, ignore_errors=True)
        raise


class NodeStore:
    def __init__(self, directory: Path):
        self.directory = directory
        self.metadata = json.loads((directory / "metadata.json").read_text(encoding="utf-8"))
        if self.metadata.get("schema_version") != INDEX_VERSION:
            raise ValueError("unsupported tree viewer index version")
        if self.metadata.get("node_record_size") != NODE.size:
            raise ValueError("tree viewer node record size mismatch")
        self.overview = json.loads((directory / "overview.json").read_text(encoding="utf-8"))
        self.node_file = (directory / "nodes.bin").open("rb")
        self.nodes = mmap.mmap(self.node_file.fileno(), 0, access=mmap.ACCESS_READ)
        expected = self.metadata["node_count"] * NODE.size
        if len(self.nodes) != expected:
            raise ValueError("tree viewer node file size mismatch")
        self.rzone = RZoneFile(Path(self.metadata["rz"])) if self.metadata.get("rz") else None
        self.tt_summary = read_tt_summary(Path(self.metadata["tt"])) if self.metadata.get("tt") else None

    def close(self) -> None:
        if self.rzone:
            self.rzone.close()
        self.nodes.close()
        self.node_file.close()

    def raw(self, node_id: int) -> tuple[int, ...]:
        if node_id < 1 or node_id > self.metadata["node_count"]:
            raise KeyError(f"node {node_id} is outside the index")
        offset = (node_id - 1) * NODE.size
        return NODE.unpack_from(self.nodes, offset)

    def node(self, node_id: int, include_board: bool = True) -> dict:
        values = self.raw(node_id)
        subtree_end, first_child, children, _, parent, board, tt_source, depth, rz_certificate, action, phase, outcome, solved, _, rz_transform, skipped = values
        parent_depth = self.raw(parent)[7] if parent else 0
        result = {
            "id": node_id,
            "parent": parent,
            "parent_depth": parent_depth,
            "depth": depth,
            "subtree_end": subtree_end,
            "children": children,
            "first_child": first_child,
            "skipped": skipped,
            "action": action,
            "action_label": action_label(action),
            "phase": PHASE_NAMES[phase] if phase < len(PHASE_NAMES) else "unknown",
            "outcome": OUTCOME_NAMES[outcome] if outcome < len(OUTCOME_NAMES) else "UNKNOWN",
            "solved_by": SOLVED_NAMES[solved] if solved < len(SOLVED_NAMES) else "NONE",
            "rz_certificate": rz_certificate,
            "rz_transform": rz_transform,
            "tt_source": tt_source,
        }
        if include_board:
            result["board"] = f"0x{board:016x}"
            result["tiles"] = board_tiles(board)
        return result

    def range(self, start: int, end: int, depth_min: int, depth_max: int, limit: int) -> dict:
        node_count = self.metadata["node_count"]
        start = max(1, min(start, node_count))
        end = max(start, min(end, node_count))
        span = end - start + 1
        stride = max(1, math.ceil(span / max(1, limit)))
        mode = "exact" if stride == 1 else "sampled"
        ids = set(range(start, end + 1, stride))

        overview = self.overview_range(start, end, 512)
        if mode == "sampled":
            for bucket in overview:
                for key in ("first_terminal", "first_rzone", "first_tt"):
                    value = bucket.get(key, 0)
                    if start <= value <= end:
                        ids.add(value)
        rows = []
        for node_id in sorted(ids):
            values = self.raw(node_id)
            depth = values[7]
            if depth < depth_min or depth > depth_max:
                continue
            rows.append(self.node(node_id, include_board=False))
            if len(rows) >= limit + 1536:
                break
        return {
            "mode": mode,
            "stride": stride,
            "from": start,
            "to": end,
            "nodes": rows,
            "overview": overview,
        }

    def overview_range(self, start: int, end: int, maximum: int) -> list[dict]:
        bucket_size = self.metadata["overview_bucket_size"]
        first = max(0, (start - 1) // bucket_size)
        last = min(len(self.overview) - 1, (end - 1) // bucket_size)
        selected = self.overview[first:last + 1]
        if not selected:
            return []
        group_size = max(1, math.ceil(len(selected) / maximum))
        grouped: list[dict] = []
        for offset in range(0, len(selected), group_size):
            group = selected[offset:offset + group_size]
            merged = {key: 0 for key in ("total", "win", "loss", "terminal", "rzone", "tt")}
            merged["start"] = group[0]["start"]
            merged["end"] = group[-1]["end"]
            merged["max_depth"] = max(item["max_depth"] for item in group)
            for key in ("first_terminal", "first_rzone", "first_tt"):
                merged[key] = next((item[key] for item in group if item[key]), 0)
            for item in group:
                for key in ("total", "win", "loss", "terminal", "rzone", "tt"):
                    merged[key] += item[key]
            grouped.append(merged)
        return grouped

    def path(self, node_id: int, limit: int = 512) -> list[dict]:
        result: list[dict] = []
        seen = set()
        while node_id and node_id not in seen and len(result) < limit:
            seen.add(node_id)
            result.append(self.node(node_id, include_board=False))
            node_id = result[-1]["parent"]
        result.reverse()
        return result

    def root_trajectory(self, limit: int = 512) -> list[dict]:
        """Return the first complete DFS trajectory without scanning the tree."""
        result: list[dict] = []
        node_id = 1
        seen = set()
        while node_id and node_id not in seen and len(result) < limit:
            seen.add(node_id)
            node = self.node(node_id, include_board=False)
            result.append(node)
            node_id = node["first_child"]
        return result

    def navigation(self, node_id: int) -> dict[str, int]:
        """Return structural keyboard-navigation targets for one node."""
        node = self.node(node_id, include_board=False)
        previous_sibling = 0
        next_sibling = 0
        parent_id = node["parent"]
        if parent_id:
            parent = self.node(parent_id, include_board=False)
            sibling_id = parent["first_child"]
            while sibling_id:
                if sibling_id == node_id:
                    candidate = node["subtree_end"] + 1
                    if (candidate <= parent["subtree_end"]
                            and self.raw(candidate)[4] == parent_id):
                        next_sibling = candidate
                    break
                sibling = self.node(sibling_id, include_board=False)
                candidate = sibling["subtree_end"] + 1
                if candidate > parent["subtree_end"] or self.raw(candidate)[4] != parent_id:
                    break
                previous_sibling = sibling_id
                sibling_id = candidate
        return {
            "previous_sibling": previous_sibling,
            "next_sibling": next_sibling,
        }


def board_tiles(board: int) -> list[int]:
    result = []
    for position in range(16):
        rank = (board >> (position * 4)) & 0xF
        result.append(0 if rank == 0 else 1 << rank)
    return result


def transform_position(position: int, transform: int) -> int:
    row, column = divmod(position, 4)
    if transform & 4:
        row = 3 - row
    for _ in range(transform & 3):
        row, column = column, 3 - row
    return row * 4 + column


def inverse_transform(transform: int) -> int:
    return (0, 3, 2, 1, 4, 5, 6, 7)[transform]


def transform_mask(mask: int, transform: int) -> int:
    result = 0
    for position in range(16):
        if mask & (1 << position):
            result |= 1 << transform_position(position, transform)
    return result


def transform_board(board: int, transform: int) -> int:
    result = 0
    for position in range(16):
        rank = (board >> (position * 4)) & 0xF
        result |= rank << (transform_position(position, transform) * 4)
    return result


class RZoneFile:
    HEADER_SIZE = 128
    RECORD_SIZE = 32
    RECORD = struct.Struct("<QIIHBBBBHQ")

    def __init__(self, path: Path):
        self.path = path
        self.file = path.open("rb")
        header = self.file.read(self.HEADER_SIZE)
        if len(header) != self.HEADER_SIZE or header[:8] != b"S2048RZ\0":
            raise ValueError(f"invalid RZone file: {path}")
        version, header_size, record_size = struct.unpack_from("<HHH", header, 8)
        if version != 1 or header_size != self.HEADER_SIZE or record_size != self.RECORD_SIZE:
            raise ValueError(f"unsupported RZone format in {path}")
        self.winning_tile = struct.unpack_from("<I", header, 32)[0]
        self.count = struct.unpack_from("<Q", header, 48)[0]
        self.mapping = mmap.mmap(self.file.fileno(), 0, access=mmap.ACCESS_READ)

    def close(self) -> None:
        self.mapping.close()
        self.file.close()

    def certificate(self, certificate_id: int, transform: int = 0) -> dict:
        if certificate_id < 1 or certificate_id > self.count:
            raise KeyError(f"RZone certificate {certificate_id} is outside the snapshot")
        offset = self.HEADER_SIZE + (certificate_id - 1) * self.RECORD_SIZE
        masked_board, child, depth, mask, kind, phase, _, argument, _, _ = self.RECORD.unpack_from(self.mapping, offset)
        kind_name = {1: "terminal", 2: "fixed_slide", 3: "universal_placement"}.get(kind, "unknown")
        phase_name = {1: "player", 2: "environment"}.get(phase, "unknown")
        inverse = inverse_transform(transform) if 0 <= transform < 8 else 0
        display_mask = transform_mask(mask, inverse)
        display_board = transform_board(masked_board, inverse)
        return {
            "id": certificate_id,
            "kind": kind_name,
            "phase": phase_name,
            "mask": f"0x{mask:04x}",
            "masked_board": f"0x{masked_board:016x}",
            "child": child,
            "depth": depth,
            "argument": argument,
            "transform": transform,
            "display_mask": f"0x{display_mask:04x}",
            "display_masked_board": f"0x{display_board:016x}",
            "winning_tile": self.winning_tile,
        }

    def proof(self, certificate_id: int, transform: int = 0, limit: int = 256) -> list[dict]:
        result = []
        seen = set()
        while certificate_id and certificate_id not in seen and len(result) < limit:
            seen.add(certificate_id)
            certificate = self.certificate(certificate_id, transform)
            result.append(certificate)
            certificate_id = certificate["child"]
            transform = 0
        return result


def read_tt_summary(path: Path) -> dict:
    with path.open("rb") as source:
        header = source.read(128)
    if len(header) != 128 or header[:8] != b"S2048TT\0":
        raise ValueError(f"invalid TT file: {path}")
    version, header_size, record_size, flags = struct.unpack_from("<HHHH", header, 8)
    if version != 1 or header_size != 128 or record_size != 16:
        raise ValueError(f"unsupported TT format in {path}")
    return {
        "version": version,
        "record_size": record_size,
        "symmetry": bool(flags & 1),
        "win_only": bool(flags & 2),
        "winning_tile": struct.unpack_from("<I", header, 28)[0],
        "player_records": struct.unpack_from("<Q", header, 40)[0],
        "environment_records": struct.unpack_from("<Q", header, 56)[0],
    }


class ViewerHandler(SimpleHTTPRequestHandler):
    store: NodeStore
    static_directory: Path

    def __init__(self, *args, **kwargs):
        super().__init__(*args, directory=str(self.static_directory), **kwargs)

    def log_request(self, _code="-", _size="-") -> None:
        pass

    def end_headers(self) -> None:
        # Viewer assets change frequently during local development.  Edge can
        # otherwise reuse a same-second Last-Modified response after reload.
        self.send_header("Cache-Control", "no-store, no-cache, must-revalidate, max-age=0")
        self.send_header("Pragma", "no-cache")
        self.send_header("Expires", "0")
        super().end_headers()

    def send_json(self, value, status: HTTPStatus = HTTPStatus.OK) -> None:
        payload = json.dumps(value, separators=(",", ":"), ensure_ascii=False).encode("utf-8")
        try:
            self.send_response(status)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(payload)))
            self.end_headers()
            self.wfile.write(payload)
        except (BrokenPipeError, ConnectionResetError):
            # Browsers may close an in-flight viewport response during navigation.
            return

    def do_GET(self) -> None:
        parsed = urlparse(self.path)
        if not parsed.path.startswith("/api/"):
            super().do_GET()
            return
        try:
            query = parse_qs(parsed.query)
            if parsed.path == "/api/meta":
                self.send_json({
                    **self.store.metadata,
                    "tt_summary": self.store.tt_summary,
                    "has_rzone": self.store.rzone is not None,
                })
                return
            if parsed.path == "/api/tree":
                count = self.store.metadata["node_count"]
                start = unsigned(query.get("from", ["1"])[0], 1)
                end = unsigned(query.get("to", [str(count)])[0], count)
                depth_min = unsigned(query.get("depth_min", ["0"])[0])
                depth_max = unsigned(query.get("depth_max", [str(self.store.metadata["maximum_depth"])])[0])
                limit = min(MAX_RANGE_NODES, max(100, unsigned(query.get("limit", ["8000"])[0], 8000)))
                self.send_json(self.store.range(start, end, depth_min, depth_max, limit))
                return
            if parsed.path == "/api/trajectory/root":
                self.send_json({"path": self.store.root_trajectory()})
                return
            if parsed.path.startswith("/api/node/"):
                suffix = parsed.path[len("/api/node/"):]
                wants_path = suffix.endswith("/path")
                if wants_path:
                    suffix = suffix[:-len("/path")]
                node_id = int(suffix)
                if wants_path:
                    self.send_json({"path": self.store.path(node_id)})
                else:
                    node = self.store.node(node_id)
                    node.update(self.store.navigation(node_id))
                    if node["rz_certificate"] and self.store.rzone:
                        node["rzone"] = self.store.rzone.certificate(
                            node["rz_certificate"], node["rz_transform"])
                    self.send_json(node)
                return
            if parsed.path.startswith("/api/rzone/"):
                if not self.store.rzone:
                    self.send_json({"error": "this index has no RZone snapshot"}, HTTPStatus.NOT_FOUND)
                    return
                suffix = parsed.path[len("/api/rzone/"):]
                wants_proof = suffix.endswith("/proof")
                if wants_proof:
                    suffix = suffix[:-len("/proof")]
                certificate_id = int(suffix)
                transform = unsigned(query.get("transform", ["0"])[0])
                value = self.store.rzone.proof(certificate_id, transform) if wants_proof else self.store.rzone.certificate(certificate_id, transform)
                self.send_json(value)
                return
            self.send_json({"error": "unknown API endpoint"}, HTTPStatus.NOT_FOUND)
        except (ValueError, KeyError) as error:
            self.send_json({"error": str(error)}, HTTPStatus.BAD_REQUEST)
        except Exception as error:
            self.send_json({"error": str(error)}, HTTPStatus.INTERNAL_SERVER_ERROR)


def serve(index: Path, host: str, port: int) -> None:
    store = NodeStore(index)
    ViewerHandler.store = store
    ViewerHandler.static_directory = Path(__file__).with_name("web")
    server = ThreadingHTTPServer((host, port), ViewerHandler)
    print(f"viewer index: {index}")
    print(f"open http://{host}:{port}/")
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        print("\nstopping viewer")
    finally:
        server.server_close()
        store.close()


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="command", required=True)
    index_parser = commands.add_parser(
        "index", help="portable reference indexer; use solver/tree_viewer_index for large SGFs")
    index_parser.add_argument("--sgf", required=True, type=Path)
    index_parser.add_argument("--rz", type=Path)
    index_parser.add_argument("--tt", type=Path)
    index_parser.add_argument("--output", required=True, type=Path)
    serve_parser = commands.add_parser("serve", help="serve one completed viewer index")
    serve_parser.add_argument("--index", required=True, type=Path)
    serve_parser.add_argument("--host", default="127.0.0.1")
    serve_parser.add_argument("--port", default=8080, type=int)
    arguments = parser.parse_args()
    if arguments.command == "index":
        build_index(arguments.sgf, arguments.output, arguments.rz, arguments.tt)
    else:
        serve(arguments.index, arguments.host, arguments.port)


if __name__ == "__main__":
    main()
