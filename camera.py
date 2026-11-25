"""Display 160x120 GRY2 frames from the STM32 OV7670 firmware."""
import argparse
import time
import cv2
import numpy as np
import serial

WIDTH, HEIGHT = 160, 120

def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", default="COM19")
    parser.add_argument("--baud", type=int, default=2000000)
    args = parser.parse_args()

    try:
        with serial.Serial(args.port, args.baud, timeout=5) as ser:
            time.sleep(3)
            ser.reset_input_buffer()
            print("Camera port open. Press Esc to exit.")
            while True:
                start = time.perf_counter()
                ser.write(b"c")

                # Ignore startup text, but recognize firmware errors.
                header = bytearray()
                deadline = time.monotonic() + 5
                while time.monotonic() < deadline:
                    byte = ser.read(1)
                    if not byte:
                        break
                    header.extend(byte)
                    if header.endswith((b"GRY2", b"ERR!")):
                        break
                    if len(header) > 1024:
                        del header[:-4]

                if header.endswith(b"ERR!"):
                    print("Firmware:", ser.readline().decode(errors="replace").strip())
                    continue
                if not header.endswith(b"GRY2"):
                    raise RuntimeError("No GRY2 frame header; check firmware, port and baud.")

                angle = ser.read(1)
                if len(angle) != 1 or angle[0] > 3:
                    raise RuntimeError("Invalid rotation byte.")
                rotation = angle[0]
                data = ser.read(WIDTH * HEIGHT)
                if len(data) != WIDTH * HEIGHT:
                    raise RuntimeError(f"Incomplete image: {len(data)} bytes")

                img = np.frombuffer(data, dtype=np.uint8).reshape(HEIGHT, WIDTH)
                if rotation:
                    img = cv2.rotate(img, {
                        1: cv2.ROTATE_90_CLOCKWISE,
                        2: cv2.ROTATE_180,
                        3: cv2.ROTATE_90_COUNTERCLOCKWISE,
                    }[rotation])
                img = cv2.resize(img, None, fx=4, fy=4)
                fps = 1 / (time.perf_counter() - start)
                for y, label in [(30, f"FPS: {fps:.1f}"),
                                 (60, f"Rotation: {rotation * 90} deg")]:
                    cv2.putText(img, label, (10, y), cv2.FONT_HERSHEY_SIMPLEX,
                                0.7, 0, 4)
                    cv2.putText(img, label, (10, y), cv2.FONT_HERSHEY_SIMPLEX,
                                0.7, 255, 1)
                cv2.imshow("OV7670 Live Camera", img)
                if cv2.waitKey(1) & 0xff == 27:
                    break
    except (serial.SerialException, RuntimeError) as exc:
        print(f"Camera error: {exc}")
    except KeyboardInterrupt:
        pass
    finally:
        cv2.destroyAllWindows()

if __name__ == "__main__":
    main()
