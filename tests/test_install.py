# porter/install.py without a device: a RomFS read from a folder outside the app's files folder (--romfs-at). The device is made up: a dict of its files and the shell's
# answers (python -m unittest discover tests).
import hashlib, json, os, shlex, shutil, struct, subprocess, sys, tempfile, threading, unittest, zipfile
from unittest import mock
from pathlib import Path
from types import SimpleNamespace

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
os.environ.setdefault('NX2A_HOME', tempfile.mkdtemp(prefix='nx2a_test_home_'))
from porter import cli, datapack, install   # noqa: E402
from porter import adb as adb_mod          # noqa: E402
from porter.adb import Adb, AdbError        # noqa: E402
from porter.upstream import BuildError, PROG   # noqa: E402

PKG = 'org.nx2a.test'
FILES = datapack.files_folder(PKG)
DUMP = '/sdcard/Games/Test/romfs'
ROM = {'Data/a.bin': 20, 'Data/sub/b.bin': 7, 'c.bin': 5}


class Device(Adb):
    """A made-up device: files {absolute path: size}, the text of the few files the installer reads, All files access as the appop says, and every step in ops.
    A file's content is the pack's (size times b'x') unless md5 {absolute path: MD5} says otherwise; md5=False: md5sum gives nothing. installed: the app is on the
    device already; requests: its manifest asks for the access"""
    def __init__(self, files=(), text=None, sdk=34, access='default', grants=True, free=1 << 40, md5=None, installed=False, requests=True):
        super().__init__('adb'); self.serial = 'dev'
        self.fs, self.text, self.sdk_, self.access, self.grants, self.free, self.ops = dict(files), dict(text or {}), sdk, access, grants, free, []
        self.md5, self.installed_, self.requests = md5 if md5 is not None else {}, installed, requests
    def pick(self): return 'dev'
    def prop(self, name): return str(self.sdk_) if name == 'ro.build.version.sdk' else 'Test'
    def gpu(self): return 'Qualcomm, Adreno (TM) 830'
    def free_bytes(self, path='/sdcard'): return self.free
    def same_volume(self, a, b): return True
    def read_text(self, path): return self.text.get(path, '')
    def remote_sizes(self, rdir): p = rdir.rstrip('/') + '/'; return {k[len(p):]: v for k, v in self.fs.items() if k.startswith(p)}
    def install(self, apk, package, replace_signer=False): self.ops.append(('install', package)); self.installed_ = True
    def installed(self, package): return self.installed_
    def requests_permission(self, package, perm): return self.requests
    def remote_md5(self, rdir, rels):
        self.ops.append(('md5', rdir, len(rels)))
        if self.md5 is False: return {}
        return {r: self.md5.get(f'{rdir}/{r}') or hashlib.md5(b'x' * self.fs[f'{rdir}/{r}']).hexdigest() for r in rels if f'{rdir}/{r}' in self.fs}
    def prepare_app_files(self, package): return False
    def push_tree(self, local, remote, skip=(), exclude=(), have=None, **kw):   # what the real one pushes: every file but skip's and exclude's folders
        local, n = Path(local), 0
        for p in sorted(local.rglob('*')):
            rel = p.relative_to(local).as_posix()
            if p.is_file() and rel not in skip and rel.split('/')[0] not in exclude:
                self.fs[f'{remote}/{rel}'] = p.stat().st_size; n += 1
                if self.md5: self.md5.pop(f'{remote}/{rel}', None)   # the pack's content now
                if rel.endswith('.txt'): self.text[f'{remote}/{rel}'] = p.read_text(encoding='utf-8')
        self.ops.append(('push_tree', remote, n))
    def push_file(self, local, dst, **kw):
        self.fs[dst] = Path(local).stat().st_size; self.ops.append(('push_file', dst))
        if dst.endswith('.txt'): self.text[dst] = Path(local).read_text(encoding='utf-8')
    def push_whole(self, local, dst):   # (its MD5 is the file's now)
        self.fs[dst] = Path(local).stat().st_size; self.ops.append(('push_whole', dst))
        if self.md5 is not False: self.md5[dst] = hashlib.md5(Path(local).read_bytes()).hexdigest()
    def shell(self, script, **kw):
        self.ops.append(('shell', script))
        if script.startswith('appops get'): return 'No operations.\nDefault mode: default\n' if self.access == 'default' else f'Uid mode: MANAGE_EXTERNAL_STORAGE: {self.access}\n'
        if script.startswith('appops set') and self.grants: self.access = 'allow'
        if script.startswith('pm grant') and self.grants: self.access = 'granted'
        if script.startswith('dumpsys package'): return '      android.permission.READ_EXTERNAL_STORAGE: granted=true\n' if self.access == 'granted' else ''
        if script.startswith('rm -rf '): d = shlex.split(script)[2] + '/'; self.fs = {k: v for k, v in self.fs.items() if not k.startswith(d)}
        return ''
    def step(self, kind, what=''):   # the index of the first op of that kind whose text holds what (-1: none)
        return next((i for i, o in enumerate(self.ops) if o[0] == kind and what in ' '.join(map(str, o[1:]))), -1)


def apk_file(path, access=True):   # an APK whose manifest's string pool holds the permission (as mkapk.py writes it, UTF-16) or not
    with zipfile.ZipFile(path, 'w') as z: z.writestr('AndroidManifest.xml', b'\x03\x00\x08\x00' + (install.ACCESS.encode('utf-16-le') if access else b'android.permission.VIBRATE'))
    return path


def axml_elements(b):   # [(tag, {attribute: value})] of a binary AndroidManifest.xml, in document order (strings and ints; booleans as True / False)
    pos, strs, out = 8, [], []
    while pos < len(b):
        t, _, size = struct.unpack_from('<HHI', b, pos)
        if t == 0x0001:   # the string pool, UTF-16
            n, _, _, start, _ = struct.unpack_from('<IIIII', b, pos + 8)
            for o in struct.unpack_from(f'<{n}I', b, pos + 28):
                p = pos + start + o; ln = struct.unpack_from('<H', b, p)[0]; strs.append(b[p + 2:p + 2 + 2 * ln].decode('utf-16-le'))
        elif t == 0x0102:   # a start element: its attributes after the 20-byte extension
            name, cnt = struct.unpack_from('<I', b, pos + 20)[0], struct.unpack_from('<H', b, pos + 28)[0]; attrs = {}
            for k in range(cnt):
                _, an, _, _, _, dt, data = struct.unpack_from('<IIIHBBI', b, pos + 36 + 20 * k)
                attrs[strs[an]] = strs[data] if dt == 0x03 else bool(data) if dt == 0x12 else data
            out.append((strs[name], attrs))
        pos += size
    return out


class RomfsAt(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp(prefix='nx2a_test_install_')); self.real, PROG.mode = install.Adb, 'off'
        d = self.data = self.t / 'Test-data'
        for rel, n in [*((f'RomFS/{r}', n) for r, n in ROM.items()), ('mods/main', 10), ('shaders.dmsv', 4)]: (d / rel).parent.mkdir(parents=True, exist_ok=True); (d / rel).write_bytes(b'x' * n)
        (d / 'env.txt').write_text('# porter\nDM_LANG=en-US\n', encoding='utf-8')
        self.bytes = sum(p.stat().st_size for p in d.rglob('*') if p.is_file())
        (d / datapack.MANIFEST).write_text(json.dumps(dict(title_id='0100000000AAA000', name='Test Game', package=PKG, version='v65536', display='1.1.0', requires=[], bytes=self.bytes)))
        self.apk = apk_file(self.t / 'Test.apk')

    def tearDown(self): install.Adb = self.real; shutil.rmtree(self.t, ignore_errors=True)

    def run_on(self, dev, **kw):
        install.Adb = lambda exe, serial=None, log=print: dev
        install.install(self.apk, self.data, 'adb', **kw)
        return dev

    def test_target(self):   # absolute folders in shared storage; app (or the files folder's own RomFS) for the files folder; folders the app cannot read refused at once
        self.assertEqual(install.romfs_target('/sdcard/Games/Pokopia/romfs/'), '/sdcard/Games/Pokopia/romfs')
        self.assertEqual(install.romfs_target('/storage/1234-ABCD/nx2a/x/../RomFS'), '/storage/1234-ABCD/nx2a/RomFS')
        for none in (None, 'app', f'{FILES}/RomFS', f'{FILES}/RomFS/'): self.assertIsNone(install.romfs_target(none, FILES))
        for bad in ('Games/romfs', '/data/local/tmp/romfs', ' /sdcard/x', '/sdcard/x\n', '/sdcard/Android/data/dev.eden.eden_emulator/files/romfs',
                    '/storage/emulated/0/android/OBB/x', '/sdcard/' + 'x' * 600):
            with self.assertRaises(BuildError, msg=bad): install.romfs_target(bad, FILES)
        with self.assertRaisesRegex(BuildError, 'installer\'s own'): install.romfs_target(f'{FILES}/romfs2', FILES)

    def test_env_row(self):   # the installer's row, last, read back from the device's env.txt; a hand-written DM_ROMFS is not the installer's
        env = install.env_with_romfs('# porter\nDM_LANG=en-US\n', DUMP)
        self.assertTrue(env.endswith(f'\nDM_ROMFS={DUMP}\n')); self.assertEqual(env.count('DM_ROMFS='), 1)
        self.assertEqual(install.device_romfs(env), DUMP)
        self.assertIsNone(install.device_romfs('DM_LANG=en-US\nDM_ROMFS=/sdcard/copy\n'))
        self.assertEqual(install.device_romfs(install.env_with_romfs(env, '/sdcard/b')), '/sdcard/b')   # the last one, as the app applies them

    def test_merge_env(self):   # merge_env: the pack's rows win for their keys; the device's other rows, comments and blank lines are kept after them, in their order
        pack = '# porter\nDM_LANG=en-US\nDM_VK_PIPE_LOG=1\n'
        dev = ("# the player's\nDM_LANG=fr-FR\nDM_PAD_LABEL=1\n\nDM_VK_TEXBIND=t.png\n"
               f"{install.ROMFS_MARK}: an earlier install\nDM_ROMFS=/sdcard/old\n")
        self.assertEqual(install.merge_env(pack, dev, prev_pack_keys=()), pack + "# the player's\nDM_PAD_LABEL=1\n\nDM_VK_TEXBIND=t.png\n")
        got = install.merge_env(pack, dev, DUMP, ())   # the installer's row last; the device's own DM_ROMFS row and mark dropped
        self.assertTrue(got.endswith(install.env_with_romfs('', DUMP)[1:])); self.assertEqual(got.count('DM_ROMFS='), 1)
        self.assertNotIn('/sdcard/old', got); self.assertIn('DM_PAD_LABEL=1', got); self.assertNotIn('DM_LANG=fr-FR', got)
        self.assertEqual(install.merge_env(pack, '', prev_pack_keys=()), pack)       # no env.txt on the device yet: the pack's as it is
        self.assertEqual(install.merge_env(pack, pack, prev_pack_keys=()), pack)     # the same rows on both sides: nothing kept twice
        once = install.merge_env(pack, dev, prev_pack_keys=())
        self.assertEqual(install.merge_env(pack, once, prev_pack_keys=()), once)     # a reinstall over a merged env.txt changes nothing
        self.assertEqual(datapack.env_key('DM_LANG=en-US '), 'DM_LANG')   # read as the app reads it (front_android.c env_file)
        for not_a_row in ('# a comment', '', 'no equals here'): self.assertIsNone(datapack.env_key(not_a_row))

    def test_merge_env_romfs_row(self):   # every DM_ROMFS row on the device goes, the installer's marked one and a hand-written one alike: it is the installer's row, which it checks and grants only for its own
        pack = '# porter\nDM_LANG=en-US\n'
        hand = 'DM_LANG=fr-FR\nDM_ROMFS=/sdcard/my_hand_dump\nDM_PAD_LABEL=1\n'
        self.assertEqual(install.merge_env(pack, hand, prev_pack_keys=()), pack + 'DM_PAD_LABEL=1\n')   # a plain install: the hand-written row dropped, the player's other row kept
        got = install.merge_env(pack, hand, DUMP, ())   # --romfs-at <folder>: the installer's row replaces it (appended last, it wins as the app applies the rows in order)
        self.assertEqual(got.count('DM_ROMFS='), 1); self.assertTrue(got.endswith(f'DM_ROMFS={DUMP}\n')); self.assertNotIn('my_hand_dump', got)
        marked = f'{install.ROMFS_MARK}: an earlier install\nDM_ROMFS=/sdcard/old\nDM_PAD_LABEL=1\n'
        self.assertEqual(install.merge_env(pack, marked, DUMP, ()), pack + 'DM_PAD_LABEL=1\n' + install.env_with_romfs('', DUMP)[1:])   # the marked pair replaced
        self.assertEqual(install.merge_env(pack, marked, prev_pack_keys=()), pack + 'DM_PAD_LABEL=1\n')   # --romfs-at app: the installer's marked row removed, the player's row kept

    def test_merge_env_prev_pack(self):   # prev_pack_keys (the previous pack's manifest env_keys): a row it defined and this pack no longer does was the pack's, not the player's, and is dropped
        v2 = 'DM_LANG=en-US\nDM_SHARED_FONT_EXT=/x.ttf\n'
        device = 'DM_LANG=fr-FR\nDM_VK_PIPE_LOG=1\nDM_SHARED_FONT_EXT=/x.ttf\nDM_VK_TEXBIND=all\nDM_PAD_LABEL=1\n'
        prev = ('DM_LANG', 'DM_VK_PIPE_LOG', 'DM_SHARED_FONT_EXT')
        got = install.merge_env(v2, device, prev_pack_keys=prev)
        self.assertEqual(got, v2 + 'DM_VK_TEXBIND=all\nDM_PAD_LABEL=1\n')   # the removed switch gone, the player's rows kept, in their order
        self.assertEqual(got.count('DM_LANG='), 1); self.assertIn('DM_LANG=en-US', got); self.assertNotIn('DM_LANG=fr-FR', got)   # a switch the new pack still defines comes from the pack
        self.assertEqual(install.merge_env(v2, device), v2)   # keys unknown (None): replaced as before the merge, so the old pack's dropped rows do not linger, and the player's go once
        self.assertEqual(install.merge_env(v2, device, DUMP), install.env_with_romfs(v2, DUMP))   # the installer's row still goes last
        self.assertEqual(install.merge_env(v2, 'DM_PAD_LABEL=1\n', prev_pack_keys=prev), v2 + 'DM_PAD_LABEL=1\n')   # a row no pack ever defined is the player's
        self.assertEqual(install.merge_env(v2, device, prev_pack_keys=()), v2 + 'DM_VK_PIPE_LOG=1\nDM_VK_TEXBIND=all\nDM_PAD_LABEL=1\n')   # known, and the old pack defined none: all kept

    def test_manifest_env_keys(self):   # datapack.build records the pack's env.txt keys in the manifest (as the app parses them); install reads them back off the device
        prof = SimpleNamespace(name='Test Game', id='0100000000AAA000', package=PKG, env=['DM_VK_PIPE_LOG=1', '# a profile comment'], settings={}, requires=[])
        man = datapack.build(self.t / 'pack', profile=prof, version='v1', display='1.0', patched=None, romfs=None, shaders=None, env=['DM_SHARED_FONT_EXT=/x.ttf'])
        self.assertEqual(man['env_keys'], ['DM_LANG', 'DM_VK_PIPE_LOG', 'DM_SHARED_FONT_EXT'])
        self.assertEqual(datapack.read_manifest(self.t / 'pack')['env_keys'], man['env_keys'])
        self.assertEqual(install.manifest_env_keys(json.dumps(man)), man['env_keys'])
        for unknown in ('', 'not json', '{"x": 1}', '[1, 2]'): self.assertIsNone(install.manifest_env_keys(unknown))   # unknown is not 'no keys'
        self.assertEqual(install.manifest_env_keys('{"env_keys": []}'), [])

    def test_env_merged_on_install(self):   # a device whose manifest has env_keys: the install merges env.txt, so the player's own row and comment stay
        pack = (self.data / 'env.txt').read_text(encoding='utf-8')          # '# porter\nDM_LANG=en-US\n'
        dev_env = 'DM_LANG=fr-FR\n' + pack + 'DM_PAD_LABEL=1\n# a note from a diagnostic run\n'   # the pack's rows, a stale value, and the player's own
        man = json.dumps({'package': PKG, 'env_keys': ['DM_LANG']})
        text = {f'{FILES}/env.txt': dev_env, f'{FILES}/{datapack.MANIFEST}': man}
        dev = self.run_on(Device({f'{FILES}/env.txt': len(dev_env), f'{FILES}/{datapack.MANIFEST}': len(man), f'{FILES}/log.txt': 9}, text=dict(text)))
        got = dev.text[f'{FILES}/env.txt']
        self.assertEqual(got, pack + 'DM_PAD_LABEL=1\n# a note from a diagnostic run\n')   # the player's row and comment kept, after the pack's rows
        self.assertEqual(got.count('DM_LANG='), 1); self.assertIn('DM_LANG=en-US', got); self.assertNotIn('DM_LANG=fr-FR', got)   # the pack's row wins
        self.assertEqual((self.data / 'env.txt').read_text(encoding='utf-8'), pack)   # the pack's env.txt on disk is unchanged
        dev = self.run_on(Device({f'{DUMP}/{r}': n for r, n in ROM.items()}, text=dict(text)), romfs_at=DUMP)
        got = dev.text[f'{FILES}/env.txt']   # the same with --romfs-at: the player's row kept, the installer's row last
        self.assertIn('DM_PAD_LABEL=1', got); self.assertNotIn('DM_LANG=fr-FR', got); self.assertEqual(install.device_romfs(got), DUMP)

    def test_env_prev_pack_on_install(self):   # install-level provenance: the device's manifest env_keys say which rows the old pack wrote — one the new pack no longer defines is removed
        pack = (self.data / 'env.txt').read_text(encoding='utf-8')          # '# porter\nDM_LANG=en-US\n'
        dev_env = pack + 'DM_VK_PIPE_LOG=1\nDM_PAD_LABEL=1\nDM_ROMFS=/sdcard/my_hand_dump\n'   # the old pack's diagnostic row, the player's row, a hand-written DM_ROMFS
        man = json.dumps({'package': PKG, 'env_keys': ['DM_LANG', 'DM_VK_PIPE_LOG']})   # the pack the device has now
        dev = self.run_on(Device({f'{FILES}/env.txt': len(dev_env), f'{FILES}/{datapack.MANIFEST}': len(man)},
                                 text={f'{FILES}/env.txt': dev_env, f'{FILES}/{datapack.MANIFEST}': man}))
        got = dev.text[f'{FILES}/env.txt']
        self.assertEqual(got, pack + 'DM_PAD_LABEL=1\n')   # the removed switch gone, the player's row kept, the hand-written DM_ROMFS dropped (the installer neither checks nor grants it)
        self.assertEqual((self.data / 'env.txt').read_text(encoding='utf-8'), pack)   # the pack's env.txt on disk is unchanged

    def test_env_replaced_when_keys_unknown(self):   # a device from before env_keys (no manifest, or one without the list): the old pack's rows cannot be told from the player's, so env.txt is the pack's, as on main
        pack = (self.data / 'env.txt').read_text(encoding='utf-8')
        dev_env = pack + 'DM_VK_MT_REPLAY=1\nDM_FPS_TARGET=60\nDM_PAD_LABEL=1\n'   # rows an older --mt-replay / --fps60 port wrote, and a player's row
        for text in ({f'{FILES}/env.txt': dev_env}, {f'{FILES}/env.txt': dev_env, f'{FILES}/{datapack.MANIFEST}': json.dumps({'package': PKG})}):
            dev = self.run_on(Device({k: len(v) for k, v in text.items()}, text=dict(text)))
            self.assertEqual(dev.text[f'{FILES}/env.txt'], pack)
        dev = self.run_on(Device({f'{DUMP}/{r}': n for r, n in ROM.items()}, text={f'{FILES}/env.txt': dev_env}), romfs_at=DUMP)
        self.assertEqual(dev.text[f'{FILES}/env.txt'], install.env_with_romfs(pack, DUMP))   # with --romfs-at: the pack's rows and the installer's row, nothing else

    def test_read_text_is_utf8(self):   # Adb.run decodes with the platform's code page unless told: env.txt's header for "Pokémon Legends: Arceus" must come back as it went
        seen = {}
        def fake(cmd, **kw): seen.update(kw); return SimpleNamespace(returncode=0, stdout='# Pokémon\n', stderr='')
        with mock.patch.object(adb_mod.subprocess, 'run', fake): out = adb_mod.Adb('adb', 'x').read_text('/sdcard/env.txt')
        self.assertEqual(seen.get('encoding'), 'utf-8'); self.assertEqual(out, '# Pokémon\n')

    def test_mismatch(self):   # the first differences of each kind, with sizes; nothing for the same listing
        self.assertEqual(install.romfs_mismatch(DUMP, ROM, dict(ROM)), '')
        msg = install.romfs_mismatch(DUMP, ROM, {'Data/a.bin': 21, 'c.bin': 5, 'x.bin': 1, 'y.bin': 1}, n=1)
        self.assertIn('1 of the game\'s files are not there (Data/sub/b.bin)', msg); self.assertIn('Data/a.bin: 21 bytes, the pack has 20', msg)
        self.assertIn('2 are not the game\'s (x.bin, …)', msg)
        self.assertIn('holds no files', install.romfs_mismatch(DUMP, ROM, {}))

    def test_in_place(self):   # the folder matches: no RomFS pushed, access granted, env.txt with the row, the earlier copy in the files folder removed only after that
        dev = self.run_on(Device({**{f'{DUMP}/{r}': n for r, n in ROM.items()}, f'{FILES}/RomFS/Data/a.bin': 20, f'{FILES}/RomFS/old.bin': 3, f'{FILES}/log.txt': 9}), romfs_at=DUMP)
        self.assertFalse([k for k in dev.fs if k.startswith(f'{FILES}/RomFS/')]); self.assertTrue(dev.fs[f'{FILES}/mods/main'])
        self.assertEqual(install.device_romfs(dev.text[f'{FILES}/env.txt']), DUMP); self.assertIn('DM_LANG=en-US', dev.text[f'{FILES}/env.txt'])
        self.assertEqual(dev.access, 'allow')
        order = [dev.step('install'), dev.step('shell', 'appops set'), dev.step('push_tree', FILES), dev.step('push_file', 'env.txt'), dev.step('shell', 'rm -rf'), dev.step('push_file', datapack.MANIFEST)]
        self.assertEqual(order, sorted(order)); self.assertNotIn(-1, order)
        self.assertEqual(dev.ops[dev.step('push_tree', FILES)][2], 2)   # mods/main and shaders.dmsv: no RomFS; env.txt (with the row) and the manifest go on their own
        dev.ops.clear(); self.run_on(dev)   # a later install without --romfs-at keeps the folder
        self.assertEqual(install.device_romfs(dev.text[f'{FILES}/env.txt']), DUMP); self.assertFalse([k for k in dev.fs if k.startswith(f'{FILES}/RomFS/')])
        self.assertEqual(dev.step('shell', 'appops set'), -1)   # already granted: nothing to set

    def test_refused_before_anything(self):   # a folder that is not the pack's RomFS: refused before the APK goes in, with the first differences and what to do
        dev = Device({f'{DUMP}/Data/a.bin': 20, f'{DUMP}/c.bin': 6})
        with self.assertRaises(BuildError) as e: self.run_on(dev, romfs_at=DUMP)
        for s in ('Data/sub/b.bin', 'c.bin: 6 bytes, the pack has 5', 'Test Game 1.1.0', '--romfs-push', '--romfs-at app'): self.assertIn(s, str(e.exception))
        self.assertEqual([o for o in dev.ops if o[0] != 'shell'], [])
        dev = Device({**{f'{DUMP}/{r}': n for r, n in ROM.items()}, f'{DUMP}/other.bin': 1})   # files that are not the game's: refused with --romfs-push too, nothing deleted
        with self.assertRaisesRegex(BuildError, 'never deletes'): self.run_on(dev, romfs_at=DUMP, romfs_push=True)
        self.assertIn(f'{DUMP}/other.bin', dev.fs); self.assertEqual(dev.step('install'), -1)
        with self.assertRaisesRegex(BuildError, 'earlier nx2android'): apk_file(self.apk, access=False); self.run_on(Device({f'{DUMP}/{r}': n for r, n in ROM.items()}), romfs_at=DUMP)

    def test_access_refused(self):   # the device does not grant it: a clear error, and no data pushed
        dev = Device({f'{DUMP}/{r}': n for r, n in ROM.items()}, grants=False)
        with self.assertRaises(BuildError) as e: self.run_on(dev, romfs_at=DUMP)
        self.assertIn('MANAGE_EXTERNAL_STORAGE): default', str(e.exception)); self.assertIn('Settings > Apps > Test Game > All files access', str(e.exception))
        self.assertEqual((dev.step('push_tree'), dev.step('push_file')), (-1, -1))

    def test_android_10(self):   # no All files access: READ_EXTERNAL_STORAGE through pm grant
        dev = self.run_on(Device({f'{DUMP}/{r}': n for r, n in ROM.items()}, sdk=29), romfs_at=DUMP)
        self.assertNotEqual(dev.step('shell', 'pm grant org.nx2a.test android.permission.READ_EXTERNAL_STORAGE'), -1); self.assertEqual(dev.step('shell', 'appops'), -1)

    def test_carried_over_and_back(self):   # a later install checks the kept folder again; app moves the RomFS back into the files folder and drops the row
        env = {f'{FILES}/env.txt': install.env_with_romfs('DM_LANG=en-US\n', DUMP)}
        with self.assertRaisesRegex(BuildError, 'earlier install\'s --romfs-at'): self.run_on(Device({f'{DUMP}/c.bin': 5}, text=env, access='allow'))
        dev = self.run_on(Device({f'{DUMP}/c.bin': 5}, text=env, access='allow'), romfs_at='app')
        self.assertEqual({k[len(FILES) + 7:] for k in dev.fs if k.startswith(f'{FILES}/RomFS/')}, set(ROM)); self.assertIsNone(install.device_romfs(dev.text[f'{FILES}/env.txt']))
        self.assertNotEqual(dev.step('push_file', 'env.txt'), -1)   # the merged env.txt went on its own: the installer's row dropped
        self.assertNotIn('DM_ROMFS', dev.text[f'{FILES}/env.txt']); self.assertIn(f'{DUMP}/c.bin', dev.fs)   # the folder is left as it is

    def test_romfs_push(self):   # --romfs-push: the missing files and those of another size go into the folder, then it is checked again
        dev = self.run_on(Device({f'{DUMP}/c.bin': 4}), romfs_at=DUMP, romfs_push=True)
        self.assertEqual(dev.remote_sizes(DUMP), ROM); self.assertNotEqual(dev.step('push_tree', DUMP), -1)
        with self.assertRaisesRegex(BuildError, '--romfs-push needs --romfs-at'): self.run_on(Device(), romfs_push=True)

    def test_content(self):   # a folder of the right names and sizes but other bytes (another game version): refused, or pushed again with --romfs-push
        rom = {f'{DUMP}/{r}': n for r, n in ROM.items()}
        dev = Device(rom, md5={f'{DUMP}/c.bin': '0' * 32})
        with self.assertRaisesRegex(BuildError, r'1 have the right size but other content \(MD5: c.bin\)'): self.run_on(dev, romfs_at=DUMP)
        self.assertEqual(dev.step('install'), -1); self.assertNotEqual(dev.step('md5', DUMP), -1)
        dev = self.run_on(Device(rom, md5={f'{DUMP}/c.bin': '0' * 32}), romfs_at=DUMP, romfs_push=True)
        self.assertNotEqual(dev.step('push_tree', DUMP), -1); self.assertFalse(dev.md5)
        dev = self.run_on(Device(rom, md5=False), romfs_at=DUMP)   # no MD5 from the device: names and sizes only, as before
        self.assertNotEqual(dev.step('push_file', 'env.txt'), -1)
        dev = Device({f'{DUMP}/c.bin': 6})   # a listing that is refused anyway: no MD5 asked for
        with self.assertRaises(BuildError): self.run_on(dev, romfs_at=DUMP)
        self.assertEqual(dev.step('md5'), -1)

    def test_sample(self):   # files of at most SAMPLE_MAX bytes, at most SAMPLE_FILES of them, spread over the sorted paths
        rom = {f'f{i:04d}': 10 for i in range(1000)} | {'big': install.SAMPLE_MAX + 1}
        s = install.romfs_sample(rom)
        self.assertEqual(len(s), install.SAMPLE_FILES); self.assertNotIn('big', s); self.assertEqual((s[0], s[-1] > 'f0990'), ('f0000', True)); self.assertEqual(s, sorted(set(s)))
        self.assertEqual(install.romfs_sample(ROM), sorted(ROM))

    def test_update_grant_first(self):   # an update whose app asks for the access: a refused grant stops the install before the APK goes in
        rom = {f'{DUMP}/{r}': n for r, n in ROM.items()}
        dev = Device(rom, grants=False, installed=True)
        with self.assertRaisesRegex(BuildError, 'did not grant it'): self.run_on(dev, romfs_at=DUMP)
        self.assertEqual(dev.step('install'), -1)
        dev = Device(rom, grants=False, installed=True, requests=False)   # an older APK that did not ask: refused after the APK, before any data
        with self.assertRaisesRegex(BuildError, 'did not grant it'): self.run_on(dev, romfs_at=DUMP)
        self.assertNotEqual(dev.step('install'), -1); self.assertEqual((dev.step('push_tree'), dev.step('push_file')), (-1, -1))
        dev = self.run_on(Device(rom, installed=True), romfs_at=DUMP)   # granted before the APK
        self.assertLess(dev.step('shell', 'appops set'), dev.step('install'))

    def test_bundles_left_by_an_earlier_pack(self):   # a pack without bundles over a device that holds one with: its bundles folder goes, and a list the pack does not bring (files.json,
        # its validators, the one before) goes with it; a pack with a files.json of its own leaves the list to its own push
        old = {f'{FILES}/bundles/b0000.tar': 9, f'{FILES}/bundles/b0001.tar.part': 3, f'{FILES}/files.json': 5, f'{FILES}/files.json.etag': 5, f'{FILES}/files.json.prev': 5, f'{FILES}/log.txt': 9}
        dev = self.run_on(Device(old))
        self.assertFalse([k for k in dev.fs if '/bundles/' in k]); self.assertIn(f'{FILES}/log.txt', dev.fs)
        rm = [o[1] for o in dev.ops if o[0] == 'shell' and o[1].startswith('cd ') and 'rm -f' in o[1]]
        self.assertEqual(len(rm), 1); self.assertEqual(set(rm[0].split()) & {'files.json', 'files.json.etag', 'files.json.prev', datapack.MANIFEST}, {'files.json', 'files.json.etag', 'files.json.prev'})
        self.assertNotIn(datapack.MANIFEST, rm[0].split())   # (the pack's manifest is pushed last)
        (self.data / 'files.json').write_text('{}', encoding='utf-8')   # a pack with a list of its own (a format 1 one)
        dev = self.run_on(Device(old)); self.assertFalse([k for k in dev.fs if '/bundles/' in k])
        self.assertEqual([o for o in dev.ops if o[0] == 'shell' and 'rm -f' in o[1] and 'files.json' in o[1]], [])
        self.assertTrue(dev.step('push_tree', FILES) >= 0)   # (files.json goes with the pack's other files)
        dev = self.run_on(Device({f'{FILES}/log.txt': 9}))   # nothing to clear on a device without them: no command
        self.assertEqual([o for o in dev.ops if o[0] == 'shell' and ('bundles' in o[1] or 'rm -f' in o[1])], [])

    def test_free_space(self):   # the RomFS in another folder needs no room in the files folder
        rom = sum(ROM.values())
        self.run_on(Device({f'{DUMP}/{r}': n for r, n in ROM.items()}, free=self.bytes - rom), romfs_at=DUMP)
        with self.assertRaisesRegex(BuildError, 'GB more'): self.run_on(Device(free=self.bytes - rom))

    def test_options(self):   # install and port --install take --romfs-at / --romfs-push; port without --install, or a folder the app cannot read, fails before any stage
        a = cli._parser().parse_args(['install', 'x-data', '--romfs-at', DUMP, '--romfs-push'])
        self.assertEqual((a.romfs_at, a.romfs_push), (DUMP, True)); self.assertIsNone(cli._parser().parse_args(['install', 'x-data']).romfs_at)
        self.assertEqual(cli._parser().parse_args(['port', 'x', '--install', '--romfs-at', 'app']).romfs_at, 'app')
        self.assertEqual(cli.main(['port', str(self.t), '--romfs-at', DUMP]), 2); self.assertEqual(cli.main(['port', str(self.t), '--install', '--romfs-at', 'romfs']), 2)

    def test_apk_asks_for_access(self):   # mkapk.py's manifest: All files access, READ_EXTERNAL_STORAGE up to Android 10, the legacy storage view there; VIBRATE as before
        so = self.t / 'libdmhost.so'; so.write_bytes(b'\x7fELF' + b'\0' * 60)
        subprocess.run([sys.executable, str(REPO / 'nxnative/dm/android/apk/mkapk.py'), str(so), str(self.t / 'a.apk'), str(self.t / 'k.json'), '--lib', 'dmhost', '--release'],
                       check=True, capture_output=True)
        self.assertTrue(install.apk_asks_access(self.t / 'a.apk')); self.assertFalse(install.apk_asks_access(apk_file(self.t / 'old.apk', access=False)))
        with zipfile.ZipFile(self.t / 'a.apk') as z: els = axml_elements(z.read('AndroidManifest.xml'))
        perms = {a['name']: a.get('maxSdkVersion') for tag, a in els if tag == 'uses-permission'}
        self.assertEqual(perms, {'android.permission.VIBRATE': None, install.ACCESS: None, 'android.permission.READ_EXTERNAL_STORAGE': 29,
                                 # local play over the LAN (DM_LAN): sockets, the Wi-Fi address, the multicast lock
                                 'android.permission.INTERNET': None, 'android.permission.ACCESS_NETWORK_STATE': None,
                                 'android.permission.ACCESS_WIFI_STATE': None, 'android.permission.CHANGE_WIFI_MULTICAST_STATE': None,
                                 # the save's cloud sync (savecloud_android.c): the wake lock its upload holds as the app leaves the screen
                                 'android.permission.WAKE_LOCK': None})
        app = next(a for tag, a in els if tag == 'application')
        self.assertIs(app['requestLegacyExternalStorage'], True); self.assertIs(app['debuggable'], False)


class Bundles(unittest.TestCase):
    """A pack with bundles (android.tsv data_bundles): the bundles the device needs go as they are, the files the pack has on their own as before, no small file and no
    tar unpacked by the device; files.json before the manifest; a rerun pushes only what is still missing"""
    def setUp(self):
        import types
        from porter import publish
        self.t = Path(tempfile.mkdtemp(prefix='nx2a_test_bund_')); self.real, PROG.mode = install.Adb, 'off'
        src = self.t / 'src'
        for i in range(40): p = src / f'RomFS/d{i % 3}/f{i:02d}.bin'; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(bytes([i]) * (100 + i * 37))
        (src / 'RomFS/big.bin').write_bytes(b'b' * 5000); (src / 'mods').mkdir(); (src / 'mods/main').write_bytes(b'm' * 300)
        prof = types.SimpleNamespace(name='Test Game', id='0100000000AAA000', package=PKG, env=[], settings={}, requires=[], bundles=(2000, 8192))
        self.data = self.t / 'Test-data'
        datapack.build(self.data, profile=prof, version='v65536', display='1.1.0', patched=src / 'mods', romfs=src / 'RomFS', shaders=None)
        self.fj = publish.manifest(self.data); self.apk = apk_file(self.t / 'Test.apk')
        self.bun = [b['path'] for b in self.fj['bundles']]
        self.assertGreater(len(self.bun), 2)

    def tearDown(self): install.Adb = self.real; shutil.rmtree(self.t, ignore_errors=True)

    def run_on(self, dev, **kw):
        install.Adb = lambda exe, serial=None, log=print: dev
        install.install(self.apk, self.data, 'adb', **kw)
        return dev

    def members(self, k): return [f for f in self.fj['files'] if f.get('bundle') == k]

    def test_fresh(self):   # every bundle, the files on their own, files.json then the manifest; no bundled file and no device-side tar
        dev = self.run_on(Device())
        self.assertEqual(sorted(o[1] for o in dev.ops if o[0] == 'push_whole'), [f'{FILES}/{b}' for b in self.bun])
        for f in self.fj['files']:
            self.assertEqual(f'{FILES}/{f["path"]}' in dev.fs, 'bundle' not in f, f['path'])   # the bundled files come out of their bundles on the device, at the app's start
        self.assertEqual(dev.step('shell', 'tar -xf'), -1)
        order = [max(i for i, o in enumerate(dev.ops) if o[0] == 'push_whole'), dev.step('push_tree', FILES), dev.step('push_file', 'files.json'), dev.step('push_file', datapack.MANIFEST)]
        self.assertEqual(order, sorted(order)); self.assertNotIn(-1, order)

    def test_rerun_pushes_what_is_missing(self):   # a bundle whose files the device has, or one already on it with the pack's MD5, is not pushed again; another MD5 is
        fs = {f'{FILES}/{f["path"]}': f['size'] for f in self.members(0)}
        fs[f'{FILES}/{self.bun[1]}'] = (self.data / self.bun[1]).stat().st_size; fs[f'{FILES}/{self.bun[2]}'] = (self.data / self.bun[2]).stat().st_size
        md5 = {f'{FILES}/{self.bun[1]}': hashlib.md5((self.data / self.bun[1]).read_bytes()).hexdigest(), f'{FILES}/{self.bun[2]}': '0' * 32}
        dev = self.run_on(Device(fs, md5=md5))
        pushed = {o[1] for o in dev.ops if o[0] == 'push_whole'}
        self.assertEqual(pushed, {f'{FILES}/{b}' for b in self.bun[2:]})
        self.assertIn(f'{FILES}/{self.bun[1]}', dev.fs)   # kept for the app to unpack
        dev.ops.clear(); self.run_on(dev)   # again: nothing pushed (the device has every bundle at its size and MD5)
        self.assertEqual([o for o in dev.ops if o[0] == 'push_whole'], [])

    def test_stale(self):   # the device's files the list lacks go, its bundled files stay; a bundle the device does not need goes
        fs = {f'{FILES}/{f["path"]}': f['size'] for f in self.members(0)} | {f'{FILES}/RomFS/old.bin': 3, f'{FILES}/{self.bun[0]}': 10, f'{FILES}/bundles/b9999.tar': 5, f'{FILES}/log.txt': 9}
        dev = self.run_on(Device(fs))
        rm = ' '.join(o[1] for o in dev.ops if o[0] == 'shell' and 'rm -f' in o[1])
        for gone in ('RomFS/old.bin', self.bun[0], 'bundles/b9999.tar'): self.assertIn(gone, rm)
        for kept in [f['path'] for f in self.members(0)] + ['log.txt']: self.assertNotIn(kept, rm)

    def test_bundles_folder_the_app_made(self):   # a bundles folder adb's shell cannot write (an older APK made it): a clear error before any bundle is pushed
        class Locked(Device):
            def shell(self, script, **kw): return 'locked\n' if '[ ! -w' in script else super().shell(script, **kw)
        dev = Locked({f'{FILES}/bundles/b0000.tar.part': 5})
        with self.assertRaisesRegex(BuildError, 'made by the app itself'): self.run_on(dev)
        self.assertEqual([o for o in dev.ops if o[0] == 'push_whole'], [])

    def test_list_and_manifest_go_before_the_bundles(self):   # the device's files.json (and its validators, and the one before) and manifest are deleted before the first bundle is
        # pushed: an install that stops after the bundles then leaves a device the app cannot take for a whole pack; the list and the manifest are written again at the end
        old = {f'{FILES}/{n}': 5 for n in ('files.json', 'files.json.etag', 'files.json.prev', datapack.MANIFEST)}
        dev = self.run_on(Device(old | {f'{FILES}/log.txt': 9}))
        rm = next(i for i, o in enumerate(dev.ops) if o[0] == 'shell' and o[1].startswith('cd ') and 'rm -f' in o[1] and datapack.MANIFEST in o[1]); cmd = dev.ops[rm][1]
        for n in ('files.json', 'files.json.etag', 'files.json.prev', datapack.MANIFEST): self.assertIn(n, cmd.split())
        self.assertNotIn('log.txt', cmd)
        first = dev.step('push_whole'); self.assertGreater(first, rm)
        self.assertLess(rm, min(dev.step('push_tree', FILES), dev.step('push_file', 'files.json'), dev.step('push_file', datapack.MANIFEST)))
        self.assertLess(dev.step('push_file', 'files.json'), dev.step('push_file', datapack.MANIFEST))   # (written again: the list, then the manifest)

    def test_space_counts_the_bundles_unpacked_at_once(self):   # as the app counts (datasync.h ds_apply): the largest bundle for each unpacked at once (at most UNPACKERS), 4 KB a file written, 64 MB
        import types
        from porter import publish
        for name, size in (('few', 24 * 1024), ('many', 2048)):   # fewer bundles than unpackers: every one counts; more: UNPACKERS of them
            prof = types.SimpleNamespace(name='Test Game', id='0100000000AAA000', package=PKG, env=[], settings={}, requires=[], bundles=(2000, size))
            data = self.t / f'Test-data-{name}'
            datapack.build(data, profile=prof, version='v65536', display='1.1.0', patched=self.t / 'src/mods', romfs=self.t / 'src/RomFS', shaders=None); fj = publish.manifest(data)
            sizes = [b['size'] for b in fj['bundles']]; par = min(install.UNPACKERS, len(sizes))
            self.assertEqual(par < install.UNPACKERS, name == 'few', (name, len(sizes))); self.assertGreater(par, 1)
            loose = sum(f['size'] for f in fj['files'] if 'bundle' not in f)
            need = sum(sizes) + loose + par * max(sizes) + len(fj['files']) * install.FILE_SLACK + install.MARGIN
            self.data = data
            self.run_on(Device(free=need))   # (just enough)
            with self.assertRaisesRegex(BuildError, 'GB more'): self.run_on(Device(free=need - 1))

    def test_plan(self):   # bundle_plan: the bundles holding a file the device lacks at its size, and which of them it has at their size
        have = {f['path']: f['size'] for f in self.fj['files'] if f.get('bundle') != 1} | {self.bun[1]: self.fj['bundles'][1]['size']}
        self.assertEqual(install.bundle_plan(self.fj, have), ([1], [1]))
        have[self.members(2)[0]['path']] += 1
        self.assertEqual(install.bundle_plan(self.fj, have), ([1, 2], [1]))

    def test_no_romfs_at(self):   # the RomFS is in the bundles: --romfs-at refused before the device is touched, and an earlier install's folder too
        dev = Device()
        with self.assertRaisesRegex(BuildError, 'bundles'): self.run_on(dev, romfs_at=DUMP)
        self.assertEqual(dev.ops, [])
        env = {f'{FILES}/env.txt': install.env_with_romfs('DM_LANG=en-US\n', DUMP)}
        with self.assertRaisesRegex(BuildError, '--romfs-at app'): self.run_on(Device(text=env))
        self.run_on(Device(text=env), romfs_at='app')


class Packs(unittest.TestCase):
    """A pack with packs (android.tsv romfs_packs): packs/ goes as plain files (the packs and their index), never a RomFS small file; the small files an earlier pack had
    on their own go as stale ones; the room counts the packs and not those files as freed; --romfs-at refused"""
    def setUp(self):
        import types
        self.t = Path(tempfile.mkdtemp(prefix='nx2a_test_packs_')); self.real, PROG.mode = install.Adb, 'off'
        src = self.src = self.t / 'src'
        for i in range(40): p = src / f'RomFS/d{i % 3}/f{i:02d}.bin'; p.parent.mkdir(parents=True, exist_ok=True); p.write_bytes(bytes([i]) * (100 + i * 37))
        (src / 'RomFS/big.bin').write_bytes(b'b' * 5000); (src / 'mods').mkdir(); (src / 'mods/main').write_bytes(b'm' * 300)
        (src / 'aoc/1').mkdir(parents=True); (src / 'aoc/1/a.bin').write_bytes(b'a' * 50)
        self.prof = types.SimpleNamespace(name='Test Game', id='0100000000AAA000', package=PKG, env=[], settings={}, requires=[], bundles=(2000, 8192), romfs_packs=True, romfs_zstd=False)
        self.data = self.t / 'Test-data'
        datapack.build(self.data, profile=self.prof, version='v65536', display='1.1.0', patched=src / 'mods', romfs=src / 'RomFS', shaders=None)
        self.apk = apk_file(self.t / 'Test.apk')
        self.local = {p.relative_to(self.data).as_posix(): p.stat().st_size for p in self.data.rglob('*') if p.is_file()}
        self.packs = sorted(r for r in self.local if r.startswith('packs/'))
        self.assertGreater(len(self.packs), 3); self.assertFalse(datapack.bundled(datapack.read_manifest(self.data)))

    def tearDown(self): install.Adb = self.real; shutil.rmtree(self.t, ignore_errors=True)

    def run_on(self, dev, **kw):
        install.Adb = lambda exe, serial=None, log=print: dev
        install.install(self.apk, self.data, 'adb', **kw)
        return dev

    def test_fresh(self):   # the pack folder as it is: the packs and the index pushed, no RomFS small file, no bundle
        dev = self.run_on(Device())
        for r in self.packs: self.assertEqual(dev.fs.get(f'{FILES}/{r}'), self.local[r], r)
        self.assertEqual({k for k in dev.fs if k.startswith(f'{FILES}/RomFS/')}, {f'{FILES}/RomFS/big.bin'})
        self.assertEqual([o for o in dev.ops if o[0] == 'push_whole'], [])
        self.assertIn(f'DM_ROMFS_PACK={FILES}/packs/romfs.nxbi', dev.text[f'{FILES}/env.txt'])

    def test_stale(self):   # the small files an earlier pack had on their own, and a pack this one does not have, go; the player's files stay
        old = {f'{FILES}/RomFS/d0/f00.bin': 100, f'{FILES}/RomFS/d1/f01.bin': 137, f'{FILES}/packs/p0099.tar': 10240, f'{FILES}/log.txt': 9}
        dev = self.run_on(Device(old))
        rm = ' '.join(o[1] for o in dev.ops if o[0] == 'shell' and 'rm -f' in o[1])
        for gone in ('RomFS/d0/f00.bin', 'RomFS/d1/f01.bin', 'packs/p0099.tar'): self.assertIn(gone, rm)
        for kept in self.packs + ['log.txt', 'RomFS/big.bin']: self.assertNotIn(kept, rm)

    def test_with_bundles(self):   # the DLC's small file in a bundle: a pack with bundles for the installer; the packs go with the loose files, the RomFS small files are stale
        from porter import publish
        datapack.build(self.data, profile=self.prof, version='v65536', display='1.1.0', patched=self.src / 'mods', romfs=self.src / 'RomFS', shaders=None, aoc=self.src / 'aoc')
        fj = publish.manifest(self.data); self.assertEqual([b['path'] for b in fj['bundles']], ['bundles/b0000.tar'])
        dev = self.run_on(Device({f'{FILES}/RomFS/d0/f00.bin': 100}))
        self.assertEqual([o[1] for o in dev.ops if o[0] == 'push_whole'], [f'{FILES}/bundles/b0000.tar'])
        for r in self.packs: self.assertIn(f'{FILES}/{r}', dev.fs)
        self.assertIn('RomFS/d0/f00.bin', ' '.join(o[1] for o in dev.ops if o[0] == 'shell' and 'rm -f' in o[1]))

    def test_space(self):   # what the push writes (the packs, not the small files' bytes the manifest counts), with an earlier pack's loose small files not counted as freed
        want = sum(self.local.values())
        self.run_on(Device(free=want))
        with self.assertRaisesRegex(BuildError, 'GB more'): self.run_on(Device(free=want - 1))
        loose = {f'{FILES}/RomFS/{p.relative_to(self.src / "RomFS").as_posix()}': p.stat().st_size for p in (self.src / 'RomFS').rglob('*') if p.is_file()}
        with self.assertRaisesRegex(BuildError, 'GB more'): self.run_on(Device(loose, free=want - 5001))   # (big.bin is there at its size: not pushed)
        self.run_on(Device(loose, free=want - 5000))

    def test_no_romfs_at(self):   # the RomFS's small files are in the packs: --romfs-at refused before the device is touched, and an earlier install's folder too
        dev = Device()
        with self.assertRaisesRegex(BuildError, 'romfs_packs'): self.run_on(dev, romfs_at=DUMP)
        self.assertEqual(dev.ops, [])
        env = {f'{FILES}/env.txt': install.env_with_romfs('DM_LANG=en-US\n', DUMP)}
        with self.assertRaisesRegex(BuildError, '--romfs-at app'): self.run_on(Device(text=env))
        self.run_on(Device(text=env), romfs_at='app')


class Hold(unittest.TestCase):
    """Adb.hold, the lock that keeps two porter installs off one device: two Adb objects for one serial (two installs), a lock file in a temporary folder"""
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory(prefix='nx2a_hold_'); self.addCleanup(self.dir.cleanup)
        self.msgs = []
        self.a, self.b = (Adb('adb', 'dev:5555', log=self.msgs.append) for _ in range(2))
        lock = self.a.hold(self.dir.name)
        if lock is None: self.skipTest('no flock or msvcrt.locking here')
        lock.close()
        self.poll, adb_mod.HOLD_POLL = adb_mod.HOLD_POLL, 0.02
        self.addCleanup(setattr, adb_mod, 'HOLD_POLL', self.poll)

    def test_second_refused_then_free(self):
        first = self.a.hold(self.dir.name)
        with self.assertRaises(AdbError): self.b.hold(self.dir.name, wait=False)
        first.close()
        second = self.b.hold(self.dir.name, wait=False); self.assertIsNotNone(second); second.close()
        self.assertEqual([p.name for p in Path(self.dir.name).iterdir()], ['install-dev_5555.lock'])

    def test_other_device_not_held(self):
        first = self.a.hold(self.dir.name)
        other = Adb('adb', 'other', log=self.msgs.append).hold(self.dir.name, wait=False)
        other.close(); first.close()

    def test_second_waits_for_first(self):
        first = self.a.hold(self.dir.name); got = []
        t = threading.Thread(target=lambda: got.append(self.b.hold(self.dir.name))); t.start()
        t.join(0.3); self.assertTrue(t.is_alive()); self.assertEqual(got, [])   # waiting
        self.assertEqual(self.msgs, ['another porter install is using dev:5555: waiting for it to end'])
        first.close(); t.join(10); self.assertFalse(t.is_alive())
        self.assertIsNotNone(got[0]); got[0].close()


if __name__ == '__main__':
    unittest.main()
