"""Opt-in memory samples for the Python preview."""

import json
import os
from pathlib import Path
import sys
import tempfile
import threading
import time
import tracemalloc


if sys.platform == "darwin":
    import ctypes

    class _MachTime(ctypes.Structure):
        _fields_ = [("seconds", ctypes.c_uint32),
                    ("microseconds", ctypes.c_uint32)]

    class _MachTaskBasicInfo(ctypes.Structure):
        _fields_ = [
            ("virtual_size", ctypes.c_uint64),
            ("resident_size", ctypes.c_uint64),
            ("resident_size_max", ctypes.c_uint64),
            ("user_time", _MachTime),
            ("system_time", _MachTime),
            ("policy", ctypes.c_int32),
            ("suspend_count", ctypes.c_int32),
        ]

    _libsystem = ctypes.CDLL("/usr/lib/libSystem.B.dylib")
    _mach_task_self = ctypes.c_uint.in_dll(_libsystem, "mach_task_self_").value
    _libsystem.task_info.argtypes = [ctypes.c_uint, ctypes.c_int,
                                     ctypes.c_void_p,
                                     ctypes.POINTER(ctypes.c_uint)]
    _libsystem.task_info.restype = ctypes.c_int


def _resident_bytes():
    if sys.platform == "darwin":
        info = _MachTaskBasicInfo()
        count = ctypes.c_uint(ctypes.sizeof(info) // ctypes.sizeof(ctypes.c_uint))
        if _libsystem.task_info(
                _mach_task_self, 20, ctypes.byref(info),
                ctypes.byref(count)) == 0:
            return info.resident_size
        return None
    if sys.platform.startswith("linux"):
        try:
            with open("/proc/self/statm", encoding="ascii") as statm:
                return int(statm.read().split()[1]) * os.sysconf("SC_PAGE_SIZE")
        except (OSError, ValueError, IndexError):
            return None
    return None


class MemoryLog:
    def __init__(self, path):
        self.path = Path(path).expanduser()
        self.path.parent.mkdir(parents=True, exist_ok=True)
        self._lock = threading.Lock()
        self._start = time.monotonic()
        tracemalloc.start()
        self.record("logging_started")

    def record(self, event, **details):
        with self._lock:
            python_bytes, python_peak_bytes = tracemalloc.get_traced_memory()
            sample = {
                "elapsed_s": round(time.monotonic() - self._start, 3),
                "event": event,
                "rss_bytes": _resident_bytes(),
                "python_bytes": python_bytes,
                "python_peak_bytes": python_peak_bytes,
                "thread": threading.current_thread().name,
            }
            sample.update(details)
            with self.path.open("a", encoding="utf-8") as output:
                output.write(json.dumps(sample, sort_keys=True) + "\n")


def create_memory_log():
    value = os.environ.get("FILMVIZ_PYTHON_MEMORY_LOG")
    if not value:
        return None
    path = (
        Path(tempfile.gettempdir()) / f"filmviz-python-memory-{os.getpid()}.jsonl"
        if value == "1" else Path(value))
    log = MemoryLog(path)
    print(f"FilmViz Python memory log: {log.path}", file=sys.stderr)
    return log
