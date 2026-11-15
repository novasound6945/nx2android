"""The audio renderer's update protocol, served by the host for SDK 1.x (nxnative/dm/host/hle_audren_upd.c): RequestUpdate's output layout, memory pool and
effect states, and wave buffers taken at the rate playback takes them (pitch, sample rate, loops, end of stream, stop)."""
import ctypes as C
import os
from pathlib import Path
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
REV2 = 0x32564552   # "REV2", what SDK 1.7.1's client sends
STARTED, STOPPED, PAUSED = 0, 1, 2


def param(rate=48000, count=240, mixbuf=6, sub=2, voices=4, sinks=1, effects=2, perf=0, rev=REV2):
    b = bytearray(0x34)
    struct.pack_into('<8I', b, 0, rate, count, mixbuf, sub, voices, sinks, effects, perf)
    struct.pack_into('<I', b, 0x30, rev)
    return bytes(b)


def voice(state=STARTED, new=0, rate=48000, pitch=1.0, wbs=(), in_use=1):
    """wbs: (slot, start, end, loop, eos); every other slot reads as sent in an earlier update."""
    b = bytearray(0x170)
    b[8], b[9], b[0xa], b[0xb] = new, in_use, state, 2   # PCM16
    struct.pack_into('<I', b, 0xc, rate)
    struct.pack_into('<Iff', b, 0x18, 1, pitch, 1.0)
    struct.pack_into('<I', b, 0x3c, len(wbs))
    for k in range(4):
        b[0x60 + 0x38 * k + 0x1a] = 1
    for slot, start, end, loop, eos in wbs:
        o = 0x60 + 0x38 * slot
        struct.pack_into('<QQII', b, o, 0x10000 + slot * 0x1000, end * 2, start, end)
        b[o + 0x18], b[o + 0x19], b[o + 0x1a] = loop, eos, 0
    return bytes(b)


def effect(type_=1, new=0, enabled=1):
    b = bytearray(0xc0)
    b[0], b[1], b[2] = type_, new, enabled
    return bytes(b)


def pool(state):
    return struct.pack('<QQI12x', 0x20000, 0x1000, state)


def update(voices, pools=(), effects=(), nsinks=1, nmix=3, nchres=4, rev=REV2, voice_size=0x170):
    secs = [struct.pack('<IIQ', rev, 0, 0), b''.join(pools), bytes(0x70 * nchres), b''.join(voices), b''.join(effects), b'',
            bytes(0x930 * nmix), bytes(0x140 * nsinks), bytes(0x10)]
    sizes = [len(s) for s in secs]
    hdr = bytearray(0x40)
    # header: +4 behaviour, +8 pools, +0xc voices, +0x10 channel resources, +0x14 effects, +0x18 mixes, +0x1c sinks, +0x20 performance, +0x24 splitter
    struct.pack_into('<10I', hdr, 0, rev, sizes[0], sizes[1], sizes[3] if voice_size == 0x170 else voice_size, sizes[2], sizes[4], sizes[6], sizes[7], sizes[8], sizes[5])
    body = b''.join(secs)
    struct.pack_into('<I', hdr, 0x3c, 0x40 + len(body))
    return bytes(hdr) + body


def answer_size(npools, nvoices, neffects, nsinks):
    return 0x40 + 0x10 * (npools + nvoices + neffects) + 0x20 * nsinks + 0x10 + 0xb0


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class AudioRendererUpdate(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        lib = Path(cls.tmp.name) / 'audren.so'
        subprocess.run(['cc', '-shared', '-fPIC', '-O2', '-Wall', '-Werror', str(ROOT / 'tests/audren_upd_fixture.c'), '-o', str(lib)], check=True, capture_output=True)
        cls.lib = C.CDLL(str(lib))
        cls.lib.fx_open.restype = C.c_void_p
        cls.lib.fx_open.argtypes = [C.c_char_p, C.POINTER(C.c_uint32)]
        cls.lib.fx_close.argtypes = [C.c_void_p]
        cls.lib.fx_start.argtypes = [C.c_void_p, C.c_int]
        cls.lib.fx_update.restype = C.c_uint32
        cls.lib.fx_update.argtypes = [C.c_void_p, C.c_char_p, C.c_size_t, C.c_void_p, C.c_size_t, C.POINTER(C.c_uint32), C.POINTER(C.c_uint32)]
        cls.lib.fx_frame.restype = C.c_uint32
        cls.lib.fx_frame.argtypes = [C.c_void_p]
        cls.lib.fx_work.restype = C.c_uint64
        cls.lib.fx_work.argtypes = [C.c_char_p]

    @classmethod
    def tearDownClass(cls):
        if os.name == 'nt':   # Windows keeps a loaded DLL's file locked: unload it before the folder goes
            C.windll.kernel32.FreeLibrary(C.c_void_p(cls.lib._handle))
        cls.tmp.cleanup()

    def open(self, **kw):
        res = C.c_uint32()
        r = self.lib.fx_open(param(**kw), C.byref(res))
        self.assertEqual(res.value, 0)
        self.addCleanup(self.lib.fx_close, r)
        return r

    def send(self, r, data, out_size=None, expect=0):
        out = C.create_string_buffer(out_size or 0x4000)
        total, new = C.c_uint32(), C.c_uint32()
        res = self.lib.fx_update(r, data, len(data), out, len(out), C.byref(total), C.byref(new))
        self.assertEqual(res, expect)
        return out.raw, total.value, new.value

    def voice_out(self, out, npools, i):
        o = 0x40 + 0x10 * npools + 0x10 * i
        played, consumed = struct.unpack_from('<QI', out, o)
        return played, consumed

    def frames(self, r, n):
        return sum(self.lib.fx_frame(r) for _ in range(n))

    def test_open_takes_rev1_to_rev4_only(self):
        res = C.c_uint32()
        self.assertFalse(self.lib.fx_open(param(rev=0x35564552), C.byref(res)))   # REV5: the SDKs that render in process
        self.assertEqual(res.value, 0x499)
        self.assertFalse(self.lib.fx_open(param(rate=44100), C.byref(res)))
        self.assertEqual(res.value, 0x1099)
        self.open(rev=0x31564552)

    def test_answer_layout(self):
        r = self.open()
        out, total, _ = self.send(r, update([voice()] * 4, pools=[pool(1)] * 10, effects=[effect()] * 2))
        self.assertEqual(total, answer_size(10, 4, 2, 1))
        rev, beh, pools, voices, chres, effects, mixes, sinks, perf = struct.unpack_from('<9I', out, 0)
        self.assertEqual((rev, beh, pools, voices, chres, effects, mixes, sinks, perf), (REV2, 0xb0, 0xa0, 0x40, 0, 0x20, 0, 0x20, 0x10))
        self.assertEqual(struct.unpack_from('<I', out, 0x3c)[0], total)

    def test_refusals(self):
        r = self.open()
        self.send(r, update([voice()] * 4, rev=0x39564552), expect=0x499)
        self.send(r, update([voice()] * 4, voice_size=0x100), expect=0x5299)              # not whole voices
        self.send(r, update([voice()] * 5), expect=0x5299)                                # more voices than opened with
        self.send(r, update([voice()] * 4), out_size=answer_size(0, 4, 0, 1) - 1, expect=0x5299)
        self.send(r, update([voice()] * 4)[:-1], expect=0x5299)                           # sections past the input

    def test_memory_pools_attach_and_detach(self):
        r = self.open()
        out, _, _ = self.send(r, update([voice(in_use=0)] * 4, pools=[pool(4), pool(2), pool(5), pool(1)]))
        # requested attach → attached, requested detach → detached, anything else left as the client has it (0)
        self.assertEqual([struct.unpack_from('<I', out, 0x40 + 0x10 * i)[0] for i in range(4)], [5, 3, 0, 0])

    def test_buffers_finish_at_the_playback_rate(self):
        for rate, pitch, per_frame in ((48000, 1.0, 240), (32000, 1.0, 160), (48000, 2.0, 480), (48000, 0.5, 120)):
            with self.subTest(rate=rate, pitch=pitch):
                r = self.open()
                self.lib.fx_start(r, 1)
                n = per_frame * 2   # a buffer of two frames' samples
                out, _, new = self.send(r, update([voice(rate=rate, pitch=pitch, wbs=[(0, 0, n, 0, 0), (1, 0, n, 0, 0)])] + [voice(in_use=0)] * 3))
                self.assertEqual((new, self.voice_out(out, 0, 0)), (2, (0, 0)))
                self.frames(r, 1)
                out, _, new = self.send(r, update([voice(rate=rate, pitch=pitch)] + [voice(in_use=0)] * 3))
                self.assertEqual((new, self.voice_out(out, 0, 0)), (0, (per_frame, 0)))   # sent once: not queued again
                self.frames(r, 1)
                out, _, _ = self.send(r, update([voice(rate=rate, pitch=pitch)] + [voice(in_use=0)] * 3))
                self.assertEqual(self.voice_out(out, 0, 0), (2 * per_frame, 1))
                self.frames(r, 2)
                out, _, _ = self.send(r, update([voice(rate=rate, pitch=pitch)] + [voice(in_use=0)] * 3))
                self.assertEqual(self.voice_out(out, 0, 0), (4 * per_frame, 2))
                self.frames(r, 3)   # starving: nothing more to take
                out, _, _ = self.send(r, update([voice(rate=rate, pitch=pitch)] + [voice(in_use=0)] * 3))
                self.assertEqual(self.voice_out(out, 0, 0), (4 * per_frame, 2))

    def test_fractional_rate_carries_over(self):
        r = self.open()
        self.lib.fx_start(r, 1)
        self.send(r, update([voice(rate=44100, wbs=[(0, 0, 100000, 0, 0)])] + [voice(in_use=0)] * 3))
        self.frames(r, 10)   # 220.5 samples a frame
        out, _, _ = self.send(r, update([voice(rate=44100)] + [voice(in_use=0)] * 3))
        self.assertEqual(self.voice_out(out, 0, 0), (2205, 0))

    def test_one_frame_spans_buffers(self):
        r = self.open()
        self.lib.fx_start(r, 1)
        self.send(r, update([voice(wbs=[(0, 0, 100, 0, 0), (1, 50, 150, 0, 0), (2, 0, 0, 0, 0), (3, 0, 1000, 0, 0)])] + [voice(in_use=0)] * 3))
        self.assertEqual(self.frames(r, 1), 3)   # 100 + 100 + an empty one, then 40 into the fourth
        out, _, _ = self.send(r, update([voice()] + [voice(in_use=0)] * 3))
        self.assertEqual(self.voice_out(out, 0, 0), (240, 3))

    def test_nothing_plays_while_the_renderer_is_stopped(self):
        r = self.open()
        self.send(r, update([voice(wbs=[(0, 0, 240, 0, 0)])] + [voice(in_use=0)] * 3))
        self.assertEqual(self.frames(r, 5), 0)
        self.lib.fx_start(r, 1)
        self.assertEqual(self.frames(r, 1), 1)

    def test_queued_while_stopped_plays_from_start(self):
        r = self.open()
        self.lib.fx_start(r, 1)
        self.send(r, update([voice(state=STOPPED, wbs=[(0, 0, 240, 0, 0)])] + [voice(in_use=0)] * 3))
        self.assertEqual(self.frames(r, 3), 0)
        self.send(r, update([voice(state=STARTED)] + [voice(in_use=0)] * 3))
        self.assertEqual(self.frames(r, 1), 1)

    def test_pause_holds_and_stop_drops_the_queue(self):
        r = self.open()
        self.lib.fx_start(r, 1)
        self.send(r, update([voice(wbs=[(0, 0, 480, 0, 0), (1, 0, 480, 0, 0), (2, 0, 480, 0, 0)])] + [voice(in_use=0)] * 3))
        self.frames(r, 1)
        self.send(r, update([voice(state=PAUSED)] + [voice(in_use=0)] * 3))
        self.assertEqual(self.frames(r, 4), 0)
        out, _, _ = self.send(r, update([voice(state=PAUSED)] + [voice(in_use=0)] * 3))
        self.assertEqual(self.voice_out(out, 0, 0), (240, 0))
        self.send(r, update([voice(state=STOPPED)] + [voice(in_use=0)] * 3))
        self.assertEqual(self.frames(r, 1), 3)   # all three come back at once
        out, _, _ = self.send(r, update([voice(state=STOPPED)] + [voice(in_use=0)] * 3))
        self.assertEqual(self.voice_out(out, 0, 0), (0, 3))

    def test_looping_buffer_never_finishes(self):
        r = self.open()
        self.lib.fx_start(r, 1)
        self.send(r, update([voice(wbs=[(0, 0, 100, 1, 0), (1, 0, 100, 0, 0)])] + [voice(in_use=0)] * 3))
        self.assertEqual(self.frames(r, 10), 0)
        out, _, _ = self.send(r, update([voice()] + [voice(in_use=0)] * 3))
        self.assertEqual(self.voice_out(out, 0, 0), (2400, 0))

    def test_end_of_stream_resets_played_samples(self):
        r = self.open()
        self.lib.fx_start(r, 1)
        self.send(r, update([voice(wbs=[(0, 0, 300, 0, 1), (1, 0, 1000, 0, 0)])] + [voice(in_use=0)] * 3))
        self.frames(r, 2)   # 300 of the first, then 180 into the second
        out, _, _ = self.send(r, update([voice()] + [voice(in_use=0)] * 3))
        self.assertEqual(self.voice_out(out, 0, 0), (180, 1))

    def test_new_voice_starts_over(self):
        r = self.open()
        self.lib.fx_start(r, 1)
        self.send(r, update([voice(wbs=[(0, 0, 240, 0, 0)])] + [voice(in_use=0)] * 3))
        self.frames(r, 1)
        out, _, _ = self.send(r, update([voice(new=1)] + [voice(in_use=0)] * 3))
        self.assertEqual(self.voice_out(out, 0, 0), (0, 0))
        out, _, _ = self.send(r, update([voice(in_use=0)] * 4))   # released
        self.assertEqual(self.voice_out(out, 0, 0), (0, 0))

    def test_effect_states(self):
        r = self.open()
        out, _, _ = self.send(r, update([voice(in_use=0)] * 4, effects=[effect(new=1), effect(enabled=0)]))
        e0 = 0x40 + 0x10 * 4
        self.assertEqual((out[e0], out[e0 + 0x10]), (4, 4))   # renderer stopped: disabled
        self.lib.fx_start(r, 1)
        out, _, _ = self.send(r, update([voice(in_use=0)] * 4, effects=[effect(), effect(enabled=0)]))
        self.assertEqual((out[e0], out[e0 + 0x10]), (3, 3))   # not through a frame yet
        self.frames(r, 1)
        out, _, _ = self.send(r, update([voice(in_use=0)] * 4, effects=[effect(), effect(enabled=0)]))
        self.assertEqual((out[e0], out[e0 + 0x10]), (3, 4))

    def test_work_buffer_size_is_page_aligned_and_grows(self):
        small, big = self.lib.fx_work(param(voices=4)), self.lib.fx_work(param(voices=96, effects=8, sub=8, mixbuf=24))
        self.assertEqual((small % 0x1000, big % 0x1000), (0, 0))
        self.assertLess(small, big)


if __name__ == '__main__':
    unittest.main()
