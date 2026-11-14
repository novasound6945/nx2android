# A data pack published on a web server, for an APK ported with --data-url: the app downloads its data itself at launch, without a PC (dm/android/datasync.h, front_android.c).
# The pack folder goes to the server as it is (its files at the same relative paths under the --data-url folder); this writes the pack's files.json, the list the app
# compares with its own copy:
#   {"format": 1, "id": <SHA-256 of the list>, "package", "version", "display", "variants", "bytes", "made",
#    "files": [{"path": "RomFS/a/b.bin", "size": 123, "sha256": "<hex>", "mtime": <ns, this PC's, for the hash cache only>}, ...]}
# id changes exactly when a path, a size or a content changes, so the app compares one string to know the pack is the same. The app needs only GET with Range and Basic
# authentication (a Synology's WebDAV share served over HTTP is enough: no listing, files.json is the listing). Hashing a 4 GB RomFS takes a while: a file with the size and
# the mtime it had in the existing files.json keeps its hash. Upload files.json last: an app that reads it while the files are still going up would download a half folder.
import hashlib, json, os, shutil, time
from pathlib import Path

from porter import datapack
from porter.upstream import BuildError

FILES = 'files.json'
SOURCE_ASSET = 'nx2a/source.bin'   # in the APK: the server's address and credentials (source_asset)
SKIP = (FILES,)                    # files of the pack folder that are not data


def check_url(url):   # --data-url as the app joins paths to it: http(s), ending in /
    u = (url or '').strip()
    if not u.lower().startswith(('http://', 'https://')) or any(c in u for c in '\r\n\0 ') or len(u) > 900:
        raise BuildError(f'--data-url takes the http:// or https:// address of a folder on a web server: {url!r}')
    return u if u.endswith('/') else u + '/'


def read_cred(path):   # --data-cred's one line, user:password; the errors never show its content
    try: t = Path(path).read_text(encoding='utf-8-sig').strip('\r\n')
    except OSError as e: raise BuildError(f'--data-cred: cannot read {path} ({e.strerror})') from None
    if '\n' in t or ':' not in t or not t.split(':', 1)[0] or len(t) > 400: raise BuildError(f'--data-cred: {path} must hold one line, user:password')
    return t


# The asset's masking is not security: anyone with the APK can undo it. It only keeps the password from showing as plain text (a hex view, a strings search,
# a file shared by mistake). The app undoes it in front_android.c sync_source.
MASK = b'nx2a-data-source:not-a-secret!'
def mask(b): return bytes(x ^ MASK[i % len(MASK)] ^ (i * 131 & 255) for i, x in enumerate(b))


def source_asset(url, cred=''):   # "NX2S", 1, then the masked "url\ncred"
    return b'NX2S\x01' + mask(f'{check_url(url)}\n{cred}'.encode())


def sha256_file(p, progress=None, done=0):
    h = hashlib.sha256()
    with open(datapack_path(p), 'rb') as f:
        while b := f.read(4 << 20):
            h.update(b); done += len(b)
            if progress: progress(done)
    return h.hexdigest(), done


def datapack_path(p):   # long paths on Windows (a RomFS can go past 260 characters)
    s = str(Path(p).resolve())
    return '\\\\?\\' + s if os.name == 'nt' and not s.startswith('\\\\?\\') else s


def list_id(files):
    h = hashlib.sha256()
    for f in sorted(files, key=lambda f: f['path']): h.update(f'{f["path"]}\0{f["size"]}\0{f["sha256"]}\n'.encode())
    return h.hexdigest()


def manifest(d, progress=None):
    """Write d's files.json (reusing the hashes of the existing one for files of the same size and mtime) and return it, with 'hashed': the files hashed this time"""
    d = Path(d).resolve()
    man = datapack.read_manifest(d)
    if not man: raise BuildError(f'not a data pack (no {datapack.MANIFEST}): {d}')
    old = {}
    for src in (cache_file(d), d / FILES):   # the port rebuilds the pack folder whole (datapack.build), so the hashes are kept beside it too
        try: old.update({f['path']: f for f in json.loads(src.read_text(encoding='utf-8'))['files']})
        except (OSError, ValueError, KeyError, TypeError): pass
    found = []
    for root, dirs, names in os.walk(d):
        dirs.sort()
        for n in sorted(names):
            p = Path(root) / n; rel = p.relative_to(d).as_posix()
            if rel in SKIP or n.endswith('.part'): continue
            if any(c in rel for c in '\r\n\0"\\'): raise BuildError(f'{rel}: a file name the app cannot take (a quote, a backslash or a control character)')
            st = p.stat(); found.append((rel, p, st.st_size, st.st_mtime_ns))
    total = sum(s for _, _, s, _ in found); todo = sum(s for r, _, s, m in found if not (r in old and old[r].get('size') == s and old[r].get('mtime') == m))
    files, done, hashed = [], 0, 0
    for rel, p, size, mt in found:
        o = old.get(rel)
        if o and o.get('size') == size and o.get('mtime') == mt and len(o.get('sha256', '')) == 64: sha = o['sha256']
        else:
            sha, done = sha256_file(p, (lambda x: progress(x, todo)) if progress else None, done); hashed += 1
        files.append(dict(path=rel, size=size, sha256=sha, mtime=mt))
    out = dict(format=1, id=list_id(files), package=man['package'], version=man['version'], display=man.get('display', ''), variants=man.get('variants', []),
               bytes=total, made=time.strftime('%Y-%m-%d %H:%M:%S'), files=files)
    tmp = d / (FILES + '.new')
    tmp.write_text(json.dumps(out, indent=1, ensure_ascii=False), encoding='utf-8', newline='\n'); os.replace(tmp, d / FILES)
    try: shutil.copyfile(d / FILES, cache_file(d))
    except OSError: pass
    return dict(out, hashed=hashed)


def cache_file(d): return Path(d).with_name(Path(d).name + '.hashes.json')   # <App>-data.hashes.json beside the pack: the hash cache across ports (not to upload)
