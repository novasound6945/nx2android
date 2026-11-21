# The background download's service in the APK (nxnative/dm/android/apk: mkapk.py, service.dex, axml.py add_services / map_attr / set_has_code): every new APK
# declares org.nx2a.DownloadService (a dataSync foreground service, not exported) with its permissions and carries its dex (classes.dex, or classes2.dex beside a --dex
# of the port's own); a released manifest from before it gets the element with an attribute name its resource map lacked (foregroundServiceType) and reads back with
# aapt2 as Android would. The committed dex is the source's (service.dex.sha256). aapt2 / apksigner / dexdump of an Android SDK when this computer has them.
# python -m unittest tests.test_bgdl_apk
import hashlib, os, shutil, subprocess, sys, tempfile, unittest, zipfile
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
APK = REPO / 'nxnative/dm/android/apk'
sys.path.insert(0, str(APK))
import axml, build_service_dex   # noqa: E402

BT = Path(os.environ.get('NX2A_BUILD_TOOLS', Path.home() / 'Library/Android/sdk/build-tools/32.1.0-rc1'))
AAPT2, APKSIGNER, DEXDUMP = BT / 'aapt2', BT / 'apksigner', BT / 'dexdump'
BASE = '5f6f417b4eeaaa49a00ac065ff816a423b7ea574'   # the last mkapk.py from before the service
SERVICE = 'org.nx2a.DownloadService'
PERMS = ['android.permission.FOREGROUND_SERVICE', 'android.permission.FOREGROUND_SERVICE_DATA_SYNC', 'android.permission.POST_NOTIFICATIONS',
         'android.permission.WAKE_LOCK', 'android.permission.ACCESS_WIFI_STATE', 'android.permission.ACCESS_NETWORK_STATE']
ATTR_IDS = {'name': 0x01010003, 'exported': 0x01010010, 'foregroundServiceType': 0x01010599}
SERVICES = [('service', [('name', SERVICE), ('exported', False), ('foregroundServiceType', ('hex', 0x1))])]


def tool(p): return p.is_file() and os.access(p, os.X_OK)


def xmltree(apk): return subprocess.run([str(AAPT2), 'dump', 'xmltree', '--file', 'AndroidManifest.xml', str(apk)], capture_output=True, text=True, check=True).stdout


def service_block(tree):   # the <service> element's lines of aapt2's xmltree
    lines = tree.splitlines(); i = next(k for k, ln in enumerate(lines) if ln.strip().startswith('E: service'))
    out = [lines[i]]
    for ln in lines[i + 1:]:
        if ln.strip().startswith('E:'): break
        out.append(ln)
    return '\n'.join(out)


def verify(test, apk):
    if not tool(APKSIGNER): return
    r = subprocess.run([str(APKSIGNER), 'verify', '--min-sdk-version', '29', str(apk)], capture_output=True, text=True)
    test.assertEqual(r.returncode, 0, r.stdout + r.stderr)


class ServiceDex(unittest.TestCase):
    def test_record_is_the_source(self):   # service.dex was built from DownloadService.java as it is now (build_service_dex.py after a change)
        rec = (APK / 'service.dex.sha256').read_text(encoding='utf-8').split()
        self.assertEqual(rec[0], build_service_dex.source_hash(), 'run nxnative/dm/android/apk/build_service_dex.py: service.dex is not the source\'s')
        self.assertEqual(rec[1], 'nxnative/dm/android/java_service/org/nx2a/DownloadService.java')
        dex = (APK / 'service.dex').read_bytes()
        self.assertEqual(dex[:4], b'dex\n'); self.assertIn(build_service_dex.MARK, dex)
        self.assertLess(len(dex), 64 << 10)

    @unittest.skipUnless(tool(DEXDUMP), 'dexdump (Android build-tools)')
    def test_classes(self):
        out = subprocess.run([str(DEXDUMP), str(APK / 'service.dex')], capture_output=True, text=True, check=True).stdout
        classes = sorted(ln.split("'")[1] for ln in out.splitlines() if 'Class descriptor' in ln)
        self.assertIn('Lorg/nx2a/DownloadService;', classes)
        self.assertTrue(all(c.startswith('Lorg/nx2a/DownloadService') for c in classes), classes)   # nothing else of org.nx2a (GameActivity's dex stays the port's own)
        self.assertIn('nativeCommand', out)

    def test_not_in_the_activity_sources(self):   # porter's --native-keyboard / --external-display compile java/org/nx2a/*.java: the service is not among them (no class twice)
        self.assertFalse((REPO / 'nxnative/dm/android/java/org/nx2a/DownloadService.java').exists())


class Mkapk(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp(prefix='nx2a_bgdl_apk_')); self.addCleanup(shutil.rmtree, self.t, ignore_errors=True)
        (self.t / 'libdmhost.so').write_bytes(b'\x7fELF host' * 512); (self.t / 'own.dex').write_bytes(b'dex\n035\0own' * 8)

    def mkapk(self, name, *more, tool_path=APK / 'mkapk.py'):
        out = self.t / name
        subprocess.run([sys.executable, str(tool_path), str(self.t / 'libdmhost.so'), str(out), str(self.t / 'key.json'), '--package', 'org.nx2a.bgtest', '--label', 'BG',
                        '--lib', 'dmhost', '--release', *more], check=True, capture_output=True)
        return out

    def test_every_apk_has_the_service(self):
        svc = (APK / 'service.dex').read_bytes()
        plain, own = self.mkapk('plain.apk'), self.mkapk('own.apk', '--dex', str(self.t / 'own.dex'), '--activity', 'org.nx2a.GameActivity')
        with zipfile.ZipFile(plain) as z: self.assertEqual((z.read('classes.dex'), 'classes2.dex' in z.namelist()), (svc, False)); m = z.read('AndroidManifest.xml')
        with zipfile.ZipFile(own) as z: self.assertEqual((z.read('classes.dex'), z.read('classes2.dex')), ((self.t / 'own.dex').read_bytes(), svc))
        self.assertEqual(axml.services(m), [SERVICE])
        for p in PERMS: self.assertIn(p, axml.permissions(m))
        app = next(a for tag, a in axml.parse(m)['elements'] if tag == 'application')
        self.assertTrue(axml._value(axml.parse(m)['strings'], app['android:hasCode']))
        verify(self, plain); verify(self, own)

    @unittest.skipUnless(tool(AAPT2), 'aapt2 (Android build-tools)')
    def test_aapt2_reads_it(self):
        apk = self.mkapk('plain.apk'); tree = xmltree(apk); svc = service_block(tree)
        self.assertIn(f'android:name(0x01010003)="{SERVICE}"', svc)
        self.assertIn('android:exported(0x01010010)=false', svc)
        self.assertIn('android:foregroundServiceType(0x01010599)=0x00000001', svc)   # dataSync
        self.assertIn('android:hasCode(0x0101000c)=true', tree)
        perms = subprocess.run([str(AAPT2), 'dump', 'permissions', str(apk)], capture_output=True, text=True, check=True).stdout
        for p in PERMS: self.assertIn(f"uses-permission: name='{p}'", perms)
        self.assertIn("package: name='org.nx2a.bgtest'", subprocess.run([str(AAPT2), 'dump', 'badging', str(apk)], capture_output=True, text=True, check=True).stdout)

    def old_mkapk(self):   # the mkapk.py of BASE (no service, hasCode only with --dex): a released port's manifest
        d = self.t / 'old'; d.mkdir(exist_ok=True)
        for f in ('mkapk.py', 'apksign.py'):
            r = subprocess.run(['git', '-C', str(REPO), 'show', f'{BASE}:nxnative/dm/android/apk/{f}'], capture_output=True)
            if r.returncode: self.skipTest(f'{BASE[:8]} not in this clone')
            (d / f).write_bytes(r.stdout)
        return d / 'mkapk.py'

    def test_released_manifest_gets_the_service(self):   # the attribute name the old resource map lacks goes in, every string reference renumbered; the rest reads as before
        old = self.mkapk('old.apk', '--landscape', tool_path=self.old_mkapk())
        with zipfile.ZipFile(old) as z: m = z.read('AndroidManifest.xml')
        self.assertEqual(axml.services(m), [])
        self.assertNotIn('foregroundServiceType', axml.parse(m)['strings'])
        n, added = axml.add_services(m, SERVICES, ATTR_IDS); n = axml.set_has_code(n)
        self.assertEqual(added, [SERVICE]); self.assertEqual(axml.services(n), [SERVICE])
        self.assertEqual(axml.add_services(n, SERVICES, ATTR_IDS), (n, []))   # once only
        self.assertEqual(axml.set_has_code(n), n)
        self.assertEqual(axml.info(n), axml.info(m)); self.assertEqual(axml.permissions(n), axml.permissions(m))
        el = lambda x: [(tag, {k: axml._value(axml.parse(x)['strings'], v) for k, v in a.items()}) for tag, a in axml.parse(x)['elements']]
        eo, en = el(m), el(n)
        self.assertEqual([e for e in en if e[0] != 'service'], [(t, dict(a, **({'android:hasCode': True} if t == 'application' else {}))) for t, a in eo])
        self.assertEqual(en[-1], ('service', {'android:name': SERVICE, 'android:exported': False, 'android:foregroundServiceType': 1}))
        self.assertEqual(axml.info(axml.set_version(axml.set_label(n, 'BG 2'), 9, '9.0'))['label'], 'BG 2')   # changed further after it
        if tool(AAPT2):
            out = self.t / 'patched.apk'
            with zipfile.ZipFile(old) as a, zipfile.ZipFile(out, 'w') as b:
                for i in a.infolist(): b.writestr(i, n if i.filename == 'AndroidManifest.xml' else a.read(i))
            tree, before = xmltree(out), xmltree(old)
            svc = service_block(tree)
            self.assertIn('android:foregroundServiceType(0x01010599)=0x00000001', svc); self.assertIn('android:exported(0x01010010)=false', svc)
            self.assertIn('android:hasCode(0x0101000c)=true', tree); self.assertIn('android:hasCode(0x0101000c)=false', before)
            keep = lambda t: [ln for ln in t.splitlines() if 'hasCode' not in ln]
            self.assertEqual(keep(tree)[:len(keep(before))], keep(before))   # everything before the service as it was: the names, the IDs, the values

    def test_map_attr(self):   # an attribute name put in the resource map: an ID the map had not, a pool string after it moved and still referenced
        m = axml.parse(open_manifest(self.mkapk('a.apk')))
        data = open_manifest(self.t / 'a.apk')
        n = axml.map_attr(data, 'roundIcon', 0x0101052c)
        self.assertEqual(axml.map_attr(n, 'roundIcon', 0x0101052c), n)
        p = axml.parse(n); k = p['strings'].index('roundIcon')
        self.assertEqual(p['strings'][:k] + p['strings'][k + 1:], m['strings'])
        self.assertEqual(axml.info(n), axml.info(data)); self.assertEqual(axml.services(n), axml.services(data)); self.assertEqual(axml.permissions(n), axml.permissions(data))


def open_manifest(apk):
    with zipfile.ZipFile(apk) as z: return z.read('AndroidManifest.xml')


if __name__ == '__main__':
    unittest.main()
