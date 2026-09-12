from frame_reader import FrameReader
import cv2

reader = FrameReader()
while True:
    frame = reader.get_frame()
    if frame is not None:
        cv2.imshow("ets2la capture", frame)
        cv2.waitKey(1)