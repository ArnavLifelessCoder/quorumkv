"""Capture all native thread stacks in a Windows minidump without resuming the target."""

import ctypes
import pathlib
import sys
from ctypes import wintypes


def main() -> None:
    import msvcrt

    pid = int(sys.argv[1])
    output = pathlib.Path(sys.argv[2]).with_suffix(".dmp")
    kernel = ctypes.WinDLL("kernel32", use_last_error=True)
    dbghelp = ctypes.WinDLL("dbghelp", use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    kernel.CloseHandle.restype = wintypes.BOOL
    dbghelp.MiniDumpWriteDump.argtypes = [
        wintypes.HANDLE,
        wintypes.DWORD,
        wintypes.HANDLE,
        wintypes.DWORD,
        ctypes.c_void_p,
        ctypes.c_void_p,
        ctypes.c_void_p,
    ]
    dbghelp.MiniDumpWriteDump.restype = wintypes.BOOL
    process = kernel.OpenProcess(0x0410, False, pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        with output.open("wb") as file:
            handle = msvcrt.get_osfhandle(file.fileno())
            if not dbghelp.MiniDumpWriteDump(process, pid, handle, 0, None, None, None):
                raise ctypes.WinError(ctypes.get_last_error())
        print(f"[watchdog] all-thread stack minidump: {output}")
    finally:
        kernel.CloseHandle(process)


if __name__ == "__main__":
    main()
