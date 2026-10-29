import re
import struct

data = open('flash_dump.bin', 'rb').read()

def hexdump(off, n=128):
    for i in range(0, n, 16):
        chunk = data[off+i:off+i+16]
        hexs = ' '.join(f'{b:02x}' for b in chunk)
        asc = re.sub(rb'[^\x20-\x7e]', b'.', chunk).decode()
        print(f'  {off+i:07x}  {hexs}  {asc}')

print('=== hexdump 0x8000 (partition table region) ===')
hexdump(0x8000, 160)
print('=== hexdump 0x70000 (xiaozhi app start) ===')
hexdump(0x70000, 64)
print('=== hexdump 0x10000 ===')
hexdump(0x10000, 64)

print('=== app image headers (0xE9 @ 4K-aligned, valid segs) ===')
for off in range(0, len(data), 0x1000):
    if data[off] == 0xE9 and 1 <= data[off+1] <= 16:
        entry = struct.unpack('<I', data[off+12:off+16])[0]
        if entry != 0:
            print(f'  @0x{off:07x} segs={data[off+1]} entry=0x{entry:08x}')

print('=== strings 0x8000-0x9000 ===')
for m in re.finditer(rb'[\x20-\x7e]{6,60}', data[0x8000:0x9000]):
    print('  ', m.group().decode())
