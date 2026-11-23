"""Shader parts made in the background for draws left out (nxnative/dm/android/gfx_vk_gpl.inc gpl_parts_async, DM_VK_ASYNC_PARTS):
vk_gpl.h gpl_parts_plan decides per draw, and the replay keeps the synchronous path wherever DM_VK_ASYNC_PIPE=0 or async_pipe would not leave the draw out."""
import re, shutil, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AND = ROOT / 'nxnative/dm/android'
HARNESS = r'''
#include <stdio.h>
#include "vk_gpl.h"
int main(void) {   // every (async, need, pend): plan and parts queued, one line each
    for (int a = 0; a < 2; a++) for (unsigned need = 0; need < 4; need++) for (unsigned pend = 0; pend < 4; pend++) {
        if (pend & ~need) continue;   // (a queued part is one that is missing)
        unsigned q = 99; int p = gpl_parts_plan(a, need, pend, &q); printf("%d %u %u %d %u\n", a, need, pend, p, q); }
    return 0;
}
'''
LINK, MAKE, QUEUE, WAIT = 0, 1, 2, 3
PR, FS = 1, 2


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class PartsPlan(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as d:
            c, exe = Path(d) / 'h.c', Path(d) / 'h'
            c.write_text(HARNESS, encoding='utf-8')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-I', str(AND), str(c), '-o', str(exe)], check=True)
            out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout
        cls.plan = {tuple(map(int, l.split()[:3])): tuple(map(int, l.split()[3:])) for l in out.splitlines()}

    def test_nothing_missing_links(self):
        for a in (0, 1): self.assertEqual(self.plan[(a, 0, 0)], (LINK, 0))

    def test_sync_makes_at_the_draw(self):   # DM_VK_ASYNC_PIPE=0 / DM_VK_ASYNC_PARTS=0 / a draw that must not be left out: as before
        for need in (PR, FS, PR | FS):
            for pend in (0, need):
                self.assertEqual(self.plan[(0, need, pend)], (MAKE, 0))

    def test_async_queues_only_what_is_not_queued(self):
        self.assertEqual(self.plan[(1, PR, 0)], (QUEUE, PR))
        self.assertEqual(self.plan[(1, FS, 0)], (QUEUE, FS))
        self.assertEqual(self.plan[(1, PR | FS, 0)], (QUEUE, PR | FS))
        self.assertEqual(self.plan[(1, PR | FS, PR)], (QUEUE, FS))   # one job per part key: the queued one is not queued again
        self.assertEqual(self.plan[(1, PR | FS, FS)], (QUEUE, PR))

    def test_async_all_queued_waits(self):
        for need in (PR, FS, PR | FS): self.assertEqual(self.plan[(1, need, need)], (WAIT, 0))


class ReplayWiring(unittest.TestCase):   # the source keeps the synchronous path where it was: a slip here is grey flashes in Dark Souls Remastered (DM_VK_ASYNC_PIPE=0)
    @classmethod
    def setUpClass(cls):
        cls.pipes = (AND / 'gfx_vk_exec_pipelines.inc').read_text(encoding='utf-8')
        cls.gpl = (AND / 'gfx_vk_gpl.inc').read_text(encoding='utf-8')

    def body(self, src, head):
        i = src.index(head); return src[i:src.index('\n}\n', i)]

    def test_skip_rule_is_async_pipes(self):
        skip = self.body(self.pipes, 'static int async_skip_ok(const pkey *k) {')
        self.assertIn('!g_async_pipe', skip)
        self.assertIn('rt_frames < 3', skip)
        self.assertIn('NVN_TEXTURE_FLAGS_LINEAR_RENDER_TARGET', skip)
        self.assertIn('raster_discard', skip)
        self.assertIn('async_skip_ok(k)', self.body(self.pipes, 'static int async_pipe(const pkey *k) {'))

    def test_parts_go_async_only_with_both_switches_and_the_skip_rule(self):
        f = self.body(self.gpl, 'static int gpl_parts_async(const pkey *k) {')
        self.assertRegex(f, r'int async = g_async_parts && async_skip_ok\(k\);\s*\n\s*if \(!async\) return 0;')
        self.assertIn('bg_qfull++', f)   # a full queue makes the part at the draw

    def test_asked_before_linking(self):
        g = self.body(self.pipes, 'static VkPipeline graphics_pipeline(const pkey *k, VkPipelineLayout layout) {')
        self.assertLess(g.index('gpl_parts_async(k)'), g.index('f = gpl_link(k, layout)'))

    def test_background_parts_are_recorded_for_the_next_start(self):
        run = self.body(self.gpl, 'static void gpl_pjob_run(struct gpl_pjob *j) {')
        self.assertIn('gpl_seen_note(0, &j->pk)', run)
        self.assertIn('gpl_seen_note(1, &j->fk)', run)
        self.assertTrue(re.search(r'nvn_hm_del\(&g_gpr_pend', run) and re.search(r'nvn_hm_del\(&g_gfs_pend', run))


if __name__ == '__main__':
    unittest.main()
