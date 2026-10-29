import sys
import serial
import time

# reset chip into app mode
import esptool
try:
    esptool.main(['--port', 'COM4', 'run'])
except SystemExit:
    pass
except Exception as e:
    print('esptool run note:', e)

time.sleep(1.5)

probes = ['{}\n', '{"type":"ping"}\n', '{"cmd":"ping"}\n', 'ping\n', 'help\n']

ser = serial.Serial('COM4', 115200, timeout=0.3, write_timeout=2)
ser.dtr = False
ser.rts = False
time.sleep(1.0)
ser.reset_input_buffer()

for p in probes:
    try:
        ser.write(p.encode())
        print(f'>>> sent {p.strip()}', flush=True)
    except Exception as e:
        print(f'>>> WRITE FAILED on {p.strip()}: {e}', flush=True)
        break
    deadline = time.time() + 1.5
    acc = b''
    while time.time() < deadline:
        acc += ser.read(2048)
    if acc:
        print('    ' + acc.decode(errors='replace').replace('\n', '\n    ').rstrip(), flush=True)
    else:
        print('    (no response)', flush=True)

ser.close()
print('done', flush=True)
