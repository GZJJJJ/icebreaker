import re
from pathlib import Path

data = (Path(__file__).resolve().parents[1] / 'archive/local/flash_dump.bin').read_bytes()

# ESP app descriptor magic 0xABCD5432 (little endian)
magic = bytes.fromhex('3254CDAB')
for m in re.finditer(re.escape(magic), data):
    off = m.start()
    d = data[off:off+112]
    # layout: magic(4) secure_version(4) reserv1(8) version[32] project_name[32] time[16] date[16] idf_ver[32]...
    version = d[16:48].split(b'\x00')[0].decode(errors='replace')
    project = d[48:80].split(b'\x00')[0].decode(errors='replace')
    idf_ver = d[96:112].split(b'\x00')[0].decode(errors='replace')
    print(f'app_desc @ 0x{off:07x}: project={project!r} version={version!r} idf={idf_ver!r}')

# interesting strings
pats = ['atk', 'dnesp', 'BOX', 'box', 'xiaozhi', 'ALIENTEK', 'alientek', 'openvela', 'NuttX']
found = {}
for m in re.finditer(rb'[\x20-\x7e]{6,64}', data[:0x400000]):
    s = m.group().decode()
    for p in pats:
        if p in s:
            found.setdefault(p, set()).add(s)
            break
for p, ss in found.items():
    print(f'--- {p} ({len(ss)} unique, first 12):')
    for s in sorted(ss)[:12]:
        print('   ', s)
