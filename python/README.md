# ets2la_capture

Python client for reading frames published by `ets2la_capture.dll`.

## Installation

```powershell
pip install .
```

## Usage

```python
from ets2la_capture import FrameReader

reader = FrameReader()
frame = reader.get_frame()
```

Frames are returned as BGR NumPy arrays with shape `(height, width, 3)`.