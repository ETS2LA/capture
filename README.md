# ets2la_capture

In-process frame capture for Euro Truck Simulator 2 / American Truck Simulator.
It hooks the game's own DirectX 11 `Present` call and publishes the frames the
game renders to any number of C++ / Python clients through shared memory.

## Build

Requires CMake 3.15+ and MSVC (Visual Studio 2019+).
The headers of the `ets2la_telemetry` repo are needed too (synced mode, see below),
by default it is expected next to this repo as `../telemetry`, otherwise pass `-DETS2LA_TELEMETRY_DIR=<path>`.
[MinHook](https://github.com/TsudaKageyu/minhook) is fetched automatically
at configure time.

```powershell
cmake -B build -A x64 -DETS2LA_TELEMETRY_DIR=../telemetry
cmake --build build --config Release
```

Outputs `ets2la_capture.dll` (put it in `<game>/bin/win_x64/plugins/`)

Python client: `pip install ./python` (see `python/README.md`).

## Reading frames

```python
from ets2la_capture import FrameReader

reader = FrameReader()
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

## Synced mode (frame + telemetry of the same game frame)

Every frame published by this DLL also carries a snapshot of `ets2la_telemetry` taken at the same `Present()`.
Install both plugins (`ets2la_capture.dll` and `ets2la_telemetry.dll`) in `plugins/`.

```python
from ets2la_capture import SyncedReader  # pip install ./python[synced]

reader = SyncedReader()
synced = reader.get()
synced.frame.to_bgr()
synced.telemetry.truck.speed  # the same Telemetry type TelemetryReader returns
```

```cpp
#include "ets2la_capture/synced_reader.h"  // ets2la_telemetry/include must be in the include path

ets2la_capture::SyncedReader reader;
ets2la_capture::SyncedFrame synced;
if (reader.get(synced)) {}
```

* `SyncedReader` offers the same calls as `FrameReader` (`get`, `get_latest`, zero-copy views with `copy=False`).
* `telemetry.valid` is `False` when no snapshot was available (telemetry plugin missing or not running).