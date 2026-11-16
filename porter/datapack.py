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
#   env.txt        host switches: the game language, the DLC folder, the profile's env rows, then the port's (--fps60, --mt-replay); on the device the installer adds its
#                  DM_ROMFS row after them when the app reads its RomFS from another folder (porter/install.py, --romfs-at): that is a fact of one device, not of the pack
#   settings.txt   the pause menu's first-launch defaults (the profile's setting rows, then the port's: --touch); the installer never replaces the player's own
#   nx2a-data.json what the pack is: the game, the package, the version, the device features the profile requires (android.tsv requires), which the installer checks;
#                  its variants. publish.py lists the pack in files.json for an APK that downloads it (porter port --data-url)
#   bundles/bNNNN.tar  with android.tsv data_bundles <max> <size> only: the game's files of at most <max> bytes under RomFS/, aoc/ and sysdata/ (BUNDLED), in path
#                  order, in uncompressed tar archives of up to <size> bytes, instead of one file each (the manifest's "bundles"). The app takes them out itself
#                  (dm/android/datasync.h: downloaded, or pushed by porter install), which costs seconds where creating each small file over adb (Android's FUSE layer) or
#                  downloading each one took hours. The pack then always has its files.json (publish.py), the list of every game file and the bundle it is in, by which
#                  the app unpacks and the installer pushes: a pack built without the row is as before
# Big trees are hard links into the extraction when the volume allows, so the pack takes no extra space (bundles are copies: the small files' bytes once more); the pack
# is rebuilt whole when anything in it changes.
import hashlib, json, os, shutil, tarfile, time
from pathlib import Path

MANIFEST = 'nx2a-data.json'
KEEP_ON_DEVICE = ('settings.txt',)          # the player's choices win over the pack's defaults
OWNED = ('mods', 'RomFS', 'aoc', 'sysdata')            # folders the pack owns on the device: files there that the pack lacks are stale (an older version's) and removed
ROMFS = 'RomFS'
VARIANT_PREFIX = 'mods-'                    # mods-<variant>/: owned as mods/ is (owned)
BUNDLES = 'bundles'                         # the bundles' folder, in the pack and in the files folder (where the app deletes each bundle once it took its files out)
BUNDLED = ('RomFS', 'aoc', 'sysdata')       # the trees whose small files go into bundles: the game's own files, which change only with its version. mods/ stays loose (a
                                            # rewriter cut changes a module at the same size: one file again, not a bundle), and so do the files the app and the installer
                                            # treat one by one (env.txt, settings.txt, the manifest, files.json)


def owned(rel):   # whether a file (a '/'-separated path in the files folder) is in a folder the pack owns
    top = rel.split('/', 1)[0]
    return '/' in rel and (top in OWNED or top.startswith(VARIANT_PREFIX))


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


# <pack>.bundles.json beside the pack (not in it: not uploaded, not pushed): each bundle's files as a key (their paths, sizes and the mtimes of their sources) and the tar's
# size and mtime. A port again keeps a bundle whose key did not change, as a hard link to the earlier pack's (the same tar, with its mtime, so publish keeps its hashes
# too): writing Fire Emblem Engage's 89 bundles read 205,000 small files and took 45 minutes on Windows, where linking the whole pack took 4
def reuse_file(d): return Path(d).with_name(Path(d).name + '.bundles.json')


def bundle_key(group): return hashlib.sha256(''.join(f'{r}\0{s}\0{m}\n' for r, _, s, m in group).encode()).hexdigest()


def bundled(man): return bool(man and man.get('bundles'))   # whether a pack's manifest says its small files are in bundles


def files_folder(package): return f'/sdcard/Android/data/{package}/files'


def romfs_sizes(d):   # {path in the RomFS: size} of the pack in d, '/'-separated as the device lists them (install --romfs-at compares a folder on the device with it)
    r = Path(d) / ROMFS
    return {p.relative_to(r).as_posix(): p.stat().st_size for p in r.rglob('*') if p.is_file()} if r.is_dir() else {}


def build(dst, *, profile, version, display, patched, romfs, shaders, aoc=None, sysdata=None, language='en-US', package=None, env=(), settings=None, files=None, variants=None, profile_h=None, progress=None):
    """Write the pack to dst (replacing it). profile: the AndroidProfile; package: the app's, when not the profile's; env: more env.txt rows, after the profile's; settings: settings.txt values over the profile's;
    files: {name in the pack: source} (the 60 fps memory writes, fpslock.tsv; the button-icon font); variants: {name: (its rewritten modules, [its env rows])}; profile_h: sha256 of the
    game_profile.h the host was built with (porter host). Returns the manifest"""
    package = package or profile.package
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
    bundles, keys = None, {}
    if small:   # the small files into bundles (copies: a bundle is the files' bytes once more), or an earlier port's bundle of the same files (reuse_file)
        groups = bundle_groups(small, bund[1])
        try: old = json.loads(reuse_file(dst).read_text(encoding='utf-8'))
        except (OSError, ValueError): old = {}
        kept = 0
        for k, g in enumerate(groups):
            name, key = bundle_name(k), bundle_key(g); o, was = old.get(name) or {}, dst / name
            try: same = o.get('key') == key and (st := was.stat()).st_size == o.get('size') and st.st_mtime_ns == o.get('mtime')
            except OSError: same = False
            if same: link_or_copy(was, tmp / name); kept += 1
            else:
                if progress: progress(f'the small files into bundle {k + 1:,} of {len(groups):,} ({len(small):,} files of at most {bund[0]:,} bytes' + (f'; {kept:,} kept from the last port)' if kept else ')'))   # (pipeline: "linking …")
                write_bundle(tmp / name, g)
            st = (tmp / name).stat(); keys[name] = dict(key=key, size=st.st_size, mtime=st.st_mtime_ns)
        if progress and kept: progress(f'{kept:,} of {len(groups):,} bundles kept from the last port (their files did not change)')
        bundles = dict(max=bund[0], size=bund[1], count=len(groups), files=len(small), bytes=sum(f[2] for f in small),
                       tar_bytes=sum((tmp / bundle_name(k)).stat().st_size for k in range(len(groups))))
    if shaders: link_or_copy(shaders, tmp / 'shaders.dmsv'); n += 1; b += shaders.stat().st_size   # none for an OpenGL ES game
    for name, src in (files or {}).items(): shutil.copyfile(src, tmp / name); n += 1; b += Path(src).stat().st_size
    rows = [f'# Written by nx2android\'s porter for {profile.name} ({profile.id}); KEY=VALUE per line, read by the app at launch',
            f'DM_LANG={language}']
    if aoc: rows.append(f'DM_AOC_DIR={files_folder(package)}/aoc')
    if sysdata: rows.append(f'DM_SYSDATA={files_folder(package)}/sysdata')
    rows += profile.env + list(env)
    (tmp / 'env.txt').write_text('\n'.join(rows) + '\n', encoding='utf-8', newline='\n')
    for v, (_, venv) in variants.items():
        if venv: (tmp / f'env-{v}.txt').write_text(f'# variant {v}: applied after env.txt when the app runs mods-{v}/\n' + '\n'.join(venv) + '\n', encoding='utf-8', newline='\n'); n += 1
    if sets := {**profile.settings, **(settings or {})}: (tmp / 'settings.txt').write_text(''.join(f'{k}={v}\n' for k, v in sets.items()), encoding='utf-8', newline='\n')
    man = dict(title_id=profile.id, name=profile.name, package=package, version=version, display=display, language=language, requires=profile.requires,
               files=n, bytes=b, dlc=sorted(int(p.name) for p in aoc.iterdir() if p.is_dir() and p.name.isdigit()) if aoc else [],
               sysdata=sorted(p.name for p in sysdata.iterdir() if p.is_dir()) if sysdata else [], variants=list(variants), made=time.strftime('%Y-%m-%d %H:%M:%S'))
    if bundles: man['bundles'] = bundles   # (no key at all for a pack without: its manifest stays as it was)
    if profile_h: man['profile_h'] = profile_h
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
