# capture

Zero-copy, in-process frame capture for Euro Truck Simulator 2 / American
Truck Simulator. Hooks the game's own DirectX 11 `Present` call and copies
frames straight off the GPU into shared memory.

## Why this exists

Screen-capture APIs (GDI, even the modern Windows Graphics Capture API) add
a compositor round-trip of latency and CPU/GPU overhead you don't need when
you just want the frames the game itself is already rendering. This project
hooks `IDXGISwapChain::Present` directly inside the game process to read the
backbuffer before it's even sent to the compositor, and hands it to any
number of consumers (Python, C++, ...) over shared memory with no
serialization overhead.

## How it works

1. **Loading**: ETS2/ATS natively loads any DLL placed in
   `bin/win_x64/plugins/` via SCS's own telemetry SDK plugin mechanism. This
   project implements the minimal two exports (`scs_telemetry_init` /
   `scs_telemetry_shutdown`) needed to be accepted as a valid plugin.
2. **Hooking**: once loaded, it creates a throwaway D3D11 device purely to
   read the `IDXGISwapChain` vtable address for `Present` (this vtable is
   shared by every swapchain in the process, so hooking it here hooks the
   game's real swapchain too) and installs the hook with
   [MinHook](https://github.com/TsudaKageyu/minhook).
3. **Reading frames**: on every `Present()` call, the backbuffer is copied
   into a rotating pool of CPU-readable staging textures using
   `D3D11_MAP_FLAG_DO_NOT_WAIT`, so a slow readback skips a frame instead of
   ever stalling the game's render thread. The most recently *available*
   frame is then published into a shared-memory ring buffer that any number
   of reader processes can consume concurrently.

## Build

Requires CMake 3.15+ and either MSVC (Visual Studio 2019+) or MinGW-w64.
[MinHook](https://github.com/TsudaKageyu/minhook) is fetched automatically
at configure time.

```powershell
cd dll
cmake -B build -A x64
cmake --build build --config Release
```

Output: `build/Release/ets2la_capture.dll`

## Reading frames

Both readers support two modes:
- **`get_frame(timeout_ms)`** blocks until a genuinely new frame is
  published, or times out.
- **`get_latest_frame()`** non-blocking, returns whatever's currently
  available instantly.

Both are safe to call from multiple readers/processes at once.

### Python
```python
from frame_reader import FrameReader
import cv2

reader = FrameReader()
while True:
    frame = reader.get_frame(timeout_ms=1000)
    if frame is not None:
        cv2.imshow("ets2la capture", frame)
        if cv2.waitKey(1) == 27:
            break
```

### C++
```cpp
#include "frame_reader.h"
#include <opencv2/opencv.hpp>

ets2la::FrameReader reader;
ets2la::Frame frame;
if (reader.get_frame(frame, 1000)) {
    cv::Mat img(frame.height, frame.width, CV_8UC4, frame.data.data());
}
```