# Installing a port on a device over adb: the APK, then the data pack into the app's files folder (porter/adb.py pushes it in resumable pieces).
# A new install whose RomFS / DLC another port of the same game version on the device already has copies them there (seed_from_sibling: the manifest's content keys).
# The player's own files there are kept: the pause menu's settings.txt, the env.txt rows no pack defines (an install merges them in, merge_env), the logs, the Vulkan
# pipeline caches; saves are in the app's internal storage, which an update keeps.
# --romfs-at <folder>: the app reads the game's RomFS from a folder already on the device (a game dump an emulator also reads, a folder on the SD card) instead of a copy in
# its files folder, which saves that copy's 7 to 18 GB. The folder must hold exactly the pack's RomFS, names and sizes and a sample's content (--romfs-push puts it there), the app gets All files
# access over adb, and the installer writes DM_ROMFS=<folder> into the device's env.txt under ROMFS_MARK. The pack's env.txt is the same on every device, and the folder is
# a fact of one device, so the row is the installer's, not the pack's: a later install without --romfs-at carries it over from the device's env.txt (and checks the folder
# again), and --romfs-at app moves the RomFS back into the files folder. The DLC (aoc/) stays in the files folder: small for most games, and laid out by the port.
# A pack with bundles (android.tsv data_bundles: datapack.py): its bundles go into the files folder's bundles/ as they are, one transfer each, and the app takes the files
# out at its next start (dm/android/datasync.h), as it writes its own folder without the cost adb pays for each file (Android's FUSE layer); the files the pack has on their
# own go as before, then files.json (the list the app unpacks by) and the manifest last. A bundle is pushed only when the device lacks one of its files at its size, and
# one the device has at its size (an earlier push, not unpacked yet) is kept when its MD5 is the pack's, so a rerun goes on where the last one stopped. --romfs-at is not
# offered for such a pack (its RomFS is in its bundles).
# A pack with packs (android.tsv romfs_packs: datapack.py): packs/ holds plain files (the packs and their index), pushed as any file of the pack; the RomFS's small files
# an earlier pack had on their own are not the pack's and go as stale ones. --romfs-at is not offered for it either.
import hashlib, json, posixpath, re, shlex, tempfile, zipfile
from pathlib import Path

from porter import datapack, paths, publish
from porter.adb import Adb, AdbError, long_path
from porter.upstream import BuildError, PROG, say

# A pack of a profile with romfs zstd holds packed RomFS files (porter/romfs_pack.py), which only this app reads: pushed into a folder another app reads (--romfs-at, --romfs-push),
# they would replace that app's own files of the same names and it would stop working. Such a pack goes only into the app's files folder, or the app's own folder on the SD card (--romfs-at sd)
PACKED_ELSEWHERE = ('a data pack with a compressed RomFS (android.tsv romfs zstd) goes only into the app\'s own folders: its packed files would replace the files of a '
                    'RomFS folder that another app reads. Install it with --romfs-at app, or --romfs-at sd (the app\'s own folder on the SD card)')
def packed_romfs(data):   # whether the pack holds packed RomFS files (its env.txt turns the host's reader on)
    try: return 'DM_ROMFS_ZSTD=1' in (Path(data) / 'env.txt').read_text(encoding='utf-8').split()
    except OSError: return False
APP = 'app'                                                  # --romfs-at app: the RomFS in the app's files folder (the default)
NO_SD = 'no-sd'                                              # --romfs-at no-sd: the RomFS back in the app's files folder when an earlier --romfs-at sd install put it on the SD card; otherwise as without --romfs-at
SD = 'sd'                                                    # --romfs-at sd: the RomFS in <the SD card>/Android/data/<package>/files/RomFS, pushed there: Android deletes that folder when the app is uninstalled (the APK stays in internal storage: Android 13 installs apps only to an adopted SD card)
ROMFS_MARK = '# porter install --romfs-at'                  # in the device's env.txt: the DM_ROMFS row under it is the installer's, carried over by later installs
ACCESS = 'android.permission.MANAGE_EXTERNAL_STORAGE'        # All files access, which the APK asks for (mkapk.py) and the installer grants
ACCESS_HOW = 'Settings > Apps > {name} > All files access (Android 10: Permissions > Files and media)'
# The room a pack with bundles needs beyond what is pushed, as the app counts it before it unpacks (nxnative/dm/android/datasync.h ds_apply: DS_UNPACKERS, DS_FILE_SLACK and its
# 64 MB margin; keep the three the same): the bundles unpacked at once each beside its files, f2fs's node and block rounding for every file written, a margin
UNPACKERS, FILE_SLACK, MARGIN = 8, 4096, 64 << 20
LIST_FILES = (publish.FILES, publish.FILES + '.etag', publish.FILES + '.prev', datapack.MANIFEST)   # what tells the app a pack is whole: the list (with its validators and the one before it), the manifest
SAMPLE_FILES, SAMPLE_MAX = 256, 4 << 20                      # the folder's content check: up to this many of its files, each of at most this many bytes (romfs_sample)


def card_folder(d, package):
    """Whether d is the app's own RomFS folder on a portable volume, the one --romfs-at sd makes: /storage/<volume id>/Android/data/<package>/files/RomFS. Android deletes it
    with the app, and no other app reads it, so the installer handles it as its own (pushes what changed, removes what is not the pack's)"""
    if not d: return False
    m = re.fullmatch(r'/storage/([^/]+)/Android/data/' + re.escape(package) + '/files/' + re.escape(datapack.ROMFS), posixpath.normpath(d))
    return bool(m) and m.group(1) not in ('emulated', 'self')   # those two are internal storage, where the files folder is


def romfs_target(arg, files=None):
    """--romfs-at's folder, checked before anything touches the device: None for the app's files folder (app, or that folder's own RomFS). files: the app's files folder"""
    if arg is None or arg in (APP, SD, NO_SD): return None
    if arg != arg.strip() or any(c in arg for c in '\r\n\0') or not arg.startswith('/'):
        raise BuildError(f'--romfs-at takes a folder on the device as an absolute path (/sdcard/…, /storage/…), or app: {arg!r}')
    d = posixpath.normpath(arg)
    if files and d == f'{files}/{datapack.ROMFS}': return None
    if files and (d + '/').startswith(files + '/'): raise BuildError(f'--romfs-at {d}: the app\'s files folder is the installer\'s own; --romfs-at app keeps the RomFS there')
    if not d.startswith(('/sdcard/', '/storage/')): raise BuildError(f'--romfs-at {d}: the app can read only shared storage, /sdcard/… or /storage/… (an SD card: /storage/<its id>/…)')
    own = files and card_folder(d, files.split('/')[-2])   # the app's own folder on the SD card (--romfs-at sd): Android removes it with the app
    if not own and re.search(r'/Android/(data|obb)(/|$)', d, re.I):   # FUSE keeps every app out of the others' Android/data and Android/obb, All files access or not
        raise BuildError(f'--romfs-at {d}: Android lets no app read a folder under another app\'s Android/data or Android/obb, even with All files access: '
                         'move the game\'s files elsewhere (/sdcard/Games/…), or keep them in this app\'s files folder with --romfs-at app')
    if len(d.encode()) > 512: raise BuildError(f'--romfs-at: a folder path of at most 512 bytes (the host\'s path buffers): {d}')
    return d


def device_romfs(env_text):   # the folder an earlier --romfs-at install wrote into the device's env.txt (the DM_ROMFS row under ROMFS_MARK), None when there is none
    lines, out = env_text.splitlines(), None
    for prev, ln in zip([''] + lines, lines):
        if prev.startswith(ROMFS_MARK) and ln.startswith('DM_ROMFS='): out = ln[len('DM_ROMFS='):].rstrip() or None
    return out


def env_with_romfs(pack_env, folder):   # the pack's env.txt with the installer's row last: the app applies the rows in order (front_android.c env_file), so it wins
    return pack_env.rstrip('\n') + f'\n{ROMFS_MARK}: the game\'s RomFS is read from this folder (later installs keep it; --romfs-at app moves it into the files folder)\nDM_ROMFS={folder}\n'


def manifest_env_keys(manifest_text):   # the env.txt keys the pack already on the device defined (its nx2a-data.json's env_keys, datapack.build), None when unknown (no manifest there, not JSON, no list: a pack from before env_keys)
    try: m = json.loads(manifest_text)
    except ValueError: return None
    keys = m.get('env_keys') if isinstance(m, dict) else None
    return [k for k in keys if isinstance(k, str)] if isinstance(keys, list) else None


def merge_env(pack_env, device_env, romfs=None, prev_pack_keys=None):
    """The pack's env.txt merged with the device's, which an install writes in its place (the pack's own env.txt is not changed): the pack's rows win for the keys they
    define (the app applies the rows in order, the last wins), and the device's rows for keys the pack does not define are kept verbatim after them, in their order, comments
    and blank lines too (one the pack already has is not kept twice, so a reinstall changes nothing). prev_pack_keys: the keys the pack previously on the device defined
    (the manifest's env_keys) — a device row with one of those keys that this pack no longer defines was the pack's, not the player's (a diagnostic switch since removed), and
    is dropped. None when unknown (no manifest on the device, or one from before env_keys): nothing says which rows were the old pack's, so env.txt is replaced as before
    this merge existed, the pack's rows alone; merging would keep the old pack's dropped rows for good (the new manifest does not list them). Either way the device's DM_ROMFS
    row goes, with its mark: it is the installer's row by design (device_romfs reads only the marked one, and the RomFS folder is checked and granted only for it), and the new one
    is appended last (romfs; None: none, --romfs-at app)"""
    if prev_pack_keys is None: return env_with_romfs(pack_env, romfs) if romfs else pack_env
    pack_lines = pack_env.splitlines()
    keys = {k for ln in pack_lines if (k := datapack.env_key(ln)) is not None}
    have = set(pack_lines)
    prev_keys = set(prev_pack_keys)
    kept = []
    for ln in device_env.splitlines():
        k = datapack.env_key(ln)
        drop = ln.startswith(ROMFS_MARK)   # the installer's comment goes with its row
        if k is not None and (k in keys or k in prev_keys or k == 'DM_ROMFS'): drop = True
        elif k is None and ln in have: drop = True   # a comment or blank line the pack already has
        if not drop: kept.append(ln)
    out = pack_env.rstrip('\n') + '\n' + ('\n'.join(kept) + '\n' if kept else '')
    return env_with_romfs(out, romfs) if romfs else out


def romfs_diff(want, have):   # want, have: {path in the RomFS: size} → (missing, of another size, not the game's), each sorted
    return sorted(want.keys() - have.keys()), sorted(r for r in want.keys() & have.keys() if want[r] != have[r]), sorted(have.keys() - want.keys())


def romfs_sample(rom, n=SAMPLE_FILES, max_size=SAMPLE_MAX):
    """The RomFS files whose content the installer compares by MD5 (romfs_changed): up to n of at most max_size bytes, spread evenly over the sorted paths. Names and
    sizes alone let a dump of another game version through when the files it changed kept their sizes; a sample of the small files catches that (at most SAMPLE_FILES x SAMPLE_MAX = 1 GB read on each side: seconds on internal storage, longer on an SD card)"""
    small = sorted(r for r, s in rom.items() if s <= max_size)
    return small if len(small) <= n else [small[i * len(small) // n] for i in range(n)]


def romfs_changed(adb, data, target, rom, there):
    """The sample's files that target holds at the pack's size but with other bytes (MD5), sorted; None when the device gave no MD5 at all (nothing compared)"""
    sample = [r for r in romfs_sample(rom) if there.get(r) == rom[r]]
    if not sample: return []
    got = adb.remote_md5(target, sample)
    if not got: return None
    return sorted(r for r in sample if r in got and got[r] != hashlib.md5(Path(long_path(Path(data) / datapack.ROMFS / r)).read_bytes()).hexdigest())


def romfs_mismatch(folder, want, have, n=5, changed=()):
    """What keeps folder's listing (have) from being the pack's RomFS (want), the first n of each kind; changed: files of the right size whose content differs
    (romfs_changed). '' when it is the same"""
    missing, other, extra = romfs_diff(want, have)
    if not have: return f'{folder} holds no files (it is not on the device, or it is empty)'
    first = lambda xs, f=str: ', '.join(f(x) for x in xs[:n]) + (', …' if len(xs) > n else '')
    sizes = lambda r: f'{r}: {have[r]:,} bytes, the pack has {want[r]:,}'
    parts = ([f'{len(missing):,} of the game\'s files are not there ({first(missing)})'] if missing else []) + \
            ([f'{len(other):,} have another size ({first(other, sizes)})'] if other else []) + \
            ([f'{len(changed):,} have the right size but other content (MD5: {first(list(changed))})'] if changed else []) + \
            ([f'{len(extra):,} are not the game\'s ({first(extra)})'] if extra else [])
    return f'{folder} is not this pack\'s RomFS ({len(want):,} files): ' + '; '.join(parts) if parts else ''


def bundle_plan(fj, have):
    """A pack with bundles on a device: (the bundles it needs, by index: those holding a file the device lacks at its size; of them, those the device has already at their
    size). fj: the pack's files.json, have: remote_sizes of the files folder"""
    full = [True] * len(fj['bundles'])
    for f in fj['files']:
        if 'bundle' in f and have.get(f['path']) != f['size']: full[f['bundle']] = False
    need = [k for k, ok in enumerate(full) if not ok]
    return need, [k for k in need if have.get(fj['bundles'][k]['path']) == fj['bundles'][k]['size']]
def sibling_parts(adb, man, files):
    """{part: files folder of another port on the device whose pack has the same content key for that part} (datapack.part_key), for the parts this pack has"""
    want = man.get('parts') or {}
    if not want: return {}
    out = {}
    lst = adb.shell('ls -d /sdcard/Android/data/*/files/nx2a-data.json 2>/dev/null; true', check=False)
    for path in lst.split():
        folder = path.rsplit('/', 1)[0]
        if folder == files: continue
        try: other = json.loads(adb.read_text(path) or 'null')
        except ValueError: continue
        if not isinstance(other, dict) or other.get('title_id') != man.get('title_id'): continue
        for part, k in want.items():
            if part not in out and (other.get('parts') or {}).get(part, {}).get('key') == k['key']: out[part] = folder
    return out


def seed_from_sibling(adb, man, files, have, data):
    """The parts the files folder does not have yet (RomFS, aoc), copied on the device from another port of the same game version (sibling_parts), instead of pushed
    over adb: a test build beside a release build of the game takes minutes, not the 7 to 18 GB push. Each copy is checked: its files and sizes, and the MD5 of a
    sample against the pack; a copy that differs is deleted and the part is pushed as before. Returns the parts copied"""
    copied = []
    empty = [p for p in (man.get('parts') or {}) if not any(r.startswith(p + '/') for r in have)]
    if not empty: return copied
    for part, folder in sibling_parts(adb, man, files).items():
        if part not in empty: continue
        k = man['parts'][part]
        there = adb.remote_sizes(f'{folder}/{part}')
        if len(there) != k['files'] or sum(there.values()) != k['bytes']: say(f'{folder}/{part} is not the whole {part} its pack says (an earlier --romfs-at or a partial install): pushing it'); continue
        PROG.set(step=f'copying the {part} on the device from {folder.split("/")[-2]} ({k["bytes"] / 2**30:.1f} GB)')
        adb.shell(f'mkdir -p {shlex.quote(files)} && cp -r {shlex.quote(folder + "/" + part)} {shlex.quote(files + "/")}', timeout=3600)
        now = adb.remote_sizes(f'{files}/{part}'); local = {p.relative_to(Path(data) / part).as_posix(): p.stat().st_size for p in (Path(data) / part).rglob('*') if p.is_file()}
        bad = now != local
        if not bad:
            sample = romfs_sample(local)
            got = adb.remote_md5(f'{files}/{part}', sample) if sample else {}
            bad = any(got.get(r) != hashlib.md5((Path(data) / part / r).read_bytes()).hexdigest() for r in sample)
        if bad:
            adb.shell(f'rm -rf {shlex.quote(files + "/" + part)}', timeout=1200); say(f'[RED] the {part} copied from {folder} differs from the pack: deleted, pushed instead'); continue
        say(f'copied the {part} from {folder} on the device ({k["files"]:,} files, {k["bytes"] / 2**30:.1f} GB): not pushed'); copied.append(part)
    return copied


def on_card(adb, package, files, romfs_at):
    """The folder the pack goes into: the app's own folder on the SD card when the player keeps its data there (datapack.device_files: the app's pointer), else files.
    --romfs-at, a RomFS outside the data folder, is for data in internal storage only"""
    d = datapack.device_files(adb, package)
    if d != datapack.files_folder(package):
        if romfs_at not in (None, APP, NO_SD): raise BuildError(f'the app keeps its game data on the SD card ({d}), where the RomFS is too: install without --romfs-at '
                                                                 '(or choose internal storage in the app first: App settings > Data location)')
        return d
    return files


def apk_asks_access(apk):   # whether the APK asks for All files access: one made before mkapk.py did cannot be given it (`appops set` takes, but FUSE checks the permission)
    try:
        with zipfile.ZipFile(apk) as z: m = z.read('AndroidManifest.xml')
    except (OSError, KeyError, zipfile.BadZipFile): return False
    return ACCESS.encode('utf-16-le') in m or ACCESS.encode() in m


def install(apk, data, adb_exe, serial=None, replace_signer=False, romfs_at=None, romfs_push=False):
    """romfs_at: a folder on the device that holds the game's RomFS (romfs_push: push the pack's RomFS into it first), 'app' for the app's files folder, None: as the
    device has it (an earlier --romfs-at install's folder, else the files folder)"""
    apk, data = Path(apk), Path(data)
    man = datapack.read_manifest(data)
    if not man: raise BuildError(f'not a data pack (no {datapack.MANIFEST}): {data}')
    if not apk.is_file(): raise BuildError(f'no APK: {apk}')
    if not adb_exe: raise BuildError('adb not found: install Android platform-tools, or set its path with `porter config adb <path>`')
    files = datapack.files_folder(man['package'])
    target = romfs_target(romfs_at, files)
    sd = romfs_at == SD
    if romfs_push and not target and not sd: raise BuildError('--romfs-push needs --romfs-at <a folder on the device outside the app\'s files folder>')
    bund = datapack.bundled(man)
    no_romfs_at = f'{man["name"]}\'s pack keeps its small files in bundles (android.tsv data_bundles), which the app unpacks into its files folder: --romfs-at is not offered for it'
    if man.get('packs'): no_romfs_at = f'{man["name"]}\'s pack keeps its RomFS\'s small files in packs the host reads in place (android.tsv romfs_packs): --romfs-at is not offered for it'
    if (bund or man.get('packs')) and (target or sd): raise BuildError(f'{no_romfs_at}; install without --romfs-at')
    packed = packed_romfs(data)
    if packed and target: raise BuildError(PACKED_ELSEWHERE)   # --romfs-at sd is the app's own folder on the card (no other app reads it): a packed pack goes there
    adb = Adb(adb_exe, serial, log=say)
    PROG.stage('install', step='listing the pack' if bund else 'finding the device')
    fj = publish.manifest(data) if bund else None   # files.json, by which the app unpacks the bundles (the port wrote it: its hashes are kept, nothing is read again)
    if bund: PROG.set(step='finding the device')
    pushing, lock = False, None
    try:
        dev = adb.pick(); lock = adb.hold(paths.user_dir())   # one install per device at a time: released in the finally below
        files = on_card(adb, man['package'], files, romfs_at)   # the app's data on the SD card (its App settings): the pack goes there
        target = romfs_target(romfs_at, files)
        model = adb.prop('ro.product.model')
        say(f'device: {model or dev} ({dev}), GPU {adb.gpu() or "unknown"}')
        if 'bcn' in man.get('requires', []):   # the profile's requires: BC-compressed textures
            bc = adb.bc_textures()
            if bc is False: raise BuildError(f'{man["name"]} needs BC-compressed textures, which this device\'s GPU ({adb.gpu()}) cannot read')
            if bc is None: say(f'[RED] {man["name"]} needs BC-compressed textures; whether this GPU ({adb.gpu() or "unknown"}) reads them is not known')
        if sd:
            vol = adb.sd_volume()
            if not vol: raise BuildError('--romfs-at sd: no SD card (portable volume) is mounted on the device')
            romfs_at = f'{vol}/Android/data/{man["package"]}/files/{datapack.ROMFS}'; target = romfs_target(romfs_at, files); romfs_push = True
            say(f'RomFS on the SD card (the first mounted portable volume, an SD card before a USB drive): {target}')
        dev_env = adb.read_text(f'{files}/env.txt') or ''
        was = device_romfs(dev_env)   # an earlier --romfs-at install's folder
        if was and posixpath.normpath(was) == f'{files}/{datapack.ROMFS}': was = None   # the data folder's own RomFS (the data moved to the card that held it): no folder of its own
        prev_keys = manifest_env_keys(adb.read_text(f'{files}/{datapack.MANIFEST}'))   # the pack already on the device: which env rows were its, not the player's. Read before the APK goes in:
        # --replace-signer uninstalls the app first, and Android deletes the files folder with it (the device's manifest is pushed last, so the one there is the previous pack's)
        sd_dir = card_folder(was, man['package'])   # the folder an earlier --romfs-at sd made
        if sd_dir: was = posixpath.normpath(was)
        if romfs_at == NO_SD:
            romfs_at = APP if sd_dir else None
        if romfs_at is None and was:
            target = romfs_target(was, files)
            if target and (bund or man.get('packs')): raise BuildError(f'{no_romfs_at}, and an earlier install had the app read its RomFS from {target}: install with --romfs-at app')
            if packed and target and not sd_dir: raise BuildError(f'the app reads its RomFS from {target} (an earlier install\'s --romfs-at). ' + PACKED_ELSEWHERE)
            if target: say(f'the app reads its RomFS from {target} (an earlier install\'s --romfs-at; --romfs-at app moves it into its files folder)')
        own = card_folder(target, man['package'])   # the installer's own folder on the card: what changed is pushed and what is not the pack's removed, as in the files folder
        if own:
            card = '/'.join(posixpath.normpath(target).split('/')[:3])   # /storage/<volume id>: Android removes it while that card is not mounted
            if not adb.is_dir(card):   # taken out, or another card: the listing would be empty and the push would go into /storage's tmpfs
                raise BuildError(f'the SD card {card.rsplit("/", 1)[-1]} that holds the RomFS ({target}) is not in the device: insert it, or install with --romfs-at app')
            romfs_push = True
        rom = datapack.romfs_sizes(data) if target else {}
        missing = other = changed = extra = []; there = {}
        if target:   # what can refuse comes before the APK goes in, so a refused install changes nothing on the device (but a new app's grant: below)
            if not rom: raise BuildError(f'{data} has no RomFS to compare {target} with')
            if not apk_asks_access(apk): raise BuildError(f'{apk.name} does not ask for All files access, which reading {target} needs (a port made by an earlier nx2android): port the game again')
            PROG.set(step=f'comparing {target} with the pack\'s RomFS')
            there = adb.remote_sizes(target)
            missing, other, extra = romfs_diff(rom, there)
            if (own or not extra) and (romfs_push or not (missing or other)):   # a listing that can be used: a sample of the files compared by content too
                PROG.set(step=f'comparing the content of {target} with the pack\'s RomFS')
                changed = romfs_changed(adb, data, target, rom, there)
                if changed is None: say(f'[RED] the device gave no MD5 of the files in {target}: only their names and sizes were compared with the pack\'s RomFS'); changed = []
            if bad := romfs_mismatch(target, rom, there, changed=changed):
                if (extra and not own) or not romfs_push:
                    whose = f'{man["name"]} {man.get("display") or man["version"]}'
                    raise BuildError(f'{bad}. ' + (f'It came from an earlier install\'s --romfs-at. ' if romfs_at is None else '') + f'Give --romfs-at the folder of {whose}\'s RomFS'
                                     + (' (--romfs-push adds files, it never deletes any: empty the folder, or choose another)' if extra else
                                        ', add --romfs-push to put the pack\'s RomFS into that folder') + ', or --romfs-at app to keep the RomFS in the app\'s files folder')
                say(f'{target}: {len(missing) + len(other) + len(changed):,} of the RomFS\'s {len(rom):,} files to push' + (f', {len(extra):,} files that are not the pack\'s to remove' if extra else '') + ('' if own else ' (--romfs-push)'))
            else: say(f'{target} holds this pack\'s RomFS ({len(rom):,} files, {sum(rom.values()) / 2**30:.1f} GB): it is not pushed')
        free = adb.free_bytes(files)
        if files == datapack.files_folder(man['package']) and not adb.installed(man['package']) and (vol := adb.sd_volume()) and (cf := adb.free_bytes(vol)) is not None:
            free = max(free or 0, cf)   # a new app puts its data on the volume with the most room at its first start (below: the room is checked again there)
        have = adb.remote_sizes(files)
        was_packed = 'DM_ROMFS_ZSTD=1' in dev_env.split()
        in_files = any(r.startswith(datapack.ROMFS + '/') for r in have)
        if target and in_files and (missing or other or changed): say(f'the RomFS is copied again into {target}: the app\'s files folder holds a copy, and {"the SD card" if sd else "another folder"} needs its own')
        elif target and there and (other or changed) and was_packed != packed: say(f'the RomFS in {target} is copied again: it was {"compressed" if was_packed else "not compressed"} and this pack\'s is {"compressed" if packed else "not compressed"}')
        elif not target and was and romfs_at == APP and (sd_dir or in_files is False): say(f'the RomFS is copied again from {was} into the app\'s files folder' + (f' ({"compressed" if packed else "not compressed"} pack)' if packed != was_packed else ''))
        if target:   # the RomFS is not pushed into the files folder; a copy an earlier install left there goes only after env.txt points elsewhere, so it is not counted as freed
            need = man['bytes'] - sum(rom.values()) - sum(s for r, s in have.items() if not r.startswith(datapack.ROMFS + '/'))
            if push := sum(rom[r] for r in missing + other + changed):
                if adb.same_volume('/sdcard', target): need += push
                elif (tfree := adb.free_bytes(target)) is not None and push > tfree: raise BuildError(f'{target}\'s volume has {tfree / 2**30:.1f} GB free; the RomFS needs {push / 2**30:.1f} GB more')
        elif bund:   # the bundles it pushes and the files it pushes on their own, and room for one bundle's files while the app unpacks (each bundle goes once its files are out)
            bneed, bthere = bundle_plan(fj, have)
            loose = [f for f in fj['files'] if 'bundle' not in f]
            nwrite = sum(1 for f in fj['files'] if have.get(f['path']) != f['size'])   # the files the device lacks at their size: written by the push or by the unpacking
            need = sum(fj['bundles'][k]['size'] for k in bneed if k not in bthere) + sum(f['size'] for f in loose if have.get(f['path']) != f['size']) \
                + min(UNPACKERS, len(bneed)) * max((fj['bundles'][k]['size'] for k in bneed), default=0) + nwrite * FILE_SLACK + MARGIN
        elif man.get('packs'):   # the pack's files the device lacks at their size, the packs among them: the RomFS's small files an earlier pack left on their own go only after
            # the push (stale, below), so they are not counted as freed (man['bytes'] counts the small files, not the packs that hold them)
            need = sum(s for r, s in ((p.relative_to(data).as_posix(), p.stat().st_size) for p in data.rglob('*') if p.is_file()) if have.get(r) != s)
        else: need = man['bytes'] - sum(have.values())
        if free is not None and need > free: raise BuildError(f'the device has {free / 2**30:.1f} GB free; the game needs {need / 2**30:.1f} GB more')
        if target and not replace_signer and adb.installed(man['package']):   # an update: the grant (the app's uid's, which the update keeps) before the APK goes in
            PROG.set(step='granting the app All files access')
            ok, state = adb.storage_access(man['package'], grant=True)
            if not ok and adb.requests_permission(man['package'], ACCESS if adb.sdk() >= 30 else 'android.permission.READ_EXTERNAL_STORAGE'):
                raise BuildError(f'the app needs All files access to read {target}, and the device did not grant it over adb ({state}): allow it on the device '
                                 f'({ACCESS_HOW.format(name=man["name"])}), then install again')
        PROG.set(step=f'installing {apk.name}')
        adb.install(apk, man['package'], replace_signer=replace_signer)
        PROG.set(step='letting the app create its files folder')
        if adb.prepare_app_files(man['package']): say('started the app once so Android made its files folder (it stops at once without its data)')
        if (now := on_card(adb, man['package'], files, romfs_at)) != files:   # a new app chose the SD card at its first start (the volume with the most room)
            say(f'the app keeps its game data on the SD card: {now}')
            if target: raise BuildError(f'the app keeps its game data on the SD card ({now}): install without --romfs-at')
            files = now
            if (f := adb.free_bytes(files)) is not None and need > f: raise BuildError(f'the SD card has {f / 2**30:.1f} GB free; the game needs {need / 2**30:.1f} GB more')
        if target:   # after the install too: a new package, one --replace-signer made again, or an update from an APK that did not ask for the access (granted above
            # when it did) has no grant yet. Before the push, so a refusal leaves the data as it was: a new app has none, and an older APK's update is the one case
            # that cannot be refused before the APK goes in
            PROG.set(step='granting the app All files access')
            ok, state = adb.storage_access(man['package'], grant=True)
            if not ok: raise BuildError(f'the app needs All files access to read {target}, and the device did not grant it over adb ({state}): allow it on the device '
                                        f'({ACCESS_HOW.format(name=man["name"])}), then install again')
            say(f'{state}: the app reads its RomFS from {target}')
        if own and (now := adb.remote_sizes(target)) != there:   # again: an install with --replace-signer uninstalls first, and Android deletes the card folder with the app
            say(f'{target} changed during the install (an uninstall deletes it with the app): {len(now):,} of its {len(there):,} files are left')
            there = now; missing, other, extra = romfs_diff(rom, there); changed = [r for r in changed if there.get(r) == rom[r]]
        PROG.set(step='pushing the game data', done=0, total=man['bytes'] - sum(rom.values()), unit='B'); pushing = True
        have = adb.remote_sizes(files)   # again: an install with --replace-signer uninstalls first, and Android deletes the files folder with the app
        if bund:   # the list and the manifest first: a device whose install stops after the bundles went in then has neither, so the app cannot take the folder for a whole pack
            # (and delete the bundles); it says the install did not finish. They are written again at the end, the list first
            adb.shell(f'cd {shlex.quote(files)} && rm -f ' + ' '.join(LIST_FILES))
        elif not target and seed_from_sibling(adb, man, files, have, data): have = adb.remote_sizes(files)   # another port of the game on the device gives its RomFS / DLC (a pack without bundles)
        PROG.set(step='pushing the game data')
        base, bkeep = push_bundles(adb, data, files, fj, have, lambda d, t, note: PROG.set(done=d, total=t, note=note)) if bund else (0, set())
        adb.push_tree(data, files, keep=datapack.KEEP_ON_DEVICE, skip=(datapack.MANIFEST, datapack.ENV) + ((publish.FILES,) if bund else ()),
                      exclude=(datapack.ROMFS,) if target else (datapack.BUNDLES,) if bund else (),
                      verify=lambda rel: rel.split('/', 1)[0] not in ('RomFS', 'aoc'),   # the game's own files change only with their version
                      progress=lambda d, t, note: PROG.set(done=base + d, total=base + t, note=note), have=have)
        if own and extra:   # files of an older game version in the installer's own card folder, which the game would still find
            PROG.set(step=f'removing {len(extra):,} files from {target} that are not the pack\'s')
            for i in range(0, len(extra), 64): adb.shell(f'cd {shlex.quote(target)} && rm -f ' + ' '.join(shlex.quote(r) for r in extra[i:i + 64]))
        if target and (missing or other or changed):   # --romfs-push: the pack's RomFS into the folder (changed: pushed again when their MD5 differs), then the folder checked again
            PROG.set(step=f'pushing the RomFS into {target}', done=0, total=sum(rom[r] for r in missing + other + changed), unit='B')
            again = set(changed)
            adb.push_tree(data / datapack.ROMFS, target, verify=lambda rel: rel in again, progress=lambda d, t, note: PROG.set(done=d, total=t, note=note), have=there)
            now = adb.remote_sizes(target)
            if bad := romfs_mismatch(target, rom, now, changed=romfs_changed(adb, data, target, rom, now) or []): raise BuildError(f'after the push, {bad}')
        # env.txt goes on its own, merged with the device's (merge_env): the pack's rows win for their keys, the player's rows for the others are kept, never replaced whole.
        # Read again here: an install with --replace-signer uninstalled the app first, and Android deleted the files folder with it
        with tempfile.TemporaryDirectory(prefix='nx2a_env_') as t:
            env = Path(t) / datapack.ENV
            env.write_text(merge_env((data / datapack.ENV).read_text(encoding='utf-8'), adb.read_text(f'{files}/{datapack.ENV}'), target, prev_keys), encoding='utf-8', newline='\n')
            adb.push_file(env, f'{files}/{datapack.ENV}')
        if not target and was and romfs_at == APP:
            if sd_dir: PROG.set(step='removing the RomFS from the SD card'); adb.shell(f'rm -rf {shlex.quote(was)}', timeout=1200); say(f'the app reads its RomFS from its files folder again; {was} (the earlier --romfs-at sd copy) is removed')
            else: say(f'the app reads its RomFS from its files folder again; {was} is left as it is')
        if bund:   # the pack's files are its list's (the bundled ones are not in the pack folder); a bundle the device has that the app does not need goes too
            listed = {f['path'] for f in fj['files']}
            stale = [rel for rel in have if (datapack.owned(rel) and rel not in listed) or (rel.startswith(datapack.BUNDLES + '/') and rel not in bkeep)]
        else:
            stale = [rel for rel in have if datapack.owned(rel) and not (target and rel.startswith(datapack.ROMFS + '/')) and not (data / rel).is_file()]   # the push only adds the pack's own files, which are never stale
            # a pack without bundles over a device that holds one with (or the app's own download of bundles): the bundles folder is of no use, and a list that names bundles must
            # not stay (a pack with a files.json of its own writes it above; the manifest is the pack's, written last). check=False: a folder the app made is its own, adb cannot empty it
            if any(r.startswith(datapack.BUNDLES + '/') for r in have): adb.shell(f'rm -rf {shlex.quote(f"{files}/{datapack.BUNDLES}")}', timeout=1200, check=False)
            if not (data / publish.FILES).is_file() and any(r in have for r in LIST_FILES[:3]):
                adb.shell(f'cd {shlex.quote(files)} && rm -f ' + ' '.join(LIST_FILES[:3]))
        if stale:   # an older version's files, which the game would still find
            PROG.set(step=f'removing {len(stale):,} stale files')
            for i in range(0, len(stale), 64): adb.shell(f'cd {shlex.quote(files)} && rm -f ' + ' '.join(shlex.quote(r) for r in stale[i:i + 64]))
        if old := [r for r in have if r.startswith(datapack.ROMFS + '/')] if target else []:   # a second copy of the game, now that env.txt points at target
            PROG.set(step='removing the RomFS copy in the files folder')
            adb.shell(f'rm -rf {shlex.quote(f"{files}/{datapack.ROMFS}")}', timeout=1200)
            say(f'removed the earlier copy of the RomFS from the app\'s files folder ({len(old):,} files, {sum(have[r] for r in old) / 2**30:.1f} GB)')
        if bund: adb.push_file(data / publish.FILES, f'{files}/{publish.FILES}')   # the list the app unpacks by, once everything it names is on the device
        adb.push_file(data / datapack.MANIFEST, f'{files}/{datapack.MANIFEST}')   # last: a pack on the device with its manifest is a complete one
    except AdbError as e:
        try: gone = pushing and adb.serial in [s for s, st, _ in adb.devices() if st == 'device'] and not adb.installed(man['package'])   # a device that went away is not an uninstall
        except AdbError: gone = False
        if gone:   # Android deletes an app's files folder with the app: a push into it then fails part-way (a size mismatch)
            raise BuildError(f'{man["package"]} was uninstalled on the device while its data was being pushed ({e})') from e
        raise BuildError(str(e)) from e
    finally:
        if lock: lock.close()
    PROG.end(f'{man["name"]} on {model or dev}')
    say(f'installed: open "{man["name"]}" on the device' + (' (it unpacks its bundles at its first start)' if bund else ''))


def push_bundles(adb, data, files, fj, have, progress=None):
    """The bundles the device needs (bundle_plan) into its files folder's bundles/, one transfer each; one the device has at its size is kept when its MD5 is the pack's
    (pushed by an install that stopped, or not unpacked yet). progress(done, total, note) after each. Returns (the bytes pushed, the bundles the device keeps: their paths)"""
    bun = fj['bundles']; need, there = bundle_plan(fj, have)
    got = adb.remote_md5(files, [bun[k]['path'] for k in there]) if there else {}
    kept = {k for k in there if got.get(bun[k]['path']) == hashlib.md5(Path(long_path(data / bun[k]['path'])).read_bytes()).hexdigest()}
    todo = [k for k in need if k not in kept]; total = sum(bun[k]['size'] for k in todo)
    say(f'{len(todo):,} of the pack\'s {len(bun):,} bundles to push, {total / 2**20:,.0f} MB ({len(kept):,} already on the device, {len(bun) - len(need):,} whose files it has)')
    q = shlex.quote(f'{files}/{datapack.BUNDLES}')
    # a bundles folder an APK from before 2026-11-15 made (it began downloading bundles itself) is the app's alone (drwxr-s---): adb's shell can neither write in it nor
    # empty it. Said so at once, before a push that cannot land is tried three times
    if todo and adb.shell(f'[ -e {q} ] && [ ! -w {q} ] && echo locked; true', check=False).strip() == 'locked':
        raise AdbError(f'{files}/{datapack.BUNDLES} was made by the app itself (it began downloading bundles from its server) and adb cannot write in it: start the app '
                       'with its server reachable and let it finish (it removes the folder), or clear the app\'s data, then install again')
    if todo: adb.shell(f'mkdir -p {q}')
    done = 0
    for i, k in enumerate(todo):
        adb.push_whole(data / bun[k]['path'], f'{files}/{bun[k]["path"]}'); done += bun[k]['size']
        if progress: progress(done, total, f'bundle {i + 1:,}/{len(todo):,}')
    if todo:
        now = adb.remote_sizes(f'{files}/{datapack.BUNDLES}')
        short = [bun[k]['path'] for k in todo if now.get(bun[k]['path'].split('/', 1)[1]) != bun[k]['size']]
        if short: raise AdbError(f'{len(short)} bundles are not on the device at their size after the push ({", ".join(short[:3])}): install again (it goes on from there)')
    return total, {bun[k]['path'] for k in need}
