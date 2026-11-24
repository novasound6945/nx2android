"""H.264 end of stream in the movie decoder (dm/host/hle_movie.c md_pump, md_avc_eos): every picture leaves without the end-of-stream flag, then one more output
carries the flag (with a copy of the last picture). SUPER ROBOT WARS X's movie player (CRI Sofdec2) releases the end-of-stream output unread, and waited for good for the
last picture when it rode on that output."""
from pathlib import Path
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))
import csrc  # noqa: E402

HARNESS = r'''
#include <assert.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MD_NIN 4
#define MD_NOUT 64
#define MD_INCAP (4u << 20)
#define MD_TYPE_AVC 1
#define MD_TYPE_VP8 2
#define MD_TYPE_VP9 3
#define MD_TYPE_AAC 4
#define MD_TYPE_VORBIS 5
#define MD_VPX(d) ((d)->type == MD_TYPE_VP8 || (d)->type == MD_TYPE_VP9)
#define MD_AUDIO(d) ((d)->type == MD_TYPE_VORBIS || (d)->type == MD_TYPE_AAC)
enum { MDJ_NONE, MDJ_DECODE, MDJ_TAKE, MDJ_TAIL, MDJ_CLOSE, MDJ_QUIT };
typedef struct mavc mavc;
typedef struct {
    pthread_mutex_t mu, dmu; int type, nout;
    uint8_t *in[MD_NIN]; int infree[MD_NIN], ninfree;
    struct { int used, pic; int64_t pts; uint32_t flags; uint8_t *frm; size_t cap, bytes; uint64_t seq; } out[MD_NOUT];
    uint64_t oseq; int outq[MD_NOUT], noutq;
    struct { int in, off, size; int64_t pts; uint32_t flags; } pend[MD_NIN]; int npend;
    int job; const uint8_t *jp; int jn, jslot, jgot; int64_t jpts, jpts_out; int jflush;
    mavc *avc; int avc_drain, avc_last, in_eos;
} mdec_t;
// the codec: pictures come out DEPTH packets after their input (MediaCodec's pipeline); the end of the stream brings them all out
#define DEPTH 3
static int codec_in, codec_out, queued, fed; static int64_t qpts[64];
static void take(mdec_t *d) {
    d->jgot = 0; if (!queued) return;
    uint8_t *f = d->out[d->jslot].frm; if (!f) { f = d->out[d->jslot].frm = malloc(16); d->out[d->jslot].cap = 16; }
    memset(f, 0, 16); f[0] = (uint8_t)qpts[0]; d->jpts_out = qpts[0];
    memmove(qpts, qpts + 1, --queued * sizeof *qpts); d->jgot = 1;
}
static void md_job(mdec_t *d, int job) {
    if (job == MDJ_DECODE && d->jn > 0) { codec_in++; while (codec_in - codec_out > DEPTH) qpts[queued++] = ++codec_out; }
    if (d->jflush) while (codec_out < codec_in) qpts[queued++] = ++codec_out;
    take(d);
}
static int events;
static void md_event(mdec_t *d, int which) { (void)d; events += which == 1; }
// the client: reads every queued output (as Sofdec2 does: an end-of-stream output is released without reading it)
static int read_pics, eos_outputs, eos_pic, last_read; static int64_t eos_pts;
static void client(mdec_t *d, int hold) {
    while (d->noutq) {
        int s = d->outq[0]; memmove(d->outq, d->outq + 1, --d->noutq * sizeof *d->outq);
        if (d->out[s].flags & 4) { eos_outputs++; eos_pic = d->out[s].pic; eos_pts = d->out[s].pts; assert(!d->out[s].pic || d->out[s].frm[0] == (uint8_t)d->out[s].pts); }
        else { assert(d->out[s].pic); assert(d->out[s].frm[0] == (uint8_t)d->out[s].pts); assert(d->out[s].pts == last_read + 1); last_read = (int)d->out[s].pts; read_pics++; }
        if (!hold) { d->out[s].used = 0; d->out[s].pic = 0; }
    }
}
static void send(mdec_t *d, int size, uint32_t flags) {
    d->pend[d->npend].in = 0; d->pend[d->npend].off = 0; d->pend[d->npend].size = size; d->pend[d->npend].pts = 0; d->pend[d->npend].flags = flags; d->npend++;
    if (flags & 4) d->in_eos = 1;
    d->ninfree = 0; fed++;
}
@@CUT@@
int main(int argc, char **argv) {
    assert(argc == 2); int mode = atoi(argv[1]);
    static mdec_t dd; mdec_t *d = &dd; pthread_mutex_init(&d->mu, NULL); pthread_mutex_init(&d->dmu, NULL);
    d->type = MD_TYPE_AVC; d->nout = mode == 1 ? 4 : 16; static uint8_t buf[MD_NIN][64]; for (int i = 0; i < MD_NIN; i++) d->in[i] = buf[i];
    int n = mode == 2 ? 0 : 40;
    for (int i = 0; i < n; i++) { send(d, 100, 0); md_pump(d); client(d, 0); }
    send(d, 0, 4); md_pump(d);
    if (mode == 1) {   // the client holds every slot it is given: the rest leaves as it releases them
        client(d, 1); for (int k = 0; k < d->nout; k++) d->out[k].used = 0;
        for (int r = 0; r < 8 && !eos_outputs; r++) { md_pump(d); client(d, 1); for (int k = 0; k < d->nout; k++) d->out[k].used = 0; }
    } else client(d, 0);
    printf("pictures %d eos outputs %d eos picture %d eos time %lld\n", read_pics, eos_outputs, eos_pic, (long long)eos_pts);
    assert(read_pics == n);           // every picture read without the flag (the movie's last one included)
    assert(eos_outputs == 1);         // then the end-of-stream output, once
    assert(!d->avc_drain);
    if (n) { assert(eos_pic && eos_pts == n); } else assert(!eos_pic);   // a copy of the last picture; none in a stream without pictures
    return 0;
}
'''


@unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
class MovieAvcEndOfStream(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        src = csrc.read('nxnative/dm/host/hle_movie.c')
        cut = csrc.function(src, 'static void md_avc_eos(mdec_t *d, int s) {', 'hle_movie.c') + csrc.function(src, 'static void md_pump(mdec_t *d) {', 'hle_movie.c')
        cls.tmp = tempfile.TemporaryDirectory()
        c = Path(cls.tmp.name) / 'h.c'; c.write_text(HARNESS.replace('@@CUT@@', cut), encoding='utf-8')
        cls.exe = Path(cls.tmp.name) / 'h'
        subprocess.run(['cc', '-std=gnu11', '-O1', '-Wall', '-Werror', '-Wno-unused-function', '-pthread', str(c), '-o', str(cls.exe)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()

    def run_mode(self, mode):
        r = subprocess.run([str(self.exe), str(mode)], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stdout + r.stderr)
        return r.stdout

    def test_last_picture_then_flag(self):
        self.assertIn('pictures 40 eos outputs 1 eos picture 1 eos time 40', self.run_mode(0))

    def test_client_holding_every_slot(self):
        self.assertIn('pictures 40 eos outputs 1 eos picture 1 eos time 40', self.run_mode(1))

    def test_stream_without_pictures(self):
        self.assertIn('pictures 0 eos outputs 1 eos picture 0', self.run_mode(2))


if __name__ == '__main__':
    unittest.main()
