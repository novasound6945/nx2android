# The data pack: everything the app reads from its files folder on the device (/sdcard/Android/data/<package>/files, front_android.c), as a folder beside the APK:
#   mods/          the rewritten code modules (patch_nso.py's outputs)
#   mods-<v>/      the same modules rewritten for a variant android.tsv declares (DM_PATCH_VARIANT=<v>: 4x3, a 4:3 screen), which the app runs instead of mods/ on such a screen
#                  (front_android.c aspect_variant); env-<v>.txt beside it: the variant's env rows, applied after env.txt
#   RomFS/         the game's files (the packed version's merged RomFS)
#   aoc/<index>/   the DLC the player has
#   sysdata/<title ID>/  system data from the player's firmware dump that the game reads (android.tsv sysdata, porter --firmware; env.txt's DM_SYSDATA)
#   shaders.dmsv   the Vulkan shaders (dm/android/tools/shader_vk.py)
#   nintendo_ext.ttf  the button-icon font, the stand-in for the Nintendo extension shared font (dm/tools/make_ext_font.py; env.txt's DM_SHARED_FONT_EXT)
#   standard.ttf   the player's font for the system's shared fonts, with --shared-font (env.txt's DM_SHARED_FONT)
#   env.txt        host switches: the game language, the DLC folder, the profile's env rows, then the port's (--fps60, --mt-replay, --no-menu-button); on the device the installer adds its
#                  DM_ROMFS row after them when the app reads its RomFS from another folder (porter/install.py, --romfs-at): that is a fact of one device, not of the pack.
#                  An install merges the device's env.txt with the pack's (porter/install.py merge_env): the pack's rows win for their keys, the device's own rows are kept
#   settings.txt   the pause menu's first-launch defaults (the profile's setting rows, then the port's: --touch); the installer never replaces the player's own
#   nx2a-data.json what the pack is: the game, the package, the version, the device features the profile requires (android.tsv requires), which the installer checks;
#                  its variants; and env_keys: the keys its env.txt defines (as the app parses them), which an install reads back off the device to tell a pack's old rows
#                  from the player's (porter/install.py merge_env). publish.py lists the pack in files.json for an APK that downloads it (porter port --data-url)
#   bundles/bNNNN.tar  with android.tsv data_bundles <max> <size> only: the game's files of at most <max> bytes under RomFS/, aoc/ and sysdata/ (BUNDLED), in path
#                  order, in uncompressed tar archives of up to <size> bytes, instead of one file each (the manifest's "bundles"). The app takes them out itself
#                  (dm/android/datasync.h: downloaded, or pushed by porter install), which costs seconds where creating each small file over adb (Android's FUSE layer) or
#                  downloading each one took hours. The pack then always has its files.json (publish.py), the list of every game file and the bundle it is in, by which
#                  the app unpacks and the installer pushes: a pack built without the row is as before
#   packs/pNNNN.tar, packs/romfs.nxbi  with android.tsv romfs_packs on (beside data_bundles, whose <max> and <size> it takes) only: the RomFS's files of at most <max>
#                  bytes in tar packs of up to <size> bytes (the bundles' tar format, members RomFS/...), which nothing unpacks: the host reads each file where it lies
#                  in its pack (dm/host/fs_pack.h), by the index romfs.nxbi (write_index: the NXBI format), so the device never holds 205,000 small files (the manifest's
#                  "packs"; env.txt's DM_ROMFS_PACK). A folder of their own, not bundles/: bundles/ holds what the app takes out and deletes (datasync.h), packs/ what
#                  stays; for the app's download and the installer they are plain files (files.json lists them as any file, and not the files in them). The small
#                  files of aoc/ and sysdata/ still go into bundles (the host looks at those folders on the disk: the DLC count, the system fonts)
# Big trees are hard links into the extraction when the volume allows, so the pack takes no extra space (bundles are copies: the small files' bytes once more); the pack
# is rebuilt whole when anything in it changes.
import hashlib, json, os, shutil, struct, tarfile, time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from porter.upstream import BuildError
from porter.upstream import settings_registry

MANIFEST = 'nx2a-data.json'
ENV = 'env.txt'
KEEP_ON_DEVICE = ('settings.txt',)          # the player's choices win over the pack's defaults; env.txt is not kept whole: install.py merges it with the pack's (merge_env)
OWNED = ('mods', 'RomFS', 'aoc', 'sysdata', 'packs')   # folders the pack owns on the device: files there that the pack lacks are stale (an older version's) and removed
ROMFS = 'RomFS'
VARIANT_PREFIX = 'mods-'                    # mods-<variant>/: owned as mods/ is (owned)
BUNDLES = 'bundles'                         # the bundles' folder, in the pack and in the files folder (where the app deletes each bundle once it took its files out)
BUNDLED = ('RomFS', 'aoc', 'sysdata')       # the trees whose small files go into bundles: the game's own files, which change only with its version. mods/ stays loose (a
                                            # rewriter cut changes a module at the same size: one file again, not a bundle), and so do the files the app and the installer
                                            # treat one by one (env.txt, settings.txt, the manifest, files.json)
PACKS = 'packs'                             # with romfs_packs: the RomFS's small files in tar packs read in place, and their index
PACK_INDEX = f'{PACKS}/romfs.nxbi'


def owned(rel):   # whether a file (a '/'-separated path in the files folder) is in a folder the pack owns
    top = rel.split('/', 1)[0]
    return '/' in rel and (top in OWNED or top.startswith(VARIANT_PREFIX))


def env_key(line):   # the KEY of an env.txt row, as the app reads it (front_android.c env_file: trailing blanks off, blank lines and # comments skipped, the first = splits), None for the rest
    ln = line.rstrip(' \n\r')
    if not ln or ln[0] == '#': return None
    eq = ln.find('=')
    return ln[:eq] if eq >= 0 else None


def link_or_copy(src, dst):
    dst.parent.mkdir(parents=True, exist_ok=True)
    try: os.link(src, dst)
    except OSError: shutil.copy2(src, dst)


def link_tree(src, dst, small=None, max_size=0):
    """(files, bytes). small: a list that takes the files of at most max_size bytes [(path under src, '/'-separated; path; size; mtime in ns)] instead of linking them"""
    n = b = 0
    for root, _, files in os.walk(src):
        for f in files:
            s = Path(root) / f; st = s.stat(); size = st.st_size; n += 1; b += size
            rel = s.relative_to(src)
            if small is not None and size <= max_size: small.append((rel.as_posix(), s, size, st.st_mtime_ns)); continue
            link_or_copy(s, dst / rel)
    return n, b


TAR_END = 1024 + tarfile.RECORDSIZE   # a tar's end: two zero blocks, then zeros to a whole record (tarfile pads the archive to 10,240 bytes)


def tar_cost(rel, size):   # the bytes a file takes in a GNU tar: its header, its data in 512-byte blocks, and for a name past 100 bytes a GNU long name (a header and the name's blocks)
    n = len(rel.encode('utf-8'))
    return 512 + -(-size // 512) * 512 + (512 + -(-(n + 1) // 512) * 512 if n > tarfile.LENGTH_NAME else 0)


def bundle_groups(files, size):
    """files [(rel, path, size, mtime)] (rel '/'-separated) → [[(rel, path, size, mtime), ...], ...]: in path order, each group as many files as a tar of at most size bytes holds
    (tar_cost, with the end blocks; one file at least)"""
    groups, cur, used = [], [], TAR_END
    for f in sorted(files, key=lambda f: f[0]):
        c = tar_cost(f[0], f[2])
        if cur and used + c > size: groups.append(cur); cur, used = [], TAR_END
        cur.append(f); used += c
    if cur: groups.append(cur)
    return groups


def bundle_name(k): return f'{BUNDLES}/b{k:04d}.tar'


def write_bundle(dst, group):
    """One bundle: a GNU tar (long names as GNU writes them, which datasync.h reads) of regular files with fixed metadata (mode 644, owner 0, time 0), so the same files make
    the same bytes in every port and the installer can tell a bundle on the device from another by its size and MD5"""
    dst.parent.mkdir(parents=True, exist_ok=True)
    with tarfile.open(dst, 'w', format=tarfile.GNU_FORMAT, encoding='utf-8') as tar:
        for rel, p, size, _ in group:
            ti = tarfile.TarInfo(rel); ti.size, ti.mode, ti.mtime, ti.uid, ti.gid, ti.uname, ti.gname = size, 0o644, 0, 0, 0, '', ''
            with open(long_path(p), 'rb') as f: tar.addfile(ti, f)


def long_path(p):   # a path Python can open on Windows past 260 characters (a deep RomFS)
    s = str(Path(p).resolve())
    return '\\\\?\\' + s if os.name == 'nt' and not s.startswith('\\\\') else s


# ---- romfs_packs: the packs' index (NXBI version 1, read by dm/host/fs_pack.h; little-endian):
#   header, 32 bytes: "NXBI", u32 version 1, u32 npacks, u32 nfiles, u64 names_bytes, u64 0
#   the packs, npacks x 16 bytes: u64 the pack's size, u64 0
#   the files, nfiles x 24 bytes: u32 name_off, u16 name_len, u16 pack, u64 data_off (the file's first byte in its pack, past its tar header), u64 size;
#     sorted by the UTF-8 bytes of their names (a name that is another's start first), each name once
#   the names, without NULs: each file's path from the RomFS's root ('Data/a.bin'), as the game names it
NXBI_HEAD, NXBI_PACK, NXBI_FILE = struct.Struct('<4sIIIQQ'), struct.Struct('<QQ'), struct.Struct('<IHHQQ')
NXBI_VERSION = 1


def pack_name(k): return f'{PACKS}/p{k:04d}.tar'


def check_romfs_packs(profile):   # what romfs_packs cannot go with, refused before anything is made
    if not getattr(profile, 'romfs_packs', False): return
    if getattr(profile, 'romfs_zstd', False):
        raise BuildError(f'{profile.name}: android.tsv romfs_packs on and romfs zstd cannot go together (the host reads a packed file from its own file, not from inside a pack)')
    if not getattr(profile, 'bundles', None): raise BuildError(f'{profile.name}: android.tsv romfs_packs on needs data_bundles <max file bytes> <pack bytes>')


def index_entries(folder, count):
    """[(name in the RomFS, pack, data offset, size)] of packs 0..count-1 in folder, as their tar headers say (tarfile's offset_data: past a GNU long name too). BuildError
    for a member that is not a regular file under RomFS/"""
    out, pre = [], ROMFS + '/'
    for k in range(count):
        with tarfile.open(long_path(Path(folder) / pack_name(k)), 'r:', encoding='utf-8') as tar:
            for m in tar:
                if not m.isreg() or not m.name.startswith(pre) or len(m.name) == len(pre): raise BuildError(f'{pack_name(k)}: {m.name} is not a RomFS file (a pack holds the RomFS\'s files only)')
                out.append((m.name[len(pre):], k, m.offset_data, m.size))
    return out


def write_index(path, pack_sizes, entries):
    """The index of packs of pack_sizes [bytes] holding entries [(name in the RomFS, pack, data offset, size)], sorted here. BuildError for a name twice, or what the
    format cannot hold"""
    ents = sorted(((n.encode('utf-8'), k, o, s) for n, k, o, s in entries), key=lambda e: e[0])
    if len(pack_sizes) > 1 << 16: raise BuildError(f'{len(pack_sizes):,} packs: an index names at most 65,536 (make data_bundles\' <bundle bytes> bigger)')
    names, recs, off = [], [], 0
    for i, (n, k, o, s) in enumerate(ents):
        if i and n == ents[i - 1][0]: raise BuildError(f'{n.decode("utf-8")} is in the packs twice')
        if not n or len(n) > 0xFFFF or not 0 <= k < len(pack_sizes): raise BuildError(f'{n.decode("utf-8", "replace")!r}: not a name or a pack an index can hold')
        names.append(n); recs.append(NXBI_FILE.pack(off, len(n), k, o, s)); off += len(n)
    if off >= 1 << 32: raise BuildError('the RomFS\'s names in the packs are past 4 GB')
    blob = b''.join(names)
    data = NXBI_HEAD.pack(b'NXBI', NXBI_VERSION, len(pack_sizes), len(ents), len(blob), 0) + b''.join(NXBI_PACK.pack(s, 0) for s in pack_sizes) + b''.join(recs) + blob
    Path(path).parent.mkdir(parents=True, exist_ok=True)
    with open(long_path(path), 'wb') as f: f.write(data)


def read_index(path):
    """(pack sizes, [(name, pack, data offset, size)] in the index's order) of an index; BuildError for one that is not whole"""
    with open(long_path(path), 'rb') as f: b = f.read()
    if len(b) < NXBI_HEAD.size: raise BuildError(f'{path}: shorter than an index\'s header')
    magic, ver, np_, nf, nb, _ = NXBI_HEAD.unpack_from(b)
    if magic != b'NXBI' or ver != NXBI_VERSION: raise BuildError(f'{path}: not an NXBI version {NXBI_VERSION} index')
    po, fo = NXBI_HEAD.size, NXBI_HEAD.size + np_ * NXBI_PACK.size
    no = fo + nf * NXBI_FILE.size
    if no + nb != len(b): raise BuildError(f'{path}: {len(b):,} bytes, where its counts make {no + nb:,}')
    sizes = [NXBI_PACK.unpack_from(b, po + k * NXBI_PACK.size)[0] for k in range(np_)]
    ents = []
    for i in range(nf):
        o, ln, k, d, s = NXBI_FILE.unpack_from(b, fo + i * NXBI_FILE.size)
        if o + ln > nb or k >= np_: raise BuildError(f'{path}: file {i}\'s name or pack is outside the index')
        ents.append((b[no + o:no + o + ln].decode('utf-8'), k, d, s))
    return sizes, ents


def check_packs(folder, sources, progress=None):
    """The port's own check of a pack's packs: every file of the index read back from its pack at the index's offset and size and compared with its source
    (sources {name in the RomFS: path}) by SHA-256; the index's names exactly the sources', its pack sizes the packs'. BuildError at the first difference"""
    folder = Path(folder)
    sizes, ents = read_index(folder / PACK_INDEX)
    for k, s in enumerate(sizes):
        if (got := (folder / pack_name(k)).stat().st_size) != s: raise BuildError(f'{pack_name(k)}: {got:,} bytes, its index says {s:,}')
    names = [e[0] for e in ents]
    if len(names) != len(sources) or set(names) != set(sources):
        miss, extra = sorted(set(sources) - set(names)), sorted(set(names) - set(sources))
        raise BuildError(f'{PACK_INDEX} does not list the RomFS\'s small files: {len(miss):,} missing ({", ".join(miss[:3])}), {len(extra):,} not the RomFS\'s or twice ({", ".join(extra[:3])})')
    by = {}
    for e in ents: by.setdefault(e[1], []).append(e)
    sha = lambda b: hashlib.sha256(b).digest()
    def one(k):   # one pack: its files in pack order (reads go forward)
        with open(long_path(folder / pack_name(k)), 'rb') as f:
            for name, _, off, size in sorted(by[k], key=lambda e: e[2]):
                f.seek(off); got = f.read(size)
                with open(long_path(sources[name]), 'rb') as s: want = s.read()
                if len(got) != size or sha(got) != sha(want): return f'{name}: the {size:,} bytes at {off:,} in {pack_name(k)} are not its file\'s'
        return None
    done = 0
    with ThreadPoolExecutor(max_workers=min(8, os.cpu_count() or 1)) as ex:   # reading a small file costs its opening on Windows: several at once
        for bad in ex.map(one, sorted(by)):
            if bad: raise BuildError(f'the packs\' check failed: {bad}')
            done += 1
            if progress and (done % 16 == 0 or done == len(by)): progress(f'the packs checked against the RomFS\'s files ({done:,} of {len(by):,})')


# <pack>.bundles.json beside the pack (not in it: not uploaded, not pushed): each bundle's files as a key (their paths, sizes and the mtimes of their sources) and the tar's
# size and mtime. A port again keeps a bundle whose key did not change, as a hard link to the earlier pack's (the same tar, with its mtime, so publish keeps its hashes
# too): writing Fire Emblem Engage's 89 bundles read 205,000 small files and took 45 minutes on Windows, where linking the whole pack took 4
def reuse_file(d): return Path(d).with_name(Path(d).name + '.bundles.json')


def bundle_key(group): return hashlib.sha256(''.join(f'{r}\0{s}\0{m}\n' for r, _, s, m in group).encode()).hexdigest()


def bundled(man): return bool(man and man.get('bundles'))   # whether a pack's manifest says its small files are in bundles


def files_folder(package): return f'/sdcard/Android/data/{package}/files'


ROOT_POINTER = 'nx2a_root.txt'   # in the internal files folder: root=<the folder the app reads its data from> while the player keeps it on an SD card (nxnative/dm/android/storage.h DM_ST_POINTER)


def pointer_root(text, package):
    """The data folder an app's pointer names: its own files folder on another volume (/storage/<volume id>/Android/data/<package>/files), None for anything else"""
    import re
    for ln in (text or '').splitlines():
        if ln.startswith('root='):
            d = ln[5:].strip().rstrip('/')
            m = re.fullmatch(r'/storage/([A-Za-z0-9-]+)/Android/data/' + re.escape(package) + '/files', d)
            if m and m.group(1) not in ('emulated', 'self'): return d
    return None


def device_files(adb, package):
    """The folder the app reads its data from on the device: its internal files folder, or the one on the SD card the player moved the data to (the app's App settings:
    the pointer it keeps in the internal files folder). A card that is not in raises: pushing into internal storage would not be read while the app's choice is the card"""
    files = files_folder(package)
    d = pointer_root(adb.read_text(f'{files}/{ROOT_POINTER}'), package)
    if not d: return files
    if not adb.is_dir(d): raise BuildError(f'{package} keeps its game data on the SD card ({d}), which is not in the device: insert it, or choose internal storage in the app (App settings > Data location)')
    return d


SEEDABLE = ('RomFS', 'aoc')   # parts another port of the same game version on the device can give a new install (porter/install.py seed_from_sibling)


def part_key(d, part):
    """{'files', 'bytes', 'key'} of a pack part (RomFS, aoc): the key is a hash of its paths and sizes. Two packs of one game version have the same key for the same
    files (a RomFS is the game's own, the same in every port of that version; a packed RomFS has other sizes), which lets an install copy the part on the device
    from another port of the game instead of pushing it again. None when the part is not in the pack"""
    import hashlib
    r = Path(d) / part
    if not r.is_dir(): return None
    files = sorted((p.relative_to(r).as_posix(), p.stat().st_size) for p in r.rglob('*') if p.is_file())
    h = hashlib.sha256(''.join(f'{rel}\t{size}\n' for rel, size in files).encode('utf-8')).hexdigest()
    return {'files': len(files), 'bytes': sum(s for _, s in files), 'key': h}


def romfs_sizes(d):   # {path in the RomFS: size} of the pack in d, '/'-separated as the device lists them (install --romfs-at compares a folder on the device with it)
    r = Path(d) / ROMFS
    return {p.relative_to(r).as_posix(): p.stat().st_size for p in r.rglob('*') if p.is_file()} if r.is_dir() else {}


def build(dst, *, profile, version, display, patched, romfs, shaders, aoc=None, sysdata=None, language='en-US', package=None, env=(), settings=None, files=None, variants=None, profile_h=None, rewrite_abi=None, progress=None):
    """Write the pack to dst (replacing it). profile: the AndroidProfile; package: the app's, when not the profile's; env: more env.txt rows, after the profile's; settings: settings.txt values over the profile's;
    files: {name in the pack: source} (the 60 fps memory writes, fpslock.tsv; the button-icon font); variants: {name: (its rewritten modules, [its env rows])}; profile_h: sha256 of the
    game_profile.h the host was built with (porter host); rewrite_abi: the contract number its modules were rewritten for (dm/host/rewrite_abi.h; porter apk-update). Returns the manifest"""
    package = package or profile.package
    check_romfs_packs(profile)
    packs_on = bool(getattr(profile, 'romfs_packs', False))   # android.tsv romfs_packs on: the RomFS's small files into packs/, not bundles/
    tmp = dst.with_name(dst.name + '.partial')
    if tmp.exists(): shutil.rmtree(tmp)
    tmp.mkdir(parents=True)
    n = b = 0
    variants = variants or {}
    bund = getattr(profile, 'bundles', None)   # android.tsv data_bundles: (max file bytes, bundle bytes)
    small = [] if bund else None
    for what, src, sub in (('the rewritten modules', patched, 'mods'), *((f'the rewritten modules of variant {v}', d, VARIANT_PREFIX + v) for v, (d, _) in variants.items()),
                           ('the RomFS', romfs, 'RomFS'), ('the DLC', aoc, 'aoc'), ('the system data', sysdata, 'sysdata')):
        if not src: continue
        if progress: progress(what)
        got = [] if bund and sub in BUNDLED else None
        dn, db = link_tree(src, tmp / sub, got, bund[0] if got is not None else 0); n += dn; b += db
        if got: small += [(f'{sub}/{r}', p, s, m) for r, p, s, m in got]
    rsmall = [f for f in small if f[0].startswith(ROMFS + '/')] if small and packs_on else []   # romfs_packs: the RomFS's small files go into packs, the others' into bundles
    if rsmall: small = [f for f in small if not f[0].startswith(ROMFS + '/')]
    bundles, packs, keys = None, None, {}
    old = {}
    if small or rsmall:
        try: old = json.loads(reuse_file(dst).read_text(encoding='utf-8'))
        except (OSError, ValueError): old = {}

    def tars(files, name_of, kind, what):   # files into tars of up to bund[1] bytes (copies: a tar is the files' bytes once more), or an earlier port's tar of the same files (reuse_file)
        groups = bundle_groups(files, bund[1])
        kept = 0
        for k, g in enumerate(groups):
            name, key = name_of(k), bundle_key(g); o, was = old.get(name) or {}, dst / name
            try: same = o.get('key') == key and (st := was.stat()).st_size == o.get('size') and st.st_mtime_ns == o.get('mtime')
            except OSError: same = False
            if same: link_or_copy(was, tmp / name); kept += 1
            else:
                if progress: progress(f'{what} into {kind} {k + 1:,} of {len(groups):,} ({len(files):,} files of at most {bund[0]:,} bytes' + (f'; {kept:,} kept from the last port)' if kept else ')'))   # (pipeline: "linking …")
                write_bundle(tmp / name, g)
            st = (tmp / name).stat(); keys[name] = dict(key=key, size=st.st_size, mtime=st.st_mtime_ns)
        if progress and kept: progress(f'{kept:,} of {len(groups):,} {kind}s kept from the last port (their files did not change)')
        return groups

    if small:   # the small files into bundles, which the app takes out
        groups = tars(small, bundle_name, 'bundle', 'the small files')
        bundles = dict(max=bund[0], size=bund[1], count=len(groups), files=len(small), bytes=sum(f[2] for f in small),
                       tar_bytes=sum((tmp / bundle_name(k)).stat().st_size for k in range(len(groups))))
    if rsmall:   # the RomFS's small files into packs, which stay as they are, and their index (where each file's bytes are), checked against the files themselves
        groups = tars(rsmall, pack_name, 'pack', 'the RomFS\'s small files')
        sizes = [(tmp / pack_name(k)).stat().st_size for k in range(len(groups))]
        if progress: progress(f'the packs\' index ({len(rsmall):,} files in {len(groups):,} packs)')
        write_index(tmp / PACK_INDEX, sizes, index_entries(tmp, len(groups)))
        check_packs(tmp, {r[len(ROMFS) + 1:]: p for r, p, _, _ in rsmall}, progress)
        packs = dict(count=len(groups), files=len(rsmall), bytes=sum(f[2] for f in rsmall), tar_bytes=sum(sizes))
    if shaders: link_or_copy(shaders, tmp / 'shaders.dmsv'); n += 1; b += shaders.stat().st_size   # none for an OpenGL ES game
    for name, src in (files or {}).items(): (tmp / name).parent.mkdir(parents=True, exist_ok=True); link_or_copy(Path(src), tmp / name); n += 1; b += Path(src).stat().st_size   # (overlay/...: a subfolder)
    rows = [f'# Written by nx2android\'s porter for {profile.name} ({profile.id}); KEY=VALUE per line, read by the app at launch',
            f'DM_LANG={language}']
    if aoc: rows.append(f'DM_AOC_DIR={files_folder(package)}/aoc')
    if sysdata: rows.append(f'DM_SYSDATA={files_folder(package)}/sysdata')
    if packs: rows.append(f'DM_ROMFS_PACK={files_folder(package)}/{PACK_INDEX}')
    rows += profile.env + list(env)
    if bad := settings_registry.check_env(rows, ('profile', 'porter'), 'env.txt '): raise ValueError('; '.join(bad))   # nxnative/dm/settings.tsv
    (tmp / ENV).write_text('\n'.join(rows) + '\n', encoding='utf-8', newline='\n')
    for v, (_, venv) in variants.items():
        if venv: (tmp / f'env-{v}.txt').write_text(f'# variant {v}: applied after env.txt when the app runs mods-{v}/\n' + '\n'.join(venv) + '\n', encoding='utf-8', newline='\n'); n += 1
    if sets := {**profile.settings, **(settings or {})}: (tmp / 'settings.txt').write_text(''.join(f'{k}={v}\n' for k, v in sets.items()), encoding='utf-8', newline='\n')
    man = dict(title_id=profile.id, name=profile.name, package=package, version=version, display=display, language=language, requires=profile.requires,
               env_keys=list(dict.fromkeys(k for k in (env_key(r) for r in rows) if k is not None)),   # what the pack's env.txt defines: an install reads these back off the device ('' kept: merge_env parses a '=v' row as the empty key too)
               files=n, bytes=b, dlc=sorted(int(p.name) for p in aoc.iterdir() if p.is_dir() and p.name.isdigit()) if aoc else [],
               sysdata=sorted(p.name for p in sysdata.iterdir() if p.is_dir()) if sysdata else [], variants=list(variants), made=time.strftime('%Y-%m-%d %H:%M:%S'))
    if bundles: man['bundles'] = bundles   # (no key at all for a pack without: its manifest stays as it was)
    if packs: man['packs'] = packs         # (the same: no key without romfs_packs)
    if profile_h: man['profile_h'] = profile_h
    if rewrite_abi is not None: man['rewrite_abi'] = rewrite_abi
    if parts := {p: k for p in SEEDABLE if (k := part_key(tmp, p))}: man['parts'] = parts   # the content keys install's seeding compares
    (tmp / MANIFEST).write_text(json.dumps(man, indent=2), encoding='utf-8')
    if dst.exists(): shutil.rmtree(dst)
    for i in range(20):   # Windows: a folder just removed can stay delete-pending while another process (an indexer, a virus scan) holds it, and the rename is refused meanwhile
        try: tmp.rename(dst); break
        except PermissionError:
            if i == 19: raise
            time.sleep(0.5)
    if keys: reuse_file(dst).write_text(json.dumps(keys, indent=0), encoding='utf-8')
    else: reuse_file(dst).unlink(missing_ok=True)
    return man


def read_manifest(d):
    try: return json.loads((Path(d) / MANIFEST).read_text(encoding='utf-8'))
    except (OSError, ValueError): return None
