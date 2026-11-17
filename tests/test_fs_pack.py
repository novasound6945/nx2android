# The RomFS's small files read in place (android.tsv romfs_packs): porter/datapack.py packs them into tar packs with their index (packs/romfs.nxbi), and
# nxnative/dm/host/fs_pack.h (built on the computer: fs_pack_fixture.c) finds, reads and lists them; checked file by file and folder by folder against the tree the
# pack was made from. The port's side (the profile row, what it refuses, the manifest, env.txt, files.json, the packs kept across ports, its own check) needs no compiler.
# python -m unittest tests.test_fs_pack (a C compiler with POSIX headers: $CC, else cc, clang or gcc; without one the fs_pack.h half is skipped)
import atexit, hashlib, json, os, shutil, struct, subprocess, sys, tarfile, tempfile, types, unittest
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO)); sys.path.insert(0, str(Path(__file__).resolve().parent))
from porter import datapack, publish   # noqa: E402
from porter.upstream import BuildError   # noqa: E402

PKG = 'org.nx2a.packtest'
MAX, SIZE = 2000, 24 * 1024   # files of at most 2,000 bytes into packs of up to 24 KB: many packs from a small tree
CC_TRIED = [n for n in (os.environ.get('CC'), 'cc', 'clang', 'gcc') if n]
CC = next((w for w in map(shutil.which, CC_TRIED) if w), None)


def posix_cc():   # the compiler builds a POSIX program (mmap, pread, pthreads): not MinGW's
    if not CC: return False
    with tempfile.TemporaryDirectory() as d:
        c = Path(d) / 'p.c'; c.write_text('#include <sys/mman.h>\n#include <pthread.h>\n#include <unistd.h>\nint main(void){return (int)pread(0,0,0,0)&0;}\n', encoding='utf-8')
        try: return subprocess.run([CC, '-std=gnu11', '-pthread', str(c), '-o', str(Path(d) / 'p')], capture_output=True, timeout=60).returncode == 0
        except (OSError, subprocess.SubprocessError): return False


def blob(n, seed): return hashlib.shake_256(f'{seed}'.encode()).digest(n) if n else b''


def case_sensitive(d):   # whether the folder's file system keeps A.bin and a.bin apart (not Windows', not macOS's by default)
    p = Path(tempfile.mkdtemp(dir=d)); (p / 'A').write_bytes(b''); r = not (p / 'a').exists(); shutil.rmtree(p); return r


LONG = 'Data/' + '/'.join(['n' * 40] * 3) + '/긴 이름의 파일.bin'   # past tar's 100 bytes: a GNU long name


def source(d, crowd=0, aoc=True, changed=None):
    """The game's files under d ({rel: bytes}): small and large RomFS files, a long name, an empty file, names around the sort's edges (a/c.txt, a/c/x, a/c0), Korean and
    Japanese names, names that differ only in case where the file system allows; crowd: that many files in one folder; aoc: small and large DLC files"""
    files = {f'RomFS/Data/d{i % 6}/f{i:03d}.bin': blob(i * 13 % MAX + 1, i) for i in range(120)}
    files.update({'RomFS/Data/big.bin': blob(50_000, 'big'), 'RomFS/Data/edge.bin': blob(MAX, 'edge'), 'RomFS/Data/over.bin': blob(MAX + 1, 'over'),
                  'RomFS/empty.bin': b'', 'RomFS/' + LONG: blob(700, 'long'), 'RomFS/한글/파일.bin': blob(300, 'ko'), 'RomFS/日本語/ファイル.dat': blob(301, 'ja'),
                  'RomFS/a/c.txt': blob(10, 'ct'), 'RomFS/a/c/x': blob(11, 'cx'), 'RomFS/a/c0': blob(12, 'c0'), 'RomFS/a/c/y/z.bin': b'', 'RomFS/a/b': blob(5, 'ab'),
                  'RomFS/top.bin': blob(64, 'top'), 'mods/main': blob(1500, 'main'), 'mods/sdk': blob(30_000, 'sdk')})
    if aoc: files.update({'aoc/1/a.bin': blob(400, 'aoc'), 'aoc/1/big.bin': blob(9000, 'aocbig')})
    files.update({f'RomFS/crowd/f{i:05d}.bin': blob(i % 7, f'c{i}') for i in range(crowd)})
    d.mkdir(parents=True, exist_ok=True)
    if case_sensitive(d): files.update({'RomFS/Case/A.bin': blob(20, 'A'), 'RomFS/Case/a.bin': blob(21, 'a')})
    files.update(changed or {})
    for rel, b in files.items(): p = d / rel; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(b)
    return files


def profile(packs=True, bundles=(MAX, SIZE), zstd=False, **kw):
    return types.SimpleNamespace(name='Pack Test', id='0100000000C00000', package=PKG, env=['DM_X=1'], settings={}, requires=[], bundles=bundles, romfs_packs=packs,
                                 romfs_zstd=zstd, **kw)


def build(t, name, src=None, size=SIZE, crowd=0, aoc=True, packs=True, progress=None, changed=None):
    src = src or t / f'src-{name}'
    if not (src / 'RomFS').is_dir(): files = source(src, crowd, aoc, changed)
    else: files = {p.relative_to(src).as_posix(): p.read_bytes() for p in src.rglob('*') if p.is_file()}
    dst = t / name
    datapack.build(dst, profile=profile(packs, (MAX, size)), version='v65536', display='1.0.1', patched=src / 'mods', romfs=src / 'RomFS', shaders=None,
                   aoc=src / 'aoc' if (src / 'aoc').is_dir() else None, progress=progress)
    return dst, files


def small_romfs(files): return {r[len('RomFS/'):]: b for r, b in files.items() if r.startswith('RomFS/') and len(b) <= MAX}


class Port(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.t = Path(tempfile.mkdtemp(prefix='nx2a_test_pk_'))
        cls.pack, cls.files = build(cls.t, 'Test-data')
        cls.man = datapack.read_manifest(cls.pack)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.t, ignore_errors=True)

    def test_layout(self):   # the RomFS's small files only in packs (with their index), the DLC's in bundles, the large ones and mods/ on their own
        loose = {p.relative_to(self.pack).as_posix() for p in self.pack.rglob('*') if p.is_file()}
        packs = sorted(r for r in loose if r.startswith('packs/') and r.endswith('.tar'))
        self.assertEqual(packs, [datapack.pack_name(k) for k in range(len(packs))]); self.assertGreater(len(packs), 3)
        self.assertIn(datapack.PACK_INDEX, loose)
        small = small_romfs(self.files)
        self.assertFalse({f'RomFS/{r}' for r in small} & loose)
        self.assertEqual({r for r in loose if r.startswith('RomFS/')}, {r for r, b in self.files.items() if r.startswith('RomFS/') and len(b) > MAX})
        self.assertEqual(sorted(r for r in loose if r.startswith('bundles/')), ['bundles/b0000.tar'])   # aoc/1/a.bin only: never a RomFS file in a bundle
        with tarfile.open(self.pack / 'bundles/b0000.tar', encoding='utf-8') as tar: self.assertEqual(tar.getnames(), ['aoc/1/a.bin'])
        members = []
        for r in packs:
            self.assertLessEqual((self.pack / r).stat().st_size, SIZE)
            with tarfile.open(self.pack / r, encoding='utf-8') as tar:
                for m in tar: self.assertTrue(m.isreg()); self.assertEqual((m.mode, m.mtime, m.uid), (0o644, 0, 0)); members.append(m.name)
        self.assertEqual(members, sorted(f'RomFS/{r}' for r in small))   # the bundles' format and order

    def test_manifest_and_env(self):   # "packs" with its counts; "bundles" counts only what bundles/ holds; env.txt's DM_ROMFS_PACK row, the files folder's index
        small = small_romfs(self.files)
        self.assertEqual(self.man['packs'], dict(count=len(list((self.pack / 'packs').glob('p*.tar'))), files=len(small), bytes=sum(map(len, small.values())),
                                                 tar_bytes=sum(p.stat().st_size for p in (self.pack / 'packs').glob('p*.tar'))))
        self.assertEqual({k: self.man['bundles'][k] for k in ('count', 'files', 'bytes')}, dict(count=1, files=1, bytes=400))
        self.assertEqual((self.man['files'], self.man['bytes']), (len(self.files), sum(map(len, self.files.values()))))   # the game's files, as without packs
        env = (self.pack / 'env.txt').read_text(encoding='utf-8').splitlines()
        self.assertIn(f'DM_ROMFS_PACK=/sdcard/Android/data/{PKG}/files/packs/romfs.nxbi', env)
        self.assertLess(env.index(f'DM_AOC_DIR=/sdcard/Android/data/{PKG}/files/aoc'), env.index(f'DM_ROMFS_PACK=/sdcard/Android/data/{PKG}/files/packs/romfs.nxbi'))
        self.assertIn('DM_ROMFS_PACK', self.man['env_keys'])
        self.assertTrue(datapack.owned('packs/p0000.tar')); self.assertTrue(datapack.owned('packs/romfs.nxbi')); self.assertFalse(datapack.owned('packs'))

    def test_index(self):   # sorted by the names' UTF-8 bytes; every file's bytes where its record says; the header's counts
        sizes, ents = datapack.read_index(self.pack / datapack.PACK_INDEX)
        names = [e[0] for e in ents]
        self.assertEqual(names, sorted(names, key=lambda n: n.encode('utf-8'))); self.assertEqual(len(set(names)), len(names))
        self.assertEqual(sizes, [(self.pack / datapack.pack_name(k)).stat().st_size for k in range(len(sizes))])
        small = small_romfs(self.files); self.assertEqual(set(names), set(small))
        for n, k, off, size in ents:
            with open(self.pack / datapack.pack_name(k), 'rb') as f: f.seek(off); self.assertEqual(f.read(size), small[n], n)
            self.assertEqual(off % 512, 0)
        b = (self.pack / datapack.PACK_INDEX).read_bytes()
        magic, ver, np_, nf, nb, z = struct.unpack_from('<4sIIIQQ', b)
        self.assertEqual((magic, ver, np_, nf, z), (b'NXBI', 1, len(sizes), len(ents), 0)); self.assertEqual(len(b), 32 + 16 * np_ + 24 * nf + nb)
        self.assertLess(names.index('a/c.txt'), names.index('a/c/x')); self.assertLess(names.index('a/c/y/z.bin'), names.index('a/c0'))   # '.' < '/' < '0'

    def test_files_json(self):   # the packs and their index listed as any file (path, size, SHA-256, mtime); the RomFS's small files not at all; format 2 for the DLC's bundle
        fj = publish.manifest(self.pack)
        self.assertEqual(fj['format'], 2)
        by = {f['path']: f for f in fj['files']}
        for p in (self.pack / 'packs').iterdir():
            rel = p.relative_to(self.pack).as_posix(); f = by[rel]
            self.assertEqual(set(f), {'path', 'size', 'sha256', 'mtime'}); self.assertEqual((f['size'], f['sha256']), (p.stat().st_size, hashlib.sha256(p.read_bytes()).hexdigest()))
        self.assertFalse({f'RomFS/{r}' for r in small_romfs(self.files)} & set(by))
        self.assertEqual([f['path'] for f in fj['files'] if 'bundle' in f], ['aoc/1/a.bin'])

    def test_without_dlc(self):   # no small file outside the RomFS: no bundles/, no "bundles" key, so not a pack with bundles for the installer and the app; files.json format 1
        d, _ = build(self.t, 'NoDlc-data', aoc=False)
        man = datapack.read_manifest(d)
        self.assertFalse((d / 'bundles').exists()); self.assertNotIn('bundles', man); self.assertFalse(datapack.bundled(man)); self.assertIn('packs', man)
        fj = publish.manifest(d); self.assertEqual(fj['format'], 1)
        self.assertTrue({datapack.PACK_INDEX, datapack.pack_name(0)} <= {f['path'] for f in fj['files']})
        self.assertTrue(all(set(f) == {'path', 'size', 'sha256', 'mtime'} for f in fj['files']))

    def test_without_the_row(self):   # romfs_packs off (or a profile from before it): no packs/, no "packs" key, no DM_ROMFS_PACK row, the same pack as a profile without the key
        d, _ = build(self.t, 'Off-data', src=self.t / 'src-Test-data', packs=False)
        old = self.t / 'Old-data'
        prof = profile(packs=False); del prof.romfs_packs; del prof.romfs_zstd
        datapack.build(old, profile=prof, version='v65536', display='1.0.1', patched=self.t / 'src-Test-data/mods', romfs=self.t / 'src-Test-data/RomFS', shaders=None, aoc=self.t / 'src-Test-data/aoc')
        self.assertFalse((d / 'packs').exists()); self.assertNotIn('packs', datapack.read_manifest(d))
        self.assertNotIn('DM_ROMFS_PACK', (d / 'env.txt').read_text(encoding='utf-8'))
        tree = lambda x: {p.relative_to(x).as_posix(): p.read_bytes() for p in x.rglob('*') if p.is_file() and p.name != datapack.MANIFEST}
        self.assertEqual(tree(d), tree(old))
        ma, mb = datapack.read_manifest(d), datapack.read_manifest(old); ma.pop('made'); mb.pop('made'); self.assertEqual(ma, mb)

    def test_refused(self):   # romfs_packs with romfs zstd, or without data_bundles: refused before anything is written
        for prof, why in ((profile(zstd=True), 'zstd'), (profile(bundles=None), 'data_bundles')):
            dst = self.t / f'Refused-{why}'
            with self.assertRaisesRegex(BuildError, why): datapack.build(dst, profile=prof, version='v1', display='1', patched=None, romfs=self.t / 'src-Test-data/RomFS', shaders=None)
            self.assertFalse(dst.exists()); self.assertFalse(dst.with_name(dst.name + '.partial').exists())
        datapack.check_romfs_packs(profile(packs=False, zstd=True))   # (zstd alone: as before)

    def test_profile_row(self):   # android.tsv romfs_packs: on or off, on only beside data_bundles; read as a bool
        sys.path.insert(0, str(REPO / 'nxnative/dm/android/tools')); import android_profile
        g = Path(tempfile.mkdtemp(prefix='nx2a_prof_', dir=self.t)) / '0100000000C00000'; g.mkdir()
        (g / 'game.tsv').write_text('title_id\t0100000000C00000\nname\tPack Test\n', encoding='utf-8')
        load = lambda: android_profile.AndroidProfile(types.SimpleNamespace(id=g.name, dir=g, get=lambda k, d='': d))
        for rows, want in (('data_bundles\t1048576\t67108864\nromfs_packs\ton\n', True), ('data_bundles\t1048576\t67108864\nromfs_packs\toff\n', False),
                           ('data_bundles\t1048576\t67108864\n', False), ('', False)):
            (g / 'android.tsv').write_text('status\texperimental\n' + rows, encoding='utf-8'); self.assertEqual(load().romfs_packs, want, rows)
        for rows in ('romfs_packs\ton\n', 'data_bundles\t1048576\t67108864\nromfs_packs\tyes\n', 'data_bundles\t1048576\t67108864\nromfs_packs\ton\nromfs_packs\ton\n'):
            (g / 'android.tsv').write_text('status\texperimental\n' + rows, encoding='utf-8')
            with self.assertRaises(android_profile.ProfileError, msg=rows): load()
        (g / 'android.tsv').write_text('status\texperimental\ndata_bundles\t1048576\t67108864\nromfs_packs\ton\nromfs\tzstd\n', encoding='utf-8')
        with self.assertRaisesRegex(BuildError, 'zstd'): datapack.check_romfs_packs(load())   # (the port refuses it: the profile alone loads)

    def test_kept_across_ports(self):   # a port again from the same files: the same index bytes, every pack kept (not written again); a changed file: its pack only
        src = self.t / 'src-Test-data'; dst = self.t / 'Keep-data'
        again = lambda **kw: datapack.build(dst, profile=profile(), version='v65536', display='1.0.1', patched=src / 'mods', romfs=src / 'RomFS', shaders=None, aoc=src / 'aoc', **kw)
        again(); first = {p.name: p.stat().st_mtime_ns for p in (dst / 'packs').glob('p*.tar')}; index = (dst / datapack.PACK_INDEX).read_bytes()
        self.assertTrue(all(k.startswith(('bundles/', 'packs/')) for k in json.loads(datapack.reuse_file(dst).read_text(encoding='utf-8'))))
        steps = []; again(progress=steps.append)
        self.assertEqual({p.name: p.stat().st_mtime_ns for p in (dst / 'packs').glob('p*.tar')}, first)
        self.assertIn(f'{len(first)} of {len(first)} packs kept from the last port (their files did not change)', steps)
        self.assertFalse([s for s in steps if 'into pack' in s], steps)
        self.assertEqual((dst / datapack.PACK_INDEX).read_bytes(), index)
        self.assertEqual(index, (self.pack / datapack.PACK_INDEX).read_bytes())   # (another folder, the same files: the same bytes)
        rel = 'RomFS/Data/d1/f001.bin'; st = (src / rel).stat(); os.utime(src / rel, ns=(st.st_atime_ns, st.st_mtime_ns + 10**9))
        again()
        k = next(e[1] for e in datapack.read_index(dst / datapack.PACK_INDEX)[1] if e[0] == rel[6:])
        self.assertEqual([n for n in first if (dst / 'packs' / n).stat().st_mtime_ns != first[n]], [Path(datapack.pack_name(k)).name])
        self.assertEqual((dst / datapack.PACK_INDEX).read_bytes(), index)   # (the same bytes in it)

    def test_own_check(self):   # the port reads every file back through the index: an index one record off is refused, and the pack folder is left as it was
        real = datapack.index_entries
        def off(folder, count):
            e = real(folder, count); n, k, o, s = e[3]; e[3] = (n, k, o + 512, s); return e
        dst = self.t / 'Check-data'; shutil.copytree(self.pack, dst)
        before = sorted(p.relative_to(dst).as_posix() for p in dst.rglob('*'))
        with mock.patch.object(datapack, 'index_entries', off), self.assertRaisesRegex(BuildError, 'packs\' check failed'): build(self.t, 'Check-data', src=self.t / 'src-Test-data')
        self.assertEqual(sorted(p.relative_to(dst).as_posix() for p in dst.rglob('*')), before)
        src = {r: self.t / 'src-Test-data/RomFS' / r for r in small_romfs(self.files)}
        datapack.check_packs(self.pack, src)   # (the real one passes)
        bad = self.t / 'Bad-index'; shutil.copytree(self.pack / 'packs', bad / 'packs')
        sizes, ents = datapack.read_index(bad / datapack.PACK_INDEX)
        for mutate, why in ((lambda e: e[:-1], 'missing'), (lambda e: [(n, k, o, s + 1) if i == 5 else (n, k, o, s) for i, (n, k, o, s) in enumerate(e)], 'not its file')):
            datapack.write_index(bad / datapack.PACK_INDEX, sizes, mutate(list(ents)))
            with self.assertRaisesRegex(BuildError, why): datapack.check_packs(bad, src)
        datapack.write_index(bad / datapack.PACK_INDEX, [s + 1 for s in sizes], ents)
        with self.assertRaisesRegex(BuildError, 'its index says'): datapack.check_packs(bad, src)
        with self.assertRaisesRegex(BuildError, 'twice'): datapack.write_index(bad / datapack.PACK_INDEX, sizes, ents + ents[:1])


def fixture_exe(d):
    exe = d / 'fx'
    r = subprocess.run([CC, '-O1', '-Wall', '-Wextra', '-std=gnu11', '-pthread', '-I', str(REPO / 'nxnative/dm/host'), str(Path(__file__).with_name('fs_pack_fixture.c')), '-o', str(exe)],
                       capture_output=True, text=True)
    assert r.returncode == 0 and not r.stderr.strip(), r.stderr   # without a warning
    return exe


@unittest.skipUnless(posix_cc(), f'no C compiler with POSIX headers (tried {", ".join(CC_TRIED)})')
class Host(unittest.TestCase):
    CROWD = 20_500   # files in one folder (Fire Emblem Engage: 38,000)

    @classmethod
    def setUpClass(cls):
        cls.t = Path(tempfile.mkdtemp(prefix='nx2a_test_pkc_'))
        cls.exe = fixture_exe(cls.t)
        cls.pack, cls.files = build(cls.t, 'Host-data', size=256 * 1024, crowd=cls.CROWD)
        cls.index = cls.pack / datapack.PACK_INDEX
        cls.small = small_romfs(cls.files)

    @classmethod
    def tearDownClass(cls): shutil.rmtree(cls.t, ignore_errors=True)

    def fx(self, *a, timeout=600):
        r = subprocess.run([str(self.exe), *map(str, a)], capture_output=True, timeout=timeout)
        self.assertEqual(r.returncode, 0, r.stderr.decode('utf-8', 'replace'))
        return r.stdout.decode('utf-8').splitlines()

    def test_whole_tree(self):   # every folder's own entries (names, kinds, sizes, order, counts) and every file's bytes, against the tree the pack was made from
        self.assertGreater(datapack.read_manifest(self.pack)['packs']['count'], 10)
        kids = {'': {}}
        for r, b in self.small.items():
            parts = r.split('/')
            for i in range(len(parts)):
                d = '/'.join(parts[:i]); kids.setdefault(d, {})
                if i < len(parts) - 1: kids[d][parts[i]] = None; kids.setdefault('/'.join(parts[:i + 1]), {})
                else: kids[d][parts[i]] = len(b)
        want_order = {d: sorted(c, key=lambda n: n.encode('utf-8') + (b'/' if c[n] is None else b'')) for d, c in kids.items()}
        out = self.fx('walk', self.index)
        self.assertEqual([ln for ln in out if ln.startswith('bad')], [])
        dirs = {f[1]: tuple(map(int, f[2:])) for f in (ln.split('\t') for ln in out) if f[0] == 'dir'}
        ents, files = {}, {}
        for f in (ln.split('\t') for ln in out):
            if f[0] == 'ent': ents.setdefault(f[1], []).append((f[2], f[3], int(f[4])))
            elif f[0] == 'file': files[f[1]] = (int(f[2]), f[3])
        self.assertEqual(set(dirs), set(kids))
        for d, c in kids.items():
            nf, nd = sum(v is not None for v in c.values()), sum(v is None for v in c.values())
            self.assertEqual(dirs[d], (nf, nd, nf + nd, 1), d)
            self.assertEqual(ents.get(d, []), [(n, 'd' if c[n] is None else 'f', c[n] or 0) for n in want_order[d]], d)
        self.assertEqual(files, {r: (len(b), hashlib.sha256(b).hexdigest()) for r, b in self.small.items()})
        self.assertEqual(dirs['crowd'][0], self.CROWD)

    def test_queries(self):   # what is not a file or not a folder, exactly; case and the names around the sort's edges
        q = {f[1]: tuple(map(int, f[2:])) for f in (ln.split('\t') for ln in self.fx('query', self.index, *QUERIES))}
        size = lambda r: len(self.small[r])
        want = {'top.bin': (1, size('top.bin'), 0, 0), 'a/c.txt': (1, 10, 0, 0), 'a/c': (0, 0, 1, 2), 'a/c0': (1, 12, 0, 0), 'a/c/x': (1, 11, 0, 0), 'a': (0, 0, 1, 4),
                'a/c/y': (0, 0, 1, 1), 'a/c.tx': (0, 0, 0, 0), 'a/c/': (0, 0, 0, 0), '/a/c.txt': (0, 0, 0, 0), 'A/c.txt': (0, 0, 0, 0), 'Data/big.bin': (0, 0, 0, 0),
                'Data/edge.bin': (1, MAX, 0, 0), 'Data/over.bin': (0, 0, 0, 0), 'nothing': (0, 0, 0, 0), 'nothing/at/all': (0, 0, 0, 0), 'top.bin/x': (0, 0, 0, 0),
                'empty.bin': (1, 0, 0, 0), LONG: (1, 700, 0, 0), '한글': (0, 0, 1, 1), '한글/파일.bin': (1, 300, 0, 0), '日本語/ファイル.dat': (1, 301, 0, 0), 'data/edge.bin': (0, 0, 0, 0),
                'crowd': (0, 0, 1, self.CROWD), 'crowd/f00000.bin': (1, 0, 0, 0), 'crowd/f20499.bin': (1, 20499 % 7, 0, 0), 'crowd/f20500.bin': (0, 0, 0, 0), 'a/b/': (0, 0, 0, 0)}
        if 'Case/A.bin' in self.small: want.update({'Case/A.bin': (1, 20, 0, 0), 'Case/a.bin': (1, 21, 0, 0), 'case/a.bin': (0, 0, 0, 0), 'Case': (0, 0, 1, 2)})
        else: print('\n(the file system folds case: A.bin and a.bin not both made)', file=sys.stderr)
        self.assertEqual({k: q[k] for k in want}, want)
        root = self.fx('query', self.index, '')[0].split('\t')
        self.assertEqual(root[2:5], ['0', '0', '1'])

    def test_pread(self):   # within the file only: an offset at or past its end reads nothing, a length past it is cut; the empty file
        b = self.small[LONG]; sha = lambda x: hashlib.sha256(x).hexdigest()
        for off, n in ((0, 700), (0, 10_000), (100, 50), (650, 100), (699, 5), (700, 1), (5000, 10), (0, 0)):
            self.assertEqual(self.fx('pread', self.index, LONG, off, n), [f'r\t{len(b[off:off + n])}\t{sha(b[off:off + n])}'], (off, n))
        self.assertEqual(self.fx('pread', self.index, 'empty.bin', 0, 100), [f'r\t0\t{sha(b"")}'])

    def test_threads(self):   # 8 threads reading every file at once, racing for the packs' first opens
        self.assertEqual(self.fx('threads', self.index), [f'threads ok {len(self.small)}'])

    def test_verify(self):
        self.assertEqual(self.fx('verify', self.index), ['ok'])

    def mutated(self, name, data, packs=None):   # an index of these bytes in a folder of its own, beside links to the packs (packs: {pack: bytes} in their place)
        d = self.t / 'mut' / name; shutil.rmtree(d, ignore_errors=True); d.mkdir(parents=True)
        for p in (self.pack / 'packs').glob('p*.tar'):
            k = int(p.stem[1:])
            if packs and k in packs: (d / p.name).write_bytes(packs[k])
            else: os.link(p, d / p.name)
        (d / 'romfs.nxbi').write_bytes(data); return d / 'romfs.nxbi'

    def test_broken_index_refused(self):   # pk_load: cut short, another magic or version, a name, a pack or a file's bytes outside their bounds
        b = bytearray(self.index.read_bytes()); _, _, np_, nf, nb, _ = struct.unpack_from('<4sIIIQQ', b)
        fo = 32 + 16 * np_; psize = struct.unpack_from('<Q', b, 32)[0]
        def put(fmt, off, *v): x = bytearray(b); struct.pack_into(fmt, x, off, *v); return bytes(x)
        first = next(i for i in range(nf) if struct.unpack_from('<H', b, fo + 24 * i + 6)[0] == 0 and struct.unpack_from('<Q', b, fo + 24 * i + 16)[0])   # a file of pack 0, not empty
        cases = {'cut': (bytes(b[:-1]), 'bytes, where its counts'), 'longer': (bytes(b) + b'x', 'bytes, where its counts'), 'header': (bytes(b[:20]), 'shorter than the header'),
                 'magic': (b'NXBJ' + bytes(b[4:]), 'not an NXBI index'), 'version': (put('<I', 4, 2), 'version 2'), 'reserved': (put('<Q', 24, 1), 'reserved'),
                 'pack reserved': (put('<Q', 40, 1), 'reserved'), 'nfiles': (put('<I', 12, nf + 1), 'bytes, where its counts'),
                 'name_off': (put('<I', fo + 24 * first, nb), 'is not inside the names'), 'name_len': (put('<H', fo + 24 * first + 4, 0), 'is not inside the names'),
                 'pack': (put('<H', fo + 24 * first + 6, np_), f'of {np_}'), 'data_off': (put('<Q', fo + 24 * first + 8, psize), 'do not fit'),
                 'unaligned': (put('<Q', fo + 24 * first + 8, 513), 'do not fit'), 'size': (put('<Q', fo + 24 * first + 16, psize), 'do not fit'),
                 'no header': (put('<Q', fo + 24 * first + 8, 0), 'do not fit')}
        for name, (data, why) in cases.items():
            out = self.fx('load', self.mutated(name.replace(' ', '_'), data))
            self.assertEqual(len(out), 1, name); self.assertTrue(out[0].startswith('err ') and why in out[0], f'{name}: {out}')
        self.assertEqual(self.fx('load', self.t / 'nothing.nxbi')[0][:16], 'err cannot open ')

    def test_pack_of_another_size(self):   # a pack that is not the index's size (a half download, an older port's): pk_open refuses it, the index itself loads
        p0 = (self.pack / 'packs/p0000.tar').read_bytes(); name = next(e[0] for e in datapack.read_index(self.index)[1] if e[1] == 0)
        for label, data in (('short', p0[:-512]), ('long', p0 + bytes(512))):
            idx = self.mutated(f'size_{label}', self.index.read_bytes(), {0: data})
            self.assertTrue(self.fx('load', idx)[0].startswith('ok '))
            self.assertEqual(self.fx('open', idx, name), ['err EIO'], label)
            self.assertIn('its size is not the index', self.fx('verify', idx)[0])
        other = next(e[0] for e in datapack.read_index(self.index)[1] if e[1] == 1)
        self.assertEqual(self.fx('open', self.mutated('size_short', self.index.read_bytes(), {0: p0[:-512]}), other), ['ok'])   # (the other packs still open)

    def test_verify_catches(self):   # pk_verify: files out of order; a record pointing at another file's bytes, or a block off
        b = bytearray(self.index.read_bytes()); _, _, np_, nf, nb, _ = struct.unpack_from('<4sIIIQQ', b); fo = 32 + 16 * np_
        rec = lambda i: bytes(b[fo + 24 * i:fo + 24 * i + 24])
        swap = bytearray(b); swap[fo:fo + 24], swap[fo + 24:fo + 48] = rec(1), rec(0)
        self.assertTrue(self.fx('load', self.mutated('swap', bytes(swap)))[0].startswith('ok '))   # (pk_load does not read the order)
        self.assertIn('not in order', self.fx('verify', self.mutated('swap', bytes(swap)))[0])
        ents = datapack.read_index(self.index)[1]
        i, k = next((i, k) for i in range(nf) for k in range(i + 1, min(nf, i + 50)) if ents[i][1] == ents[k][1] and ents[i][3] == ents[k][3] > 0)   # two of one size in one pack
        nxt = bytearray(b); struct.pack_into('<Q', nxt, fo + 24 * i + 8, ents[k][2])
        self.assertIn('another file\'s', self.fx('verify', self.mutated('next', bytes(nxt)))[0])
        j = next(j for j in range(10, nf) if ents[j][3] >= 512 + 1 and ents[j][2] + 512 + ents[j][3] <= datapack.read_index(self.index)[0][ents[j][1]])
        blk = bytearray(b); struct.pack_into('<Q', blk, fo + 24 * j + 8, ents[j][2] + 512)
        out = self.fx('verify', self.mutated('block', bytes(blk)))[0]
        self.assertTrue(out.startswith('err ') and 'tar header' in out, out)

    def test_speed(self):   # an index of 200,000 files, 40,000 in one folder: load, a million finds and that folder's walk in milliseconds, not seconds
        n_crowd, n = 40_000, 200_000
        names = [f'Data/big/f{i:05d}.bin' for i in range(n_crowd)] + [f'Data/d{i % 400:03d}/x{i:06d}.bin' for i in range(n - n_crowd)]
        idx = self.t / 'speed' / 'romfs.nxbi'
        datapack.write_index(idx, [1 << 40], [(nm, 0, 512 * (i + 1), 7) for i, nm in enumerate(names)])
        out = self.fx('bench', idx, 'Data/big', 1_000_000)[0].split()
        r = dict(zip(out[0::2], out[1::2]))
        print(f'\n(fs_pack.h, {n:,} files: load {float(r["load_ms"]):.1f} ms, 1,000,000 finds {float(r["find_ms"]):.1f} ms, '
              f'a folder of {n_crowd:,} walked in {float(r["walk_ms"]):.1f} ms)', file=sys.stderr)
        self.assertEqual((int(r['entries']), int(r['hits'])), (n_crowd, 1_000_000))
        self.assertLess(float(r['load_ms']), 2000); self.assertLess(float(r['find_ms']), 5000); self.assertLess(float(r['walk_ms']), 1000)
        self.assertEqual(self.fx('bench', idx, 'Data', 0)[0].split()[7], '401')   # Data: big/ and the 400 d*/ folders, each once


QUERIES = ['top.bin', 'a/c.txt', 'a/c', 'a/c0', 'a/c/x', 'a', 'a/c/y', 'a/c.tx', 'a/c/', '/a/c.txt', 'A/c.txt', 'Data/big.bin', 'Data/edge.bin', 'Data/over.bin', 'nothing',
           'nothing/at/all', 'top.bin/x', 'empty.bin', LONG, '한글', '한글/파일.bin', '日本語/ファイル.dat', 'data/edge.bin', 'crowd', 'crowd/f00000.bin', 'crowd/f20499.bin',
           'crowd/f20500.bin', 'a/b/', 'Case/A.bin', 'Case/a.bin', 'case/a.bin', 'Case']


if __name__ == '__main__':
    unittest.main()
