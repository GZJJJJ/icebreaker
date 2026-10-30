import json
from pathlib import Path

d = json.loads((Path(__file__).resolve().parents[1] / 'archive/local/xz_release.json').read_text(encoding='utf-8'))
print('tag:', d['tag_name'], '| date:', d['published_at'])
for a in d['assets']:
    n = a['name'].lower()
    if 'box' in n or 'atk' in n:
        print(f"  {a['name']}  {a['size']/1048576:.1f}MB")
        print(f"    {a['browser_download_url']}")
