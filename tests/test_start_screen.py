"""The Android start screen (dm/android/startscreen.c) drawn on the computer with the committed fonts, at the Retroid Pocket Nova's 1280 x 960 and the Retroid Pocket 6's
1920 x 1080: each of the design's screens (tests/start_screen_fixture.c: the start's steps and questions, the launcher and its pages), the touch areas, the speed and time-left estimate. NX2A_START_SHOTS=<folder> keeps the PNGs."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
DM = ROOT / 'nxnative/dm'
NOTO = DM / 'third_party/noto-sans-kr'
PIXEL = DM / 'third_party/fusion-pixel-font/fusion-pixel-12px-proportional-zh_tw.nxpf'
SCREENS = ('1-main', '2-download', '3-conflict', '4-offline', '5-other-device', '6-need-network', '7-offline-shaders', '8-checking-files', '12-launcher', '13-launcher-preparing', '13a-launcher-preparing-item',
           '13b-launcher-checking-files', '13c-launcher-cloud-checking',
           '13d-launcher-cloud-checking-chosen', '13e-launcher-cloud-checked', '14-cloud-settings',
           '15-app-settings', '15a-app-settings-on-sd', '15b-app-move-confirm', '15c-app-moving', '15d-app-moved', '15e-app-settings-no-sd', '16-after-test', '17-manual-pending', '18-launcher-long-name', '19-launcher-korean-name',
           '20-launcher-two-line-name', '21-cloud-unlinked', '22-cloud-linked', '22b-cloud-unlink-armed', '23-link-code', '24-link-expired', '25-link-done', '26-link-fail', '27-sd-missing')


class StartScreen(unittest.TestCase):
    def test_screens_render(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc: self.skipTest('a C compiler is needed')
        with tempfile.TemporaryDirectory() as t:
            exe, out = Path(t) / 'ssf', Path(os.environ.get('NX2A_START_SHOTS') or Path(t) / 'shots')
            out.mkdir(parents=True, exist_ok=True)
            r = subprocess.run([cc, '-std=c11', '-O2', '-Wall', '-Werror', '-Wno-unused-function', f'-I{DM / "android"}', str(ROOT / 'tests/start_screen_fixture.c'),
                                str(DM / 'android/startscreen.c'), '-lm', '-lz', '-o', str(exe)], capture_output=True, text=True)
            if r.returncode and 'zlib.h' in r.stderr: self.skipTest('zlib is needed')
            self.assertEqual(r.returncode, 0, r.stderr)
            for w, h in ((1280, 960), (1920, 1080)):
                subprocess.run([str(exe), str(NOTO / 'NotoSansKR-Regular.subset.ttf'), str(NOTO / 'NotoSansKR-Bold.subset.ttf'), str(PIXEL), str(out), str(w), str(h)], check=True, capture_output=True)
                for s in SCREENS: self.assertGreater((out / f'{s}-{w}x{h}.png').stat().st_size, 5000, s)

    def test_font_covers_the_screens(self):
        try: from fontTools.ttLib import TTFont
        except ImportError: self.skipTest('fontTools is needed')
        for name in ('NotoSansKR-Regular.subset.ttf', 'NotoSansKR-Bold.subset.ttf'):
            f = TTFont(NOTO / name); cmap = f.getBestCmap()
            self.assertEqual(f['head'].unitsPerEm, 1000); self.assertEqual((f['hhea'].ascent, f['hhea'].descent), (1160, -288))   # startscreen.c's ASC and LHN
            self.assertIn('glyf', f); self.assertNotIn('GSUB', f)
            hangul = [ord(bytes([a, b]).decode('euc-kr')) for a in range(0xB0, 0xC9) for b in range(0xA1, 0xFF)]
            self.assertEqual(len(hangul), 2350); self.assertFalse([c for c in hangul if c not in cmap], name)
            for ch in '·…←→✓0123456789ABCabc%/()?.,': self.assertIn(ord(ch), cmap, (name, ch))
            self.assertLess((NOTO / name).stat().st_size, 600 << 10)

    def test_port_packs_the_start_font_and_its_license(self):
        from porter import pipeline
        a = pipeline.ui_font_assets()
        for n in ('font/start-regular.ttf', 'font/start-bold.ttf', 'font/LICENSES/noto-sans-kr/OFL.txt'): self.assertTrue(a[n].is_file(), n)
        self.assertIn('SIL OPEN FONT LICENSE Version 1.1', a['font/LICENSES/noto-sans-kr/OFL.txt'].read_text())

    def test_start_icon(self):
        try: from PIL import Image
        except ImportError: self.skipTest('Pillow is needed')
        from porter import pipeline
        with tempfile.TemporaryDirectory() as t:
            b = pipeline.start_icon(Image.new('RGB', (256, 256), (200, 100, 50)), Path(t) / 'i.bin').read_bytes()
        self.assertEqual(b[:8], b'NXIC\xa0\x00\xa0\x00'); self.assertEqual(len(b), 8 + 160 * 160 * 4); self.assertEqual(b[8:12], bytes([200, 100, 50, 255]))


if __name__ == '__main__':
    unittest.main()
