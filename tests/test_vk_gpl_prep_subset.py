"""The shader preparation of a big bundle (nxnative/dm/android/gfx_vk_gpl.inc gpl_keys_load, vk_gpl.h gpl_prep_subset): more vertex + fragment shaders than
DM_VK_GPL_PREP_MAX prepare only those the device's records name (vk_pipelines.bin, vk_gpl_seen.bin), so Final Fantasy Resonance's 70,558 no longer keep every
shader's SPIR-V until the app dies; a smaller bundle is prepared whole, as before. MemAvailable under DM_MEM_FLOOR_MB stops the preparation and the warm-up."""
import re, shutil, subprocess, tempfile, unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
AND = ROOT / 'nxnative/dm/android'
HARNESS = r'''
#include <stdio.h>
#include "vk_gpl.h"
int main(void) {
    printf("def %d\n", GPL_PREP_MAX_DEF);
    unsigned n[] = {3099, 27144, 32768, 32769, 70558};
    long m[] = {GPL_PREP_MAX_DEF, 0, -1, 1000};
    for (int i = 0; i < 5; i++) for (int j = 0; j < 4; j++) printf("sub %u %ld %d\n", n[i], m[j], gpl_prep_subset(n[i], m[j]));
    uint64_t k[] = {5, 0, 3, 5, 0xffffffffffffffffull, 3, 1, 0};
    int c = gpl_keys_uniq(k, 8); printf("uniq %d", c); for (int i = 0; i < c; i++) printf(" %llx", (unsigned long long)k[i]); printf("\n");
    printf("uniq0 %d %d\n", gpl_keys_uniq(k, 0), gpl_keys_uniq(NULL, 3));
    return 0;
}
'''


@unittest.skipUnless(shutil.which('cc'), 'a host C compiler is required')
class SubsetRule(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with tempfile.TemporaryDirectory() as d:
            c, exe = Path(d) / 'h.c', Path(d) / 'h'
            c.write_text(HARNESS, encoding='utf-8')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Werror', '-I', str(AND), str(c), '-o', str(exe)], check=True)
            cls.out = subprocess.run([str(exe)], check=True, capture_output=True, text=True).stdout.splitlines()
        cls.sub = {(int(a), int(b)): int(r) for _, a, b, r in (l.split() for l in cls.out if l.startswith('sub '))}
        cls.default = int(cls.out[0].split()[1])

    def test_games_prepared_whole_stay_whole(self):   # Dark Souls: Remastered, Super Robot Wars Y: the preparation as before
        for n in (3099, 27144, 32768): self.assertEqual(self.sub[(n, self.default)], 0)

    def test_final_fantasy_resonance_is_a_subset(self):
        self.assertEqual(self.sub[(70558, self.default)], 1)
        self.assertEqual(self.sub[(32769, self.default)], 1)

    def test_zero_or_less_is_always_whole(self):
        for n in (3099, 70558):
            self.assertEqual(self.sub[(n, 0)], 0); self.assertEqual(self.sub[(n, -1)], 0)
        self.assertEqual(self.sub[(3099, 1000)], 1)

    def test_keys_sorted_once_without_zero(self):
        self.assertIn('uniq 4 1 3 5 ffffffffffffffff', self.out)
        self.assertIn('uniq0 0 0', self.out)

    def test_registry_default_matches(self):
        row = [l for l in (ROOT / 'nxnative/dm/settings.tsv').read_text(encoding='utf-8').splitlines() if l.startswith('DM_VK_GPL_PREP_MAX\t')]
        self.assertEqual(len(row), 1)
        f = row[0].split('\t'); self.assertEqual((f[1], f[2], f[4]), ('int', str(self.default), 'start'))


class Wiring(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.gpl = (AND / 'gfx_vk_gpl.inc').read_text(encoding='utf-8')

    def body(self, head):
        i = self.gpl.index(head); return self.gpl[i:self.gpl.index('\n}\n', i)]

    def test_subset_reads_the_records_and_replaces_the_key_lists(self):
        f = self.body('static void gpl_keys_load(void) {')
        self.assertIn('getenv("DM_VK_GPL_PREP_MAX")', f)
        self.assertIn('gpl_prep_subset(', f)
        self.assertLess(f.index('gpl_prep_subset('), f.index('vk_pipelines.bin'))   # (a whole preparation reads no records, as before)
        self.assertIn('fh[0] == PIPE_MAGIC && fh[1] == (uint32_t)sizeof(pkey)', f)   # the prewarm's file and header
        for skip in ('raster_discard', 'alt', 'debug_mode', 'gs_split'): self.assertIn(f'!k->{skip}', f)   # as gpl_eligible
        self.assertIn('g_gs[i]', f)   # the parts the last runs made at draws name their shaders too
        self.assertIn('g_gk[0] = v; g_gk[1] = fr;', f)

    def test_recorded_pipelines_get_their_draws_keys(self):
        self.assertIn('gpl_keys_of(k, S.sh[0], S.sh[1], S.sh[2]', self.body('static void gpl_draw_keys('))
        m = self.body('static void gpl_pipe_make(const pkey *k, uint64_t *made) {')
        self.assertIn('gpl_keys_of(k, vs, fs, gs', m)
        self.assertIn('vs->xfb', m)

    def test_counts_hold_the_pipeline_block(self):
        self.assertRegex(self.gpl, r'static int g_gtotal\(void\) \{ return g_gsn \+ g_gpn \+ ')
        self.assertRegex(self.gpl, r'static int g_gint0\(void\) \{ return g_gsn \+ g_gpn \+ ')
        p = self.body('static void gpl_part_i(int i) {')
        self.assertLess(p.index('i -= g_gsn;'), p.index('gpl_pipe_make(&g_gp[i]'))
        self.assertLess(p.index('gpl_pipe_make(&g_gp[i]'), p.index('i -= g_gpn;'))

    def test_memory_floor_stops_preparation_and_warm_up(self):
        self.assertIn('if (!gpl_mem_room()) break;', self.body('static void *gpl_prep_thread(void *arg) {'))
        r = self.body('static int gpl_mem_room(void) {')
        self.assertIn('vk_mem_floor_mb()', r); self.assertIn('vk_mem_avail_mb()', r)
        prep = self.body('int vk_gpl_prepare(')
        self.assertEqual(prep.count('|| atomic_load(&g_gstop)) break;'), 2)   # both progress loops end when it stops
        self.assertLess(prep.rindex('atomic_load(&g_gstop)) break;'), prep.rindex('want.complete = 1;'))   # and the mark is still written


if __name__ == '__main__':
    unittest.main()
