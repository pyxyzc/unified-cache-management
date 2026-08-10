#!/usr/bin/env python3
"""Extract and summarize Delegator LOAD timing logs in place.

Usage:
    python3 scripts/summarize_delegator_load_log.py 64-buffer-64-stream.log

The input file is overwritten with a compact log containing the useful LOAD
events and a timing summary. The original file is left untouched when no
LOAD d2d_timing samples are found.
"""

from __future__ import annotations

import argparse
import re
import statistics
from pathlib import Path


CONFIG_RE = re.compile(
    r"(?P<buffer>\d+)-buffer-(?P<stream>\d+)-stream", re.IGNORECASE
)
INT_RE_TEMPLATE = r"\b{key}=(\d+)"
FLOAT_RE_TEMPLATE = r"\b{key}=([0-9]+(?:\.[0-9]+)?)ms"


def parse_int(line: str, key: str) -> int | None:
    match = re.search(INT_RE_TEMPLATE.format(key=re.escape(key)), line)
    return int(match.group(1)) if match else None


def parse_ms(line: str, key: str) -> float | None:
    match = re.search(FLOAT_RE_TEMPLATE.format(key=re.escape(key)), line)
    return float(match.group(1)) if match else None


def metric_stats(values: list[float]) -> str:
    if not values:
        return "n/a"
    return (
        f"min={min(values):.3f}ms, max={max(values):.3f}ms, "
        f"avg={statistics.fmean(values):.3f}ms"
    )


def int_stats(values: list[int]) -> str:
    if not values:
        return "n/a"
    return (
        f"min={min(values)}, max={max(values)}, "
        f"avg={statistics.fmean(values):.1f}"
    )


def selected_source_lines(lines: list[str]) -> list[str]:
    selected = []
    for line in lines:
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if "request_id:" in line and "total_blocks_num:" in line:
            selected.append(stripped)
            continue
        if "Delegator LOAD" in line and "processing KVCache shards=" not in line:
            selected.append(stripped)
            # Keep one blank line between consecutive LOAD rounds, matching
            # the manually curated log format.
            if "stage=scatter_complete" in line:
                selected.append("")
    return selected


def summarize(path: Path) -> str:
    lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
    selected = selected_source_lines(lines)

    d2d_samples: list[dict[str, int | float | None]] = []
    scatter_values: list[float] = []
    sync_values: list[float] = []
    d2d_values: list[float] = []
    backend_values: list[float] = []
    stream_values: list[int] = []
    shard_values: list[int] = []
    group_values: list[int] = []

    for line in lines:
        if "Delegator LOAD" not in line:
            continue

        if "stage=d2d_timing" in line:
            scatter = parse_ms(line, "scatter_enqueue_duration")
            sync = parse_ms(line, "sync_all_duration")
            shards = parse_int(line, "shards")
            streams = parse_int(line, "stream_count")
            groups = parse_int(line, "groups")
            if scatter is not None:
                scatter_values.append(scatter)
            if sync is not None:
                sync_values.append(sync)
            if shards is not None:
                shard_values.append(shards)
            if streams is not None:
                stream_values.append(streams)
            if groups is not None:
                group_values.append(groups)
            d2d_samples.append(
                {
                    "scatter": scatter,
                    "sync": sync,
                    "shards": shards,
                    "streams": streams,
                    "groups": groups,
                }
            )

        if "stage=backend_complete" in line:
            backend = parse_ms(line, "backend_duration")
            if backend is not None:
                backend_values.append(backend)

        if "stage=scatter_complete" in line:
            d2d = parse_ms(line, "d2d_duration")
            if d2d is not None:
                d2d_values.append(d2d)

    if not d2d_samples:
        raise ValueError("no Delegator LOAD stage=d2d_timing samples found")

    config_match = CONFIG_RE.search(path.name)
    if config_match:
        buffer_number = config_match.group("buffer")
        stream_number = config_match.group("stream")
        config_line = (
            f"# Configuration inferred from filename: "
            f"delegator_buffer_number={buffer_number}, "
            f"delegator_stream_number={stream_number}"
        )
    else:
        config_line = (
            "# Configuration could not be inferred from filename; "
            "use stream_count in the samples."
        )

    scatter_avg = statistics.fmean(scatter_values) if scatter_values else None
    sync_avg = statistics.fmean(sync_values) if sync_values else None
    d2d_avg = statistics.fmean(d2d_values) if d2d_values else None
    measured_sum = None
    if scatter_avg is not None and sync_avg is not None:
        measured_sum = scatter_avg + sync_avg
    if measured_sum and d2d_avg:
        scatter_share = scatter_avg / measured_sum * 100.0
        sync_share = sync_avg / measured_sum * 100.0
        share_line = (
            f"# Average enqueue/sync share: scatter={scatter_share:.1f}%, "
            f"sync={sync_share:.1f}%"
        )
    else:
        share_line = "# Average enqueue/sync share: n/a"

    output: list[str] = [
        "# Delegator LOAD timing extract",
        f"# Source: {path.name}",
        config_line,
        "# The original input is replaced by this compact extract.",
        "# A 64-slot batch is only full when the workload provides at least 64 shards.",
        "",
        "## Selected source events",
    ]
    output.extend(selected)
    if not output or output[-1] != "":
        output.append("")
    output.extend(
        [
            "## Quick summary",
        f"# d2d timing samples: {len(d2d_samples)}",
        f"# shards per batch: {int_stats(shard_values)}",
        f"# groups per batch: {int_stats(group_values)}",
        f"# stream_count: {int_stats(stream_values)}",
        f"# scatter_enqueue_duration: {metric_stats(scatter_values)}",
        f"# sync_all_duration: {metric_stats(sync_values)}",
        f"# d2d_duration: {metric_stats(d2d_values)}",
        f"# backend_duration: {metric_stats(backend_values)}",
        share_line,
        ]
    )
    return "\n".join(output) + "\n"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Extract Delegator LOAD timings and overwrite the log in place."
    )
    parser.add_argument("log_file", type=Path)
    args = parser.parse_args()

    if not args.log_file.is_file():
        parser.error(f"log file does not exist: {args.log_file}")

    try:
        compact_log = summarize(args.log_file)
    except (OSError, UnicodeError, ValueError) as error:
        parser.error(str(error))

    args.log_file.write_text(compact_log, encoding="utf-8")
    print(f"Updated {args.log_file} in place")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
