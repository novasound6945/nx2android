# The pointer file (porter port --data-pointer; porter pointer): the line porter/publish.py seals, opened again as the app does (dm/android/datasync.h ds_pointer_open: the
# C half is in tests/test_datasync.py, PointerC), the APK's asset of version 2, the port's options and the pointer command. The asset of an APK without a pointer (version 1)
# stays byte for byte what it was. python -m unittest tests.test_pointer
import base64, contextlib, hashlib, io, os, shutil, sys, tempfile, types, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO))
os.environ['NX2A_HOME'] = tempfile.mkdtemp(prefix='nx2a_test_home_')

from porter import cli, pipeline, publish   # noqa: E402
from porter.upstream import BuildError       # noqa: E402

KEY = bytes(range(32))
SECRET = 'reader:s3cr3t-pass'


def unmask(asset): return publish.mask(asset[5:]).decode()   # (the mask is its own inverse)


class Seal(unittest.TestCase):
    def test_round_trip(self):
        for root, cred in (('https://example.com/share/', SECRET), ('https://example.com:5006/a/b/', ''), ('https://example.com/x', 'u:p:with:colons')):
            line = publish.pointer_seal(root, cred, KEY)
            self.assertTrue(line.startswith('NX2P1:')); self.assertTrue(line.isascii()); self.assertLessEqual(len(line), publish.POINTER_MAX)
            self.assertEqual(publish.pointer_open(line, KEY), (root if root.endswith('/') else root + '/', cred))
            self.assertEqual(publish.pointer_open('  \n' + line + '\r\n\n', KEY)[0][:8], 'https://')   # spaces and line ends around it are ignored
            self.assertNotIn(b'example', base64.b64decode(line[6:])); self.assertNotIn(b's3cr3t', base64.b64decode(line[6:]))

    def test_layout(self):   # nonce 16 ‖ ciphertext ‖ tag 32, the keystream and the tag as the design says (an independent computation)
        import hmac
        nonce = b'N' * 16; plain = b'https://example.com/r/\n' + SECRET.encode()
        line = publish.pointer_seal('https://example.com/r/', SECRET, KEY, nonce=nonce); raw = base64.b64decode(line[6:])
        enc = hashlib.sha256(b'nx2p-enc' + KEY).digest(); mac = hashlib.sha256(b'nx2p-mac' + KEY).digest()
        ks = b''.join(hashlib.sha256(enc + nonce + i.to_bytes(4, 'little')).digest() for i in range(4))
        ct = bytes(a ^ b for a, b in zip(plain, ks))
        self.assertEqual(raw, nonce + ct + hmac.new(mac, b'NX2P1' + nonce + ct, hashlib.sha256).digest())

    def test_nonce_fresh(self):
        self.assertNotEqual(publish.pointer_seal('https://example.com/', '', KEY), publish.pointer_seal('https://example.com/', '', KEY))

    def test_tampered(self):   # a byte changed in the nonce, the ciphertext or the tag, a line cut short, another key: refused, and nothing of the content in the reason
        line = publish.pointer_seal('https://example.com/share/', SECRET, KEY); raw = bytearray(base64.b64decode(line[6:]))
        for at in (0, 15, 16, 20, len(raw) - 33, len(raw) - 32, len(raw) - 1):
            b = bytearray(raw); b[at] ^= 0x01
            with self.assertRaises(BuildError, msg=at) as e: publish.pointer_open('NX2P1:' + base64.b64encode(bytes(b)).decode(), KEY)
            self.assertNotIn('s3cr3t', str(e.exception)); self.assertNotIn('example', str(e.exception))
        for bad in ('NX2P1:' + base64.b64encode(bytes(raw[:-1])).decode(), 'NX2P1:' + base64.b64encode(bytes(raw[:40])).decode(), line[:-2], 'NX2P0:' + line[6:],
                    'NX2P1:!!!!', '', line.replace('NX2P1:', ''), 'NX2P1:' + 'A' * 6000):
            with self.assertRaises(BuildError, msg=bad[:20]): publish.pointer_open(bad, KEY)
        with self.assertRaises(BuildError) as e: publish.pointer_open(line, bytes(32))
        self.assertIn('key', str(e.exception)); self.assertNotIn('s3cr3t', str(e.exception))

    def test_root_https_only(self):   # plain HTTP is not a server to move to (Android refuses cleartext; the password would go in the clear); a root ends in /
        for bad in ('http://example.com/share/', 'ftp://example.com/', 'example.com/', 'https://', 'https://exa mple.com/', 'HTTPS://example.com/', 'https://example.com/a\\b/'):
            with self.assertRaises(BuildError, msg=bad): publish.pointer_seal(bad, '', KEY)
        self.assertEqual(publish.check_root('https://example.com/share'), 'https://example.com/share/')
        # a line sealed by hand around http:// (or without the / at the end) is refused when opened, as the app refuses it
        import hmac
        for root in ('http://example.com/share/', 'https://example.com/share'):
            nonce = b'\0' * 16; plain = (root + '\n').encode(); enc, mac = publish._pointer_keys(KEY)
            ct = bytes(a ^ b for a, b in zip(plain, publish._keystream(enc, nonce, len(plain))))
            line = 'NX2P1:' + base64.b64encode(nonce + ct + hmac.new(mac, b'NX2P1' + nonce + ct, hashlib.sha256).digest()).decode()
            with self.assertRaises(BuildError, msg=root) as e: publish.pointer_open(line, KEY)
            self.assertIn('https://', str(e.exception))

    def test_too_long(self):
        with self.assertRaises(BuildError): publish.pointer_seal('https://example.com/' + 'a' * 880 + '/', 'u:' + 'p' * 2000, KEY)


class Asset(unittest.TestCase):
    V1 = '4e58325301068f409852d15cce118c55fa65e666ac6ba938e269f165c448c958d152cc47c031f02ef9'   # source_asset('https://h:8443/data/x/', 'reader:secret') before the pointer

    def test_version_1_unchanged(self):   # no pointer: the same bytes as before the pointer existed (the app of every APK out there reads exactly these)
        self.assertEqual(publish.source_asset('https://h:8443/data/x/', 'reader:secret').hex(), self.V1)
        self.assertEqual(publish.source_asset('https://h:8443/data/x/', 'reader:secret', None).hex(), self.V1)
        a = publish.source_asset('http://example.com/p', ''); self.assertEqual(a[:5], b'NX2S\x01'); self.assertEqual(unmask(a), 'http://example.com/p/\n')

    def test_version_2(self):
        a = publish.source_asset('https://example.com/share/zelda/1.1.0/', SECRET, ('https://example.org/raw/pointer.txt', KEY, 'zelda/1.1.0/'))
        self.assertEqual(a[:5], b'NX2S\x02'); self.assertNotIn(b's3cr3t', a); self.assertNotIn(KEY.hex().encode(), a); self.assertNotIn(b'example', a)
        self.assertEqual(unmask(a).split('\n'), ['https://example.com/share/zelda/1.1.0/', SECRET, 'https://example.org/raw/pointer.txt', KEY.hex(), 'zelda/1.1.0/'])
        self.assertEqual(unmask(publish.source_asset('https://example.com/r/', '', ('https://example.org/p', KEY, ''))).split('\n')[-1], '')   # the pack at the root itself
        for bad in (('http://example.org/p', KEY, ''), ('https://example.org/p', KEY, '/abs/'), ('https://example.org/p', KEY, 'a/../b/'), ('https://example.org/p', KEY, 'a/b'),
                    ('https://example.org/p', KEY, 'a\\b/'), ('https://example.org/p', KEY, 'a//b/')):
            with self.assertRaises(BuildError, msg=bad): publish.source_asset('https://example.com/r/', '', bad)

    def test_subpath(self):
        self.assertEqual(publish.data_subpath('https://example.com/share/', 'https://example.com/share/zelda/1.1.0/'), 'zelda/1.1.0/')
        self.assertEqual(publish.data_subpath('https://example.com/share/', 'https://example.com/share/'), '')
        for url in ('https://example.com/other/zelda/', 'https://example.com/portingx/zelda/', 'http://example.com/share/zelda/'):
            with self.assertRaisesRegex(BuildError, 'under --data-root', msg=url): publish.data_subpath('https://example.com/share/', url)


class Keys(unittest.TestCase):
    def setUp(self): self.t = Path(tempfile.mkdtemp(prefix='nx2a_test_ptr_'))
    def tearDown(self): shutil.rmtree(self.t, ignore_errors=True)

    def test_new_and_read(self):
        p = publish.new_key(self.t / 'k'); key = publish.read_key(p); self.assertEqual(len(key), 32)
        self.assertEqual(p.read_text(), key.hex() + '\n')
        with self.assertRaises(BuildError): publish.new_key(self.t / 'k')   # never over an existing key
        self.assertEqual(p.read_text(), key.hex() + '\n')
        (self.t / 'bad').write_text('zz' * 32)
        with self.assertRaises(BuildError) as e: publish.read_key(self.t / 'bad')
        self.assertNotIn('zz', str(e.exception))
        with self.assertRaises(BuildError): publish.read_key(self.t / 'missing')


class PortOptions(unittest.TestCase):
    def setUp(self):
        self.t = Path(tempfile.mkdtemp(prefix='nx2a_test_ptr_')); (self.t / 'game').mkdir()
        self.key = publish.new_key(self.t / 'key'); (self.t / 'cred').write_text(SECRET + '\n')
    def tearDown(self): shutil.rmtree(self.t, ignore_errors=True)

    def opts(self, **kw):
        d = dict(folder=self.t / 'game', data_url='https://example.com/share/zelda/1.1.0/', data_root='https://example.com/share/', data_pointer='https://example.org/raw/p.txt',
                 data_pointer_key=self.key, data_cred=self.t / 'cred')
        d.update(kw); return pipeline.Options(**{k: v for k, v in d.items() if v is not ...})

    def test_parsed(self):
        a = cli._parser().parse_args(['port', 'x', '--data-url', 'u', '--data-root', 'r', '--data-pointer', 'p', '--data-pointer-key', 'k'])
        self.assertEqual((a.data_root, a.data_pointer, a.data_pointer_key), ('r', 'p', 'k'))
        a = cli._parser().parse_args(['port', 'x']); self.assertEqual((a.data_root, a.data_pointer, a.data_pointer_key), ('', '', None))

    def test_bad_combinations(self):   # each fails before any stage (before the keys are looked for), and no message shows the key or the password
        key = publish.read_key(self.key).hex()
        for label, kw, want in (('no root', dict(data_root=...), 'all four'), ('no key', dict(data_pointer_key=...), 'all four'), ('no url', dict(data_url=..., data_cred=...), 'all four'),
                                ('root alone', dict(data_pointer=..., data_pointer_key=...), 'all four'), ('key alone', dict(data_pointer=..., data_root=...), 'all four'),
                                ('not a prefix', dict(data_root='https://example.com/other/'), 'under --data-root'),
                                ('http pointer', dict(data_pointer='http://example.org/p.txt'), 'data-pointer'),
                                ('bad key file', dict(data_pointer_key=self.t / 'cred'), 'data-pointer-key')):
            out = io.StringIO()
            with self.assertRaises(BuildError, msg=label) as e, contextlib.redirect_stdout(out), contextlib.redirect_stderr(out): pipeline.Port(self.opts(**kw)).run()
            self.assertIn(want, str(e.exception), label)
            for s in (str(e.exception), out.getvalue()): self.assertNotIn('s3cr3t', s, label); self.assertNotIn(key, s, label)

    def test_asset(self):   # the port's asset: version 2 with the pointer, version 1 without; the log names the pointer, never the key or the password
        o = self.opts(); o.data_url = publish.check_url(o.data_url); o.data_root = publish.check_url(o.data_root)
        port = types.SimpleNamespace(o=o, aw=self.t); out = io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(out): args = pipeline.Port.data_source(port)
        a = (self.t / 'data_source.bin').read_bytes(); self.assertEqual(a[:5], b'NX2S\x02'); self.assertNotIn('--cleartext', args)
        self.assertEqual(unmask(a).split('\n'), ['https://example.com/share/zelda/1.1.0/', SECRET, 'https://example.org/raw/p.txt', publish.read_key(self.key).hex(), 'zelda/1.1.0/'])
        self.assertIn('https://example.org/raw/p.txt', out.getvalue()); self.assertNotIn('s3cr3t', out.getvalue()); self.assertNotIn(publish.read_key(self.key).hex(), out.getvalue())
        o2 = self.opts(data_root=..., data_pointer=..., data_pointer_key=...); o2.data_url = publish.check_url(o2.data_url)
        with contextlib.redirect_stdout(io.StringIO()): pipeline.Port.data_source(types.SimpleNamespace(o=o2, aw=self.t))
        self.assertEqual((self.t / 'data_source.bin').read_bytes(), publish.source_asset(o2.data_url, SECRET))   # without the pointer: version 1, as before


class Command(unittest.TestCase):
    def setUp(self): self.t = Path(tempfile.mkdtemp(prefix='nx2a_test_ptr_'))
    def tearDown(self): shutil.rmtree(self.t, ignore_errors=True)

    def run_cli(self, *argv, stdin=''):
        out, err = io.StringIO(), io.StringIO(); old = sys.stdin; sys.stdin = io.StringIO(stdin)
        try:
            with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err): rc = cli.main(list(argv))
        finally: sys.stdin = old
        return rc, out.getvalue(), err.getvalue()

    def test_three_modes(self):
        k = self.t / 'key'; rc, out, err = self.run_cli('pointer', '--new-key', str(k)); self.assertEqual(rc, 0, err)
        key = publish.read_key(k); self.assertNotIn(key.hex(), out + err)
        self.assertEqual(self.run_cli('pointer', '--new-key', str(k))[0], 1)   # not over the key
        (self.t / 'cred').write_text(SECRET + '\n')
        rc, out, err = self.run_cli('pointer', '--root', 'https://example.com/share', '--cred', str(self.t / 'cred'), '--key', str(k)); self.assertEqual(rc, 0, err)
        line = out.strip(); self.assertEqual(publish.pointer_open(line, key), ('https://example.com/share/', SECRET))
        self.assertNotIn('s3cr3t', out + err); self.assertNotIn(key.hex(), out + err); self.assertNotIn('example', out)
        rc, out, err = self.run_cli('pointer', '--root', 'https://example.com/share/', '--key', str(k), '-o', str(self.t / 'p.txt')); self.assertEqual(rc, 0, err)
        self.assertEqual(out, ''); self.assertEqual(publish.pointer_open((self.t / 'p.txt').read_text(), key), ('https://example.com/share/', ''))
        (self.t / 'line').write_text(line + '\n')
        rc, out, err = self.run_cli('pointer', '--check', str(self.t / 'line'), '--key', str(k)); self.assertEqual(rc, 0, err)
        self.assertEqual(out.strip(), 'https://example.com/share/ (with credentials)'); self.assertNotIn('s3cr3t', out + err)
        rc, out, err = self.run_cli('pointer', '--check', '-', '--key', str(k), stdin=(self.t / 'p.txt').read_text()); self.assertEqual((rc, out.strip()), (0, 'https://example.com/share/ (no credentials)'))
        (self.t / 'other').write_text(os.urandom(32).hex())
        rc, out, err = self.run_cli('pointer', '--check', str(self.t / 'line'), '--key', str(self.t / 'other')); self.assertNotEqual(rc, 0); self.assertNotIn('s3cr3t', out + err)
        rc, out, err = self.run_cli('pointer', '--root', 'http://example.com/share/', '--key', str(k)); self.assertNotEqual(rc, 0); self.assertEqual(out, '')
        self.assertNotEqual(self.run_cli('pointer', '--root', 'https://example.com/', '--key', str(self.t / 'missing'))[0], 0)
        self.assertNotEqual(self.run_cli('pointer', '--root', 'https://example.com/')[0], 0)   # no key


if __name__ == '__main__':
    unittest.main()
