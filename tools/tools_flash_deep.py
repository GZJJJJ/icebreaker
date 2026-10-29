import re
import struct

data = open('flash_dump.bin', 'rb').read()

# 1. partition table @ 0x8000: 32-byte entries, magic 0x50AA
print('=== partition table ===')
off = 0x8000
while True:
    e = data[off:off+32]
    if e[:2] != b'\xaaP':
        break
    magic, ptype, sub, offset_, size = struct.unpack('<HBBII', e[:12])
    name = e[12:28].split(b'\x00')[0].decode()
    flags = struct.unpack('<I', e[28:32])[0]
    tmap = {0: 'app', 1: 'data'}
    print(f'  {name:<12} type={tmap.get(ptype, ptype):<4} sub={sub:<3} off=0x{offset_:07x} size=0x{size:07x} flags={flags:#x}')
    off += 32

# 2. IDF app descriptors anywhere in flash
print('=== IDF app descriptors ===')
for m in re.finditer(re.escape(bytes.fromhex('3254CDAB')), data):
    o = m.start()
    d = data[o:o+112]
    version = d[16:48].split(b'\x00')[0].decode(errors='replace')
    project = d[48:80].split(b'\x00')[0].decode(errors='replace')
    idf_ver = d[96:112].decode(errors='replace').split('\x00')[0]
    print(f'  @0x{o:07x} project={project!r} version={version!r} idf={idf_ver!r}')

# 3. MCUboot images: header magic 0x96f3b83d? MCUboot v1: magic f396' — image header first 4 bytes 0x96f3b83a? Let's scan known 'MCUboot' strings instead
print('=== MCUboot markers ===')
cnt = 0
for m in re.finditer(rb'MCUboot|mcuboot', data):
    s = data[max(0, m.start()-32):m.start()+48]
    if cnt < 6:
        print('  @0x%07x' % m.start(), re.sub(rb'[^\x20-\x7e]', b'.', s).decode(errors='replace'))
    cnt += 1
print('  total:', cnt)

# 4. distinctive strings across FULL flash
print('=== distinctive strings (full 16MB) ===')
pats = {
    'icebreaker': r'icebreaker[^\x00]{0,80}',
    'jsonl': r'[a-z-]*jsonl[^\x00]{0,40}',
    'dnesp': r'dnesp[^\x00]{0,60}|DNESP[^\x00]{0,60}',
    'box variant': r'[\w-]*box[023]?[\w-]*',
    'ssid/wifi': r'(?:ssid|SSID|wifi_ssid)[^\x00]{0,40}',
    'atk': r'atk[^\x00]{0,60}|ATK[^\x00]{0,60}',
    'screen/lcd': r'(?:st7789|ili9341|ST7789|ILI9341|lcd|LCD)[^\x00]{0,40}',
    'audio': r'(?:es8311|es7210|ES8311|ES7210|pa_mute|PA_MUTE)[^\x00]{0,40}',
}
for label, pat in pats.items():
    ss = set()
    for m in re.finditer(pat.encode(), data):
        ss.add(m.group().decode(errors='replace'))
    ss = sorted(ss)[:10]
    print(f'--- {label} ({len(ss)}):')
    for s in ss:
        print('   ', s)
