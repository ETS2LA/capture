from ets2la_capture import FrameReader
import cv2

reader = FrameReader()

while True:
    frame = reader.get_frame()
    if frame is None:
        continue

    cv2.imshow("ets2la_capture", frame.to_bgr())
    cv2.waitKey(1)