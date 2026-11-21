# A data pack with bundles (android.tsv data_bundles): porter/datapack.py packs the small files of the game's trees into tar bundles, porter/publish.py lists them
# (files.json format 2), and nxnative/dm/android/datasync.h (built on the computer: datasync_fixture.c, with a folder as the server) downloads what the folder lacks,
# unpacks the bundles (downloaded, or pushed by porter install) and refuses what is not the list's. A pack without the row is checked to stay as it was.
# python -m unittest tests.test_datasync (a C compiler: $CC, else cc, clang or gcc; without one the sync's half is skipped, and says which names were tried)
import atexit, base64, hashlib, io, json, os, shutil, subprocess, sys, tarfile, tempfile, types, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO)); sys.path.insert(0, str(Path(__file__).resolve().parent))
from porter import datapack, publish   # noqa: E402
from porter.upstream import BuildError   # noqa: E402

PKG = 'org.nx2a.bundletest'
MAX, SIZE = 2000, 24 * 1024   # files of at most 2,000 bytes into bundles of up to 24 KB: many bundles from a small pack
BEFORE = '314fd85231d068bb91627761aa3a4a4137115917'   # the last datasync.h from before bundles (an app ported before them)
CC_TRIED = [n for n in (os.environ.get('CC'), 'cc', 'clang', 'gcc') if n]   # the compiler: $CC, else the first of these on PATH
CC = next((w for w in map(shutil.which, CC_TRIED) if w), None)
NO_CC = f'no C compiler (tried {", ".join(CC_TRIED)})'
UNPACKERS = 8   # DS_UNPACKERS of datasync.h (the harness is built with -DDS_PTHREADS)


def blob(n, seed): return hashlib.shake_256(f'{seed}'.encode()).digest(n) if n else b''


def source(d, changed=None):   # the game files: small and large under RomFS/ and aoc/, a long name (past tar's 100 bytes), a Korean one, an empty one; changed: {rel: bytes}
    files = {f'RomFS/Data/d{i % 6}/f{i:03d}.bin': blob(i * 13 % MAX + 1, i) for i in range(120)}
    files.update({'RomFS/Data/big.bin': blob(50_000, 'big'), 'RomFS/Data/edge.bin': blob(MAX, 'edge'), 'RomFS/Data/over.bin': blob(MAX + 1, 'over'), 'RomFS/empty.bin': b'',
                  'RomFS/' + '/'.join(['n' * 30] * 3) + '/긴 이름.bin': blob(700, 'long'), 'RomFS/한글/파일.bin': blob(300, 'ko'),
                  'aoc/1/a.bin': blob(400, 'aoc'), 'aoc/1/big.bin': blob(9000, 'aocbig'), 'mods/main': blob(1500, 'main'), 'mods/sdk': blob(30_000, 'sdk')})
    files.update(changed or {})
    for rel, b in files.items(): p = d / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b)
    return files


def profile(bundles=(MAX, SIZE)):
    return types.SimpleNamespace(name='Bundle Test', id='0100000000B00000', package=PKG, env=['DM_ADAPTIVE_FPS=0'], settings={'overlay': '1'}, requires=[], bundles=bundles)


def make_pack(t, name='Test-data', bundles=(MAX, SIZE), changed=None):
    src = t / f'src-{name}'; shutil.rmtree(src, ignore_errors=True); files = source(src, changed)
    dst = t / name
    datapack.build(dst, profile=profile(bundles), version='v65536', display='1.0.1', patched=src / 'mods', romfs=src / 'RomFS', shaders=None, aoc=src / 'aoc')
    return dst, files, publish.manifest(dst)


_built = {}
def fixture(old=False):   # the harness, once a run; old: with the datasync.h of BEFORE
    if old in _built: return _built[old]
    d = Path(tempfile.mkdtemp(prefix='nx2a_dsfx_')); atexit.register(shutil.rmtree, d, ignore_errors=True); exe = d / ('fx.exe' if os.name == 'nt' else 'fx'); args = []
    if old:
        r = subprocess.run(['git', '-C', str(REPO), 'show', f'{BEFORE}:nxnative/dm/android/datasync.h'], capture_output=True)
        if r.returncode: _built[old] = None; return None
        (d / 'datasync_old.h').write_bytes(r.stdout); args = [f'-DDS_H="{(d / "datasync_old.h").as_posix()}"']
    # -Wno-unused-function: the header's helpers the harness does not call (the address, the asset); -DDS_PTHREADS: the unpack's writer threads, as on Android
    r = subprocess.run([CC, '-O1', '-Wall', '-Wextra', '-Wno-unused-function', '-std=gnu11', '-pthread', '-DDS_PTHREADS', *args, '-I', str(REPO / 'nxnative/dm/android'),
                        str(Path(__file__).with_name('datasync_fixture.c')), '-o', str(exe)], capture_output=True, text=True)
    assert r.returncode == 0 and not (r.stderr.strip() and not old), r.stderr   # the new header builds without a warning
    _built[old] = exe; return exe


def sync(server, root, *a, old=False, timeout=None):   # timeout: seconds, else the test fails (a sync that does not end)
    try: r = subprocess.run([str(fixture(old)), str(server), str(root), PKG, *map(str, a)], capture_output=True, text=True, encoding='utf-8', timeout=timeout)
    except subprocess.TimeoutExpired: raise AssertionError(f'the sync did not end in {timeout} s (a loop)') from None
    lines = r.stdout.splitlines(); last = (lines or [''])[-1].split(' ', 10)
    if last[:1] != ['result']: return dict(result=None, text=r.stdout + r.stderr + f'\n(exit {r.returncode & 0xffffffff:#x})', fetched=[])
    return dict(result=last[1], files=int(last[3]), bytes=int(last[5]), space_need=int(last[7]), space_free=int(last[9]), err=last[10][4:] if len(last) > 10 else '', text=r.stdout,
                fetched=[ln.split(' ')[1] for ln in lines if ln.startswith('fetch ')])


def tree(d, skip=()):   # {rel: bytes} of the game's files in a folder
    return {p.relative_to(d).as_posix(): p.read_bytes() for p in d.rglob('*') if p.is_file() and p.relative_to(d).parts[0] in ('RomFS', 'aoc', 'mods') and p.relative_to(d).as_posix() not in skip}


class Pack(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = Path(tempfile.mkdtemp(prefix='nx2a_test_ds_'))
        cls.pack, cls.files, cls.fj = make_pack(cls.t)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.t, ignore_errors=True)

    def test_layout(self):   # the small files of RomFS/ and aoc/ only in bundles, in path order, each bundle within its size; the others on their own
        small = sorted(r for r, b in self.files.items() if len(b) <= MAX and r.split('/')[0] in datapack.BUNDLED)
        loose = {p.relative_to(self.pack).as_posix() for p in self.pack.rglob('*') if p.is_file()} - {publish.FILES}
        tars = sorted(r for r in loose if r.startswith('bundles/'))
        self.assertEqual(tars, [datapack.bundle_name(k) for k in range(len(tars))]); self.assertGreater(len(tars), 3)
        self.assertEqual(loose - set(tars), {r for r in self.files if r not in small} | {'env.txt', 'settings.txt', datapack.MANIFEST})   # mods/ stays loose, whatever its size
        members = []
        for r in tars:
            self.assertLessEqual((self.pack / r).stat().st_size, SIZE)
            with tarfile.open(self.pack / r, encoding='utf-8') as tar:
                for m in tar: self.assertTrue(m.isreg()); self.assertEqual((m.mode, m.mtime, m.uid), (0o644, 0, 0)); self.assertEqual(tar.extractfile(m).read(), self.files[m.name]); members.append(m.name)
        self.assertEqual(members, small)
        man = datapack.read_manifest(self.pack)
        self.assertEqual({k: man['bundles'][k] for k in ('max', 'size', 'count', 'files')}, dict(max=MAX, size=SIZE, count=len(tars), files=len(small)))
        self.assertEqual((man['files'], man['bytes']), (len(self.files), sum(map(len, self.files.values()))))   # the game's files, bundled or not (as without bundles)

    def test_list(self):   # format 2: every game file with its hash, the bundled ones first with their bundle, then the others; the bundles with their hashes and counts
        fj = self.fj; self.assertEqual(fj['format'], 2)
        on_disk = json.loads((self.pack / publish.FILES).read_text(encoding='utf-8')); self.assertEqual(on_disk['id'], fj['id'])
        self.assertEqual(fj['id'], publish.list_id(fj['files']))
        b = [f for f in fj['files'] if 'bundle' in f]; n = len(b)
        self.assertEqual(fj['files'][:n], b, 'the bundled files come first'); self.assertEqual([f['bundle'] for f in b], sorted(f['bundle'] for f in b))
        for f in fj['files']:
            want = self.files.get(f['path']) if f['path'] in self.files else (self.pack / f['path']).read_bytes()
            self.assertEqual((f['size'], f['sha256']), (len(want), hashlib.sha256(want).hexdigest()), f['path'])
        for k, bd in enumerate(fj['bundles']):
            raw = (self.pack / bd['path']).read_bytes(); mine = [f for f in b if f['bundle'] == k]
            self.assertEqual((bd['size'], bd['sha256'], bd['files'], bd['bytes']), (len(raw), hashlib.sha256(raw).hexdigest(), len(mine), sum(f['size'] for f in mine)))
        self.assertEqual(fj['bytes'], sum(f['size'] for f in fj['files']))
        self.assertEqual(publish.manifest(self.pack)['hashed'], 0)   # again: every hash kept (the bundles' files too)
        lines = (self.pack / publish.FILES).read_text(encoding='utf-8').splitlines()
        self.assertEqual(len(lines), len(fj['files']) + len(fj['bundles']) + 14)   # one line per file and per bundle

    def test_bundles_kept_across_ports(self):   # a port again from the same files keeps the earlier pack's bundles (hard links, same mtime: nothing hashed again); a source
        # file changed has its bundle written again, the others kept
        src = self.t / 'src-Test-data'; dst = self.t / 'Keep-data'
        build = lambda: datapack.build(dst, profile=profile(), version='v65536', display='1.0.1', patched=src / 'mods', romfs=src / 'RomFS', shaders=None, aoc=src / 'aoc')
        build(); publish.manifest(dst); first = {b: (dst / b).stat().st_mtime_ns for b in (p.relative_to(dst).as_posix() for p in (dst / 'bundles').iterdir())}
        self.assertTrue(datapack.reuse_file(dst).is_file())
        steps = []; datapack.build(dst, profile=profile(), version='v65536', display='1.0.1', patched=src / 'mods', romfs=src / 'RomFS', shaders=None, aoc=src / 'aoc', progress=steps.append)
        self.assertEqual({b: (dst / b).stat().st_mtime_ns for b in first}, first); self.assertIn(f'{len(first)} of {len(first)} bundles kept from the last port (their files did not change)', steps)
        self.assertEqual(publish.manifest(dst)['hashed'], 3)   # env.txt, settings.txt and the manifest, written again each port; no bundle
        rel = 'RomFS/Data/d1/f001.bin'; os.utime(src / rel, ns=((src / rel).stat().st_atime_ns, (src / rel).stat().st_mtime_ns + 10**9))
        build(); k = next(f['bundle'] for f in self.fj['files'] if f['path'] == rel)
        self.assertEqual([b for b in first if (dst / b).stat().st_mtime_ns != first[b]], [datapack.bundle_name(k)])
        fj = publish.manifest(dst); self.assertEqual([f for f in fj['files'] if 'bundle' in f], [f for f in self.fj['files'] if 'bundle' in f])   # (the same bytes)

    def test_same_files_same_bundles(self):   # a port again: the same bytes (the installer keeps a bundle on the device by its size and MD5)
        again, _, fj = make_pack(self.t, 'Again-data')
        self.assertEqual([b['sha256'] for b in fj['bundles']], [b['sha256'] for b in self.fj['bundles']])
        self.assertEqual([f for f in fj['files'] if 'bundle' in f], [f for f in self.fj['files'] if 'bundle' in f])

    def test_without_the_row(self):   # a profile without data_bundles: no bundles, files.json format 1 as before (every file on its own, path order), no bundles key
        d, files, fj = make_pack(self.t, 'Plain-data', bundles=None)
        self.assertFalse((d / 'bundles').exists()); self.assertNotIn('bundles', datapack.read_manifest(d))
        self.assertEqual(list(fj), ['format', 'id', 'package', 'version', 'display', 'variants', 'bytes', 'made', 'files', 'hashed'])
        self.assertEqual(fj['format'], 1); self.assertTrue(all(set(f) == {'path', 'size', 'sha256', 'mtime'} for f in fj['files']))
        self.assertEqual({f['path'] for f in fj['files']} - {'env.txt', 'settings.txt', datapack.MANIFEST}, set(files))
        self.assertTrue((d / publish.FILES).read_text(encoding='utf-8').startswith('{\n "format": 1,\n "id": '))   # json.dumps(indent=1), as before

    def test_profile_row(self):   # android.tsv data_bundles: checked, read as (max, size)
        sys.path.insert(0, str(REPO / 'nxnative/dm/android/tools')); import android_profile
        g = Path(tempfile.mkdtemp(prefix='nx2a_prof_', dir=self.t)) / '0100000000B00000'; g.mkdir()
        (g / 'game.tsv').write_text('title_id\t0100000000B00000\nname\tBundle Test\n', encoding='utf-8')
        for row, want in (('1048576\t67108864', (1048576, 67108864)), ('', None)):
            (g / 'android.tsv').write_text('status\texperimental\n' + (f'data_bundles\t{row}\n' if row else ''), encoding='utf-8')
            self.assertEqual(android_profile.AndroidProfile(types.SimpleNamespace(id=g.name, dir=g, get=lambda k, d='': d)).bundles, want)
        for bad in ('64 32', '0 10', '1048576', '1 99999999999', 'x 10'):
            (g / 'android.tsv').write_text(f'status\texperimental\ndata_bundles\t{bad}\n', encoding='utf-8')
            with self.assertRaisesRegex(android_profile.ProfileError, 'data_bundles', msg=bad): android_profile.AndroidProfile(types.SimpleNamespace(id=g.name, dir=g, get=lambda k, d='': d))


@unittest.skipUnless(CC, NO_CC)
class Sync(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = Path(tempfile.mkdtemp(prefix='nx2a_test_dss_'))
        cls.server, cls.files, cls.fj = make_pack(cls.t, 'server')
        cls.bundles = [b['path'] for b in cls.fj['bundles']]

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.t, ignore_errors=True)

    def root(self, name):
        r = self.t / name; shutil.rmtree(r, ignore_errors=True); r.mkdir(); return r

    def pushed(self, name):   # a files folder as porter install leaves it: the bundles, the files on their own, files.json, the manifest (env.txt as itself)
        r = self.root(name); shutil.copytree(self.server, r, dirs_exist_ok=True); return r

    def whole(self, r):   # the folder holds the pack: every game file with its bytes, no bundle, no part or temporary file left
        self.assertEqual(tree(r), self.files)
        self.assertFalse((r / 'bundles').exists(), 'the bundles folder is left')
        self.assertFalse([p for p in r.rglob('*') if p.name.endswith(('.part', '.new'))])

    def test_fresh(self):   # an empty folder: the bundles and the large files downloaded, no small file asked for on its own; then nothing to do, and offline it starts
        r = self.root('fresh'); x = sync(self.server, r)
        self.assertEqual(x['result'], 'DONE', x['text']); self.whole(r)
        self.assertEqual((r / publish.FILES).read_bytes(), (self.server / publish.FILES).read_bytes())
        small = {f['path'] for f in self.fj['files'] if 'bundle' in f}
        self.assertFalse(small & set(x['fetched'])); self.assertEqual(set(self.bundles) - set(x['fetched']), set())
        self.assertIn('RomFS/Data/big.bin', x['fetched']); self.assertIn('taken out of', x['text'])
        y = sync(self.server, r); self.assertEqual((y['result'], y['files']), ('UPTODATE', 1), y['text'])
        self.assertEqual(sync('-', r)['result'], 'OFFLINE')

    def test_one_small_file_changed(self):   # a new pack with one small file changed (same size): its bundle only, and the list
        r = self.root('changed'); self.assertEqual(sync(self.server, r)['result'], 'DONE')
        rel = 'RomFS/Data/d3/f003.bin'; s2, _, fj2 = make_pack(self.t, 'server2', changed={rel: blob(len(self.files[rel]), 'new')})
        k = next(f['bundle'] for f in fj2['files'] if f['path'] == rel)
        x = sync(s2, r); self.assertEqual(x['result'], 'DONE', x['text'])
        self.assertEqual(x['fetched'][:2], [publish.FILES, fj2['bundles'][k]['path']], x['text'])
        self.assertLessEqual(set(x['fetched'][2:]), {datapack.MANIFEST}, x['text'])   # (the manifest says when the pack was made)
        self.assertEqual((r / rel).read_bytes(), blob(len(self.files[rel]), 'new')); self.assertFalse((r / 'bundles').exists())

    def test_verify_finds_damaged_files(self):   # files damaged at their size (a sample cannot see that): a usual sync takes the folder as whole; the launcher's Check the data
        # again hashes every file and downloads only what is damaged (its bundle for a bundled one), then the folder is whole again
        r = self.root('verify'); self.assertEqual(sync(self.server, r)['result'], 'DONE')
        small = next(f['path'] for f in self.fj['files'] if 'bundle' in f); big = 'RomFS/Data/big.bin'
        for rel in (small, big): b = (r / rel).read_bytes(); (r / rel).write_bytes(bytes(x ^ 0x5A for x in b))
        x = sync(self.server, r); self.assertEqual(x['result'], 'UPTODATE', x['text'])
        x = sync(self.server, r, '--verify'); self.assertEqual(x['result'], 'DONE', x['text']); self.whole(r)
        k = next(f['bundle'] for f in self.fj['files'] if f['path'] == small)
        self.assertEqual(set(x['fetched']) - {publish.FILES, datapack.MANIFEST}, {big, self.fj['bundles'][k]['path']}, x['text'])
        x = sync(self.server, r, '--verify'); self.assertEqual(x['result'], 'UPTODATE', x['text']); self.assertEqual(x['fetched'], [publish.FILES], x['text'])

    def test_pushed_no_server(self):   # porter install's folder, an APK without a server: the bundles unpacked from the folder, the game starts; later starts need nothing
        r = self.pushed('pushed'); x = sync('none', r)
        self.assertEqual(x['result'], 'OFFLINE', x['text']); self.whole(r); self.assertEqual(x['files'], 0)
        self.assertEqual(sync('none', r)['result'], 'OFFLINE')

    def test_pushed_with_server(self):   # the same folder with a server: no bundle downloaded, they are unpacked from the folder (env.txt comes once for its env.pack.txt,
        # as after any porter install)
        r = self.pushed('pushed-srv'); x = sync(self.server, r)
        self.assertEqual(x['result'], 'DONE', x['text']); self.assertEqual(x['fetched'], [publish.FILES, 'env.txt'], x['text']); self.whole(r)
        self.assertEqual(sync(self.server, r)['fetched'], [publish.FILES])

    def test_pushed_over_loose_files(self):   # a folder holding some files on their own already (an older push) and the bundles: those files are kept, the rest unpacked
        r = self.pushed('over'); keep = 'RomFS/Data/d0/f000.bin'; (r / keep).parent.mkdir(parents=True, exist_ok=True); (r / keep).write_bytes(self.files[keep])
        x = sync('none', r); self.assertEqual(x['result'], 'OFFLINE', x['text']); self.whole(r)

    def test_corrupt_bundle(self):   # a pushed bundle with other bytes: refused and deleted, the others unpacked; without a server the game does not start, with one it is downloaded
        r = self.pushed('corrupt'); bad = r / self.bundles[1]; b = bytearray(bad.read_bytes()); b[600] ^= 0xFF; bad.write_bytes(bytes(b))
        x = sync('none', r); self.assertEqual(x['result'], 'ERR_LOCAL', x['text']); self.assertIn('1 bundles and 0 other files', x['err']); self.assertIn('install the pack again', x['err'])
        self.assertFalse(bad.exists()); self.assertTrue((r / 'bundles').exists())
        self.assertEqual(sync('none', r)['result'], 'ERR_LOCAL')   # (still not whole)
        y = sync(self.server, r); self.assertEqual(y['result'], 'DONE', y['text']); self.assertEqual(y['fetched'], [publish.FILES, self.bundles[1], 'env.txt']); self.whole(r)

    def test_cut_bundle_resumes(self):   # a push cut short: its bytes are kept and the download goes on from there
        r = self.pushed('cut'); p = r / self.bundles[2]; full = p.stat().st_size; p.write_bytes(p.read_bytes()[:full // 2])
        x = sync(self.server, r); self.assertEqual(x['result'], 'DONE', x['text']); self.whole(r)
        self.assertEqual(x['fetched'], [publish.FILES, self.bundles[2], 'env.txt']); self.assertLess(x['bytes'], (self.server / publish.FILES).stat().st_size + (self.server / 'env.txt').stat().st_size + full - full // 2 + 1)

    def test_break_and_resume(self):   # downloads broken off again and again: each try goes on, nothing taken twice, and an interrupted folder never starts offline
        r = self.root('resume'); total = sum((self.server / b).stat().st_size for b in self.bundles) + sum(f['size'] for f in self.fj['files'] if 'bundle' not in f)
        got, step = 0, (self.server / publish.FILES).stat().st_size + 20_000   # each try gets the list and 20 KB more
        for i in range(60):
            x = sync(self.server, r, '--break', step); got += x['bytes']
            if x['result'] != 'ERR_NET': break
            self.assertNotEqual(sync('-', r)['result'], 'OFFLINE')
        self.assertEqual(x['result'], 'DONE', x['text']); self.whole(r); self.assertGreater(i, 3)
        self.assertLess(got, total + (i + 1) * (self.server / publish.FILES).stat().st_size + 1, 'bytes downloaded again')

    def stray_root(self, name, manifest=True, list_format1=False):   # a files folder an install that stopped left: the bundles porter install pushes first, over an earlier pack's files
        r = self.root(name); (r / 'RomFS').mkdir(); (r / 'RomFS' / 'old.bin').write_bytes(b'old')
        shutil.copytree(self.server / 'bundles', r / 'bundles')
        if manifest:   # the earlier, non-bundled pack's manifest: no "bundles" key
            old = {'title_id': '0100000000B00000', 'name': 'Bundle Test', 'package': PKG, 'version': 'v0', 'files': 1, 'bytes': 1}
            (r / datapack.MANIFEST).write_text(json.dumps(old, indent=2), encoding='utf-8')
        if list_format1:   # the earlier pack's files.json (format 1: no bundles), with every file of it there
            plain, files, _ = make_pack(self.t, 'plain-earlier', bundles=None)
            for rel, b in files.items(): p = r / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b)
            shutil.copy(plain / publish.FILES, r / publish.FILES)
        return r

    def test_unfinished_install_is_not_whole(self):   # porter install pushed the bundles and stopped before files.json: the earlier pack's manifest (no "bundles" key), or a
        # format 1 files.json, with the bundles folder is not a whole pack: no server (or none reachable), the bundles stay and the game does not start (ERR_LOCAL)
        for label, kw in (('manifest', dict()), ('format1', dict(manifest=False, list_format1=True)), ('both', dict(list_format1=True))):
            for mode in ('none', '-'):
                r = self.stray_root(f'stray-{label}', **kw); before = sorted(p.name for p in (r / 'bundles').iterdir()); self.assertEqual(before, sorted(Path(b).name for b in self.bundles))
                x = sync(mode, r); self.assertEqual(x['result'], 'ERR_LOCAL', (label, mode, x['text'])); self.assertIn('install the pack again', x['err'])
                self.assertEqual(sorted(p.name for p in (r / 'bundles').iterdir()), before, f'{label} {mode}: the bundles were deleted')
                self.assertEqual(sync(mode, r)['result'], 'ERR_LOCAL')   # (and at every start)

    def test_unfinished_install_with_server(self):   # the same folders with a server: its list decides; the bundles in the folder are unpacked (none downloaded) and the folder is whole
        for label, kw in (('manifest', dict()), ('format1', dict(manifest=False, list_format1=True))):
            r = self.stray_root(f'stray-srv-{label}', **kw); x = sync(self.server, r)
            self.assertEqual(x['result'], 'DONE', x['text']); self.assertFalse(set(self.bundles) & set(x['fetched']), x['text'])
            self.assertEqual(tree(r, skip=('RomFS/old.bin',)), self.files); self.assertFalse((r / 'bundles').exists())   # (the earlier pack's old.bin is not the app's to delete)

    def test_unfinished_install_same_list_as_server(self):   # a format 1 list the server has too (same id) with a bundles folder beside it: not taken for the folder's pack by a sample
        # (the server's 304 or its same list), the folder is checked by that list and the bundles folder goes only after it
        plain, files, fj1 = make_pack(self.t, 'plain-same', bundles=None); r = self.root('stray-same'); shutil.copytree(plain, r, dirs_exist_ok=True)
        shutil.copytree(self.server / 'bundles', r / 'bundles'); self.assertEqual(sync('none', r)['result'], 'ERR_LOCAL')
        x = sync(plain, r); self.assertIn(x['result'], ('DONE', 'UPTODATE'), x['text']); self.assertFalse((r / 'bundles').exists()); self.assertEqual(tree(r), files)

    def test_killed_unpack(self):   # an unpack killed part way (a bundle taken out and deleted, another half taken out with its last file cut short, the rest untouched): the
        # folder's list stands, so the rest is unpacked, with or without a server, and the bundles folder goes
        for mode in ('none', 'server'):
            r = self.pushed(f'killed-{mode}')
            for k in (0, 1):
                with tarfile.open(r / self.bundles[k], encoding='utf-8') as t:
                    ms = [m for m in t if m.isreg()]
                    if k == 1: ms = ms[:len(ms) // 2]
                    for m in ms: p = r / m.name; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(t.extractfile(m).read())
                    if k == 1: last = r / ms[-1].name; b = last.read_bytes(); last.write_bytes(b[:len(b) // 2])
            (r / self.bundles[0]).unlink()
            x = sync('none' if mode == 'none' else self.server, r); self.assertIn(x['result'], ('OFFLINE', 'DONE', 'UPTODATE'), x['text']); self.whole(r)

    def test_offline_success_has_no_error(self):   # a server that cannot be reached with a whole folder: the game starts, and no error text is left
        r = self.root('offline-ok'); self.assertEqual(sync(self.server, r)['result'], 'DONE')
        x = sync('-', r); self.assertEqual((x['result'], x['err']), ('OFFLINE', ''), x['text'])

    def test_space(self):   # not enough room: refused before anything is downloaded, with how much more is needed and how much is free (the screen says them in the
        # app's language: datasync_android.c); the room counted is what comes down and one bundle's files beside the bundles, with a margin of 64 MB
        x = sync(self.server, self.root('space'), '--free', 100_000); self.assertEqual(x['result'], 'ERR_SPACE', x['text']); self.assertEqual(x['fetched'], [publish.FILES])
        sizes = [b['size'] for b in self.fj['bundles']]; par = min(UNPACKERS, len(sizes)); self.assertGreater(par, 1)
        nfiles = len([f for f in self.fj['files'] if f['path'] != publish.FILES])
        want = sum(f['size'] for f in self.fj['files'] if 'bundle' not in f) + sum(sizes) + par * max(sizes) + nfiles * 4096 + (64 << 20)
        self.assertEqual((x['space_need'], x['space_free']), (want - 100_000, 100_000), x['text'])
        self.assertIn(f'{(want - 100_000) / 2**30:.1f} GB more space is needed', x['err'])   # (the log's English, as before)
        y = sync(self.server, self.root('space2')); self.assertEqual((y['result'], y['space_need'], y['space_free']), ('DONE', 0, 0), y['text'])   # (room: none set)

    def test_space_counts_the_bundles_unpacked_at_once(self):   # one bundle's room for each the unpackers take out at once (at most DS_UNPACKERS, or fewer bundles), and 4 KB for each file written
        h = (REPO / 'nxnative/dm/android/datasync.h').read_text(encoding='utf-8')
        self.assertRegex(h, rf'#define DS_UNPACKERS {UNPACKERS}'); self.assertRegex(h, r'#define DS_FILE_SLACK 4096')
        from porter import install; self.assertEqual((install.UNPACKERS, install.FILE_SLACK, install.MARGIN), (UNPACKERS, 4096, 64 << 20))   # porter install counts the same
        for name, bundles in (('few', (MAX, 80 * 1024)), ('one', (MAX, 600 * 1024))):   # fewer bundles than unpackers; a single bundle
            s, _, fj = make_pack(self.t, f'space-{name}', bundles=bundles); sizes = [b['size'] for b in fj['bundles']]; par = min(UNPACKERS, len(sizes))
            if name == 'few': self.assertTrue(1 < par < UNPACKERS, len(sizes))
            else: self.assertEqual(par, 1)
            nfiles = len([f for f in fj['files'] if f['path'] != publish.FILES])
            want = sum(f['size'] for f in fj['files'] if 'bundle' not in f) + sum(sizes) + par * max(sizes) + nfiles * 4096 + (64 << 20)
            x = sync(s, self.root(f'space-{name}-root'), '--free', 1); self.assertEqual((x['result'], x['space_need']), ('ERR_SPACE', want - 1), x['text'])
        r = self.pushed('space-pushed')   # bundles in the folder, no server: no download counted, the bundles unpacked at once and the files written still are
        x = sync('none', r, '--free', 1); sizes = [b['size'] for b in self.fj['bundles']]
        nfiles = len([f for f in self.fj['files'] if 'bundle' in f])
        self.assertEqual((x['result'], x['space_need']), ('ERR_SPACE', min(UNPACKERS, len(sizes)) * max(sizes) + nfiles * 4096 + (64 << 20) - 1), x['text'])

    def test_old_app(self):   # an app from before bundles on a pack with them: its first download is a bundled file's, an HTTP 404 it stops at, nothing else downloaded
        if not fixture(old=True): self.skipTest(f'no {BEFORE[:8]} in this clone')
        r = self.root('old'); x = sync(self.server, r, old=True)
        self.assertEqual(x['result'], 'ERR_SERVER', x['text']); self.assertIn('HTTP 404 for RomFS/', x['err']); self.assertEqual(x['fetched'], [publish.FILES])
        self.assertEqual([p.name for p in r.rglob('*') if p.is_file() and (p.stat().st_size or not p.name.endswith('.part'))], [])   # (only the empty part it began)
        r = self.root('old-whole'); self.assertEqual(sync(self.server, r)['result'], 'DONE')   # a folder that holds every bundled file: it works as before
        self.assertEqual(sync(self.server, r, old=True)['result'], 'UPTODATE')

    def test_plain_pack(self):   # a pack without bundles: as before (every file on its own, no bundles folder)
        s, files, _ = make_pack(self.t, 'plain', bundles=None); r = self.root('plain-root')
        x = sync(s, r); self.assertEqual(x['result'], 'DONE', x['text']); self.assertEqual(tree(r), files); self.assertNotIn('bundles', ''.join(x['fetched']))
        self.assertEqual(len(x['fetched']), len([b for b in files.values() if b]) + 4)   # every file (an empty one is not asked for), env.txt, settings.txt, the manifest, files.json
        self.assertEqual(sync(s, r)['result'], 'UPTODATE'); self.assertEqual(sync('-', r)['result'], 'OFFLINE')

    def test_check_notes_by_time(self):   # a folder with the pack's files but no files.json (every file hashed, as a device whose list is gone): the check notes its
        # progress every DS_NOTE_MS (200 ms) of the sync's clock, whatever the files' count, and the bytes hashed are its progress; not more often than that
        s, files, _ = make_pack(self.t, 'plain-notes', bundles=None)
        def notes(step):
            r = self.root(f'notes-{step}'); shutil.copytree(s, r, dirs_exist_ok=True); (r / publish.FILES).unlink()
            x = sync(s, r, '--notes', '--clock-step', step); self.assertIn(x['result'], ('DONE', 'UPTODATE'), x['text'])
            return [tuple(map(int, ln.split()[1:])) for ln in x['text'].splitlines() if ln.startswith('note ')]
        fast = notes(50)   # 50 ms a read of the clock: a note every 4 reads (one a file, one each MB hashed)
        n = len(files) + 3
        self.assertGreater(len(fast), n // 3, fast)   # (the old rule, a note every 4,096 files: 1 for this pack)
        done = [d for _, _, d, _ in fast]; total = fast[0][3]
        self.assertEqual(done, sorted(done)); self.assertGreater(done[-1], total * 3 // 4); self.assertTrue(all(t == total for *_, t in fast))
        self.assertEqual([k for _, k, _, _ in fast], sorted(k for _, k, _, _ in fast))   # (the file's number goes up with it)
        self.assertLessEqual(len(notes(1)), 3)   # 1 ms a read: the whole check is a few hundred ms of the clock, so a note or two after the first


@unittest.skipUnless(CC, NO_CC)
class Tar(unittest.TestCase):
    """Bundles that are not what porter writes: datasync.h takes only regular files with safe names, at the list's sizes, and every file the list puts in a bundle"""
    def setUp(self): self.t = Path(tempfile.mkdtemp(prefix='nx2a_test_tar_'))
    def tearDown(self): shutil.rmtree(self.t, ignore_errors=True)

    def server(self, members, listed=None, fmt=2):   # a server with one bundle of members [(name, bytes, type)] and a list of listed {path: bytes} (default: the members)
        s = self.t / 'server'; shutil.rmtree(s, ignore_errors=True); (s / 'bundles').mkdir(parents=True)
        with tarfile.open(s / 'bundles/b0000.tar', 'w', format=tarfile.GNU_FORMAT, encoding='utf-8') as tar:
            for name, data, kind in members:
                ti = tarfile.TarInfo(name); ti.type = kind
                if kind == tarfile.SYMTYPE: ti.linkname = '../../outside'; tar.addfile(ti)
                else: ti.size = len(data); tar.addfile(ti, io.BytesIO(data))
        raw = (s / 'bundles/b0000.tar').read_bytes()
        listed = listed if listed is not None else {n: d for n, d, _ in members}
        files = [dict(path=p, size=len(d), sha256=hashlib.sha256(d).hexdigest(), bundle=0) for p, d in listed.items()]
        fj = dict(format=fmt, id='ab' * 32, package=PKG, version='v0', display='1', variants=[], bytes=sum(f['size'] for f in files), made='',
                  bundles=[dict(path='bundles/b0000.tar', size=len(raw), sha256=hashlib.sha256(raw).hexdigest(), files=len(files), bytes=0)], files=files)
        (s / publish.FILES).write_text(json.dumps(fj), encoding='utf-8')
        return s

    def root(self): r = self.t / 'root'; shutil.rmtree(r, ignore_errors=True); r.mkdir(); return r

    def test_long_and_korean_names(self):   # GNU long names and UTF-8 names come out at their paths
        names = {'RomFS/' + 'x' * 120 + '/y.bin': b'long', 'RomFS/한글 이름/파일.bin': b'ko', 'RomFS/a.bin': b'', 'RomFS/b.bin': b'b' * 1025}
        r = self.root(); x = sync(self.server([(n, d, tarfile.REGTYPE) for n, d in names.items()]), r)
        self.assertEqual(x['result'], 'DONE', x['text'])
        self.assertEqual({p.relative_to(r).as_posix(): p.read_bytes() for p in (r / 'RomFS').rglob('*') if p.is_file()}, names)

    def test_unsafe_names(self):   # a member named .. or absolute: the bundle is refused before anything is written outside the folder, and deleted
        for bad in ('RomFS/../../evil.bin', '/abs/evil.bin', 'RomFS/./x.bin'):
            r = self.root(); s = self.server([('RomFS/a.bin', b'aa', tarfile.REGTYPE), (bad, b'evil', tarfile.REGTYPE)], listed={'RomFS/a.bin': b'aa'})
            x = sync(s, r); self.assertEqual(x['result'], 'ERR_DATA', x['text']); self.assertIn('does not write', x['err'])
            self.assertFalse((self.t / 'evil.bin').exists()); self.assertFalse(list(self.t.rglob('evil.bin')), bad)
            self.assertFalse((r / 'bundles/b0000.tar').exists())

    def test_not_a_file(self):   # a link (or anything but a file): refused
        r = self.root(); x = sync(self.server([('RomFS/a.bin', b'aa', tarfile.REGTYPE), ('RomFS/l', b'', tarfile.SYMTYPE)], listed={'RomFS/a.bin': b'aa'}), r)
        self.assertEqual(x['result'], 'ERR_DATA', x['text']); self.assertIn("type '2'", x['err'])

    def test_member_missing_or_other_size(self):   # the list puts a file in the bundle that it lacks, or lists another size: refused, no list written
        r = self.root(); x = sync(self.server([('RomFS/a.bin', b'aa', tarfile.REGTYPE)], listed={'RomFS/a.bin': b'aa', 'RomFS/b.bin': b'bb'}), r)
        self.assertEqual(x['result'], 'ERR_DATA', x['text']); self.assertIn('lacks 1 of the 2', x['err']); self.assertFalse((r / publish.FILES).exists())
        r = self.root(); x = sync(self.server([('RomFS/a.bin', b'aa', tarfile.REGTYPE)], listed={'RomFS/a.bin': b'aaa'}), r)
        self.assertEqual(x['result'], 'ERR_DATA', x['text']); self.assertIn('the list says 3', x['err'])

    def test_damaged_header(self):   # a header whose checksum is wrong (and the bundle's SHA-256 listed for those bytes): refused
        s = self.server([('RomFS/a.bin', b'aa', tarfile.REGTYPE)]); p = s / 'bundles/b0000.tar'; b = bytearray(p.read_bytes()); b[0] ^= 1; p.write_bytes(bytes(b))
        fj = json.loads((s / publish.FILES).read_text()); fj['bundles'][0]['sha256'] = hashlib.sha256(bytes(b)).hexdigest(); (s / publish.FILES).write_text(json.dumps(fj))
        x = sync(s, self.root()); self.assertEqual(x['result'], 'ERR_DATA', x['text']); self.assertIn('damaged header', x['err'])

    @staticmethod
    def header(name, kind, size_field):   # a ustar header by hand: the size field as given (porter writes octal; GNU tar writes base-256 for a size past 8 GB)
        h = bytearray(512); h[0:len(name)] = name.encode()
        h[100:108] = b'0000644\0'; h[108:116] = b'0000000\0'; h[116:124] = b'0000000\0'
        h[124:136] = size_field; h[136:148] = b'00000000000\0'; h[156:157] = kind; h[257:265] = b'ustar  \0'
        h[148:156] = b' ' * 8; h[148:156] = b'%06o\0 ' % sum(h)
        return bytes(h)

    def raw_server(self, first):   # a bundle that begins with the header `first`, then a file a.bin (listed), then the end blocks
        data = b'aa'; s = self.t / 'server'; shutil.rmtree(s, ignore_errors=True); (s / 'bundles').mkdir(parents=True)
        body = first + self.header('RomFS/a.bin', b'0', b'%011o\0' % len(data)) + data + b'\0' * (512 - len(data)) + b'\0' * 1024
        (s / 'bundles/b0000.tar').write_bytes(body)
        fj = dict(format=2, id='ab' * 32, package=PKG, version='v0', display='1', variants=[], bytes=2, made='',
                  bundles=[dict(path='bundles/b0000.tar', size=len(body), sha256=hashlib.sha256(body).hexdigest(), files=1, bytes=2)],
                  files=[dict(path='RomFS/a.bin', size=2, sha256=hashlib.sha256(data).hexdigest(), bundle=0)])
        (s / publish.FILES).write_text(json.dumps(fj), encoding='utf-8'); return s

    def test_size_past_the_end(self):   # an entry whose size reaches past the bundle's end (base-256 2**64 - 512 once cast to -512 and the skip read the same header again, for ever; a
        # size of 2**62 too): refused at once, whatever the entry's type (a folder, a file the list does not have)
        for label, kind, size in (('neg512-dir', b'5', 2**64 - 512), ('neg512-unlisted', b'0', 2**64 - 512), ('huge-dir', b'5', 2**62), ('huge-unlisted', b'0', 2**62), ('max-dir', b'5', 2**64 - 1)):
            name = 'RomFS/d/' if kind == b'5' else 'RomFS/zz.bin'
            s = self.raw_server(self.header(name, kind, b'\x80' + size.to_bytes(11, 'big')))
            x = sync(s, self.root(), timeout=15); self.assertEqual(x['result'], 'ERR_DATA', (label, x['text'])); self.assertIn('reaches past its end', x['err'])
        s = self.raw_server(self.header('RomFS/d/', b'5', b'%011o\0' % 0))   # a folder entry of size 0 is fine (the control)
        r = self.root(); x = sync(s, r, timeout=15); self.assertEqual(x['result'], 'DONE', x['text']); self.assertEqual((r / 'RomFS/a.bin').read_bytes(), b'aa')

    def test_newer_format(self):   # a list of a format this app does not know: refused, with what to do
        x = sync(self.server([('RomFS/a.bin', b'aa', tarfile.REGTYPE)], fmt=3), self.root())
        self.assertEqual(x['result'], 'ERR_SERVER', x['text']); self.assertIn('format 3', x['err'])


@unittest.skipUnless(CC, NO_CC)
class PointerC(unittest.TestCase):
    """The pointer file and the APK's asset as the app reads them (datasync.h ds_pointer_open, ds_source, ds_hmac): what porter/publish.py writes opens to the same text,
    anything changed is refused (tests/test_pointer.py has the Python half)"""
    KEY = bytes(range(32))
    def setUp(self): self.t = Path(tempfile.mkdtemp(prefix='nx2a_test_ptrc_'))
    def tearDown(self): shutil.rmtree(self.t, ignore_errors=True)

    def fx(self, *a):
        r = subprocess.run([str(fixture()), *map(str, a)], capture_output=True, text=True, encoding='utf-8', timeout=30)
        self.assertEqual(r.returncode, 0, r.stderr); return r.stdout

    def open_line(self, line, key=None):
        p = self.t / 'line'; p.write_bytes(line.encode() if isinstance(line, str) else line)
        return self.fx('--pointer', (key or self.KEY).hex(), p)

    def test_hmac_rfc4231(self):   # RFC 4231 test cases 1 and 2, and a key longer than a block (case 6) against Python's hmac
        import hmac
        self.assertEqual(self.fx('--hmac', '0b' * 20, b'Hi There'.hex()).strip(), 'b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7')
        self.assertEqual(self.fx('--hmac', b'Jefe'.hex(), b'what do ya want for nothing?'.hex()).strip(), '5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843')
        k, m = b'\xaa' * 131, b'Test Using Larger Than Block-Size Key - Hash Key First'
        self.assertEqual(self.fx('--hmac', k.hex(), m.hex()).strip(), '60e431591ee0b67f0d8a26aacbf5b77f8e0bc6213728c5140546040f0ee37f54')
        self.assertEqual(self.fx('--hmac', k.hex(), m.hex()).strip(), hmac.new(k, m, hashlib.sha256).hexdigest())

    def test_python_line_opens(self):   # the cross-check: Python seals, C opens to the same root and credentials (a long one: several keystream blocks)
        for root, cred in (('https://example.com/share/', 'reader:s3cr3t'), ('https://example.com:5006/a/b/c/', ''), ('https://example.com/' + 'd' * 300 + '/', 'u:' + 'p' * 390)):
            line = publish.pointer_seal(root, cred, self.KEY)
            self.assertEqual(self.open_line(line), f'ok {root}\ncred {cred}\n')
            self.assertEqual(self.open_line('\r\n  ' + line + '\n\n'), f'ok {root}\ncred {cred}\n')   # around it: ignored

    def test_tampered(self):   # a byte of the nonce, the ciphertext or the tag changed, cut short, another key, another magic, not base64: refused before decrypting
        line = publish.pointer_seal('https://example.com/share/', 'reader:s3cr3t', self.KEY); raw = bytearray(base64.b64decode(line[6:]))
        for at in (0, 15, 16, 30, len(raw) - 33, len(raw) - 32, len(raw) - 1):
            b = bytearray(raw); b[at] ^= 0x80; out = self.open_line('NX2P1:' + base64.b64encode(bytes(b)).decode())
            self.assertTrue(out.startswith('bad '), (at, out)); self.assertNotIn('s3cr3t', out); self.assertNotIn('example', out)
        out = self.open_line(line, bytes(32)); self.assertIn('key', out); self.assertTrue(out.startswith('bad '))
        for bad in (line[:-4], 'NX2P0:' + line[6:], line[6:], 'NX2P1:' + line[6:-1] + '!', 'NX2P1:' + base64.b64encode(bytes(raw[:47])).decode(), 'NX2P1:' + 'A' * 6000, ''):
            self.assertTrue(self.open_line(bad).startswith('bad '), bad[:24])

    def seal_raw(self, plain):   # a line around any plaintext, sealed as publish.pointer_seal does (to see what the app refuses inside a good tag)
        import hmac
        nonce = b'\x07' * 16; enc, mac = publish._pointer_keys(self.KEY)
        ct = bytes(a ^ b for a, b in zip(plain, publish._keystream(enc, nonce, len(plain))))
        return 'NX2P1:' + base64.b64encode(nonce + ct + hmac.new(mac, b'NX2P1' + nonce + ct, hashlib.sha256).digest()).decode()

    def test_content_refused(self):   # inside a good tag: plain HTTP, no / at the end, one line or three, credentials without a colon, a NUL, a space
        self.assertTrue(self.open_line(self.seal_raw(b'https://example.com/ok/\nu:p')).startswith('ok '))
        for plain in (b'http://example.com/share/\n', b'https://example.com/share\n', b'https://example.com/share/', b'https://example.com/share/\nu:p\nx',
                      b'https://example.com/share/\nnocolon', b'https://example.com/share/\n:p', b'https://exa\0mple.com/share/\n', b'https://exa mple.com/share/\n'):
            out = self.open_line(self.seal_raw(plain)); self.assertTrue(out.startswith('bad '), (plain, out))

    def source(self, asset):
        p = self.t / 'source.bin'; p.write_bytes(asset); return self.fx('--source', p).split('\n')

    def test_asset_versions(self):   # version 1 as before (no pointer); version 2 with the pointer's fields; a damaged one refused
        self.assertEqual(self.source(publish.source_asset('https://h:8443/data/x/', 'reader:secret'))[:6], ['v1', 'https://h:8443/data/x/', 'reader:secret', '', '00' * 32, ''])
        self.assertEqual(self.source(publish.source_asset('http://example.com/p/', ''))[:3], ['v1', 'http://example.com/p/', ''])
        a = publish.source_asset('https://example.com/share/zelda/1.1.0/', 'reader:s3cr3t', ('https://example.org/raw/pointer.txt', self.KEY, 'zelda/1.1.0/'))
        self.assertEqual(self.source(a)[:6], ['v2', 'https://example.com/share/zelda/1.1.0/', 'reader:s3cr3t', 'https://example.org/raw/pointer.txt', self.KEY.hex(), 'zelda/1.1.0/'])
        self.assertEqual(self.source(publish.source_asset('https://example.com/r/', '', ('https://example.org/p', self.KEY, '')))[:6],
                         ['v2', 'https://example.com/r/', '', 'https://example.org/p', self.KEY.hex(), ''])
        long = publish.source_asset('https://example.com/' + 'x' * 870 + '/', 'u:' + 'p' * 390, ('https://example.org/' + 'y' * 870, self.KEY, 'x' * 870 + '/'))
        self.assertGreater(len(long), 2048); self.assertEqual(self.source(long)[0], 'v2')   # past version 1's 2,048 bytes
        m = lambda v, t: b'NX2S' + bytes([v]) + publish.mask(t.encode())
        for bad in (m(3, 'https://e/\n'), m(2, 'https://e/\n\nhttp://p\n' + '00' * 32 + '\n'), m(2, 'https://e/\n\nhttps://p\n' + '0' * 63 + '\n'),
                    m(2, 'https://e/\n\nhttps://p\n' + 'zz' * 32 + '\n'), m(2, 'https://e/\n\nhttps://p\n' + '00' * 32 + '\n../'), m(2, 'https://e/\n\nhttps://p\n' + '00' * 32 + '\na/b'),
                    m(2, 'https://e/\n\nhttps://p\n' + '00' * 32), m(2, 'https://e/\n\nhttps://p\n' + '00' * 32 + '\na/\nextra'), m(1, 'no newline'), m(1, 'x' * 2049 + '\n'), b'NX2S'):
            self.assertEqual(self.source(bad)[0], 'bad', bad[:8])


if __name__ == '__main__':
    unittest.main()
