# porter apk-update without a server, a device or the game's files (porter/apkupdate.py): the judgment of a data pack against this checkout's host (the REWRITE_ABI number,
# the rewriter's git history for a pack from before it, the profile header's defines), and the APK made again with every entry kept but the host and the manifest's version,
# signed by the same key (nxnative/dm/android/apk/axml.py, apkdriver.py). The pack is a folder made here; build.py is stood in for (python -m unittest discover tests).
import hashlib, json, os, shutil, struct, subprocess, sys, tempfile, unittest, zipfile, zlib
from pathlib import Path
from unittest import mock

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
os.environ.setdefault('NX2A_HOME', tempfile.mkdtemp(prefix='nx2a_test_home_'))
from porter import apkupdate, host, paths, pipeline, publish   # noqa: E402
from porter.upstream import BuildError                # noqa: E402
import axml, nacp, rewrite_abi                       # noqa: E402 (dm/android/apk, dm/tools: on sys.path through porter)

APK = REPO / 'nxnative/dm/android/apk'
TOOLS = REPO / 'nxnative/dm/tools'
ATRAIN, PKG = '0100A3E010E56000', 'org.nx2a.t0100a3e010e56000'   # a profile without patches.py, version v65536 registered
KO = 'A열차로 가자: 시작해요 관광지 개발'   # its name_ko (the Korean name in its control data)
WAKE = 'android.permission.WAKE_LOCK'
RELEASED = Path.home() / 'd2rm/ports/release-2026-11-21/OnePieceGrandGourmet/OnePieceGrandGourmet.apk'   # a released port (read only), when this computer has it
AAPT2 = Path.home() / 'Library/Android/sdk/build-tools/32.1.0-rc1/aapt2'
APKSIGNER = AAPT2.with_name('apksigner')
SERVICE_BASE = '5f6f417b4eeaaa49a00ac065ff816a423b7ea574'   # the last mkapk.py from before the background download's service
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
        self.mkapk_args = ([str(paths.signing_key()), '--package', PKG, '--label', 'A-Train', '--lib', 'dmhost',
                            '--version-code', '65542', '--version-name', '1.0.1-5', '--release', '--landscape', '--icon', str(icon), '--dex', str(dex), '--activity', 'org.nx2a.GameActivity',
                            '--asset', f'overlay/theme.tsv={asset}', '--asset', f'nx2a/source.bin={src}', '--extra-so', str(drv)]
                           + [a for k, f in pipeline.ui_font_assets().items() for a in ('--asset', f'{k}={f}')])   # (a released APK has the overlay font)
        subprocess.run([sys.executable, str(APK / 'mkapk.py'), str(so), str(self.apk)] + self.mkapk_args, check=True, capture_output=True)
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

    def test_manifest_label(self):   # the label changed in place, in Korean (UTF-16 in the pool): every other attribute as it was, the version too
        with zipfile.ZipFile(self.apk) as z: m = z.read('AndroidManifest.xml')
        a = axml.info(m); n = axml.set_label(m, KO); b = axml.info(n)
        self.assertEqual((a['label'], b['label']), ('A-Train', KO))
        self.assertEqual({k: v for k, v in a.items() if k != 'label'}, {k: v for k, v in b.items() if k != 'label'})
        self.assertEqual(axml.parse(n)['strings'], axml.parse(m)['strings'] + [KO])   # appended: the indexes before it (the resource map's) do not move
        self.assertEqual(axml.info(axml.set_version(n, 65545, '1.0.1-8'))['label'], KO)   # the version after it keeps it
        self.assertEqual(axml.set_label(m, 'A-Train'), m)   # the same label: the same bytes
        self.assertEqual(axml.info(axml.set_label(m, 'A열차 🚃'))['label'], 'A열차 🚃')   # beyond the BMP: two UTF-16 units
        for bad in ('', 'a\nb'):
            with self.assertRaises(ValueError): axml.set_label(m, bad)
        el = axml.parse(m)['elements']; off = next(at['android:label'][0] for tag, at in el if tag == 'application')
        ref = bytearray(m); struct.pack_into('<IHBBI', ref, off + 8, 0xffffffff, 8, 0, 0x01, 0x7f030000)   # @string/app_name: not one mkapk.py writes
        with self.assertRaises(ValueError) as e: axml.set_label(bytes(ref), KO)
        self.assertIn('resource reference', str(e.exception))

    def old_apk(self, name='old.apk', without=(WAKE,)):   # the APK of an older mkapk.py, one that did not ask for the permissions without (a copy with them left out)
        d = self.t / 'oldmkapk'; d.mkdir(exist_ok=True)
        shutil.copy(APK / 'apksign.py', d); shutil.copy(APK / 'service.dex', d)
        src = (APK / 'mkapk.py').read_text(encoding='utf-8')
        for p in without:
            line = f"    ('{p}', []),\n"; self.assertIn(line, src); src = src.replace(line, '')
        (d / 'mkapk.py').write_text(src, encoding='utf-8')
        out = self.t / name
        subprocess.run([sys.executable, str(d / 'mkapk.py'), str(self.t / 'libdmhost.so'), str(out)] + self.mkapk_args, check=True, capture_output=True)
        return out

    def test_manifest_permissions(self):   # a permission the APK lacks put in before <application>: the pool appended to, every other element and attribute as it was
        want = apkupdate.permissions()
        self.assertIn((WAKE, []), want); self.assertIn(('android.permission.READ_EXTERNAL_STORAGE', [('maxSdkVersion', 29)]), want)
        with zipfile.ZipFile(self.apk) as z: cur = z.read('AndroidManifest.xml')
        with zipfile.ZipFile(self.old_apk()) as z: old = z.read('AndroidManifest.xml')
        self.assertEqual(axml.add_permissions(cur, want), (cur, []))   # this mkapk.py's: the same bytes
        self.assertNotIn(WAKE, axml.permissions(old))
        n, added = axml.add_permissions(old, want)
        self.assertEqual(added, [WAKE])
        self.assertEqual(axml.permissions(n), axml.permissions(old) + [WAKE])   # after the others
        self.assertEqual(axml.parse(n)['strings'], axml.parse(old)['strings'] + [WAKE])   # appended: the resource map's indexes do not move
        self.assertEqual(axml.info(n), axml.info(old))
        el = lambda m: [(tag, {k: axml._value(axml.parse(m)['strings'], v) for k, v in a.items()}) for tag, a in axml.parse(m)['elements']]
        eo, en = el(old), el(n); i = next(k for k, (tag, _) in enumerate(eo) if tag == 'application')
        self.assertEqual(en, eo[:i] + [('uses-permission', {'android:name': WAKE})] + eo[i:])   # one element more, right before <application>
        self.assertEqual(sorted(el(n), key=repr), sorted(el(cur), key=repr))   # what this mkapk.py writes, but for the order
        self.assertEqual(axml.add_permissions(n, want), (n, []))   # once only
        self.assertEqual(axml.info(axml.set_version(n, 65545, '1.0.1-8'))['version_name'], '1.0.1-8')   # changed further after it
        n2, added = axml.add_permissions(old, [('org.nx2a.TEST', [('maxSdkVersion', 33)])])   # with an int attribute
        self.assertEqual((added, el(n2)[i][1]), (['org.nx2a.TEST'], {'android:name': 'org.nx2a.TEST', 'android:maxSdkVersion': 33}))
        with self.assertRaises(ValueError) as e: axml.add_permissions(old, [('org.nx2a.TEST', [('protectionLevel', 1)])])   # an attribute the map has not
        self.assertIn('resource map', str(e.exception))

    def test_update_adds_permissions(self):   # an APK released before WAKE_LOCK: apk-update gives it the permission, and nothing more for one that has it
        d = make_pack(self.t / 'pack')
        r = apkupdate.update(self.old_apk(), apkupdate.Pack(d), self.t / 'out', build=8, jobs=1)
        self.assertEqual(r['permissions_added'], [WAKE])
        with zipfile.ZipFile(r['apk']) as z: self.assertEqual(axml.permissions(z.read('AndroidManifest.xml'))[-1], WAKE)
        self.assertEqual(apkupdate.update(self.apk, apkupdate.Pack(d), self.t / 'out2', build=8, jobs=1)['permissions_added'], [])   # (build 8 as the other tests: builds.json is shared)

    def test_update_savecloud(self):   # the cloud save server: an APK without assets/nx2a/savecloud.txt gets the porter's savecloud.txt, one with it keeps its own, none: none
        d = make_pack(self.t / 'pack'); f = paths.user_dir() / publish.SAVECLOUD_FILE; self.addCleanup(lambda: f.unlink(missing_ok=True)); f.unlink(missing_ok=True)
        name = 'assets/' + publish.SAVECLOUD_ASSET
        r = apkupdate.update(self.apk, apkupdate.Pack(d), self.t / 'out', build=8, jobs=1)
        self.assertIsNone(r['savecloud'])
        with zipfile.ZipFile(r['apk']) as z: self.assertNotIn(name, z.namelist())
        f.write_text('url=https://saves.example.com/\n', encoding='utf-8')
        said = []
        with mock.patch.object(apkupdate, 'say', side_effect=said.append): r = apkupdate.update(self.apk, apkupdate.Pack(d), self.t / 'out2', build=8, jobs=1)
        out = '\n'.join(said)
        self.assertEqual(r['savecloud'], 'added')
        self.assertIn('cloud save server: configured', out); self.assertNotIn('saves.example.com', out)   # (the address is never printed)
        with zipfile.ZipFile(r['apk']) as z: self.assertEqual(z.read(name), b'url=https://saves.example.com\n')
        f.write_text('url=https://other.example.com\n', encoding='utf-8')
        r2 = apkupdate.update(r['apk'], apkupdate.Pack(d), self.t / 'out3', build=9, jobs=1)
        self.assertEqual(r2['savecloud'], 'kept')
        with zipfile.ZipFile(r2['apk']) as z: self.assertEqual(z.read(name), b'url=https://saves.example.com\n')   # the APK's own, not this computer's

    @unittest.skipUnless(RELEASED.is_file() and AAPT2.is_file(), 'a released APK and aapt2 (the Android SDK) on this computer')
    def test_released_manifest_permissions(self):   # a released port's manifest (read only) with WAKE_LOCK added: aapt2 reads it as Android would
        with zipfile.ZipFile(RELEASED) as a: man = a.read('AndroidManifest.xml')
        n, added = axml.add_permissions(man, apkupdate.permissions())
        if WAKE in axml.permissions(man): self.skipTest(f'{RELEASED} has {WAKE} already')
        self.assertEqual(added, [p for p, _ in apkupdate.permissions() if p not in axml.permissions(man)])   # WAKE_LOCK, and the background download's
        self.assertIn(WAKE, added)
        out = self.t / 'released.apk'
        with zipfile.ZipFile(RELEASED) as a, zipfile.ZipFile(out, 'w') as b:
            for i in a.infolist():
                b.writestr(i, n if i.filename == 'AndroidManifest.xml' else a.read(i))
        dump = lambda what, apk: subprocess.run([str(AAPT2), 'dump', what, str(apk)], capture_output=True, text=True, check=True).stdout
        self.assertIn(f"uses-permission: name='{WAKE}'", dump('permissions', out))
        before, after = dump('badging', RELEASED), dump('badging', out)
        self.assertEqual([x for x in after.splitlines() if not any(p in x for p in added)], before.splitlines())   # the label, the version, the rest as they were
        self.assertIn("application-label:", after)

    def base_apk(self, name, own_dex):   # a port of the mkapk.py from before the background download's service (no <service>, no service dex; hasCode only with a dex)
        d = self.t / 'basemkapk'; d.mkdir(exist_ok=True)
        for f in ('mkapk.py', 'apksign.py'):
            r = subprocess.run(['git', '-C', str(REPO), 'show', f'{SERVICE_BASE}:nxnative/dm/android/apk/{f}'], capture_output=True)
            if r.returncode: self.skipTest(f'{SERVICE_BASE[:8]} not in this clone')
            (d / f).write_bytes(r.stdout)
        args = self.mkapk_args if own_dex else [a for i, a in enumerate(self.mkapk_args) if a not in ('--dex', '--activity') and self.mkapk_args[i - 1] not in ('--dex', '--activity')]
        out = self.t / name
        subprocess.run([sys.executable, str(d / 'mkapk.py'), str(self.t / 'libdmhost.so'), str(out)] + args, check=True, capture_output=True)
        return out

    def test_update_service_added(self):   # a released APK without the service: its <service>, hasCode, the permissions and the dex (classes.dex, or classes2.dex beside its own)
        d = make_pack(self.t / 'pack'); svc = (APK / 'service.dex').read_bytes()
        for own, dex_name, out in ((False, 'classes.dex', 'out-a'), (True, 'classes2.dex', 'out-b')):
            old = self.base_apk(f'base-{own}.apk', own)
            with zipfile.ZipFile(old) as z: self.assertFalse(any(apkupdate.SERVICE_MARK in z.read(n) for n in z.namelist() if n.endswith('.dex')))
            r = apkupdate.update(old, apkupdate.Pack(d), self.t / out, build=8, jobs=1)
            self.assertEqual((r['service_dex'], r['services_added']), ('added', ['org.nx2a.DownloadService']))
            for p in ('android.permission.FOREGROUND_SERVICE', 'android.permission.FOREGROUND_SERVICE_DATA_SYNC', 'android.permission.POST_NOTIFICATIONS'): self.assertIn(p, r['permissions_added'])
            with zipfile.ZipFile(r['apk']) as z, zipfile.ZipFile(old) as a:
                self.assertEqual(z.read(dex_name), svc)
                if own: self.assertEqual(z.read('classes.dex'), a.read('classes.dex'))   # the port's own dex kept
                self.assertEqual([n for n in z.namelist() if n != dex_name], a.namelist())   # every other entry kept, in order
                m = z.read('AndroidManifest.xml')
            self.assertEqual(axml.services(m), ['org.nx2a.DownloadService'])
            self.assertEqual(axml.info(m)['activity'], axml.info(old.read_bytes() and zipfile.ZipFile(old).read('AndroidManifest.xml'))['activity'])
            if AAPT2.is_file():
                tree = subprocess.run([str(AAPT2), 'dump', 'xmltree', '--file', 'AndroidManifest.xml', r['apk']], capture_output=True, text=True, check=True).stdout
                self.assertIn('android:foregroundServiceType(0x01010599)=0x00000001', tree); self.assertIn('android:hasCode(0x0101000c)=true', tree)
            if APKSIGNER.is_file():
                v = subprocess.run([str(APKSIGNER), 'verify', '--min-sdk-version', '29', r['apk']], capture_output=True, text=True)
                self.assertEqual(v.returncode, 0, v.stdout + v.stderr)
            r2 = apkupdate.update(r['apk'], apkupdate.Pack(d), self.t / (out + '2'), build=9, jobs=1)   # again: nothing more
            self.assertEqual((r2['service_dex'], r2['services_added'], r2['permissions_added']), ('kept', [], []))

    def test_update_service_replaced(self):   # the service's dex an earlier update put in, replaced by this checkout's in its place
        d = make_pack(self.t / 'pack'); older = b'dex\n035\0' + apkupdate.SERVICE_MARK + b' an older build'
        src = self.t / 'older.apk'
        with zipfile.ZipFile(self.apk) as a: entries = [(i.filename, older if i.filename == 'classes2.dex' else a.read(i), 16384 if i.filename.startswith('lib/') else 4) for i in a.infolist()]
        import apksign, json as _j
        kj = _j.loads(paths.signing_key().read_text()); k = {n: int(v) for n, v in kj.items() if n != 'cert'}
        z, cd, eocd = apksign.build_zip(entries); src.write_bytes(apksign.sign_v2(z, cd, eocd, k, bytes.fromhex(kj['cert'])))
        r = apkupdate.update(src, apkupdate.Pack(d), self.t / 'out', build=8, jobs=1)
        self.assertEqual(r['service_dex'], 'updated')
        with zipfile.ZipFile(r['apk']) as z: self.assertEqual(z.read('classes2.dex'), (APK / 'service.dex').read_bytes()); self.assertNotIn('classes3.dex', z.namelist())

    def test_mkapk_korean_label(self):   # mkapk.py's pool: the label's length in UTF-16 units
        out = self.t / 'ko.apk'
        subprocess.run([sys.executable, str(APK / 'mkapk.py'), str(self.t / 'libdmhost.so'), str(out), str(paths.signing_key()), '--package', PKG, '--label', 'A열차 🚃',
                        '--lib', 'dmhost'], check=True, capture_output=True)
        self.assertEqual(axml.apk_info(out)['label'], 'A열차 🚃')

    def test_nacp_korean_name(self):   # the Korean entry (12) of the title area, plain and compressed; the first name without a language
        t = bytearray(0x3000)
        for i, n in ((0, 'A-Train: All Aboard! Tourism'), (2, 'A列車で行こう はじまる観光計画'), (12, KO)): u = n.encode(); t[i * 0x300:i * 0x300 + len(u)] = u
        plain = bytes(t) + bytes(0x1000)
        c = zlib.compressobj(9, zlib.DEFLATED, -15); z = c.compress(bytes(t) + bytes(0x3000)) + c.flush()
        packed = struct.pack('<H', len(z)) + z; packed += bytes(0x4000 - len(packed))
        for b in (plain, packed):
            f = self.t / 'control.nacp'; f.write_bytes(b)
            self.assertEqual((nacp.name(f), nacp.name(f, 'ko'), nacp.name(f, 'ja'), nacp.name(f, 'fr')), ('A-Train: All Aboard! Tourism', KO, 'A列車で行こう はじまる観光計画', ''))
        self.assertEqual(nacp.name(self.t / 'none.nacp', 'ko'), '')
        with self.assertRaises(ValueError): nacp.name(f, 'xx')

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
        self.assertEqual({k: ni[k] for k in ('package', 'activity', 'debuggable')}, {k: oi[k] for k in ('package', 'activity', 'debuggable')})
        self.assertEqual((oi['label'], ni['label'], r['label']), ('A-Train', KO, KO))   # the profile's name_ko
        self.assertEqual(apkupdate.profile_label(ATRAIN), KO)
        self.assertEqual(r['data_files'], [])   # the profile is this checkout's: nothing for the server (name_ko is not in its header)
        self.assertEqual(apkupdate.source_of(new), ('https://example.invalid/pack/', 'u:p'))
        self.assertEqual(json.loads((paths.user_dir() / 'builds.json').read_text())[PKG], 8)

    def test_update_label_kept(self):   # the APK has the profile's name_ko already, or the profile has none: its label as it was
        d = make_pack(self.t / 'pack')
        r = apkupdate.update(self.apk, apkupdate.Pack(d), self.t / 'out', build=8, jobs=1)
        r2 = apkupdate.update(r['apk'], apkupdate.Pack(d), self.t / 'out2', build=9, jobs=1)
        with zipfile.ZipFile(r['apk']) as a, zipfile.ZipFile(r2['apk']) as b:
            self.assertEqual(axml.parse(a.read('AndroidManifest.xml'))['strings'] + ['1.0.1-9'], axml.parse(b.read('AndroidManifest.xml'))['strings'])   # the new version name only: no label again
        self.assertEqual(r2['label'], KO)
        with mock.patch.object(apkupdate, 'profile_label', return_value=''):
            r3 = apkupdate.update(self.apk, apkupdate.Pack(d), self.t / 'out3', build=10, jobs=1)
        self.assertEqual(r3['label'], 'A-Train')

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

    def mkapk(self, name, *over):   # an APK of this checkout's mkapk.py with some of its arguments replaced (flag, value pairs)
        args = list(self.mkapk_args)
        for k, v in zip(over[::2], over[1::2]): args[args.index(k) + 1] = v
        out = self.t / name
        subprocess.run([sys.executable, str(APK / 'mkapk.py'), str(self.t / 'libdmhost.so'), str(out)] + args, check=True, capture_output=True)
        return out

    def test_manifest_package(self):   # the package replaced in place; class names relative to the old package written out with it; what cannot be renamed refuses
        rel = self.mkapk('rel.apk', '--activity', '.GameActivity')
        with zipfile.ZipFile(rel) as z: m = z.read('AndroidManifest.xml')
        n, changed = axml.set_package(m, 'kr.example.atrain')
        self.assertEqual(changed, [('activity', '.GameActivity', PKG + '.GameActivity')])
        a, b = axml.parse(m), axml.parse(n)
        self.assertEqual(axml.info(n), dict(axml.info(m), package='kr.example.atrain', activity=PKG + '.GameActivity'))
        self.assertEqual(b['strings'][:len(a['strings'])], a['strings'])   # appended only: the resource map's indexes stay
        self.assertEqual(axml.services(n), axml.services(m)); self.assertEqual(axml.permissions(n), axml.permissions(m))
        with zipfile.ZipFile(self.apk) as z: m2 = z.read('AndroidManifest.xml')
        n2, changed = axml.set_package(m2, 'kr.wook.srw.srwvswitchns')
        self.assertEqual(changed, []); self.assertEqual(axml.info(n2)['package'], 'kr.wook.srw.srwvswitchns'); self.assertEqual(axml.info(n2)['activity'], 'org.nx2a.GameActivity')
        self.assertEqual(axml.set_package(m2, PKG), (m2, []))   # the same package: the same bytes
        for bad in ('srwv', 'kr.wook.', 'kr..x', 'kr.1x', 'kr.wook-srw', 'kr.한글', ''):
            with self.assertRaises(ValueError, msg=bad): axml.set_package(m2, bad)

    @unittest.skipUnless(AAPT2.is_file(), 'aapt2 (the Android SDK) on this computer')
    def test_manifest_package_aapt2(self):   # aapt2 reads the renamed manifest as Android would: the new package, the activity and the label as they were
        rel = self.mkapk('rel2.apk', '--activity', '.GameActivity')
        out = self.t / 'renamed.apk'
        with zipfile.ZipFile(rel) as a, zipfile.ZipFile(out, 'w') as b:
            for i in a.infolist(): b.writestr(i, axml.set_package(a.read(i), 'kr.example.atrain')[0] if i.filename == 'AndroidManifest.xml' else a.read(i))
        dump = subprocess.run([str(AAPT2), 'dump', 'badging', str(out)], capture_output=True, text=True, check=True).stdout
        self.assertIn("package: name='kr.example.atrain'", dump)
        self.assertIn(f"launchable-activity: name='{PKG}.GameActivity'", dump)

    def test_update_package(self):   # --package: another app with the old one's everything (label, signer, services, the data server), reading the old package's pack
        new = 'kr.example.atrain'; d = make_pack(self.t / 'pack')
        def seen():
            try: return json.loads((paths.user_dir() / 'builds.json').read_text())
            except (OSError, ValueError): return {}
        want = max(5, seen().get(PKG, 0), seen().get(new, 0)) + 1   # one more than the APK's build (5) and both packages' last
        said = []
        with mock.patch.object(apkupdate, 'say', side_effect=said.append): r = apkupdate.update(self.apk, apkupdate.Pack(d), self.t / 'out', jobs=1, package=new)
        self.assertEqual((r['package'], r['data_package'], r['renamed'], r['version_name']), (new, PKG, [], f'1.0.1-{want}'))
        self.assertGreater(r['version_code'], 65542); self.assertEqual(seen()[new], want)
        self.assertEqual(self.builds[-1][0], ATRAIN)
        self.assertIn(f'package: {PKG} → {new}', '\n'.join(said))
        oi, ni = axml.apk_info(self.apk), axml.apk_info(r['apk'])
        self.assertEqual(ni, dict(oi, package=new, version_code=r['version_code'], version_name=r['version_name'], label=KO))
        with zipfile.ZipFile(self.apk) as a, zipfile.ZipFile(r['apk']) as b:
            self.assertEqual(b.read('assets/' + apkupdate.DATAPKG_ASSET), (PKG + '\n').encode())
            self.assertEqual([n for n in b.namelist() if n not in a.namelist()], ['assets/' + apkupdate.DATAPKG_ASSET])
            self.assertEqual(axml.services(b.read('AndroidManifest.xml')), axml.services(a.read('AndroidManifest.xml')))
            self.assertEqual(axml.permissions(b.read('AndroidManifest.xml')), axml.permissions(a.read('AndroidManifest.xml')))
        self.assertEqual(apkupdate.data_package(r['apk']), PKG); self.assertEqual(apkupdate.data_package(self.apk), PKG)
        self.assertEqual(apkupdate.source_of(r['apk']), ('https://example.invalid/pack/', 'u:p'))
        # the renamed APK updated again (no --package): it stays the new app, reads the same pack, its build goes on
        r2 = apkupdate.update(r['apk'], apkupdate.Pack(d), self.t / 'out2', jobs=1)
        self.assertEqual((r2['package'], r2['data_package'], r2['version_name']), (new, PKG, f'1.0.1-{want + 1}'))
        with zipfile.ZipFile(r2['apk']) as z: self.assertEqual(z.namelist().count('assets/' + apkupdate.DATAPKG_ASSET), 1)
        # an APK of another package (not renamed) still refuses the pack; a package name Android would not take refuses before the host is built
        with self.assertRaises(BuildError) as e: apkupdate.update(self.mkapk('other.apk', '--package', 'org.nx2a.other'), apkupdate.Pack(d), self.t / 'out3', build=60)
        self.assertIn('not this game', str(e.exception))
        n = len(self.builds)
        with self.assertRaises(BuildError) as e: apkupdate.update(self.apk, apkupdate.Pack(d), self.t / 'out4', build=61, package='kr.wook-srw')
        self.assertIn('--package', str(e.exception)); self.assertEqual(len(self.builds), n)

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
