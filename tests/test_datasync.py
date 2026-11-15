# A data pack with bundles (android.tsv data_bundles): porter/datapack.py packs the small files of the game's trees into tar bundles, porter/publish.py lists them
# (files.json format 2), and nxnative/dm/android/datasync.h (built on the computer: datasync_fixture.c, with a folder as the server) downloads what the folder lacks,
# unpacks the bundles (downloaded, or pushed by porter install) and refuses what is not the list's. A pack without the row is checked to stay as it was.
# python -m unittest tests.test_datasync (a C compiler as `cc`, else the sync's half is skipped)
import hashlib, io, json, os, shutil, subprocess, sys, tarfile, tempfile, types, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO)); sys.path.insert(0, str(Path(__file__).resolve().parent))
from porter import datapack, publish   # noqa: E402
from porter.upstream import BuildError   # noqa: E402

PKG = 'org.nx2a.bundletest'
MAX, SIZE = 2000, 24 * 1024   # files of at most 2,000 bytes into bundles of up to 24 KB: many bundles from a small pack
BEFORE = '314fd85231d068bb91627761aa3a4a4137115917'   # the last datasync.h from before bundles (an app ported before them)
CC = shutil.which('cc')


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
    return types.SimpleNamespace(name='Bundle Test', id='0100000000B00000', package=PKG, env=['DM_X=1'], settings={'overlay': '1'}, requires=[], bundles=bundles)


def make_pack(t, name='Test-data', bundles=(MAX, SIZE), changed=None):
    src = t / f'src-{name}'; shutil.rmtree(src, ignore_errors=True); files = source(src, changed)
    dst = t / name
    datapack.build(dst, profile=profile(bundles), version='v65536', display='1.0.1', patched=src / 'mods', romfs=src / 'RomFS', shaders=None, aoc=src / 'aoc')
    return dst, files, publish.manifest(dst)


_built = {}
def fixture(old=False):   # the harness, once a run; old: with the datasync.h of BEFORE
    if old in _built: return _built[old]
    d = Path(tempfile.mkdtemp(prefix='nx2a_dsfx_')); exe = d / ('fx.exe' if os.name == 'nt' else 'fx'); args = []
    if old:
        r = subprocess.run(['git', '-C', str(REPO), 'show', f'{BEFORE}:nxnative/dm/android/datasync.h'], capture_output=True)
        if r.returncode: _built[old] = None; return None
        (d / 'datasync_old.h').write_bytes(r.stdout); args = [f'-DDS_H="{(d / "datasync_old.h").as_posix()}"']
    # -Wno-unused-function: the header's helpers the harness does not call (the address, the asset); -DDS_PTHREADS: the unpack's writer threads, as on Android
    r = subprocess.run([CC, '-O1', '-Wall', '-Wextra', '-Wno-unused-function', '-std=gnu11', '-pthread', '-DDS_PTHREADS', *args, '-I', str(REPO / 'nxnative/dm/android'),
                        str(Path(__file__).with_name('datasync_fixture.c')), '-o', str(exe)], capture_output=True, text=True)
    assert r.returncode == 0 and not (r.stderr.strip() and not old), r.stderr   # the new header builds without a warning
    _built[old] = exe; return exe


def sync(server, root, *a, old=False):
    r = subprocess.run([str(fixture(old)), str(server), str(root), PKG, *map(str, a)], capture_output=True, text=True, encoding='utf-8')
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


@unittest.skipUnless(CC, 'no C compiler (cc)')
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

    def test_space(self):   # not enough room: refused before anything is downloaded, with how much more is needed and how much is free (the screen says them in the
        # app's language: datasync_android.c); the room counted is what comes down and one bundle's files beside the bundles, with a margin of 64 MB
        x = sync(self.server, self.root('space'), '--free', 100_000); self.assertEqual(x['result'], 'ERR_SPACE', x['text']); self.assertEqual(x['fetched'], [publish.FILES])
        sizes = [b['size'] for b in self.fj['bundles']]
        want = sum(f['size'] for f in self.fj['files'] if 'bundle' not in f) + sum(sizes) + max(sizes) + (64 << 20)
        self.assertEqual((x['space_need'], x['space_free']), (want - 100_000, 100_000), x['text'])
        self.assertIn(f'{(want - 100_000) / 2**30:.1f} GB more space is needed', x['err'])   # (the log's English, as before)
        y = sync(self.server, self.root('space2')); self.assertEqual((y['result'], y['space_need'], y['space_free']), ('DONE', 0, 0), y['text'])   # (room: none set)

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


@unittest.skipUnless(CC, 'no C compiler (cc)')
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

    def test_newer_format(self):   # a list of a format this app does not know: refused, with what to do
        x = sync(self.server([('RomFS/a.bin', b'aa', tarfile.REGTYPE)], fmt=3), self.root())
        self.assertEqual(x['result'], 'ERR_SERVER', x['text']); self.assertIn('format 3', x['err'])


if __name__ == '__main__':
    unittest.main()
