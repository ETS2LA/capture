from __future__ import annotations

import ctypes
import struct
import time
from typing import Optional

import numpy as np

CONTROL_MAGIC = 0x32435445
DATA_MAGIC = 0x32445445
CONTROL_NAME = "Local\\ETS2LA_Capture_Control"
DATA_NAME_FMT = "Local\\ETS2LA_Capture_Data_{}"
EVENT_NAMES = ("Local\\ETS2LA_Capture_Frame0", "Local\\ETS2LA_Capture_Frame1")

CONTROL_BYTES = 4096
SLOT_HEADER_BYTES = 256
MAX_READERS = 32
READER_TIMEOUT_MS = 2000

CTL_MAGIC = 0
CTL_SLOT_COUNT = 4
CTL_QPC_FREQ = 12
CTL_WRITER_PID = 20
CTL_GENERATION = 64
CTL_CAPTURING = 68
CTL_LATEST = 72
CTL_PUBLISHED = 80
CTL_DROPPED = 88
CTL_LAST_PUBLISH_QPC = 96
CTL_READERS = 128
READER_STRIDE = 64

# DataHeader: magic, generation, slot_count, slot_stride, slots_offset, width, height, stride, format, bytes_per_pixel, pad, total_size
DATA_HEADER_FMT = "<IIIQQIIIIIIQ"

# SlotHeader: seq, frame_index, timestamp_qpc, publish_qpc
SLOT_FMT = "<QQQQ"

SYNCHRONIZE = 0x00100000
FILE_MAP_WRITE = 0x0002
FILE_MAP_READ = 0x0004

_LAYOUTS = {
    # dxgi_format: (name, bytes_per_pixel)
    87: "BGRA8", 90: "BGRA8", 91: "BGRA8",
    88: "BGRX8", 92: "BGRX8", 93: "BGRX8",
    27: "RGBA8", 28: "RGBA8", 29: "RGBA8",
    23: "RGB10A2", 24: "RGB10A2", 89: "RGB10A2",
    9: "RGBA16F", 10: "RGBA16F", 11: "RGBA16F",
}


def _load_kernel32():
    try:
        k = ctypes.WinDLL("kernel32", use_last_error=True)
    except (AttributeError, OSError):
        return None
    from ctypes import wintypes as wt

    k.OpenFileMappingW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
    k.OpenFileMappingW.restype = wt.HANDLE
    k.MapViewOfFile.argtypes = [wt.HANDLE, wt.DWORD, wt.DWORD, wt.DWORD, ctypes.c_size_t]
    k.MapViewOfFile.restype = ctypes.c_void_p
    k.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
    k.UnmapViewOfFile.restype = wt.BOOL
    k.OpenEventW.argtypes = [wt.DWORD, wt.BOOL, wt.LPCWSTR]
    k.OpenEventW.restype = wt.HANDLE
    k.WaitForSingleObject.argtypes = [wt.HANDLE, wt.DWORD]
    k.WaitForSingleObject.restype = wt.DWORD
    k.CloseHandle.argtypes = [wt.HANDLE]
    k.CloseHandle.restype = wt.BOOL
    k.GetTickCount64.argtypes = []
    k.GetTickCount64.restype = ctypes.c_uint64
    k.QueryPerformanceCounter.argtypes = [ctypes.POINTER(ctypes.c_int64)]
    k.QueryPerformanceCounter.restype = wt.BOOL
    k.GetCurrentProcessId.argtypes = []
    k.GetCurrentProcessId.restype = wt.DWORD
    return k


kernel32 = _load_kernel32()


class _Guard:
    def __init__(self, handle, addr):
        self.handle = handle
        self.addr = addr

    def __del__(self):
        try:
            if self.addr:
                kernel32.UnmapViewOfFile(self.addr)
            if self.handle:
                kernel32.CloseHandle(self.handle)
        except Exception:
            pass


class _DataMapping:
    def __init__(self, handle, addr, generation):
        self.guard = _Guard(handle, addr)
        self.addr = addr
        self.generation = generation
        head = (ctypes.c_ubyte * 4096).from_address(addr)
        (magic, gen, self.slot_count, self.slot_stride, self.slots_offset,
         self.width, self.height, self.stride, self.format, self.bytes_per_pixel,
         _pad, self.total_size) = struct.unpack_from(DATA_HEADER_FMT, head, 0)
        if magic != DATA_MAGIC or gen != generation or self.slot_count == 0:
            raise OSError("data mapping header invalid")
        self.buf = (ctypes.c_ubyte * self.total_size).from_address(addr)
        self.buf._guard = self.guard
        self.layout = _LAYOUTS.get(self.format, "unknown")


class Frame:
    """One captured frame.

    pixels        numpy array in the game's native format:
                    BGRA8/BGRX8/RGBA8 -> (H, W, 4) uint8
                    RGB10A2           -> (H, W)    uint32 (packed)
                    RGBA16F           -> (H, W, 4) float16
    layout        'BGRA8', 'RGBA8', ... (see `format` for the raw DXGI_FORMAT)
    frame_index   strictly increasing; gaps mean frames you skipped
    """

    __slots__ = ("pixels", "width", "height", "format", "layout", "frame_index",
                 "timestamp_qpc", "publish_qpc", "_check", "_reader")

    def is_valid(self) -> bool:
        """Always True for copies. For copy=False frames: False if the writer
        overwrote the frame while you were reading it (discard the result)."""
        return True if self._check is None else self._check()

    def age_ms(self) -> float:
        """Milliseconds since the game presented this frame."""
        return self._reader.ticks_to_ms(self._reader.qpc_now() - self.timestamp_qpc)

    def to_bgr(self) -> np.ndarray:
        """Contiguous (H, W, 3) uint8 BGR image (8-bit formats only)."""
        return self._convert(rgb=False)

    def to_rgb(self) -> np.ndarray:
        """Contiguous (H, W, 3) uint8 RGB image (8-bit formats only)."""
        return self._convert(rgb=True)

    def _convert(self, rgb: bool) -> np.ndarray:
        if self.layout not in ("BGRA8", "BGRX8", "RGBA8"):
            raise NotImplementedError(f"cannot convert layout {self.layout} to 8-bit RGB/BGR")
        src_is_bgr = self.layout in ("BGRA8", "BGRX8")
        try:
            import cv2

            if src_is_bgr:
                code = cv2.COLOR_BGRA2RGB if rgb else cv2.COLOR_BGRA2BGR
            else:
                code = cv2.COLOR_RGBA2RGB if rgb else cv2.COLOR_RGBA2BGR
            return cv2.cvtColor(self.pixels, code)
        except ImportError:
            pass
        if src_is_bgr == (not rgb):
            return np.ascontiguousarray(self.pixels[:, :, :3])
        return np.ascontiguousarray(self.pixels[:, :, 2::-1])


class FrameReader:
    """Lock-free reader. See module docstring.

    wait_for_capture=False (default): raise OSError if the capture DLL is not
    running. wait_for_capture=True: never raise; get_frame() returns None until
    the game (and DLL) come up, then connects automatically.
    """

    def __init__(self, wait_for_capture: bool = False):
        if kernel32 is None:
            raise OSError("ets2la_capture only works on Windows")
        self._wait_for_capture = wait_for_capture
        self._qpc = ctypes.c_int64()
        self._ctl_handle = None
        self._ctl_addr = 0
        self._events = [None, None]
        self._map: Optional[_DataMapping] = None
        self._entry_addr = 0
        self._my_id = 0
        self._last_index = 0
        self._freq = 0
        self._last_reconnect = 0.0
        self.last_error = ""
        if not self._connect() and not wait_for_capture:
            raise OSError(self.last_error)

    @property
    def connected(self) -> bool:
        return self._ctl_addr != 0

    def get_frame(self, timeout_ms: int = 1000, copy: bool = True) -> Optional[Frame]:
        """Block until a frame NEWER than the last one returned exists.

        Returns None on timeout. copy=False returns a zero-copy view into
        shared memory (check frame.is_valid() after using it).
        """
        return self._acquire(True, timeout_ms, copy)

    def get_latest_frame(self, copy: bool = True) -> Optional[Frame]:
        """Non-blocking: the newest frame right now (may repeat the last one)."""
        return self._acquire(False, 0, copy)

    def stats(self) -> dict:
        if not self.connected:
            return {}
        u64 = lambda off: ctypes.c_uint64.from_address(self._ctl_addr + off).value
        u32 = lambda off: ctypes.c_uint32.from_address(self._ctl_addr + off).value
        return {
            "latest_frame": u64(CTL_LATEST),
            "frames_published": u64(CTL_PUBLISHED),
            "frames_dropped_by_capture": u64(CTL_DROPPED),
            "generation": u32(CTL_GENERATION),
            "capturing": bool(u32(CTL_CAPTURING)),
            "writer_pid": u32(CTL_WRITER_PID),
        }

    def qpc_now(self) -> int:
        kernel32.QueryPerformanceCounter(ctypes.byref(self._qpc))
        return self._qpc.value

    def ticks_to_ms(self, ticks: int) -> float:
        return ticks * 1000.0 / self._freq if self._freq else 0.0

    def close(self):
        self._unregister()
        self._map = None
        if self._ctl_addr:
            kernel32.UnmapViewOfFile(self._ctl_addr)
            self._ctl_addr = 0
        if self._ctl_handle:
            kernel32.CloseHandle(self._ctl_handle)
            self._ctl_handle = None
        for i, e in enumerate(self._events):
            if e:
                kernel32.CloseHandle(e)
                self._events[i] = None

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def __del__(self):
        try:
            self.close()
        except Exception:
            pass

    def _fail(self, msg: str) -> bool:
        self.last_error = msg
        return False

    def _connect(self) -> bool:
        self.close()
        h = kernel32.OpenFileMappingW(FILE_MAP_READ | FILE_MAP_WRITE, False, CONTROL_NAME)
        if not h:
            return self._fail("capture not running (OpenFileMapping failed). Is ets2la_capture.dll loaded?")
        addr = kernel32.MapViewOfFile(h, FILE_MAP_READ | FILE_MAP_WRITE, 0, 0, CONTROL_BYTES)
        if not addr:
            kernel32.CloseHandle(h)
            return self._fail("MapViewOfFile(control) failed")
        magic = ctypes.c_uint32.from_address(addr + CTL_MAGIC).value
        if magic != CONTROL_MAGIC:
            kernel32.UnmapViewOfFile(addr)
            kernel32.CloseHandle(h)
            return self._fail("capture DLL not ready")
        events = [kernel32.OpenEventW(SYNCHRONIZE, False, n) for n in EVENT_NAMES]
        if not all(events):
            for e in events:
                if e:
                    kernel32.CloseHandle(e)
            kernel32.UnmapViewOfFile(addr)
            kernel32.CloseHandle(h)
            return self._fail("OpenEvent failed")

        self._ctl_handle, self._ctl_addr, self._events = h, addr, events
        self._c_latest = ctypes.c_uint64.from_address(addr + CTL_LATEST)
        self._c_gen = ctypes.c_uint32.from_address(addr + CTL_GENERATION)
        self._freq = ctypes.c_uint64.from_address(addr + CTL_QPC_FREQ).value
        self._register()
        self._last_index = self._c_latest.value
        self.last_error = ""
        return True

    _counter = 0

    def _register(self):
        FrameReader._counter += 1
        pid = kernel32.GetCurrentProcessId()
        self._my_id = (pid << 32) | (FrameReader._counter & 0xFFFFFFFF)
        now = kernel32.GetTickCount64()
        start = pid % MAX_READERS
        chosen = start
        for i in range(MAX_READERS):
            idx = (start + i) % MAX_READERS
            base = self._ctl_addr + CTL_READERS + idx * READER_STRIDE
            rid = ctypes.c_uint64.from_address(base).value
            hb = ctypes.c_uint64.from_address(base + 8).value
            if rid == 0 or now - hb > READER_TIMEOUT_MS * 2:
                chosen = idx
                break
        self._entry_addr = self._ctl_addr + CTL_READERS + chosen * READER_STRIDE
        self._c_hb_id = ctypes.c_uint64.from_address(self._entry_addr)
        self._c_hb_ms = ctypes.c_uint64.from_address(self._entry_addr + 8)
        self._heartbeat()

    def _heartbeat(self):
        if self._entry_addr:
            self._c_hb_id.value = self._my_id
            self._c_hb_ms.value = kernel32.GetTickCount64()

    def _unregister(self):
        if self._entry_addr and self._ctl_addr and self._c_hb_id.value == self._my_id:
            self._c_hb_ms.value = 0
            self._c_hb_id.value = 0
        self._entry_addr = 0

    def _ensure_mapping(self, generation: int) -> Optional[_DataMapping]:
        m = self._map
        if m is not None and m.generation == generation:
            return m
        h = kernel32.OpenFileMappingW(FILE_MAP_READ, False, DATA_NAME_FMT.format(generation))
        if not h:
            return None
        addr = kernel32.MapViewOfFile(h, FILE_MAP_READ, 0, 0, 0)
        if not addr:
            kernel32.CloseHandle(h)
            return None
        try:
            m = _DataMapping(h, addr, generation)
        except OSError:
            _Guard(h, addr)
            return None
        self._map = m
        return m

    def _snapshot(self, min_exclusive: int):
        for _ in range(16):
            latest = self._c_latest.value
            if latest == 0 or latest <= min_exclusive:
                return None
            m = self._ensure_mapping(self._c_gen.value)
            if m is None:
                return None
            slot_off = m.slots_offset + (latest % m.slot_count) * m.slot_stride
            s1, idx, ts, pub = struct.unpack_from(SLOT_FMT, m.buf, slot_off)
            if (s1 & 1) or idx != latest:
                continue
            return m, slot_off, s1, idx, ts, pub
        return None

    def _wait_newer(self, deadline: float) -> bool:
        ev = self._events[(self._last_index + 1) & 1]
        while True:
            if self._c_latest.value > self._last_index:
                return True
            remaining = deadline - time.perf_counter()
            if remaining <= 0:
                return False
            kernel32.WaitForSingleObject(ev, max(1, min(5, int(remaining * 1000 + 0.999))))
            self._heartbeat()

    def _make_frame(self, m, slot_off, s1, idx, ts, pub, copy):
        off = slot_off + SLOT_HEADER_BYTES
        h, w = m.height, m.width
        if m.layout == "RGB10A2":
            pix = np.frombuffer(m.buf, np.uint32, h * w, off).reshape(h, w)
        elif m.layout == "RGBA16F":
            pix = np.frombuffer(m.buf, np.float16, h * w * 4, off).reshape(h, w, 4)
        else:
            pix = np.frombuffer(m.buf, np.uint8, h * w * m.bytes_per_pixel, off).reshape(h, w, m.bytes_per_pixel)
        seq_view = ctypes.c_uint64.from_address(m.addr + slot_off)

        def still_valid():
            return seq_view.value == s1

        if copy:
            pix = pix.copy()
            if not still_valid():
                return None
        else:
            pix.flags.writeable = False
        f = Frame()
        f.pixels = pix
        f.width, f.height, f.format, f.layout = w, h, m.format, m.layout
        f.frame_index, f.timestamp_qpc, f.publish_qpc = idx, ts, pub
        f._check = None if copy else still_valid
        f._reader = self
        return f

    def _acquire(self, only_new: bool, timeout_ms: int, copy: bool) -> Optional[Frame]:
        if not self.connected:
            now = time.perf_counter()
            if now - self._last_reconnect >= 0.25:
                self._last_reconnect = now
                self._connect()
            if not self.connected:
                time.sleep(min(timeout_ms, 50) / 1000.0)
                return None
        self._heartbeat()
        deadline = time.perf_counter() + (timeout_ms if only_new else 5) / 1000.0
        while True:
            if only_new and not self._wait_newer(deadline):
                return None
            snap = self._snapshot(self._last_index if only_new else 0)
            if snap is not None:
                frame = self._make_frame(*snap, copy)
                if frame is not None:
                    self._last_index = frame.frame_index
                    return frame
            elif not only_new:
                return None
            if time.perf_counter() >= deadline:
                return None
            time.sleep(0)