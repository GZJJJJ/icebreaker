import json

d = json.load(open('xz_release.json', encoding='utf-8'))
print('tag:', d['tag_name'], '| date:', d['published_at'])
for a in d['assets']:
    n = a['name'].lower()
    if 'box' in n or 'atk' in n:
        print(f"  {a['name']}  {a['size']/1048576:.1f}MB")
        print(f"    {a['browser_download_url']}")
