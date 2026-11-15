"""nvn_sparse_tile_layout: 64 KB sparse tile shapes by bytes per block (Disgaea 4 sizes a buffer from width/height)."""
import ctypes as C
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
NVN = ROOT / 'nxnative/dm/gfx/nvn'

T1D, T2D, T3D, TCUBE = 0, 1, 2, 8
R8, RGBA8, RGBA16F, RGBA32F, DXT1, BPTC = 0x01, 0x25, 0x29, 0x2E, 0x42, 0x4D


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class SparseTileLayout(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        lib = Path(cls.tmp.name) / 'sparse.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-I', str(NVN), str(ROOT / 'tests/sparse_tile_fixture.c'),
                        '-o', str(lib)], check=True, capture_output=True)
        cls.lib = C.CDLL(str(lib))
        cls.lib.sparse_layout.argtypes = [C.c_int, C.c_int, C.c_int] + [C.c_uint] * 4 + [C.POINTER(C.c_int32)]

    @classmethod
    def tearDownClass(cls):
        if os.name == 'nt':
            C.windll.kernel32.FreeLibrary(C.c_void_p(cls.lib._handle))
        cls.tmp.cleanup()

    def layout(self, target, fmt, w, h, d=1, levels=1, samples=0):
        out = (C.c_int32 * 8)(*([-1] * 8))
        self.lib.sparse_layout(target, fmt, samples, w, h, d, levels, out)
        self.assertEqual(list(out[4:]), [0, 0, 0, 0])
        return tuple(out[:4])

    def test_2d_shapes_by_bytes_per_block(self):
        self.assertEqual(self.layout(T2D, RGBA8, 1920, 1080), (1, 128, 128, 1))
        self.assertEqual(self.layout(T2D, R8, 960, 544), (1, 256, 256, 1))
        self.assertEqual(self.layout(T2D, RGBA16F, 1024, 1024)[1:], (128, 64, 1))
        self.assertEqual(self.layout(T2D, RGBA32F, 1024, 1024)[1:], (64, 64, 1))

    def test_compressed_shapes_are_in_blocks(self):
        self.assertEqual(self.layout(T2D, BPTC, 1024, 1024, levels=11), (3, 256, 256, 1))   # 1024, 512, 256 fit; 128 does not
        self.assertEqual(self.layout(T2D, DXT1, 1024, 1024)[1:], (512, 256, 1))

    def test_other_non_3d_targets_use_2d_shapes(self):
        self.assertEqual(self.layout(TCUBE, RGBA8, 256, 256)[1:], (128, 128, 1))
        self.assertEqual(self.layout(T1D, RGBA8, 256, 1)[1:], (128, 128, 1))

    def test_3d(self):
        # 64x64x64 RGBA8: tile 32x32x16; levels 64,32 fit in x/y/z (z 64,32 >= 16), level 2 is 16 < 32
        self.assertEqual(self.layout(T3D, RGBA8, 64, 64, 64, levels=7), (2, 32, 32, 16))
        self.assertEqual(self.layout(T3D, R8, 64, 64, 64)[1:], (64, 32, 32))
        self.assertEqual(self.layout(T3D, RGBA32F, 64, 64, 8)[:1], (0,))   # depth 8 < 16

    def test_small_and_unmipped(self):
        self.assertEqual(self.layout(T2D, RGBA8, 64, 64)[0], 0)
        self.assertEqual(self.layout(T2D, RGBA8, 128, 128, levels=0)[0], 1)

    def test_multisampled_and_unknown_are_zero(self):
        self.assertEqual(self.layout(T2D, RGBA8, 1920, 1080, samples=4), (0, 0, 0, 0))
        self.assertEqual(self.layout(T2D, 0x7FFF, 1920, 1080), (0, 0, 0, 0))


if __name__ == '__main__':
    unittest.main()
