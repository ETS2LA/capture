# ets2la_capture

Lock-free reader for the `ets2la_capture.dll` shared-memory.

## Installation

```powershell
pip install ./python
```

## Usage

```python
from ets2la_capture import FrameReader

reader = FrameReader(wait_for_capture=True)   # may start before the game
frame = reader.get_frame(timeout_ms=1000)     # next new frame, or None
img = frame.pixels                            # (H, W, 4) uint8 in native order (frame.layout)
bgr = frame.to_bgr()                          # contiguous BGR for OpenCV
```

* `get_frame(copy=False)` returns a zero-copy view into shared memory. Check `frame.is_valid()` after use, if it is `False` the writer lapped you, discard.
* `get_latest_frame()` is non-blocking.
* `frame.age_ms()` is the time since the game presented the frame.
* `reader.stats()` shows published / dropped frames.