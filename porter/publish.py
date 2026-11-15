# A data pack published on a web server, for an APK ported with --data-url: the app downloads its data itself at launch, without a PC (dm/android/datasync.h, front_android.c).
# The pack folder goes to the server as it is (its files at the same relative paths under the --data-url folder); this writes the pack's files.json, the list the app
# compares with its own copy:
#   {"format": 1, "id": <SHA-256 of the list>, "package", "version", "display", "variants", "bytes", "made",
#    "files": [{"path": "RomFS/a/b.bin", "size": 123, "sha256": "<hex>", "mtime": <ns, this PC's, for the hash cache only>}, ...]}
# id changes exactly when a path, a size or a content changes, so the app compares one string to know the pack is the same. The app needs only GET with Range and Basic
# authentication (a Synology's WebDAV share served over HTTP is enough: no listing, files.json is the listing). Hashing a 4 GB RomFS takes a while: a file with the size and
# the mtime it had in the existing files.json keeps its hash. Upload files.json last: an app that reads it while the files are still going up would download a half folder.
# A pack with bundles (android.tsv data_bundles: datapack.py) gets format 2, one line per file:
#   {"format": 2, "id", ... as above, "bundles": [{"path": "bundles/b0000.tar", "size", "sha256", "files": <its files>, "bytes": <their bytes>, "mtime"}, ...],
#    "files": [{"path", "size", "sha256", "bundle": <index in bundles>}, ... the bundled ones first, in bundle order, then {"path", "size", "sha256", "mtime"} for the others]}
# "files" still lists every game file (the id is the same as format 1's for the same files, and the app decides what it lacks file by file, as before); a bundled one is on
# the server only inside its bundle, which the app downloads (or finds pushed by porter install) and takes the files it needs out of. An app from before format 2 reads
# "files" and asks for each file on its own: the bundled ones come first, so its first download is a bundled file's, an HTTP 404 it stops at with nothing else downloaded,
# and it never starts the game on half a pack (a folder that already holds every bundled file is whole for it, and it downloads the rest as before).
import hashlib, json, os, shutil, tarfile, time
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


def bad_name(rel):   # a file name the app cannot take
    return any(c in rel for c in '\r\n\0"\\')


def bundle_files(p, progress=None, done=0):
    """A bundle's SHA-256 and its files [(path, size, SHA-256)] in their order, read once; BuildError for anything but the regular files datapack.write_bundle writes"""
    whole = hashlib.sha256(); out = []
    class Tee:   # the tar's bytes into the bundle's hash as tarfile reads them (stream mode reads in order, never seeks)
        def __init__(self, f): self.f = f
        def read(self, n=-1):
            nonlocal done
            b = self.f.read(n); whole.update(b); done += len(b)
            if progress: progress(done)
            return b
    with open(datapack_path(p), 'rb') as f:
        with tarfile.open(fileobj=Tee(f), mode='r|', encoding='utf-8') as tar:
            for m in tar:
                if not m.isreg(): raise BuildError(f'{p}: {m.name} is not a regular file (a bundle holds files only)')
                h, src = hashlib.sha256(), tar.extractfile(m)
                while b := src.read(4 << 20): h.update(b)
                out.append((m.name, m.size, h.hexdigest()))
        while b := f.read(4 << 20): whole.update(b); done += len(b)   # the end blocks tarfile did not read
    return whole.hexdigest(), out, done


def manifest(d, progress=None):
    """Write d's files.json (reusing the hashes of the existing one for files of the same size and mtime) and return it, with 'hashed': the files hashed this time"""
    d = Path(d).resolve()
    man = datapack.read_manifest(d)
    if not man: raise BuildError(f'not a data pack (no {datapack.MANIFEST}): {d}')
    bund = datapack.bundled(man)
    old, oldb = {}, {}
    for src in (cache_file(d), d / FILES):   # the port rebuilds the pack folder whole (datapack.build), so the hashes are kept beside it too
        try:
            j = json.loads(src.read_text(encoding='utf-8'))
            old.update({f['path']: f for f in j['files'] if 'bundle' not in f})
            if bund:   # a bundle of the same size and mtime keeps its hash and its files'
                by = [dict(b, members=[]) for b in j.get('bundles', [])]
                for f in j['files']:
                    if 'bundle' in f: by[f['bundle']]['members'].append((f['path'], f['size'], f['sha256']))
                oldb.update({b['path']: b for b in by})
        except (OSError, ValueError, KeyError, TypeError, IndexError): pass
    found, tars = [], []
    for root, dirs, names in os.walk(d):
        dirs.sort()
        for n in sorted(names):
            p = Path(root) / n; rel = p.relative_to(d).as_posix()
            if rel in SKIP or n.endswith('.part'): continue
            if bad_name(rel): raise BuildError(f'{rel}: a file name the app cannot take (a quote, a backslash or a control character)')
            st = p.stat()
            (tars if bund and rel.startswith(datapack.BUNDLES + '/') else found).append((rel, p, st.st_size, st.st_mtime_ns))
    fresh = lambda o, size, mt: o and o.get('size') == size and o.get('mtime') == mt and len(o.get('sha256', '')) == 64
    total = sum(s for _, _, s, _ in found)
    todo = sum(s for r, _, s, m in found if not fresh(old.get(r), s, m)) + sum(s for r, _, s, m in tars if not fresh(oldb.get(r), s, m))   # the bytes to hash
    files, done, hashed = [], 0, 0
    for rel, p, size, mt in found:
        o = old.get(rel)
        if fresh(o, size, mt): sha = o['sha256']
        else:
            sha, done = sha256_file(p, (lambda x: progress(x, todo)) if progress else None, done); hashed += 1
        files.append(dict(path=rel, size=size, sha256=sha, mtime=mt))
    if not bund:
        out = dict(format=1, id=list_id(files), package=man['package'], version=man['version'], display=man.get('display', ''), variants=man.get('variants', []),
                   bytes=total, made=time.strftime('%Y-%m-%d %H:%M:%S'), files=files)
        text = json.dumps(out, indent=1, ensure_ascii=False)
    else:
        bundles, members, paths = [], [], {f['path'] for f in files}
        for k, (rel, p, size, mt) in enumerate(tars):
            if rel != datapack.bundle_name(k): raise BuildError(f'{d}: {rel} where {datapack.bundle_name(k)} was expected (the bundles are numbered from 0 without a gap)')
            o = oldb.get(rel)
            if fresh(o, size, mt): sha, got = o['sha256'], o['members']
            else:
                sha, got, done = bundle_files(p, (lambda x: progress(x, todo)) if progress else None, done); hashed += len(got)
            for m, s, h in got:
                if bad_name(m) or m in paths or not m.split('/', 1)[0] in datapack.BUNDLED: raise BuildError(f'{rel}: {m} is a name a bundle cannot hold (outside {", ".join(datapack.BUNDLED)}, twice, or one the app cannot take)')
                paths.add(m); members.append(dict(path=m, size=s, sha256=h, bundle=k))
            bundles.append(dict(path=rel, size=size, sha256=sha, files=len(got), bytes=sum(s for _, s, _ in got), mtime=mt))
        total += sum(m['size'] for m in members)
        out = dict(format=2, id=list_id(members + files), package=man['package'], version=man['version'], display=man.get('display', ''), variants=man.get('variants', []),
                   bytes=total, made=time.strftime('%Y-%m-%d %H:%M:%S'), bundles=bundles, files=members + files)
        head = {k: v for k, v in out.items() if k not in ('bundles', 'files')}   # one line per bundle and per file: 207,000 files in indented JSON was 40 MB
        line = lambda x: json.dumps(x, ensure_ascii=False, separators=(',', ':'))
        text = (json.dumps(head, indent=1, ensure_ascii=False)[:-2] + ',\n "bundles": [\n  ' + ',\n  '.join(map(line, bundles)) + '\n ],\n "files": [\n  '
                + ',\n  '.join(map(line, out['files'])) + '\n ]\n}')
    tmp = d / (FILES + '.new')
    tmp.write_text(text, encoding='utf-8', newline='\n'); os.replace(tmp, d / FILES)
    try: shutil.copyfile(d / FILES, cache_file(d))
    except OSError: pass
    return dict(out, hashed=hashed)


def cache_file(d): return Path(d).with_name(Path(d).name + '.hashes.json')   # <App>-data.hashes.json beside the pack: the hash cache across ports (not to upload)
