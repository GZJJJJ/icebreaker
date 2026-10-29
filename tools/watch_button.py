import sys
import time
import serial

port = "COM3"
deadline = time.time() + 45
ser = None
while time.time() < deadline:
    try:
        ser = serial.Serial(port, 115200, timeout=0.3)
        break
    except Exception:
        time.sleep(0.3)

if ser is None:
    print("PORT_OPEN_FAILED")
    sys.exit(1)

start = time.time()
while time.time() - start < 45:
    data = ser.read(4096)
    if data:
        sys.stdout.write(data.decode("utf-8", "replace"))
        sys.stdout.flush()
ser.close()
print("\n---END---")
