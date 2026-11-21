# porter pack-add-font (porter/packfont.py): a published pack's changed files with the shared font added, the same as publish.manifest lists the pack folder holding them
# (its id, order and bytes), and env.txt's row where porter port --shared-font writes it (python -m unittest discover tests).
import json, os, sys, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
os.environ.setdefault('NX2A_HOME', tempfile.mkdtemp(prefix='nx2a_test_home_'))
from porter import apkupdate, datapack, packfont, publish   # noqa: E402
from porter.upstream import BuildError                     # noqa: E402

PKG = 'org.nx2a.t01006bb00c6f0000'
FOLDER = f'/sdcard/Android/data/{PKG}/files'
ENV = ('# Written by nx2android\'s porter for The Legend of Zelda: Link\'s Awakening (01006BB00C6F0000); KEY=VALUE per line, read by the app at launch\n'
       f'DM_LANG=ko\nDM_FPS_PIN=60\nDM_SHARED_FONT_EXT={FOLDER}/nintendo_ext.ttf\nDM_VK_DRIVER=libvulkan.purple.so\n')
TTF = b'\x00\x01\x00\x00' + b'font' * 100


def make_pack(d, env=ENV):   # a format 1 pack folder as a port writes it, and its files.json
    d = Path(d); (d / 'mods').mkdir(parents=True); (d / 'RomFS/a').mkdir(parents=True)
    (d / 'mods/main').write_bytes(b'm' * 10); (d / 'RomFS/a/x.bin').write_bytes(b'x' * 7); (d / 'RomFS/top.bin').write_bytes(b't' * 3)
    (d / 'nintendo_ext.ttf').write_bytes(b'\x00\x01\x00\x00ext'); (d / 'shaders.dmsv').write_bytes(b's' * 5); (d / 'env.txt').write_text(env, encoding='utf-8', newline='\n')
    keys = list(dict.fromkeys(k for k in (datapack.env_key(r) for r in env.split('\n')) if k is not None))
    man = dict(title_id='01006BB00C6F0000', name='Zelda', package=PKG, version='v131072', display='1.1.0', language='ko', requires=[], env_keys=keys,
               files=5, bytes=32, dlc=[], sysdata=[], variants=[], made='2026-11-18 05:50:05', profile_h='ab' * 32)
    (d / datapack.MANIFEST).write_text(json.dumps(man, indent=2), encoding='utf-8')
    publish.manifest(d)
    return d


class EnvRow(unittest.TestCase):
    def test_after_the_button_icons(self):
        rows = packfont.env_with_font(ENV, PKG).split('\n')
        i = rows.index(f'DM_SHARED_FONT_EXT={FOLDER}/nintendo_ext.ttf')
        self.assertEqual(rows[i + 1], f'DM_SHARED_FONT={FOLDER}/standard.ttf')
        self.assertEqual(rows[i + 2], 'DM_VK_DRIVER=libvulkan.purple.so')
        self.assertEqual(ENV.split('\n'), [r for r in rows if not r.startswith('DM_SHARED_FONT=')])   # nothing else changes

    def test_without_button_icons_before_the_port_rows(self):
        env = 'DM_LANG=ko\nDM_FPS_PIN=60\nDM_VK_DRIVER=x.so\n'
        self.assertEqual(packfont.env_with_font(env, PKG), f'DM_LANG=ko\nDM_FPS_PIN=60\nDM_SHARED_FONT={FOLDER}/standard.ttf\nDM_VK_DRIVER=x.so\n')
        self.assertEqual(packfont.env_with_font('DM_LANG=ko', PKG), f'DM_LANG=ko\nDM_SHARED_FONT={FOLDER}/standard.ttf\n')

    def test_an_existing_row_is_replaced_in_place(self):
        env = 'DM_LANG=ko\nDM_SHARED_FONT=/elsewhere.ttf\nDM_FPS_PIN=60\nDM_SHARED_FONT=/again.ttf\n'
        self.assertEqual(packfont.env_with_font(env, PKG), f'DM_LANG=ko\nDM_SHARED_FONT={FOLDER}/standard.ttf\nDM_FPS_PIN=60\n')


class AddFont(unittest.TestCase):
    def setUp(self):
        self.tmp = Path(tempfile.mkdtemp(prefix='packfont_')); self.addCleanup(__import__('shutil').rmtree, self.tmp, True)
        self.font = self.tmp / 'NotoSansKR-Switch.ttf'; self.font.write_bytes(TTF)

    def test_as_manifest_lists_the_pack_with_the_files(self):
        d = make_pack(self.tmp / 'Zelda-data'); before = json.loads((d / publish.FILES).read_text(encoding='utf-8'))
        out = self.tmp / 'out'
        r = packfont.add_font(apkupdate.Pack(d), self.font, out)
        self.assertEqual(r['written'], ['standard.ttf', 'env.txt', 'nx2a-data.json', 'files.json'])
        self.assertFalse(r['same']); self.assertEqual(r['id'][0], before['id']); self.assertNotEqual(r['id'][1], before['id'])
        self.assertEqual((out / 'standard.ttf').read_bytes(), TTF)
        got = json.loads((out / publish.FILES).read_text(encoding='utf-8'))
        man = json.loads((out / datapack.MANIFEST).read_text(encoding='utf-8')); old = datapack.read_manifest(d)
        self.assertEqual(man['env_keys'], ['DM_LANG', 'DM_FPS_PIN', 'DM_SHARED_FONT_EXT', 'DM_SHARED_FONT', 'DM_VK_DRIVER'])
        self.assertEqual((man['files'], man['bytes']), (old['files'] + 1, old['bytes'] + len(TTF)))
        self.assertEqual({k: v for k, v in man.items() if k not in ('env_keys', 'files', 'bytes')}, {k: v for k, v in old.items() if k not in ('env_keys', 'files', 'bytes')})
        for n in ('standard.ttf', 'env.txt', 'nx2a-data.json'): (d / n).write_bytes((out / n).read_bytes())   # the folder as the server holds it after the upload
        (d / publish.FILES).unlink(); publish.cache_file(d).unlink()
        fresh = publish.manifest(d)
        self.assertEqual(got['id'], fresh['id']); self.assertEqual(got['bytes'], fresh['bytes'])
        self.assertEqual([(f['path'], f['size'], f['sha256']) for f in got['files']], [(f['path'], f['size'], f['sha256']) for f in fresh['files']])   # the same order too
        self.assertEqual({k: v for k, v in got.items() if k not in ('id', 'bytes', 'made', 'files')}, {k: v for k, v in before.items() if k not in ('id', 'bytes', 'made', 'files')})

    def test_twice_is_nothing_to_upload(self):
        d = make_pack(self.tmp / 'Zelda-data'); out = self.tmp / 'out'
        packfont.add_font(apkupdate.Pack(d), self.font, out)
        for n in ('standard.ttf', 'env.txt', 'nx2a-data.json', 'files.json'): (d / n).write_bytes((out / n).read_bytes())
        again = self.tmp / 'again'
        r = packfont.add_font(apkupdate.Pack(d), self.font, again)
        self.assertTrue(r['same']); self.assertEqual(r['written'], []); self.assertFalse(again.exists())

    def test_a_new_font_replaces_the_old(self):
        d = make_pack(self.tmp / 'Zelda-data'); out = self.tmp / 'out'
        packfont.add_font(apkupdate.Pack(d), self.font, out)
        for n in ('standard.ttf', 'env.txt', 'nx2a-data.json', 'files.json'): (d / n).write_bytes((out / n).read_bytes())
        other = self.tmp / 'other.ttf'; other.write_bytes(TTF + b'more')
        r = packfont.add_font(apkupdate.Pack(d), other, self.tmp / 'out2')
        got = json.loads((self.tmp / 'out2' / publish.FILES).read_text(encoding='utf-8'))
        self.assertEqual([f['size'] for f in got['files'] if f['path'] == 'standard.ttf'], [len(TTF) + 4])
        man = json.loads((self.tmp / 'out2' / datapack.MANIFEST).read_text(encoding='utf-8'))
        self.assertEqual(man['files'], 6); self.assertEqual((self.tmp / 'out2' / 'env.txt').read_bytes(), (out / 'env.txt').read_bytes()); self.assertFalse(r['same'])

    def test_refused(self):
        d = make_pack(self.tmp / 'Zelda-data')
        bad = self.tmp / 'x.ttf'; bad.write_bytes(b'<html>')
        with self.assertRaises(BuildError): packfont.add_font(apkupdate.Pack(d), bad, self.tmp / 'o')
        with self.assertRaises(BuildError): packfont.add_font(apkupdate.Pack(d), self.tmp / 'none.ttf', self.tmp / 'o')
        (d / publish.FILES).unlink()
        with self.assertRaises(BuildError): packfont.add_font(apkupdate.Pack(d), self.font, self.tmp / 'o')

    def test_format_2_adds_among_the_loose_files(self):
        fj = dict(format=2, id='', package=PKG, bytes=3, made='', bundles=[dict(path='bundles/b0000.tar', size=1, sha256='0' * 64, files=1, bytes=3)],
                  files=[dict(path='RomFS/a', size=3, sha256='1' * 64, bundle=0), dict(path='env.txt', size=1, sha256='2' * 64, mtime=1),
                         dict(path='shaders.dmsv', size=1, sha256='3' * 64, mtime=1)])
        got = packfont.put_file(fj, 'standard.ttf', TTF)
        self.assertEqual([f['path'] for f in got['files']], ['RomFS/a', 'env.txt', 'shaders.dmsv', 'standard.ttf'])
        self.assertEqual(got['id'], publish.list_id(got['files'])); self.assertEqual(got['bytes'], 3 + len(TTF))


if __name__ == '__main__':
    unittest.main()
