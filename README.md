# ets2la_capture

In-process frame capture for Euro Truck Simulator 2 / American Truck Simulator.
It hooks the game's own DirectX 11 `Present` call and publishes the frames the
game renders to any number of C++ / Python clients through shared memory.

## Build

Requires CMake 3.15+ and MSVC (Visual Studio 2019+).
[MinHook](https://github.com/TsudaKageyu/minhook) is fetched automatically
at configure time.

```powershell
cmake -B build -A x64
cmake --build build --config Release
```

Outputs `ets2la_capture.dll` (put it in `<game>/bin/win_x64/plugins/`)

Python client: `pip install ./python` (see `python/README.md`).

## Reading frames

```python
from ets2la_capture import FrameReader

reader = FrameReader(wait_for_capture=True)
frame = reader.get_frame()
bgr = frame.to_bgr()
```

```cpp
#include "ets2la_capture/frame_reader.h"

ets2la_capture::FrameReader reader;
ets2la_capture::Frame frame;
if (reader.get_frame(frame)) {}
```

* `get_frame` blocks for a frame newer than the last one this reader got.
  First call after connecting waits for a fresh frame.
* `get_latest_frame` is non-blocking.
* Zero-copy views stay valid for ~5 newer frames (ring of 6). Finish (or
  `.copy()`) within about 4 frame times, then check `valid()`.
* GPU clients: `examples/python/torch_gpu.py` uploads straight from shared memory.