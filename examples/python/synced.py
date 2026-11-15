from ets2la_capture import SyncedReader
import cv2

reader = SyncedReader()

while True:
    synced = reader.get()
    if synced is None or not synced.telemetry.valid:
        continue

    speed = synced.telemetry.truck.speed * 3.6
    image = synced.frame.to_bgr()
    cv2.putText(image, f"{speed:.0f} km/h", (10, 30), cv2.FONT_HERSHEY_SIMPLEX, 1.0, (0, 255, 0), 2)
    cv2.imshow("ets2la_capture synced", image)
    cv2.waitKey(1)