#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause

from __future__ import annotations

import argparse
import os
from collections import Counter, defaultdict
from pathlib import Path


def default_log() -> Path:
    override = os.environ.get("FILMVIZ_OFX_LOG_PATH")
    if override:
        return Path(override).expanduser()
    return Path.home() / "Library" / "Logs" / "FilmViz" / "filmviz_ofx.log"


def parse_line(line: str):
    fields = line.rstrip("\n").split()
    if len(fields) < 3:
        return None
    timestamp = fields[0]
    data = {}
    for field in fields[1:]:
        if "=" in field:
            key, value = field.split("=", 1)
            data[key] = value
    return timestamp, data, line.rstrip("\n")


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Summarize the FilmViz OFX timing/cache log.")
    parser.add_argument("--log", type=Path, default=default_log())
    parser.add_argument("--last", type=int, default=120,
                        help="number of recent timeline records to print")
    parser.add_argument("--timeline-only", action="store_true")
    args = parser.parse_args()

    log = args.log.expanduser()
    if not log.is_file():
        print(f"FilmViz OFX log not found: {log}")
        return 1

    parsed = []
    for line in log.read_text(encoding="utf-8", errors="replace").splitlines():
        item = parse_line(line)
        if item:
            parsed.append(item)

    recent = parsed[-max(0, args.last):] if args.last else parsed
    print(f"FilmViz OFX log: {log}")
    print(f"records: {len(parsed)}")
    print("\nRecent timeline:")
    for timestamp, data, raw in recent:
        event = data.get("event", "unknown")
        ms = data.get("elapsed_ms") or data.get("ms")
        result = data.get("result")
        cache = data.get("cache") or data.get("name")
        node = data.get("node")
        parts = [timestamp, event]
        if node:
            parts.append(f"node={node}")
        if result:
            parts.append(f"result={result}")
        if cache:
            parts.append(f"cache={cache}")
        if ms:
            parts.append(f"ms={ms}")
        if event == "render":
            for key in ("time", "backend", "backend_request", "negative", "exposure"):
                if key in data:
                    parts.append(f"{key}={data[key]}")
        print("  " + " ".join(parts))

    if args.timeline_only:
        return 0

    events = Counter()
    results = Counter()
    durations = defaultdict(list)

    for _, data, _ in parsed:
        event = data.get("event", "unknown")
        events[event] += 1
        if "result" in data:
            results[(event, data["result"])] += 1
        try:
            durations[event].append(float(data.get("elapsed_ms", data.get("ms", ""))))
        except (KeyError, ValueError):
            pass

    print("\nEvent counts:")
    for event, count in events.most_common():
        print(f"  {event:24s} {count:8d}")

    if results:
        print("\nResults:")
        for (event, result), count in sorted(results.items()):
            print(f"  {event:24s} {result:20s} {count:8d}")

    if durations:
        print("\nTiming summary (ms):")
        for event in sorted(durations):
            values = durations[event]
            if not values:
                continue
            print(
                f"  {event:24s} n={len(values):5d} "
                f"avg={sum(values)/len(values):9.3f} "
                f"max={max(values):9.3f}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
