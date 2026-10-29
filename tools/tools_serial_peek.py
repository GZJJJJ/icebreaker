import serial
import time

ser = serial.Serial('COM4', 115200, timeout=1)
ser.dtr = False
ser.rts = False
end = time.time() + 12
n = 0
out = []
while time.time() < end:
    line = ser.readline()
    if line:
        try:
            out.append(line.decode(errors='replace').rstrip())
            n += 1
        except Exception:
            pass
ser.close()
print(f'--- {n} lines ---')
for l in out[-120:]:
    print(l)
