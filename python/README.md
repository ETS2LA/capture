# ets2la_capture

Lock-free reader for the `ets2la_capture.dll` shared-memory.

## Installation

```powershell
pip install ./python
```

## Usage

```python
from ets2la_capture import FrameReader

reader = FrameReader()
frame = reader.get_frame(timeout_ms=1000)     # next new frame, or None
img = frame.pixels                            # (H, W, 4) uint8 in native order (frame.layout)
bgr = frame.to_bgr()                          # contiguous BGR for OpenCV
```

* `get_frame(copy=False)` returns a zero-copy view into shared memory. Check `frame.is_valid()` after use, if it is `False` the writer lapped you, discard.
* `get_latest_frame()` is non-blocking.
* `frame.age_ms()` is the time since the game presented the frame.
* `reader.stats()` shows published / dropped frames.
* `reader.connected()` reports whether a capture DLL is currently attached.

## Synced mode

`SyncedReader` returns every frame together with the telemetry of the same game frame (needs `ets2la_telemetry.dll` and
`pip install ./python[synced]` with the `ets2la_telemetry` package installed from the telemetry repo):

```python
from ets2la_capture import SyncedReader

reader = SyncedReader()
synced = reader.get(timeout_ms=1000)  # next new frame, or None
image = synced.frame.to_bgr()
speed = synced.telemetry.truck.speed  # the same Telemetry type TelemetryReader returns
```

`get(copy=False)` returns zero-copy views, check `synced.is_valid()` after use (covers frame and telemetry).