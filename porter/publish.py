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
# A pack with packs (android.tsv romfs_packs: datapack.py) needs nothing of its own here: packs/pNNNN.tar and packs/romfs.nxbi are listed as any file (path, size,
# SHA-256, mtime), and the RomFS files inside them are not listed at all (the host reads them in the packs); its format is 1, or 2 for its aoc/ and sysdata/ bundles.
import base64, hashlib, json, os, shutil, tarfile, time
from pathlib import Path

from porter import datapack
from porter.upstream import BuildError

FILES = 'files.json'
SOURCE_ASSET = 'nx2a/source.bin'   # in the APK: the server's address and credentials (source_asset)
SKIP = (FILES,)                    # files of the pack folder that are not data
SAVECLOUD_ASSET = 'nx2a/savecloud.txt'   # in the APK: the cloud save server's address (savecloud_asset); an APK without it has no cloud save (savesync.h ss_link_server)
SAVECLOUD_FILE = 'savecloud.txt'         # in the porter's own folder (paths.user_dir), never in the checkout: one line, url=<the server's http(s) address>


def check_url(url):   # --data-url as the app joins paths to it: http(s), ending in /
    u = (url or '').strip()
    if not u.lower().startswith(('http://', 'https://')) or any(c in u for c in '\r\n\0 ') or len(u) > 900:
        raise BuildError(f'--data-url takes the http:// or https:// address of a folder on a web server: {url!r}')
    return u if u.endswith('/') else u + '/'


def savecloud_url():
    """The cloud save server this computer's ports and updates get: <user dir>/savecloud.txt's url= line (blank lines and # comments passed over), its trailing / dropped.
    None without the file (the APK gets no server, and the app no cloud save). The address is the porter's own: callers log only that one is configured"""
    from porter import paths
    f = paths.user_dir() / SAVECLOUD_FILE
    try: text = f.read_text(encoding='utf-8-sig')
    except FileNotFoundError: return None
    except OSError as e: raise BuildError(f'{f}: cannot read it ({e.strerror})') from None
    url = next((ln.strip()[4:].strip() for ln in text.splitlines() if ln.strip().startswith('url=')), None)
    u = (url or '').rstrip('/')
    if not u.startswith(('http://', 'https://')) or not u.split('://', 1)[1] or any(ord(c) <= 0x20 or ord(c) >= 0x7f or c == '\\' for c in u) or len(u) > 500:
        raise BuildError(f'{f} must hold a line url=<the cloud save server\'s http:// or https:// address>')
    return u


def savecloud_asset(url, out):   # the APK's assets/nx2a/savecloud.txt for url (savecloud_url), written to out → out
    out = Path(out); out.parent.mkdir(parents=True, exist_ok=True); out.write_text(f'url={url}\n', encoding='ascii', newline='\n'); return out


def read_cred(path):   # --data-cred's one line, user:password; the errors never show its content
    try: t = Path(path).read_text(encoding='utf-8-sig').strip('\r\n')
    except OSError as e: raise BuildError(f'--data-cred: cannot read {path} ({e.strerror})') from None
    if '\n' in t or ':' not in t or not t.split(':', 1)[0] or len(t) > 400: raise BuildError(f'--data-cred: {path} must hold one line, user:password')
    return t


# The asset's masking is not security: anyone with the APK can undo it. It only keeps the password from showing as plain text (a hex view, a strings search,
# a file shared by mistake). The app undoes it in front_android.c sync_source.
MASK = b'nx2a-data-source:not-a-secret!'
def mask(b): return bytes(x ^ MASK[i % len(MASK)] ^ (i * 131 & 255) for i, x in enumerate(b))


def source_asset(url, cred='', pointer=None):
    """"NX2S", 1, then the masked "url\\ncred" (as before: an APK without a pointer gets exactly these bytes). pointer (pointer_url, key bytes, subpath): "NX2S", 2, then
    the masked "url\\ncred\\npointer_url\\nkey_hex\\nsubpath": the app reads the server's root from the pointer file first (pointer_seal), url and cred being its fallback"""
    if pointer is None: return b'NX2S\x01' + mask(f'{check_url(url)}\n{cred}'.encode())
    purl, key, sub = pointer
    return b'NX2S\x02' + mask(f'{check_url(url)}\n{cred}\n{check_pointer_url(purl)}\n{bytes(key).hex()}\n{check_subpath(sub)}'.encode())


# ---- the pointer file: where the data server is now (porter port --data-pointer; porter pointer). One ASCII line, "NX2P1:" + base64(nonce 16 ‖ ciphertext ‖ tag 32),
# of "<root URL>\n<user:password or nothing>" encrypted with a 32-byte key the APK carries: the server can move (a new line in that file) without porting the games again.
# SHA-256 only (no dependency, and the app has it already: dm/android/datasync.h ds_pointer_open does the same): enc = SHA256("nx2p-enc" ‖ K), mac = SHA256("nx2p-mac" ‖ K),
# keystream block i = SHA256(enc ‖ nonce ‖ LE32(i)), ciphertext = plaintext XOR keystream, tag = HMAC-SHA256(mac, "NX2P1" ‖ nonce ‖ ciphertext), checked first.
POINTER_MAGIC = 'NX2P1:'
POINTER_MAX, POINTER_PLAIN_MAX = 4096, 2048   # the line's bytes, the plaintext's


def _url_ok(u): return not any(c in u for c in '\r\n\0 \t\\') and all(32 < ord(c) < 127 for c in u)


def check_pointer_url(url):   # --data-pointer: the pointer file's address, https:// only (the app fetches it without credentials)
    u = (url or '').strip()
    if not u.startswith('https://') or len(u) <= len('https://') or not _url_ok(u) or len(u) > 900:
        raise BuildError(f'--data-pointer takes the https:// address of the pointer file: {url!r}')
    return u


def check_root(url, what='--root'):   # a pointer's server root: https://, ending in / (the app refuses anything else: plain HTTP is not a server one moves to)
    u = (url or '').strip()
    if not u.startswith('https://') or len(u) <= len('https://') or not _url_ok(u) or len(u) > 900:
        raise BuildError(f'{what} takes the https:// address of the folder the game packs are under: {url!r}')
    return u if u.endswith('/') else u + '/'


def check_subpath(sub):   # the pack's folder under the root: relative, ending in / ('' for the root itself), no .. and no backslash
    s = sub or ''
    if s and (not s.endswith('/') or s.startswith('/') or not _url_ok(s) or len(s) > 900 or any(p in ('', '.', '..') for p in s[:-1].split('/'))):
        raise BuildError(f'the data pack\'s folder under --data-root is not a plain relative path: {sub!r}')
    return s


def data_subpath(root, url):   # --data-root and --data-url (both as check_url makes them): the pack's folder under the root
    if not url.startswith(root): raise BuildError(f'--data-url must be under --data-root: {url} is not under {root}')
    return check_subpath(url[len(root):])


def read_key(path, what='--data-pointer-key'):   # a key file: one line of 64 hex digits; the errors never show its content
    try: t = Path(path).read_text(encoding='utf-8-sig').strip()
    except OSError as e: raise BuildError(f'{what}: cannot read {path} ({e.strerror})') from None
    if len(t) != 64 or any(c not in '0123456789abcdefABCDEF' for c in t): raise BuildError(f'{what}: {path} must hold one line of 64 hex digits (porter pointer --new-key makes one)')
    return bytes.fromhex(t)


def new_key(path):   # a fresh random key into a new file (never over one: the APKs that carry the old key would no longer read the pointer)
    p = Path(path)
    try: fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
    except FileExistsError: raise BuildError(f'{p} exists already: the APKs ported with it need it, give another name') from None
    except OSError as e: raise BuildError(f'cannot write {p} ({e.strerror})') from None
    with os.fdopen(fd, 'w', encoding='ascii', newline='\n') as f: f.write(os.urandom(32).hex() + '\n')
    return p


def _pointer_keys(key):
    if len(key) != 32: raise BuildError('a pointer key is 32 bytes')
    return hashlib.sha256(b'nx2p-enc' + key).digest(), hashlib.sha256(b'nx2p-mac' + key).digest()


def _keystream(enc, nonce, n):
    return b''.join(hashlib.sha256(enc + nonce + i.to_bytes(4, 'little')).digest() for i in range((n + 31) // 32))[:n]


def pointer_seal(root, cred, key, nonce=None):   # the pointer file's line for the server root (check_root) and its user:password ('' for none)
    import hmac
    plain = f'{check_root(root)}\n{cred or ""}'.encode()
    if '\n' in (cred or '') or len(plain) > POINTER_PLAIN_MAX: raise BuildError('the pointer\'s address and credentials are too long, or the credentials are not one line')
    enc, mac = _pointer_keys(key); nonce = os.urandom(16) if nonce is None else nonce
    ct = bytes(a ^ b for a, b in zip(plain, _keystream(enc, nonce, len(plain))))
    tag = hmac.new(mac, b'NX2P1' + nonce + ct, hashlib.sha256).digest()
    return POINTER_MAGIC + base64.b64encode(nonce + ct + tag).decode('ascii')


def pointer_open(line, key):   # (root, cred) from a pointer line, as the app reads it; BuildError for any line it would refuse (the reason never shows the content)
    import binascii, hmac
    t = (line.decode('ascii', 'replace') if isinstance(line, bytes) else line or '').strip()
    if len(t) > POINTER_MAX or not t.startswith(POINTER_MAGIC): raise BuildError('not a pointer line (NX2P1:…)')
    try: raw = base64.b64decode(t[len(POINTER_MAGIC):], validate=True)
    except (binascii.Error, ValueError): raise BuildError('the pointer line is not base64') from None
    if len(raw) < 16 + 32 or len(raw) - 48 > POINTER_PLAIN_MAX: raise BuildError('the pointer line has the wrong length')
    nonce, ct, tag = raw[:16], raw[16:-32], raw[-32:]
    enc, mac = _pointer_keys(key)
    if not hmac.compare_digest(hmac.new(mac, b'NX2P1' + nonce + ct, hashlib.sha256).digest(), tag): raise BuildError('the pointer line does not open with this key (another key, or changed)')
    try: plain = bytes(a ^ b for a, b in zip(ct, _keystream(enc, nonce, len(ct)))).decode('utf-8')
    except UnicodeDecodeError: raise BuildError('the pointer\'s content is not text') from None
    root, sep, cred = plain.partition('\n')
    if not sep or '\n' in cred or '\0' in plain: raise BuildError('the pointer\'s content is not two lines')
    if not root.startswith('https://') or len(root) <= len('https://') or not root.endswith('/') or not _url_ok(root): raise BuildError('the pointer\'s server is not an https:// folder address ending in /')
    if cred and (':' not in cred or not cred.split(':', 1)[0] or any(ord(c) < 32 or ord(c) == 127 for c in cred)): raise BuildError('the pointer\'s credentials are not user:password')
    return root, cred


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
    text = dump(out)
    tmp = d / (FILES + '.new')
    tmp.write_text(text, encoding='utf-8', newline='\n'); os.replace(tmp, d / FILES)
    try: shutil.copyfile(d / FILES, cache_file(d))
    except OSError: pass
    return dict(out, hashed=hashed)


def dump(out):   # files.json's text: indented for format 1; for format 2 one line per bundle and per file (207,000 files in indented JSON was 40 MB)
    if out.get('format') != 2: return json.dumps(out, indent=1, ensure_ascii=False)
    head = {k: v for k, v in out.items() if k not in ('bundles', 'files')}
    line = lambda x: json.dumps(x, ensure_ascii=False, separators=(',', ':'))
    return (json.dumps(head, indent=1, ensure_ascii=False)[:-2] + ',\n "bundles": [\n  ' + ',\n  '.join(map(line, out['bundles'])) + '\n ],\n "files": [\n  '
            + ',\n  '.join(map(line, out['files'])) + '\n ]\n}')


def replace_file(fj, rel, data):
    """files.json fj (a dict) with the loose file rel's entry for data (bytes) instead: its size and SHA-256, the list's id and bytes again (porter apk-update: a new
    nx2a-data.json in a published pack, the rest of the pack unchanged). BuildError when rel is not a loose file of the list"""
    out = json.loads(json.dumps(fj))
    f = next((f for f in out.get('files', []) if f.get('path') == rel and 'bundle' not in f), None)
    if f is None: raise BuildError(f'{FILES} lists no file {rel}')
    out['bytes'] = out.get('bytes', 0) - f['size'] + len(data)
    f.update(size=len(data), sha256=hashlib.sha256(data).hexdigest(), mtime=0)   # (mtime: the hash cache's, of the PC that hashed it; 0 makes the next porter publish hash it again)
    out['id'] = list_id(out['files']); out['made'] = time.strftime('%Y-%m-%d %H:%M:%S')
    return out


def cache_file(d): return Path(d).with_name(Path(d).name + '.hashes.json')   # <App>-data.hashes.json beside the pack: the hash cache across ports (not to upload)
