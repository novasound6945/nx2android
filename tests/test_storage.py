"""Where the app keeps its game data (nxnative/dm/android/storage.h): the choice at the start, a move to an SD card and back (cancelled, cut short, finished at the
next start), on the computer with a scratch folder (tests/storage_fixture.c)."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'


class Storage(unittest.TestCase):
    def test_choice_and_move(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc: self.skipTest('a C compiler is needed')
        with tempfile.TemporaryDirectory() as t:
            exe = Path(t) / 'storage'
            subprocess.run([cc, '-std=gnu11', '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', f'-I{DM / "android"}', str(ROOT / 'tests/storage_fixture.c'), '-o', str(exe)], check=True)
            (Path(t) / 'w').mkdir()
            r = subprocess.run([str(exe), str(Path(t) / 'w')], capture_output=True, text=True)
            self.assertEqual(r.returncode, 0, r.stderr + r.stdout)
            self.assertIn('storage ok', r.stdout)

    def test_front_reads_the_root_from_it(self):   # everything of the start goes by the chosen root; the log, saves/ and sync.txt by the internal files folder
        src = (DM / 'android/front_android.c').read_text(encoding='utf-8')
        create = src[src.index('void ANativeActivity_onCreate('):]; create = create[:create.index('\n}\n')]
        self.assertIn('dm_storage_start(act, root', create)
        self.assertNotIn('snprintf(g_root', create)   # (root_apply sets it)
        self.assertIn('"%s/log.txt", home', create)
        self.assertIn('dm_saves_start(dm_storage_home())', src)
        cloud = (DM / 'android/savecloud_android.c').read_text(encoding='utf-8')
        self.assertNotIn('"%s/sync.txt", root', cloud)


if __name__ == '__main__':
    unittest.main()
