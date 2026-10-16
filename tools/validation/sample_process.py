#!/usr/bin/env python3
"""Sample CPU time and resident memory of an already-running macOS process."""

import argparse
import ctypes
import json
import os
import platform
import subprocess
import sys
import time
from datetime import datetime, timezone
from pathlib import Path


def proc_path(pid: int) -> str:
    """Return the executable path reported by macOS libproc."""
    if sys.platform != "darwin":
        raise RuntimeError("this sampler requires macOS")
    libsystem = ctypes.CDLL("/usr/lib/libSystem.B.dylib", use_errno=True)
    function = libsystem.proc_pidpath
    function.argtypes = [ctypes.c_int, ctypes.c_void_p, ctypes.c_uint32]
    function.restype = ctypes.c_int
    buffer = ctypes.create_string_buffer(4096)
    size = function(pid, buffer, len(buffer))
    if size <= 0:
        raise RuntimeError(f"cannot inspect PID {pid} (process exited or access denied)")
    return os.fsdecode(buffer.value)


def cpu_seconds(value: str) -> float:
    """Parse BSD ps cumulative CPU time: [[dd-]hh:]mm:ss[.fraction]."""
    days = 0
    if "-" in value:
        day_part, value = value.split("-", 1)
        days = int(day_part)
    fields = value.split(":")
    if len(fields) == 3:
        hours, minutes, seconds = fields
    elif len(fields) == 2:
        hours, minutes, seconds = 0, fields[0], fields[1]
    else:
        raise ValueError(f"unrecognized ps CPU time: {value}")
    return days * 86400 + int(hours) * 3600 + int(minutes) * 60 + float(seconds)


def process_sample(pid: int) -> tuple[float, int]:
    output = subprocess.check_output(
        ["/bin/ps", "-p", str(pid), "-o", "time=", "-o", "rss="],
        text=True,
        stderr=subprocess.DEVNULL,
    ).strip()
    fields = output.split()
    if len(fields) != 2:
        raise RuntimeError(f"PID {pid} is no longer available")
    return cpu_seconds(fields[0]), int(fields[1])


def host_metadata() -> dict[str, str]:
    model = subprocess.check_output(
        ["/usr/sbin/sysctl", "-n", "hw.model"], text=True
    ).strip()
    return {
        "os": f"macOS {platform.mac_ver()[0]}",
        "architecture": platform.machine(),
        "model": model,
    }


def emit(stream, record: dict) -> None:
    stream.write(json.dumps(record, separators=(",", ":")) + "\n")
    stream.flush()


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Sample an already-running TelaViva process; does not launch it."
    )
    parser.add_argument("pid", type=int, help="PID of the running TelaViva process")
    parser.add_argument(
        "--expected-executable",
        required=True,
        type=Path,
        help="exact executable path to verify against the target PID",
    )
    parser.add_argument(
        "--interval", type=float, default=1.0, help="sample interval in seconds (default: 1)"
    )
    parser.add_argument(
        "--duration", type=float, default=60.0, help="sample duration in seconds (default: 60)"
    )
    parser.add_argument("--output", type=Path, help="JSON Lines output file; default: stdout")
    args = parser.parse_args()

    if args.pid <= 1:
        parser.error("PID must be greater than 1")
    if args.interval < 0.1:
        parser.error("--interval must be at least 0.1 seconds")
    if args.duration <= 0:
        parser.error("--duration must be greater than 0")

    try:
        expected = str(args.expected_executable.resolve(strict=True))
        actual = proc_path(args.pid)
        if os.path.realpath(actual) != expected:
            raise RuntimeError(
                f"PID {args.pid} runs {actual!r}, expected exactly {expected!r}"
            )
        metadata = host_metadata()
        initial_cpu, _ = process_sample(args.pid)
    except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
        print(f"error: {error}", file=sys.stderr)
        return 2

    stream = args.output.open("w", encoding="utf-8") if args.output else sys.stdout
    try:
        emit(
            stream,
            {
                "type": "metadata",
                "pid": args.pid,
                "executable": actual,
                **metadata,
                "interval_seconds": args.interval,
                "duration_seconds": args.duration,
                "cpu_semantics": {
                    "cumulative_cpu_seconds": "ps total CPU time since process start",
                    "sampled_average_cpu_percent": "delta cumulative CPU / wall interval * 100; one fully used core is 100%",
                },
                "rss_unit": "bytes",
                "rss_source_unit": "ps kilobytes converted to bytes",
            },
        )
        start = time.monotonic()
        previous_wall = start
        previous_cpu = initial_cpu
        next_sample = start
        while True:
            next_sample += args.interval
            remaining = next_sample - time.monotonic()
            if remaining > 0:
                time.sleep(remaining)
            try:
                actual_now = proc_path(args.pid)
                if os.path.realpath(actual_now) != expected:
                    raise RuntimeError("PID executable changed during sampling")
                cumulative_cpu, rss_kb = process_sample(args.pid)
            except (OSError, RuntimeError, ValueError, subprocess.CalledProcessError) as error:
                print(f"error: {error}", file=sys.stderr)
                return 2
            now = time.monotonic()
            wall_interval = now - previous_wall
            emit(
                stream,
                {
                    "type": "sample",
                    "pid": args.pid,
                    "timestamp": datetime.now(timezone.utc).isoformat(),
                    "elapsed_seconds": now - start,
                    "cumulative_cpu_seconds": cumulative_cpu,
                    "sampled_average_cpu_percent": max(
                        0.0, (cumulative_cpu - previous_cpu) / wall_interval * 100.0
                    ),
                    "rss_bytes": rss_kb * 1024,
                },
            )
            previous_wall, previous_cpu = now, cumulative_cpu
            if now - start >= args.duration:
                break
    finally:
        if args.output:
            stream.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
