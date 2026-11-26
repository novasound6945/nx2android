# Port.pull_captures: shaders the app captured reach the next port whenever adb can reach the device, also without --install (issue #512).
import os, sys, tempfile, types, unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
os.environ.setdefault('NX2A_HOME', tempfile.mkdtemp(prefix='nx2a_test_home_'))
from porter import pipeline             # noqa: E402
from porter.adb import AdbError         # noqa: E402


class FakeAdb:
    error, pulled, pointer, card = None, [], None, True   # pointer: the app's nx2a_root.txt (datapack.device_files: this fork's data on an SD card); card: the card is in
    def __init__(self, exe, serial=None, log=print): pass
    def pick(self):
        if self.error: raise AdbError(self.error)
    def read_text(self, path): return FakeAdb.pointer if path.endswith('/nx2a_root.txt') else None
    def is_dir(self, path): return FakeAdb.card
    def pull_new(self, rdir, local, pattern=None): FakeAdb.pulled.append((rdir, Path(local))); return ['0123456789abcdef.bin']


class PullCaptures(unittest.TestCase):
    def port(self, install=False, adb='adb'):
        p = pipeline.Port.__new__(pipeline.Port)
        p.o, p.cfg, p.tools, p.package, p.aw = types.SimpleNamespace(install=install, device=None), {}, {'adb': adb}, 'org.nx2a.test', Path('android')
        return p

    def pull(self, port, error=None, pointer=None, card=True):
        FakeAdb.error, FakeAdb.pulled, FakeAdb.pointer, FakeAdb.card = error, [], pointer, card
        with mock.patch.object(pipeline, 'Adb', FakeAdb), mock.patch.object(pipeline, 'say') as say: port.pull_captures()
        return [c.args[0] for c in say.call_args_list]

    def test_port_without_install_pulls(self):
        said = self.pull(self.port())
        self.assertEqual(FakeAdb.pulled[0][1], Path('android/captured_shaders')); self.assertIn('captured on the device', said[0])

    def test_no_adb_is_quiet(self):
        self.assertEqual(self.pull(self.port(adb=None)), []); self.assertEqual(FakeAdb.pulled, [])

    def test_no_device(self):   # only an install, which needs the device, reports it
        self.assertEqual(self.pull(self.port(), 'no device connected'), [])
        self.assertIn('not pulled', self.pull(self.port(install=True), 'no device connected')[0])

    def test_several_devices_are_reported(self):
        self.assertIn('choose one', self.pull(self.port(), 'several devices connected (a, b): choose one')[0])

    def test_sd_card_data(self):   # this fork's: the app keeps its data on the SD card (its pointer), so the captures are pulled from there; a card that is not in is reported
        card = '/storage/1234-ABCD/Android/data/org.nx2a.test/files'
        self.pull(self.port(), pointer=f'root={card}\n')
        self.assertEqual(FakeAdb.pulled[0][0], f'{card}/captured_shaders')
        self.pull(self.port())
        self.assertEqual(FakeAdb.pulled[0][0], '/sdcard/Android/data/org.nx2a.test/files/captured_shaders')
        said = self.pull(self.port(), pointer=f'root={card}\n', card=False)
        self.assertEqual(FakeAdb.pulled, []); self.assertIn('not pulled', said[0]); self.assertIn('SD card', said[0])

    def test_run_no_longer_gates_on_install(self):
        src = Path(pipeline.__file__).read_text(encoding='utf-8')
        self.assertNotIn('if o.install: self.pull_captures()', src); self.assertIn('        self.pull_captures()\n', src)


if __name__ == '__main__': unittest.main()
