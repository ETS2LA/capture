from ets2la_capture import FrameReader
import torch

reader = FrameReader(wait_for_capture=True)

while True:
    frame = reader.get_frame(timeout_ms=1000, copy=False)  # no copy on the CPU
    if frame is None:
        continue

    x = torch.from_numpy(frame.pixels).cuda(non_blocking=False)  # H x W x 4 uint8, native order
    if not frame.is_valid():  # writer lapped us: drop it
        continue

    # BGRA -> RGB float CHW on the GPU
    x = x[..., [2, 1, 0]] if frame.layout in ("BGRA8", "BGRX8") else x[..., :3]
    x = x.permute(2, 0, 1).float().div_(255)

    # ...