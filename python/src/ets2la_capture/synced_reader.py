from __future__ import annotations

import ctypes
from typing import Optional

from ets2la_telemetry import Telemetry, TelemetryData

from .frame_reader import (
    SYNC_TELEMETRY_REPEATED,
    SYNC_TELEMETRY_SKIPPED,
    SYNC_TELEMETRY_STALE,
    SYNC_TELEMETRY_VALID,
    Frame,
    FrameReader,
)

TELEMETRY_SIZE = ctypes.sizeof(TelemetryData)


class SyncedFrame:
    """One captured frame together with the telemetry snapshot of the same game frame."""

    __slots__ = ("frame", "telemetry", "flags")

    frame: Frame
    telemetry: Telemetry
    flags: int

    @property
    def telemetry_repeated(self) -> bool:
        """Same snapshot as the previous delivered frame"""
        return bool(self.flags & SYNC_TELEMETRY_REPEATED)

    @property
    def telemetry_skipped(self) -> bool:
        """Snapshots were skipped since the previous delivered frame"""
        return bool(self.flags & SYNC_TELEMETRY_SKIPPED)

    @property
    def telemetry_stale(self) -> bool:
        """The newest snapshot was older than kTelemetryStaleMs"""
        return bool(self.flags & SYNC_TELEMETRY_STALE)

    def is_valid(self) -> bool:
        """Always True for copies. For copy=False results: False if the writer overwrote the slot while you were reading it (discard the result)."""
        return self.frame.is_valid()

    def age_ms(self) -> float:
        """Milliseconds since the game presented this frame."""
        return self.frame.age_ms()


class SyncedReader(FrameReader):
    """Lock-free reader that returns every frame together with the telemetry of the same game frame.

    Needs ets2la_capture.dll and ets2la_telemetry.dll.
    """

    def get(self, timeout_ms: int = 1000, copy: bool = True) -> Optional[SyncedFrame]:
        """Block until a frame newer than the last one returned exists.

        Returns None on timeout. copy=False returns zero-copy views into
        shared memory (check synced.is_valid() after using them).
        """
        return self._wrap(self._acquire(True, timeout_ms, copy))

    def get_latest(self, copy: bool = True) -> Optional[SyncedFrame]:
        """Non-blocking: the newest frame right now (may repeat the last one)."""
        return self._wrap(self._acquire(False, 0, copy))

    def _make_frame(self, m, slot_off, s1, idx, ts, pub, sync, copy):
        frame = super()._make_frame(m, slot_off, s1, idx, ts, pub, sync, copy)
        if frame is None:
            return None
        frame._reader = self
        telemetry = Telemetry()
        if (frame.sync_flags & SYNC_TELEMETRY_VALID) and m.telemetry_size:
            if m.telemetry_size != TELEMETRY_SIZE:
                self.last_error = "telemetry layout mismatch (install the same version of ets2la_telemetry as the DLL)"
            else:
                off = slot_off + m.telemetry_offset
                seq_view = ctypes.c_uint64.from_address(m.addr + slot_off)
                if copy:
                    telemetry = Telemetry.from_buffer_copy(m.buf, off)
                    if seq_view.value != s1:
                        return None
                else:
                    telemetry = Telemetry.from_buffer(m.buf, off)
                    telemetry._check = frame._check
                telemetry.frame_index = frame.telemetry_frame_index
                telemetry.timestamp_qpc = frame.telemetry_timestamp_qpc
                telemetry.publish_qpc = frame.telemetry_publish_qpc
                telemetry.valid = True
                telemetry._reader = self
        frame._telemetry = telemetry
        return frame

    def _wrap(self, frame: Optional[Frame]) -> Optional[SyncedFrame]:
        if frame is None:
            return None
        synced = SyncedFrame()
        synced.frame = frame
        synced.telemetry = frame._telemetry
        synced.flags = frame.sync_flags
        return synced