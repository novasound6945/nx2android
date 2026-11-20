# porter apk-update without a server, a device or the game's files (porter/apkupdate.py): the judgment of a data pack against this checkout's host (the REWRITE_ABI number,
# the rewriter's git history for a pack from before it, the profile header's defines), and the APK made again with every entry kept but the host and the manifest's version,
# signed by the same key (nxnative/dm/android/apk/axml.py, apkdriver.py). The pack is a folder made here; build.py is stood in for (python -m unittest discover tests).
import hashlib, json, os, shutil, subprocess, sys, tempfile, unittest, zipfile
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
os.environ.setdefault('NX2A_HOME', tempfile.mkdtemp(prefix='nx2a_test_home_'))
from porter import apkupdate, host, paths, pipeline, publish   # noqa: E402
from porter.upstream import BuildError                # noqa: E402
import axml, rewrite_abi                             # noqa: E402 (dm/android/apk, dm/tools: on sys.path through porter)

APK = REPO / 'nxnative/dm/android/apk'
TOOLS = REPO / 'nxnative/dm/tools'
ATRAIN, PKG = '0100A3E010E56000', 'org.nx2a.t0100a3e010e56000'   # a profile without patches.py, version v65536 registered
HAS_GIT = subprocess.run(['git', 'rev-parse', '--git-dir'], cwd=TOOLS, capture_output=True).returncode == 0


def sha(b): return hashlib.sha256(b).hexdigest()


def record(**over):   # a mods/build_src.sha256 of this checkout's rewriter, with some files' hashes replaced
    rows = {r: sha((TOOLS / r).read_bytes()) for r in ('patch_nso.py', 'nso_code.py', 'nso_asm.py', 'sdk_patches.py')}
    rows.update(over)
    return ''.join(f'{h}  {r}\n' for r, h in rows.items())


def make_pack(d, abi=None, profile_h=None, version='v65536', record_text=None, bid='00' * 16):
    """A data pack's small files in d: nx2a-data.json, files.json, mods/ (one module, main, with no registered patches)"""
    d = Path(d); (d / 'mods').mkdir(parents=True, exist_ok=True)
    man = dict(title_id=ATRAIN, name='A-Train', package=PKG, version=version, display='1.0.1', language='ko', requires=[], env_keys=[], files=1, bytes=1, dlc=[],
               sysdata=[], variants=[], made='2026-11-20 00:00:00', profile_h=profile_h if profile_h is not None else host.profile_hash(ATRAIN))
    if abi is not None: man['rewrite_abi'] = abi
    (d / 'nx2a-data.json').write_text(json.dumps(man, indent=2), encoding='utf-8')
    (d / 'mods/build_src.sha256').write_text(record() if record_text is None else record_text)
    (d / 'mods/main.cuts.tsv').write_text(f'# BUILD_ID\t{bid}\n'); (d / 'mods/main.obs.tsv').write_text(f'# BUILD_ID\t{bid}\n')
    (d / 'mods/main.fixups.tsv').write_text('# PATCH_OFF\t0x10000\n# TARGET\tandroid\n' + (f'# REWRITE_ABI\t{abi}\n' if abi is not None else '') + f'# BUILD_ID\t{bid}\n')
    files = [dict(path=p.relative_to(d).as_posix(), size=p.stat().st_size, sha256=sha(p.read_bytes()), mtime=1) for p in sorted(d.rglob('*')) if p.is_file()]
    (d / 'files.json').write_text(publish.dump(dict(format=1, id=publish.list_id(files), package=PKG, version=version, display='1.0.1', variants=[], bytes=sum(f['size'] for f in files),
                                                     made='x', files=files)), encoding='utf-8')
    return d


class Judgment(unittest.TestCase):
    def test_numbered_pack(self):   # a pack that carries REWRITE_ABI: the number alone decides
        now = rewrite_abi.current()
        mods = {'mods': [{'abi': now, 'name': 'main'}]}
        self.assertEqual(apkupdate.judge_rewrite({'rewrite_abi': now}, mods, '')['verdict'], 'same')
        self.assertEqual(apkupdate.judge_rewrite({'rewrite_abi': now + 1}, {'mods': [{'abi': now + 1, 'name': 'main'}]}, record())['verdict'], 'different')
        self.assertEqual(apkupdate.judge_rewrite({}, {'mods': [{'abi': now, 'name': 'main'}, {'abi': None, 'name': 'sdk'}]}, '')['verdict'], 'different')   # mixed: not one rewrite

    def test_header_and_fixups(self):   # the one definition: rewrite_abi.h, as patch_nso.py writes it and the host reads it
        self.assertEqual(rewrite_abi.current(), int(next(ln.split()[2] for ln in (REPO / 'nxnative/dm/host/rewrite_abi.h').read_text().splitlines() if ln.startswith('#define DM_REWRITE_ABI'))))
        self.assertEqual(rewrite_abi.from_fixups('# PATCH_OFF\t0x1\n# REWRITE_ABI\t7\n'), 7)
        self.assertIsNone(rewrite_abi.from_fixups('# PATCH_OFF\t0x1\n# TARGET\tandroid\n'))
        self.assertIn("rewrite_abi.current()", (TOOLS / 'patch_nso.py').read_text())

    def test_legacy_same_rewriter(self):   # no number: the rewriter's files the same as this checkout's
        r = apkupdate.judge_rewrite({}, {'mods': [{'abi': None, 'name': 'main'}]}, record())
        self.assertEqual(r['verdict'], 'same'); self.assertIsNone(r['abi'])
        self.assertEqual(apkupdate.judge_rewrite({}, {'mods': [{'abi': None, 'name': 'main'}]}, '')['verdict'], 'unknown')
        self.assertEqual(apkupdate.judge_rewrite({}, {'mods': [{'abi': None, 'name': 'main'}]}, record(**{'patch_nso.py': '0' * 64}))['verdict'], 'unknown')   # in no commit

    @unittest.skipUnless(HAS_GIT, 'no git history')
    def test_legacy_history(self):   # a version of the rewriter from git: the commits after it judged by COMMITS (or by their tokens: comments only)
        def of(commit): return sha(rewrite_abi.blob(commit, 'patch_nso.py'))
        for c, want in (('51c9e7603b516db77d6ff7a24436d3898af96f38', 'same'), ('df0f938a61ea84355e1c1640033ce38c36efaaf6', 'same'),
                        ('9d1c05ac29b0784a3ca05698db1b3b07edf9b587', 'different')):   # the pre-Android rewriter: 9013fc91 changed the contract
            if rewrite_abi.blob(c, 'patch_nso.py') is None: self.skipTest(f'{c[:8]} not in this clone')
            self.assertEqual(rewrite_abi.legacy_verdict({'patch_nso.py': of(c), 'nso_code.py': sha((TOOLS / 'nso_code.py').read_bytes()),
                                                         'nso_asm.py': sha((TOOLS / 'nso_asm.py').read_bytes())})[0], want, c)
        crlf = rewrite_abi.blob('51c9e7603b516db77d6ff7a24436d3898af96f38', 'patch_nso.py').replace(b'\n', b'\r\n')   # a Windows checkout's hash of the same version
        self.assertEqual(rewrite_abi.legacy_verdict({'patch_nso.py': sha(crlf), 'nso_code.py': sha((TOOLS / 'nso_code.py').read_bytes()), 'nso_asm.py': sha((TOOLS / 'nso_asm.py').read_bytes())})[0], 'same')
        with mock.patch.dict(rewrite_abi.COMMITS, {}, clear=True):   # a code change nobody judged: unknown, never same
            self.assertEqual(rewrite_abi.legacy_verdict({'patch_nso.py': of('51c9e7603b516db77d6ff7a24436d3898af96f38'), 'nso_code.py': sha((TOOLS / 'nso_code.py').read_bytes()),
                                                         'nso_asm.py': sha((TOOLS / 'nso_asm.py').read_bytes())})[0], 'unknown')

    def test_windows_record(self):   # backslashes in a pack rewritten on Windows: another game's patches.py is left out, as with '/'
        other = next(p for p in sorted((REPO / 'nxnative/dm/games').glob('*/patches.py')) if p.parent.name != ATRAIN)
        rec = record() + f'{"0" * 64}  ..\\games\\{other.parent.name}\\patches.py\n'
        self.assertEqual(host.record_rows(rec)[-1][1], f'../games/{other.parent.name}/patches.py')
        self.assertEqual(host.stale_sources(rec, ATRAIN), [])
        self.assertTrue(host.outside('C:/Users/x/build/patches.py'))

    def test_profile_change(self):   # which defines may change under a released pack
        old = b'// Generated\n#define DM_GAME_ID "X"\n#define DM_GAME_MODULES {"main", "sdk"}\n#define DM_GAME_MEM_ALIAS 0x10000000ull\n#define DM_GAME_DISPLAY_VERSIONS {"aa", "1.0"}\n'
        self.assertEqual(apkupdate.profile_change(old, old.replace(b'0x10000000', b'0x30000000')), (['DM_GAME_MEM_ALIAS'], []))
        self.assertEqual(apkupdate.profile_change(old, old + b'#define DM_GAME_FPS_CEILING 30\n'), (['DM_GAME_FPS_CEILING'], []))
        self.assertEqual(apkupdate.profile_change(old, old.replace(b'"sdk"}', b'"sdk", "subsdk0"}')), ([], ['DM_GAME_MODULES']))
        self.assertEqual(apkupdate.profile_change(old, old.replace(b'{"aa", "1.0"}', b'{"aa", "1.0"}, {"bb", "1.1"}')), (['DM_GAME_DISPLAY_VERSIONS'], []))   # a version added
        self.assertEqual(apkupdate.profile_change(old, old.replace(b'{"aa", "1.0"}', b'{"bb", "1.1"}')), ([], ['DM_GAME_DISPLAY_VERSIONS']))                  # the pack's gone
        self.assertEqual(apkupdate.profile_change(old, old + b'#define DM_GAME_SOMETHING_NEW 1\n'), ([], ['DM_GAME_SOMETHING_NEW']))                            # not known: refused
        self.assertEqual(apkupdate.profile_change(old, old.replace(b'// Generated', b'// Generated elsewhere')), ([], []))

    def test_judge_profile(self):   # the released header found in git: host-only changes go on with the new profile_h, data changes refuse, none found is unknown
        cur = host.profile_header(ATRAIN)
        layout = cur.replace(b'#define DM_GAME_MEM_STACK ', b'#define DM_GAME_MEM_STACK 0x1') if b'#define DM_GAME_MEM_STACK ' in cur else cur + b'#define DM_GAME_MEM_STACK 0x1\n'
        mods = cur.replace(b'#define DM_GAME_NMOD ', b'#define DM_GAME_NMOD 9')
        man = lambda h: dict(version='v65536', profile_h=h)
        self.assertEqual(apkupdate.judge_profile(man(sha(cur)), ATRAIN)['verdict'], 'same')
        self.assertEqual(apkupdate.judge_profile(man(sha(cur.replace(b'\n', b'\r\n'))), ATRAIN)['verdict'], 'same')   # made on Windows
        with mock.patch.object(apkupdate, 'header_history', return_value=[('c0ffee' * 6 + 'abcd', layout), ('beef' * 10, mods)]):
            r = apkupdate.judge_profile(man(sha(layout)), ATRAIN)
            self.assertEqual((r['verdict'], r['changed'], r['profile_h']), ('host-only', ['DM_GAME_MEM_STACK'], sha(cur)))
            self.assertEqual(apkupdate.judge_profile(man(sha(mods)), ATRAIN)['verdict'], 'different')
            self.assertEqual(apkupdate.judge_profile(man('0' * 64), ATRAIN)['verdict'], 'unknown')
        self.assertEqual(apkupdate.judge_profile(dict(version='v999999', profile_h=sha(cur)), ATRAIN, modules=None)['verdict'], 'unknown')   # a derived profile's port, nothing to derive from
        self.assertEqual(apkupdate.judge_profile(man(sha(cur)), ATRAIN, old_header=b'another')['verdict'], 'different')   # --profile-header that is not the pack's

    def test_judge_pack(self):
        with tempfile.TemporaryDirectory() as t:
            r = apkupdate.judge(apkupdate.Pack(make_pack(t, abi=rewrite_abi.current())))
            self.assertEqual((r['verdict'], r['ok'], r['rewrite']['abi']), ('same', True, rewrite_abi.current()))
            r = apkupdate.judge(apkupdate.Pack(make_pack(Path(t) / 'b', abi=rewrite_abi.current() + 1)))
            self.assertEqual((r['verdict'], r['ok']), ('different', False))


class ApkUpdate(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp(prefix='nx2a_apkupdate_test_')); self.addCleanup(shutil.rmtree, self.t, ignore_errors=True)
        so, dex, drv, asset, icon = (self.t / n for n in ('libdmhost.so', 'classes.dex', 'libvulkan.test.so', 'theme.tsv', 'icon.png'))
        so.write_bytes(b'\x7fELF old host' * 64); dex.write_bytes(b'dex\n035\0' * 16); drv.write_bytes(b'\x7fELF driver' * 64); asset.write_text('theme\n'); icon.write_bytes(b'\x89PNG fake')
        src = self.t / 'source.bin'; src.write_bytes(publish.source_asset('https://example.invalid/pack/', 'u:p'))
        self.apk = self.t / 'ATrain.apk'
        subprocess.run([sys.executable, str(APK / 'mkapk.py'), str(so), str(self.apk), str(paths.signing_key()), '--package', PKG, '--label', 'A-Train', '--lib', 'dmhost',
                        '--version-code', '65542', '--version-name', '1.0.1-5', '--release', '--landscape', '--icon', str(icon), '--dex', str(dex), '--activity', 'org.nx2a.GameActivity',
                        '--asset', f'overlay/theme.tsv={asset}', '--asset', f'nx2a/source.bin={src}', '--extra-so', str(drv)]
                       + [a for k, f in pipeline.ui_font_assets().items() for a in ('--asset', f'{k}={f}')], check=True, capture_output=True)   # (a released APK has the overlay font)
        self.builds = []
        def build_host(package, title_id, debug, out, jobs=1, games_dir=None):   # build.py stood in for
            self.builds.append((title_id, debug, games_dir)); p = Path(out) / 'libdmhost.so'; p.write_bytes(b'\x7fELF new host' * 64); return p
        p = mock.patch.object(apkupdate.host, 'build_host', side_effect=build_host); p.start(); self.addCleanup(p.stop)

    def test_manifest_version(self):   # the manifest's version changed in place: every other attribute as mkapk.py wrote it
        with zipfile.ZipFile(self.apk) as z: m = z.read('AndroidManifest.xml')
        a = axml.info(m)
        b = axml.info(axml.set_version(m, 65545, '1.0.1-8'))
        self.assertEqual((a['version_code'], a['version_name']), (65542, '1.0.1-5'))
        self.assertEqual((b['version_code'], b['version_name']), (65545, '1.0.1-8'))
        self.assertEqual({k: v for k, v in a.items() if not k.startswith('version')}, {k: v for k, v in b.items() if not k.startswith('version')})
        self.assertEqual(axml.parse(axml.set_version(m, 7, 'A-Train'))['strings'], axml.parse(m)['strings'])   # a name the pool has: no string added
        with self.assertRaises(ValueError): axml.set_version(m, 0, 'x')

    def test_update_keeps_the_apk(self):
        d = make_pack(self.t / 'pack')
        r = apkupdate.update(self.apk, apkupdate.Pack(d), self.t / 'out', build=8, jobs=1)
        new = Path(r['apk'])
        self.assertEqual((r['version_code'], r['version_name']), (65545, '1.0.1-8')); self.assertEqual(self.builds, [(ATRAIN, False, None)])
        with zipfile.ZipFile(self.apk) as a, zipfile.ZipFile(new) as b:
            self.assertEqual([i.filename for i in a.infolist()], [i.filename for i in b.infolist()])   # the same entries in the same order
            changed = [n for n in a.namelist() if a.read(n) != b.read(n)]
            self.assertEqual(changed, ['AndroidManifest.xml', 'lib/arm64-v8a/libdmhost.so'])
            self.assertEqual(b.read('lib/arm64-v8a/libdmhost.so'), b'\x7fELF new host' * 64)
            self.assertTrue(all(i.compress_type == zipfile.ZIP_STORED for i in b.infolist()))
        oi, ni = axml.apk_info(self.apk), axml.apk_info(new)
        self.assertEqual(ni['signers'], oi['signers']); self.assertEqual(len(ni['signers']), 1)
        self.assertEqual({k: ni[k] for k in ('package', 'label', 'activity', 'debuggable')}, {k: oi[k] for k in ('package', 'label', 'activity', 'debuggable')})
        self.assertEqual(r['data_files'], [])   # the profile is this checkout's: nothing for the server
        self.assertEqual(apkupdate.source_of(new), ('https://example.invalid/pack/', 'u:p'))
        self.assertEqual(json.loads((paths.user_dir() / 'builds.json').read_text())[PKG], 8)

    def test_update_refusals(self):
        d = make_pack(self.t / 'pack')
        for build, says in ((5, 'not above'), (4, 'not above'), (70000, 'versionCode offset')):
            with self.assertRaises(BuildError) as e: apkupdate.update(self.apk, apkupdate.Pack(d), self.t / 'out', build=build)
            self.assertIn(says, str(e.exception))
        with self.assertRaises(BuildError) as e: apkupdate.update(self.apk, apkupdate.Pack(make_pack(self.t / 'p2', abi=rewrite_abi.current() + 1)), self.t / 'out', build=9)
        self.assertIn('does not fit', str(e.exception))
        with self.assertRaises(BuildError) as e: apkupdate.update(self.apk, apkupdate.Pack(d), self.apk.parent, build=9)
        self.assertIn('over itself', str(e.exception))
        other = self.t / 'other-key.json'
        so = self.t / 'libdmhost.so'
        subprocess.run([sys.executable, str(APK / 'mkapk.py'), str(so), str(self.t / 'o.apk'), str(other), '--package', PKG, '--lib', 'dmhost', '--version-code', '2'], check=True, capture_output=True)
        with self.assertRaises(BuildError) as e: apkupdate.update(self.t / 'o.apk', apkupdate.Pack(d), self.t / 'out', build=9)
        self.assertIn('another key', str(e.exception))
        self.assertEqual(self.builds, [])   # refused before the host was built

    def test_update_new_profile_h(self):   # a host-only profile change: the pack's nx2a-data.json again, and its files.json with that file's new hash
        cur = host.profile_header(ATRAIN); old = cur + b'#define DM_GAME_FPS_HOLD 0\n'
        d = make_pack(self.t / 'pack', profile_h=sha(old))
        try: last = json.loads((paths.user_dir() / 'builds.json').read_text()).get(PKG, 0)
        except (OSError, ValueError): last = 0
        with mock.patch.object(apkupdate, 'header_history', return_value=[('ab' * 20, old)]):
            r = apkupdate.update(self.apk, apkupdate.Pack(d), self.t / 'out', jobs=1)
        self.assertEqual(r['version_name'], f'1.0.1-{max(5, last) + 1}')   # by default one more than the APK's build (5) and this computer's last
        self.assertEqual(r['data_files'], ['nx2a-data.json', 'files.json'])
        m = json.loads((self.t / 'out/nx2a-data.json').read_text()); self.assertEqual(m['profile_h'], sha(cur))
        fj = json.loads((self.t / 'out/files.json').read_text()); old_fj = json.loads((d / 'files.json').read_text())
        entry = next(f for f in fj['files'] if f['path'] == 'nx2a-data.json')
        self.assertEqual(entry['sha256'], sha((self.t / 'out/nx2a-data.json').read_bytes())); self.assertEqual(fj['id'], publish.list_id(fj['files']))
        self.assertEqual([f for f in fj['files'] if f['path'] != 'nx2a-data.json'], [f for f in old_fj['files'] if f['path'] != 'nx2a-data.json'])


if __name__ == '__main__':
    unittest.main()
