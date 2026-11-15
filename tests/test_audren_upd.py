"""The audio renderer's update protocol, served by the host for SDK 1.x (nxnative/dm/host/hle_audren_upd.c): RequestUpdate's output layout, memory pool and
effect states, wave buffers taken at the rate playback takes them (pitch, sample rate, loops, end of stream, stop), and the "DSP" that plays them: PCM / DSP-ADPCM
decoding, resampling, biquad filters, volume ramps, the routing through submixes, effects and sinks to the stereo int16 output."""
import ctypes as C
import math
import os
from pathlib import Path
import random
import shutil
import struct
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[1]
REV1, REV2 = 0x31564552, 0x32564552   # "REV2", what SDK 1.7.1's client sends
STARTED, STOPPED, PAUSED = 0, 1, 2
PCM16, ADPCM = 2, 6
UNUSED_MIX = 0x7fffffff


def param(rate=48000, count=240, mixbuf=6, sub=2, voices=4, sinks=1, effects=2, perf=0, rev=REV2):
    b = bytearray(0x34)
    struct.pack_into('<8I', b, 0, rate, count, mixbuf, sub, voices, sinks, effects, perf)
    struct.pack_into('<I', b, 0x30, rev)
    return bytes(b)


def voice(state=STARTED, new=0, rate=48000, pitch=1.0, wbs=(), in_use=1, fmt=PCM16, chans=1, volume=1.0, mix=0, chres=(), coef=0, biquads=()):
    """wbs: (slot, start, end, loop, eos[, address, size[, context]]); every other slot reads as sent in an earlier update. Without an address the buffer points at
    0x10000 + slot × 0x1000, which no memory pool covers. biquads: (filter, (b0, b1, b2, a1, a2) in Q14)."""
    b = bytearray(0x170)
    b[8], b[9], b[0xa], b[0xb] = new, in_use, state, fmt
    struct.pack_into('<I', b, 0xc, rate)
    struct.pack_into('<Iff', b, 0x18, chans, pitch, volume)
    for f, k in biquads:
        b[0x24 + 12 * f] = 1
        struct.pack_into('<5h', b, 0x26 + 12 * f, *k)
    struct.pack_into('<I', b, 0x3c, len(wbs))
    struct.pack_into('<QQ', b, 0x48, coef, 0x20 if coef else 0)
    struct.pack_into('<i', b, 0x58, mix)
    struct.pack_into('<6i', b, 0x140, *(list(chres) + [0] * (6 - len(chres))))
    for k in range(4):
        b[0x60 + 0x38 * k + 0x1a] = 1
    for wb in wbs:
        slot, start, end, loop, eos = wb[:5]
        addr, size = wb[5:7] if len(wb) > 5 else (0x10000 + slot * 0x1000, end * 2)
        ctx = wb[7] if len(wb) > 7 else 0
        o = 0x60 + 0x38 * slot
        struct.pack_into('<QQII', b, o, addr, size, start, end)
        b[o + 0x18], b[o + 0x19], b[o + 0x1a] = loop, eos, 0
        struct.pack_into('<QQ', b, o + 0x20, ctx, 6 if ctx else 0)
    return bytes(b)


def chres_in(id_, vols):
    """A voice channel resource: its mix volumes onto the destination's buffers ({buffer: volume}), in use."""
    b = bytearray(0x70)
    struct.pack_into('<I', b, 0, id_)
    for j, g in vols.items():
        struct.pack_into('<f', b, 4 + 4 * j, g)
    b[0x64] = 1
    return bytes(b)


def mix_in(bufs=2, volume=1.0, matrix=None, dest=UNUSED_MIX, used=1):
    """A mix: its buffer count, volume, 24 × 24 matrix onto the destination ({(source buffer, destination buffer): volume}) and destination."""
    b = bytearray(0x930)
    struct.pack_into('<fII', b, 0, volume, 0, bufs)
    b[0xc] = used
    for (i, j), g in (matrix or {}).items():
        struct.pack_into('<f', b, 0x24 + 4 * (i * 24 + j), g)
    struct.pack_into('<i', b, 0x924, dest)
    return bytes(b)


def device_sink(inputs, used=1):
    b = bytearray(0x140)
    b[0], b[1] = 1, used
    struct.pack_into('<I', b, 0x120, len(inputs))
    b[0x124:0x124 + len(inputs)] = bytes(inputs)
    return bytes(b)


def circular_sink(addr, size, inputs, used=1):
    b = bytearray(0x140)
    b[0], b[1] = 2, used
    struct.pack_into('<QII', b, 0x20, addr, size, len(inputs))
    b[0x40:0x40 + len(inputs)] = bytes(inputs)
    return bytes(b)


def buffer_mixer(mix, pairs, order=0, enabled=1):
    """A buffer mixer effect on a mix: (input buffer, output buffer, volume) pairs."""
    b = bytearray(0xc0)
    b[0], b[2] = 1, enabled
    struct.pack_into('<i', b, 4, mix)
    struct.pack_into('<I', b, 0x18, order)
    for k, (i, o, g) in enumerate(pairs):
        b[0x20 + k], b[0x38 + k] = i, o
        struct.pack_into('<f', b, 0x50 + 4 * k, g)
    struct.pack_into('<I', b, 0xb0, len(pairs))
    return bytes(b)


def adpcm_decode(data, coefs, nsamples, ps=0, h0=0, h1=0):
    """The DSP-ADPCM decoder, written out from the format (8-byte frames: predictor / scale, 14 nibbles high first)."""
    out = []
    for s in range(nsamples):
        f, j = divmod(s, 14)
        if j == 0:
            ps = data[f * 8]
        byte = data[f * 8 + 1 + j // 2]
        nib = (byte >> 4) if j % 2 == 0 else (byte & 15)
        nib = nib - 16 if nib >= 8 else nib
        scale, ci = ps & 15, (ps >> 4) & 7
        v = (nib * (1 << scale) * 2048 + 1024 + coefs[2 * ci] * h0 + coefs[2 * ci + 1] * h1) >> 11
        v = max(-32768, min(32767, v))
        h1, h0 = h0, v
        out.append(v)
    return out


def rnd(v):
    return int(v + 0.5) if v >= 0 else -int(-v + 0.5)


def effect(type_=1, new=0, enabled=1):
    b = bytearray(0xc0)
    b[0], b[1], b[2] = type_, new, enabled
    return bytes(b)


def pool(state, addr=0x20000, size=0x1000):
    return struct.pack('<QQI12x', addr, size, state)


def update(voices, pools=(), effects=(), nsinks=1, nmix=3, nchres=4, rev=REV2, voice_size=0x170, chres=None, mixes=None, sinks=None, flags=0):
    secs = [struct.pack('<IIQ', rev, 0, flags), b''.join(pools), b''.join(chres) if chres is not None else bytes(0x70 * nchres), b''.join(voices), b''.join(effects), b'',
            b''.join(mixes) if mixes is not None else bytes(0x930 * nmix), b''.join(sinks) if sinks is not None else bytes(0x140 * nsinks), bytes(0x10)]
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
        cls.lib.fx_render.restype = C.c_uint32
        cls.lib.fx_render.argtypes = [C.c_void_p, C.POINTER(C.c_int16), C.POINTER(C.c_uint32)]
        cls.lib.fx_out_max.restype = C.c_uint32
        cls.out_max = cls.lib.fx_out_max()
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

    def render(self, r, n):
        """n frames of output: the left and right samples, and the frames' stats summed (peak: the largest)."""
        buf, st = (C.c_int16 * (2 * self.out_max))(), (C.c_uint32 * 7)()
        left, right, tot = [], [], [0] * 7
        for _ in range(n):
            m = self.lib.fx_render(r, buf, st)
            left += buf[0:2 * m:2]
            right += buf[1:2 * m:2]
            tot = [max(tot[0], st[0]), tot[1] + st[1], tot[2] + st[2], tot[3] + st[3], tot[4] | st[4], tot[5] | st[5], tot[6] + st[6]]
        return left, right, dict(zip(('peak', 'clipped', 'unmapped', 'mixed', 'warn', 'fx_skipped', 'finished'), tot))

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

    # ---- the "DSP": decoding, resampling, filters, routing, output ----

    def started(self, **kw):
        r = self.open(**kw)
        self.lib.fx_start(r, 1)
        return r, Arena()

    def one_voice(self, r, m, v, chres, mixes, sinks, effects=(), attach=True, flags=0):
        return self.send(r, update([v] + [voice(in_use=0)] * 3, pools=[m.pool(4 if attach else 0)], chres=chres, mixes=mixes, sinks=sinks, effects=effects, flags=flags))

    def test_pcm16_plays_through_at_the_renderers_rate(self):
        r, m = self.started()
        x = [random.Random(1).randint(-10000, 10000) for _ in range(480)]
        a = m.put(struct.pack('<480h', *x))
        self.one_voice(r, m, voice(wbs=[(0, 0, 480, 0, 0, a, 960)], volume=0.5, chres=(0,)), [chres_in(0, {0: 1.0, 1: 0.5})], [mix_in(2)], [device_sink([0, 1])])
        left, right, st = self.render(r, 3)
        z = [0] * 3 + x + [0] * 237   # the resampler's three samples of delay
        self.assertEqual(left, [rnd(s * 0.5) for s in z])
        self.assertEqual(right, [rnd(s * 0.25) for s in z])
        self.assertEqual((st['unmapped'], st['clipped'], st['mixed'], st['finished']), (0, 0, 3, 1))

    def test_channel_resources_are_taken_by_their_place_in_the_section(self):
        # as Disgaea 5 sends them: entry 0 in use with its volumes, the entries after it all zero (their ID field reads 0 too)
        r, m = self.started()
        a = m.put(struct.pack('<480h', *([1000] * 480)))
        self.one_voice(r, m, voice(wbs=[(0, 0, 480, 0, 0, a, 960)], chres=(0,)), [chres_in(0, {0: 0.5, 1: 0.5})] + [bytes(0x70)] * 3, [mix_in(2)], [device_sink([0, 1])])
        left, right, _ = self.render(r, 1)
        self.assertEqual((left[3:], right[3:]), ([500] * 237, [500] * 237))

    def test_pcm_is_read_by_its_sample_offsets_not_its_size_field(self):
        # Disgaea 5's music: stereo buffers of [0, 30720) frames, 0x1e000 bytes apart, whose size field says 0xf000
        r, m = self.started()
        x = [random.Random(8).randint(-9000, 9000) for _ in range(960)]
        a = m.put(struct.pack('<960h', *x))
        self.one_voice(r, m, voice(chans=2, wbs=[(0, 0, 480, 0, 0, a, 960)], chres=(0, 1)), [chres_in(0, {0: 1.0}), chres_in(1, {1: 1.0})], [mix_in(2)], [device_sink([0, 1])])
        left, right, st = self.render(r, 2)
        self.assertEqual((left, right), ([0] * 3 + x[0::2][:477], [0] * 3 + x[1::2][:477]))
        self.assertEqual(st['finished'], 1)

    def test_nothing_is_read_outside_the_attached_memory_pools(self):
        r, m = self.started()
        a = m.put(struct.pack('<480h', *([1000] * 480)))
        self.one_voice(r, m, voice(wbs=[(0, 0, 480, 0, 0, a, 960)], chres=(0,)), [chres_in(0, {0: 1.0})], [mix_in(2)], [device_sink([0, 1])], attach=False)
        left, right, st = self.render(r, 2)
        self.assertEqual(set(left + right), {0})
        self.assertEqual(st['finished'], 1)   # taken at the playback rate all the same
        self.assertGreater(st['unmapped'], 0)
        # force mapping (behaviour flag bit 0): the client vouches for addresses outside its pools
        r, m = self.started()
        a = m.put(struct.pack('<480h', *([1000] * 480)))
        self.one_voice(r, m, voice(wbs=[(0, 0, 480, 0, 0, a, 960)], chres=(0,)), [chres_in(0, {0: 1.0})], [mix_in(2)], [device_sink([0, 1])], attach=False, flags=1)
        left, _, st = self.render(r, 1)
        self.assertEqual((left[3:], st['unmapped']), ([1000] * 237, 0))

    def test_a_detached_pool_goes_silent(self):
        r, m = self.started()
        a = m.put(struct.pack('<4800h', *([1000] * 4800)))
        v = voice(wbs=[(0, 0, 4800, 0, 0, a, 9600)], chres=(0,))
        args = [chres_in(0, {0: 1.0})], [mix_in(2)], [device_sink([0, 1])]
        self.one_voice(r, m, v, *args)
        left, _, _ = self.render(r, 1)
        self.assertEqual(left[3:], [1000] * 237)
        self.send(r, update([voice(chres=(0,))] + [voice(in_use=0)] * 3, pools=[m.pool(2)], chres=args[0], mixes=args[1], sinks=args[2]))
        left, _, st = self.render(r, 1)
        self.assertEqual(left[3:], [0] * 237)   # (the first three come from the last frame's samples)
        self.assertGreater(st['unmapped'], 0)

    def test_adpcm_frames_decode(self):
        rng = random.Random(2)
        coefs = [v for _ in range(8) for v in (rng.randint(0, 4096), rng.randint(-2048, 0))]
        data = b''.join(bytes([rng.randint(0, 7) << 4 | rng.randint(0, 11)] + [rng.randint(0, 255) for _ in range(7)]) for _ in range(20))
        for rev, ctx, start in ((REV2, (0x17, 1000, -500), (1000, -500)), (REV2, None, (0, 0)), (REV1, (0x17, 1000, -500), (0, 0))):
            with self.subTest(rev=hex(rev), ctx=ctx):
                r, m = self.started(rev=rev)
                ca, da = m.put(struct.pack('<16h', *coefs)), m.put(data)
                xa = m.put(struct.pack('<3h', *ctx)) if ctx else 0
                v = voice(fmt=ADPCM, coef=ca, wbs=[(0, 0, 280, 0, 0, da, len(data), xa)], chres=(0,))
                self.one_voice(r, m, v, [chres_in(0, {0: 1.0})], [mix_in(1)], [device_sink([0])])
                left, right, st = self.render(r, 2)
                want = [0] * 3 + adpcm_decode(data, coefs, 280, 0, *start) + [0] * 197
                self.assertEqual((left, right), (want, want))   # one input: the same on both sides
                self.assertEqual(st['unmapped'], 0)

    def test_adpcm_takes_the_context_again_each_time_its_loop_comes_back(self):
        rng = random.Random(3)
        coefs = [2048, 0] * 8   # each sample is the last one plus the nibble: the history shows
        data = b''.join(bytes([0x02] + [rng.randint(0, 255) for _ in range(7)]) for _ in range(5))
        r, m = self.started()
        ca, da, xa = m.put(struct.pack('<16h', *coefs)), m.put(data), m.put(struct.pack('<3h', 2, 300, 0))
        self.one_voice(r, m, voice(fmt=ADPCM, coef=ca, wbs=[(0, 0, 70, 1, 0, da, len(data), xa)], chres=(0,)), [chres_in(0, {0: 1.0})], [mix_in(1)], [device_sink([0])])
        left, _, _ = self.render(r, 2)
        loop = adpcm_decode(data, coefs, 70, 0, 300, 0)
        self.assertEqual(left[3:480], (loop * 7)[:477])

    def test_stereo_pcm_takes_each_channel_from_its_own_index(self):
        r, m = self.started()
        rng = random.Random(4)
        lr = [rng.randint(-8000, 8000) for _ in range(960)]
        a = m.put(struct.pack('<960h', *lr))
        self.one_voice(r, m, voice(chans=2, wbs=[(0, 0, 480, 0, 0, a, 1920)], chres=(0, 1)), [chres_in(0, {0: 1.0}), chres_in(1, {1: 1.0})], [mix_in(2)], [device_sink([0, 1])])
        left, right, _ = self.render(r, 2)
        self.assertEqual(left, [0] * 3 + lr[0::2][:477])
        self.assertEqual(right, [0] * 3 + lr[1::2][:477])

    def test_a_looping_buffer_repeats(self):
        r, m = self.started()
        x = [(k - 50) * 100 for k in range(100)]
        a = m.put(struct.pack('<100h', *x))
        self.one_voice(r, m, voice(wbs=[(0, 0, 100, 1, 0, a, 200)], chres=(0,)), [chres_in(0, {0: 1.0})], [mix_in(2)], [device_sink([0, 1])])
        left, _, _ = self.render(r, 4)
        self.assertEqual(left[3:], (x * 10)[:957])

    def test_resampling_keeps_pitch_and_level(self):
        for rate, pitch, period, freq in ((44100, 1.0, 100, 441.0), (48000, 1.5, 96, 750.0), (48000, 0.5, 96, 250.0), (32000, 1.0, 64, 500.0), (22050, 1.0, 49, 22050 / 49)):
            with self.subTest(rate=rate, pitch=pitch):
                r, m = self.started()
                n = int(48000 * rate / 48000 * pitch) + 2000
                x = [round(10000 * math.sin(2 * math.pi * k / period)) for k in range(n)]
                a = m.put(struct.pack(f'<{n}h', *x))
                self.one_voice(r, m, voice(rate=rate, pitch=pitch, wbs=[(0, 0, n, 0, 0, a, 2 * n)], chres=(0,)), [chres_in(0, {0: 1.0})], [mix_in(2)], [device_sink([0, 1])])
                left, _, _ = self.render(r, 200)
                y = left[960:]
                up = [i for i in range(1, len(y)) if y[i - 1] < 0 <= y[i]]
                got = (len(up) - 1) * 48000 / (up[-1] - up[0])
                self.assertAlmostEqual(got, freq, delta=0.2)
                self.assertTrue(9800 <= max(y) <= 10100 and -10100 <= min(y) <= -9800, (min(y), max(y)))

    def test_the_resampler_interpolates_with_four_points(self):
        # a parabola at half the renderer's rate: four-point (cubic) interpolation is exact on it between the samples, two-point (linear) is 1 off at every half position
        r, m = self.started()
        x = [4 * (n - 80) ** 2 for n in range(161)]
        a = m.put(struct.pack('<161h', *x))
        self.one_voice(r, m, voice(rate=24000, wbs=[(0, 0, 161, 0, 0, a, 322)], chres=(0,)), [chres_in(0, {0: 1.0})], [mix_in(2)], [device_sink([0, 1])])
        left, _, _ = self.render(r, 2)
        self.assertEqual(left[8:321], [rnd(4 * (i / 2 - 3 - 80) ** 2) for i in range(8, 321)])   # output i: source sample i / 2, three samples late

    def test_voice_submix_final_mix_sink_routing(self):
        # voice → mix 2 → mix 1 → the final mix (deepest first), a buffer mixer on the final mix, then its volume
        r, m = self.started()
        x = [16 * random.Random(5).randint(-100, 100) for _ in range(480)]
        a = m.put(struct.pack('<480h', *x))
        mixes = [mix_in(2, volume=2.0), mix_in(2, 1.0, {(0, 0): 1.0, (1, 0): 1.0, (1, 1): 0.5}, dest=0), mix_in(2, 0.5, {(0, 0): 1.0, (1, 1): 1.0}, dest=1)]
        self.one_voice(r, m, voice(wbs=[(0, 0, 480, 0, 0, a, 960)], mix=2, chres=(0,)), [chres_in(0, {0: 1.0, 1: 0.5})], mixes, [device_sink([0, 1])],
                       effects=[buffer_mixer(0, [(0, 1, 0.25)])])
        left, right, st = self.render(r, 2)
        z = [0] * 3 + x[:477]
        self.assertEqual(left, [rnd(1.5 * s) for s in z])      # (0.5 · 1 + 0.5 · 0.5) · 2
        self.assertEqual(right, [rnd(0.625 * s) for s in z])   # (0.5 · 0.5 · 0.5 + 0.25 · 0.75) · 2
        self.assertEqual(st['fx_skipped'], 0)

    def test_effects_that_are_not_processed_pass_through(self):
        r, m = self.started()
        a = m.put(struct.pack('<480h', *([1000] * 480)))
        aux = bytearray(buffer_mixer(0, [(0, 1, 1.0)]))
        aux[0] = 2   # an aux send: the game's own processing, which the host does not do
        self.one_voice(r, m, voice(wbs=[(0, 0, 480, 0, 0, a, 960)], chres=(0,)), [chres_in(0, {0: 1.0, 1: 1.0})], [mix_in(2)], [device_sink([0, 1])], effects=[bytes(aux)])
        left, right, st = self.render(r, 1)
        self.assertEqual((left[3:], right[3:], st['fx_skipped']), ([1000] * 237, [1000] * 237, 1 << 2))

    def test_biquad_filters(self):
        rng = random.Random(6)
        x = [2 * rng.randint(-5000, 5000) for _ in range(480)]
        for k, ref in (((8192, 0, 0, 0, 0), lambda z: [0.5 * s for s in z]), ((16384, 0, 0, 8192, 0), None)):
            with self.subTest(k=k):
                r, m = self.started()
                a = m.put(struct.pack('<480h', *x))
                self.one_voice(r, m, voice(wbs=[(0, 0, 480, 0, 0, a, 960)], chres=(0,), biquads=[(0, k)]), [chres_in(0, {0: 1.0})], [mix_in(2)], [device_sink([0, 1])])
                left, _, _ = self.render(r, 2)
                z = [0] * 3 + x[:477]
                if ref:
                    self.assertEqual(left, [rnd(s) for s in ref(z)])
                else:   # y[n] = x[n] + 0.5 y[n - 1]
                    y, want = 0.0, []
                    for s in z:
                        y = s + 0.5 * y
                        want.append(max(-32768, min(32767, rnd(y))))
                    self.assertLessEqual(max(abs(p - q) for p, q in zip(left, want)), 1)

    def test_six_channels_down_to_stereo(self):
        for buf, lr in ((0, (1.0, 0.0)), (1, (0.0, 1.0)), (2, (0.707, 0.707)), (3, (0.251, 0.251)), (4, (0.707, 0.0)), (5, (0.0, 0.707))):
            with self.subTest(buf=buf):
                r, m = self.started()
                a = m.put(struct.pack('<480h', *([10000] * 480)))
                self.one_voice(r, m, voice(wbs=[(0, 0, 480, 0, 0, a, 960)], chres=(0,)), [chres_in(0, {buf: 1.0})], [mix_in(6)], [device_sink([0, 1, 2, 3, 4, 5])])
                left, right, _ = self.render(r, 1)
                self.assertEqual((left[100], right[100]), (rnd(10000 * lr[0]), rnd(10000 * lr[1])))

    def test_output_is_clamped_and_counted(self):
        r, m = self.started()
        a = m.put(struct.pack('<480h', *([20000] * 240 + [-20000] * 240)))
        self.one_voice(r, m, voice(wbs=[(0, 0, 480, 0, 0, a, 960)], volume=4.0, chres=(0,)), [chres_in(0, {0: 1.0, 1: 1.0})], [mix_in(2)], [device_sink([0, 1])])
        left, right, st = self.render(r, 2)
        self.assertEqual((left[100], left[400], right[100]), (32767, -32768, 32767))
        self.assertEqual(st['clipped'], 2 * 477)
        self.assertGreaterEqual(st['peak'], 65535)

    def test_volume_ramps_over_a_frame(self):
        r, m = self.started()
        a = m.put(struct.pack('<4800h', *([10000] * 4800)))
        args = [chres_in(0, {0: 1.0})], [mix_in(2)], [device_sink([0, 1])]
        self.one_voice(r, m, voice(wbs=[(0, 0, 4800, 0, 0, a, 9600)], chres=(0,)), *args)
        self.render(r, 1)
        self.send(r, update([voice(chres=(0,), volume=0.0)] + [voice(in_use=0)] * 3, pools=[m.pool(5)], chres=args[0], mixes=args[1], sinks=args[2]))
        left, _, _ = self.render(r, 2)
        self.assertLessEqual(max(abs(left[i] - 10000 * (1 - i / 240)) for i in range(240)), 1)
        self.assertEqual(left[240:], [0] * 240)

    def test_a_32khz_renderer_comes_out_at_48khz(self):
        r, m = self.started(rate=32000, count=160)
        a = m.put(struct.pack('<6400h', *([1000] * 6400)))
        self.one_voice(r, m, voice(rate=32000, wbs=[(0, 0, 6400, 0, 0, a, 12800)], chres=(0,)), [chres_in(0, {0: 1.0, 1: 1.0})], [mix_in(2)], [device_sink([0, 1])])
        left, right, st = self.render(r, 10)
        self.assertEqual(len(left), 2400)   # 160 samples a frame at 32 kHz, 240 at 48 kHz
        self.assertLessEqual(max(abs(s - 1000) for s in left[240:] + right[240:]), 1)
        self.assertEqual(st['finished'], 0)

    def test_a_circular_buffer_sink_gets_the_final_mix(self):
        r, m = self.started(sinks=2)
        rng = random.Random(7)
        x = [rng.randint(-9000, 9000) for _ in range(960)]
        a, cb = m.put(struct.pack('<960h', *x)), m.put(bytes(4 * 960))
        sinks = [device_sink([0, 1]), circular_sink(cb, 4 * 960, [0, 1])]
        self.one_voice(r, m, voice(wbs=[(0, 0, 960, 0, 0, a, 1920)], chres=(0,)), [chres_in(0, {0: 1.0, 1: 0.5})], [mix_in(2)], sinks)
        left, right, st = self.render(r, 3)
        raw = C.string_at(cb, 3 * 960)
        for f in range(3):   # each frame: the left input's 240 samples, then the right's
            self.assertEqual(list(struct.unpack_from('<240h', raw, 960 * f)), left[240 * f:240 * (f + 1)])
            self.assertEqual(list(struct.unpack_from('<240h', raw, 960 * f + 480)), right[240 * f:240 * (f + 1)])
        out, _, _ = self.send(r, update([voice(chres=(0,))] + [voice(in_use=0)] * 3, pools=[m.pool(5)], chres=[chres_in(0, {0: 1.0, 1: 0.5})], mixes=[mix_in(2)], sinks=sinks))
        self.assertEqual(struct.unpack_from('<I', out, 0x40 + 0x10 + 0x40 + 0x20)[0], 960)   # the last written offset: two frames back, as the service reports it


class Arena:
    """Guest memory for the tests: one buffer, which a single memory pool covers."""
    def __init__(self, size=1 << 20):
        self.buf = C.create_string_buffer(size)
        self.base, self.size, self.n = C.addressof(self.buf), size, 0

    def put(self, data):
        self.n = (self.n + 15) & ~15
        a = self.base + self.n
        C.memmove(a, data, len(data))
        self.n += len(data)
        return a

    def pool(self, state):
        return pool(state, self.base, self.size)


if __name__ == '__main__':
    unittest.main()
