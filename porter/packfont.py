# porter pack-add-font: a published data pack with the player's shared font added, without porting the game again (python -m porter pack-add-font …).
# What porter port --shared-font puts in a pack (pipeline.Port.data), for a pack made without it: standard.ttf (the font as it is: porter never changes it), the env.txt row
# DM_SHARED_FONT=<the app's files folder>/standard.ttf (hle_sdk16.c font_get; where a port puts it: after DM_SHARED_FONT_EXT), and the pack's lists made right again:
# nx2a-data.json (env_keys as datapack.build reads them off env.txt, files and bytes with the font counted) and files.json (the font's entry, env.txt's and nx2a-data.json's
# sizes and hashes, its id and bytes: publish.replace_file). The pack is read from its folder or its server (apkupdate.Pack) and nothing is changed there: the changed files
# go to an output folder, to upload into the pack's folder, files.json last (the app compares its id and downloads what changed: dm/android/datasync.h).
# The Legend of Zelda: Link's Awakening's pack of 2026-11-18 was ported without --shared-font: Roboto has no U+25A1 and the game crashed at start on the glyph it lacked.
import hashlib, json, time
from pathlib import Path

from porter import datapack, publish
from porter.upstream import BuildError, say

FONT = 'standard.ttf'
KEY = 'DM_SHARED_FONT'
AFTER = 'DM_SHARED_FONT_EXT'                                                   # pipeline.Port.data: the shared font's row follows the button icons'
BEFORE = ('DM_ROMFS_ZSTD', 'DM_VK_MT_REPLAY', 'DM_MENU_BUTTON', 'DM_VK_DRIVER')   # and comes before these port rows (without a DM_SHARED_FONT_EXT row)


def font_row(package): return f'{KEY}={datapack.files_folder(package)}/{FONT}'


def env_with_font(text, package):
    """env.txt's text with the shared font's row (font_row) where a port with --shared-font writes it: an existing DM_SHARED_FONT row replaced in place, else after
    DM_SHARED_FONT_EXT, else before the first of BEFORE, else at the end. The other rows are kept as they are"""
    rows, row = text.split('\n'), font_row(package)
    if rows and rows[-1] == '': rows.pop()
    keys = [datapack.env_key(r) for r in rows]
    if KEY in keys:   # the first such row takes the new value, any later one goes (the app takes a key's last row)
        first = keys.index(KEY); rows[first] = row
        rows = [r for i, r in enumerate(rows) if keys[i] != KEY or i == first]
    elif AFTER in keys: rows.insert(len(keys) - keys[::-1].index(AFTER), row)
    else: rows.insert(next((i for i, k in enumerate(keys) if k in BEFORE), len(rows)), row)
    return '\n'.join(rows) + '\n'


def walk_key(path):   # where publish.manifest lists a loose file: os.walk's order, the names sorted and each folder's files before its folders
    *dirs, name = path.split('/')
    return [(1, d) for d in dirs] + [(0, name)]


def put_file(fj, rel, data, mtime=0):
    """files.json fj (a dict) with the loose file rel's entry for data (bytes): replaced when the list has it (publish.replace_file), else added where publish.manifest
    would list it (walk_key; after the bundled files of a format 2 list), with the list's id, bytes and made again"""
    if any(f.get('path') == rel and 'bundle' not in f for f in fj.get('files', [])): return publish.replace_file(fj, rel, data)
    if publish.bad_name(rel) or any(f.get('path') == rel for f in fj.get('files', [])): raise BuildError(f'{rel}: not a name a loose file of {publish.FILES} can take')
    out = json.loads(json.dumps(fj)); files = out.setdefault('files', [])
    loose = [i for i, f in enumerate(files) if 'bundle' not in f]
    at = next((i for i in loose if walk_key(files[i]['path']) > walk_key(rel)), len(files))
    files.insert(at, dict(path=rel, size=len(data), sha256=hashlib.sha256(data).hexdigest(), mtime=mtime))
    out['bytes'] = out.get('bytes', 0) + len(data)
    out['id'] = publish.list_id(files); out['made'] = time.strftime('%Y-%m-%d %H:%M:%S')
    return out


def add_font(pack, font, out):
    """The pack's changed files with the shared font added (module comment), written to out → {'package', 'written': [their names, in upload order], 'id': (old, new), 'same'}.
    pack: an apkupdate.Pack; font: the .ttf to add (as it is). Nothing is written when the pack has this font and row already ('same')"""
    font = Path(font)
    if not font.is_file() or font.suffix.lower() != '.ttf': raise BuildError(f'--shared-font: not a .ttf font: {font}')
    data = font.read_bytes()
    if data[:4] not in (b'\x00\x01\x00\x00', b'true'): raise BuildError(f'--shared-font: {font} is not a TrueType font (its first bytes)')
    man, fj, env = pack.json(datapack.MANIFEST), pack.json(publish.FILES), pack.text(datapack.ENV)
    if not isinstance(man, dict) or not man.get('package'): raise BuildError(f'{pack}: no {datapack.MANIFEST} with a package (not a data pack)')
    if not isinstance(fj, dict) or not isinstance(fj.get('files'), list): raise BuildError(f'{pack}: no {publish.FILES} (the list of the pack\'s files, porter publish)')
    if env is None: raise BuildError(f'{pack}: no {datapack.ENV}')
    if fj.get('package') != man['package']: raise BuildError(f'{pack}: {publish.FILES} is of {fj.get("package")}, {datapack.MANIFEST} of {man["package"]}')
    from porter.upstream import settings_registry
    if bad := settings_registry.check_env([font_row(man['package'])], ('porter',), 'env.txt '): raise BuildError('; '.join(bad))
    env2 = env_with_font(env, man['package']).encode('utf-8')
    old = next((f for f in fj['files'] if f.get('path') == FONT and 'bundle' not in f), None)
    if env2 == env.encode('utf-8') and old and (old.get('size'), old.get('sha256')) == (len(data), hashlib.sha256(data).hexdigest()):
        return {'package': man['package'], 'written': [], 'id': (fj.get('id'), fj.get('id')), 'same': True}
    rows = env2.decode('utf-8').split('\n')
    m2 = dict(man, env_keys=list(dict.fromkeys(k for k in (datapack.env_key(r) for r in rows) if k is not None)),   # (datapack.build's expression)
              files=man.get('files', 0) + (0 if old else 1), bytes=man.get('bytes', 0) - (old['size'] if old else 0) + len(data))
    mtext = json.dumps(m2, indent=2).encode('utf-8')   # (as datapack.build and apk-update write it; its made stays the port's)
    fj2 = put_file(put_file(put_file(fj, FONT, data), datapack.ENV, env2), datapack.MANIFEST, mtext)
    out = Path(out); out.mkdir(parents=True, exist_ok=True)
    (out / FONT).write_bytes(data); (out / datapack.ENV).write_bytes(env2); (out / datapack.MANIFEST).write_bytes(mtext)
    (out / publish.FILES).write_text(publish.dump(fj2), encoding='utf-8', newline='\n')
    written = [FONT, datapack.ENV, datapack.MANIFEST, publish.FILES]
    say(f'{man["package"]}: {FONT} ({len(data):,} bytes) and the row {KEY} added; upload {", ".join(written)} from {out} into the pack\'s folder in this order ({publish.FILES} last)')
    return {'package': man['package'], 'written': written, 'id': (fj.get('id'), fj2['id']), 'same': False}
