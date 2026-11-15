"""shader_split.nmpl_shaders: Nippon Ichi's Nmpl shader objects (Disgaea 4 Return keeps its 65 shaders as 'NMPL_NVN' … 'SHADER' objects in main's data) are read whole, the same bytes in
other data add nothing, and raw_shaders no longer reports their control sections as having no code area"""
import contextlib, importlib.util, io, os, struct, sys, tempfile, unittest
from pathlib import Path
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1] / 'nxnative/dm/gfx/tools'


@unittest.skipUnless(importlib.util.find_spec('lz4'), 'no lz4')
class NmplShaders(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        sys.path.insert(0, str(TOOLS))
        spec = importlib.util.spec_from_file_location('shader_split_nmpl', TOOLS / 'shader_split.py')
        cls.s = importlib.util.module_from_spec(spec)
        with patch.dict(os.environ, {'DM_GAME': '0100000000010000'}):   # the module reads a profile at import; any one in dm/games does
            spec.loader.exec_module(cls.s)

    def obj(self, sph, ncode=0x200, nctl=0x850, names=b'GLOBAL_UB\0in_position\0to_texcoord\0', code704=None, imap=0):
        """One object as the game's are laid out: the 0x30-byte header, the reflection, the control section (its own length at +0x20, the code length at +0x704), the code area
        (imap: SPH word 6, the input map of attributes 0-3, which a pixel shader's cover is chosen by)"""
        refl = names + bytes(-len(names) % 0x10)
        ctl = bytearray(nctl); ctl[:4] = self.s.CTL_MAGIC
        struct.pack_into('<I', ctl, 0x20, nctl); struct.pack_into('<I', ctl, 0x704, ncode if code704 is None else code704)
        code = bytearray(ncode); code[:4] = self.s.GFX_MAGIC; struct.pack_into('<I', code, 0x30, sph); struct.pack_into('<I', code, 0x48, imap)
        co = 0x30 + len(refl)
        head = b'NMPL_NVN\xc1\x1cSHADER' + struct.pack('<8I', 0x30, co, co + nctl, len(refl), nctl, ncode, 0, 0x6e)
        return head + refl + bytes(ctl) + bytes(code), bytes(ctl), bytes(code)

    def test_objects_back_to_back(self):   # main+0x7e6490 on: one object starts where the code of the one before ends
        vs, vctl, vcode = self.obj(0x2060461)
        ps, pctl, pcode = self.obj(0x65462, ncode=0x300, nctl=0x838)
        gs, _, _ = self.obj(0x10061061, ncode=0x500)
        d = bytes(0x40) + vs + ps + gs
        got = self.s.nmpl_shaders(d, 'main')
        self.assertEqual([(w, st) for w, st, _, _ in got], [('main#NMPL@0x40', 'vs'), (f'main#NMPL@{0x40 + len(vs):#x}', 'ps'), (f'main#NMPL@{0x40 + len(vs) + len(ps):#x}', 'gs')])
        self.assertEqual(got[0][2:], (vctl, vcode)); self.assertEqual(got[1][2:], (pctl, pcode))
        found = []; self.s.scan_blob(d, 'main', found)   # main goes through scan_blob
        self.assertEqual(len(found), 3)

    def test_same_bytes_in_other_data(self):   # the magic in a string or a hash: no object, no red line
        vs, _, _ = self.obj(0x2060461)
        for d in (b'NMPL_NVN', b'NMPL_NVN\xc1\x1cSHADER' + bytes(0x20), b'xxNMPL_NVN\xc1\x1cTEXTUR' + vs[16:], vs[:0x14] + struct.pack('<I', 0x400) + vs[0x18:], vs[:0x200]):
            with self.subTest(d=d[:24]): self.assertEqual(self.s.nmpl_shaders(d, 'main'), [])

    def test_object_that_fails_the_triple_check_is_red(self):
        bad, _, _ = self.obj(0x2060461, code704=0x100)   # the control section names another code length
        with self.assertRaises(SystemExit) as e: self.s.nmpl_shaders(bad, 'main')
        self.assertIn('[RED] main NMPL shader @0x0', str(e.exception))

    def test_single_stage_cover(self):   # no program table: every vertex shader is paired with the pixel shader cover
        vs, ps = self.obj(0x2060461)[0], self.obj(0x65462, imap=0xaa)[0]
        keys = self.s.uniq_keys(self.s.nmpl_shaders(vs + ps, 'main'))
        pairs, cover = self.s.separable_pairs(keys)
        v = next(k for k, x in keys.items() if x[1] == 'vs'); p = next(k for k, x in keys.items() if x[1] == 'ps')
        self.assertEqual((pairs, cover), ({(v, p)}, [p]))

    def test_raw_shaders_leaves_them_to_the_reader(self):   # before the reader: 65 "[RED] main control section …: no code area" lines and no shader
        image = bytearray(0x4000)
        struct.pack_into('<I', image, 4, 0x40); struct.pack_into('<i', image, 0x44, 0x40)   # a minimal MOD0 / dynamic table, no relocations
        o, _, _ = self.obj(0x2060461); image[0x1000:0x1000 + len(o)] = o; image = bytes(image)
        found = self.s.nmpl_shaders(image, 'main')
        out = io.StringIO()
        with contextlib.redirect_stdout(out): raw = self.s.raw_shaders(image, 'main', {c for _, _, c, _ in found})
        self.assertEqual((len(found), raw, out.getvalue()), (1, [], ''))
        with contextlib.redirect_stdout(out): self.s.raw_shaders(image, 'main')
        self.assertIn('no code area', out.getvalue())

    def test_loose_file_is_scanned(self):   # a RomFS file holding objects passes scan_file's quick filter
        vs, ps = self.obj(0x2060461)[0], self.obj(0x65462)[0]
        with tempfile.TemporaryDirectory() as t:
            p = os.path.join(t, 'shaders.bin')
            with open(p, 'wb') as f: f.write(b'\xff' * 0x10 + vs + ps)
            rel, uniq, pairs, extra = self.s.scan_file((p, 'shaders.bin'))
        self.assertEqual(sorted(v[1] for v in uniq.values()), ['ps', 'vs'])


if __name__ == '__main__':
    unittest.main()
