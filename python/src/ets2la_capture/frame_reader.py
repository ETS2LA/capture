import ctypes.wintypes as wt
import numpy as np
import ctypes
import struct
import time


SHM_NAME = "Local\\ETS2LA_FrameShare"
EVENT_NAME = "Local\\ETS2LA_FrameReady"
MUTEX_NAME = "Local\\ETS2LA_FrameMutex"

HEADER_FMT = "<IIII QQ I"
HEADER_SIZE = struct.calcsize(HEADER_FMT)
FRAME_MAGIC = 0x45545332

MAX_WIDTH, MAX_HEIGHT = 7680, 2160
MAX_FRAME_BYTES = MAX_WIDTH * MAX_HEIGHT * 4
SHM_TOTAL_SIZE = HEADER_SIZE + MAX_FRAME_BYTES

kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenMutexW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
kernel32.OpenMutexW.restype = wt.HANDLE
kernel32.ReleaseMutex.argtypes = [wt.HANDLE]
kernel32.ReleaseMutex.restype = wt.BOOL
kernel32.OpenFileMappingW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
kernel32.OpenFileMappingW.restype = wt.HANDLE
kernel32.OpenEventW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
kernel32.OpenEventW.restype = wt.HANDLE
kernel32.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]
kernel32.MapViewOfFile.restype = ctypes.c_void_p
kernel32.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
kernel32.WaitForSingleObject.restype = wt.DWORD
kernel32.CloseHandle.argtypes = [wt.HANDLE]
kernel32.CloseHandle.restype = wt.BOOL
kernel32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
kernel32.UnmapViewOfFile.restype = wt.BOOL

FILE_MAP_ALL_ACCESS = 0xF001F
FILE_MAP_READ = 0x0004
EVENT_ALL_ACCESS = 0x1F0003
MUTEX_ALL_ACCESS = 0x1F0001
WAIT_OBJECT_0 = 0x0
WAIT_ABANDONED = 0x80
WAIT_TIMEOUT = 0x102

def _last_error(context: str) -> str:
    code = ctypes.get_last_error()
    return f"{context} (WinError {code}: {ctypes.FormatError(code)})"


class FrameReader:
    def __init__(self):
        self._map_handle = kernel32.OpenFileMappingW(FILE_MAP_ALL_ACCESS, False, SHM_NAME)
        if not self._map_handle:
            raise OSError(_last_error(
                "Could not open shared memory, is ets2la_capture.dll loaded?"
            ))
        self._event_handle = kernel32.OpenEventW(EVENT_ALL_ACCESS, False, EVENT_NAME)
        if not self._event_handle:
            raise OSError(_last_error("Could not open frame-ready event"))

        self._mutex_handle = kernel32.OpenMutexW(MUTEX_ALL_ACCESS, False, MUTEX_NAME)
        if not self._mutex_handle:
            raise OSError(_last_error("Could not open frame mutex"))

        addr = kernel32.MapViewOfFile(self._map_handle, FILE_MAP_READ, 0, 0, SHM_TOTAL_SIZE)
        if not addr:
            raise OSError(_last_error("MapViewOfFile failed"))
        self._view_addr = addr
        self._buf = (ctypes.c_ubyte * SHM_TOTAL_SIZE).from_address(addr)
        self._last_frame_index = 0

    def _read_locked(self):
        raw_header = bytes(self._buf[:HEADER_SIZE])
        magic, width, height, stride, frame_index, timestamp_qpc, fmt = struct.unpack(HEADER_FMT, raw_header)
        if magic != FRAME_MAGIC or width == 0 or height == 0:
            return frame_index, None

        raw = np.ctypeslib.as_array(self._buf, shape=(SHM_TOTAL_SIZE,))
        bgra = raw[HEADER_SIZE:HEADER_SIZE + stride * height].reshape(height, stride // 4, 4)
        frame = bgra[:, :width, :3].copy()
        return frame_index, frame

    def get_frame(self, timeout_ms=1000):
        """
        Blocks until a NEW frame (one this reader hasnt seen yet) is
        available, or timeout_ms elapses. Returns a BGR numpy array
        (H, W, 3), or None on timeout.
        """
        deadline = time.perf_counter() + timeout_ms / 1000.0
        while True:
            remaining_ms = max(0, int((deadline - time.perf_counter()) * 1000))
            result = kernel32.WaitForSingleObject(self._event_handle, remaining_ms if remaining_ms > 0 else 0)
            if result == WAIT_TIMEOUT:
                return None

            mutex_wait = kernel32.WaitForSingleObject(self._mutex_handle, 100)
            if mutex_wait not in (WAIT_OBJECT_0, WAIT_ABANDONED):
                return None

            try:
                frame_index, frame = self._read_locked()
            finally:
                kernel32.ReleaseMutex(self._mutex_handle)

            if frame is not None and frame_index != self._last_frame_index:
                self._last_frame_index = frame_index
                return frame

            if time.perf_counter() >= deadline:
                return None
            time.sleep(0)

    def get_latest_frame(self):
        """
        Non-blocking: returns whatever frame is currently available right
        now (even if this reader already returned it before), or None if no
        frame has ever been published.
        """
        mutex_wait = kernel32.WaitForSingleObject(self._mutex_handle, 50)
        if mutex_wait not in (WAIT_OBJECT_0, WAIT_ABANDONED):
            return None
        try:
            frame_index, frame = self._read_locked()
        finally:
            kernel32.ReleaseMutex(self._mutex_handle)
        if frame is not None:
            self._last_frame_index = frame_index
        return frame

    def __del__(self):
        try:
            kernel32.UnmapViewOfFile(self._view_addr)
            kernel32.CloseHandle(self._map_handle)
            kernel32.CloseHandle(self._event_handle)
            kernel32.CloseHandle(self._mutex_handle)
        except Exception:
            pass