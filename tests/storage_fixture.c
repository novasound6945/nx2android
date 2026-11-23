// The data root's choice and move (dm/android/storage.h) on the computer: tests/test_storage.py builds and runs it with a scratch folder. Usage: storage_fixture <dir>
#include "storage.h"
#include <assert.h>

static const char *B;   // the scratch folder
static void put(const char *rel, const char *text) {
    char p[1200]; snprintf(p, sizeof p, "%s/%s", B, rel);
    for (char *q = p + strlen(B) + 1; (q = strchr(q, '/')); q++) { *q = 0; mkdir(p, 0775); *q = '/'; }
    FILE *f = fopen(p, "wb"); assert(f); fputs(text, f); fclose(f);
}
static int has(const char *rel) { char p[1200]; struct stat st; snprintf(p, sizeof p, "%s/%s", B, rel); return !stat(p, &st); }
static char *get(const char *rel) { char p[1200]; snprintf(p, sizeof p, "%s/%s", B, rel); return dm_st_read(p, 1 << 16); }
static const char *at(const char *rel) { static char p[4][1200]; static int k; k = (k + 1) & 3; snprintf(p[k], sizeof p[k], "%s/%s", B, rel); return p[k]; }
static volatile int g_cancel; static uint64_t g_cancel_at;
static void note(void *u, uint64_t done, uint64_t total) { (void)u; (void)total; if (g_cancel_at && done >= g_cancel_at) g_cancel = 1; }

int main(int argc, char **argv) {
    assert(argc == 2); B = argv[1];
    // ---- the choice's file ----
    char v[600];
    assert(dm_st_value("root=/storage/AB-CD/Android/data/a.b/files\n", "root", v, sizeof v) && !strcmp(v, "/storage/AB-CD/Android/data/a.b/files"));
    assert(dm_st_value("# x\r\nfrom=/a\r\nto=/b \r\n", "to", v, sizeof v) && !strcmp(v, "/b"));
    assert(!dm_st_value("rooted=/x\n", "root", v, sizeof v) && !dm_st_value("", "root", v, sizeof v) && !dm_st_value("root=/0123456789", "root", v, 5));
    // ---- the choice at the start ----
    dm_vol vs[3]; memset(vs, 0, sizeof vs);
    snprintf(vs[0].path, sizeof vs[0].path, "/storage/emulated/0/Android/data/a.b/files"); vs[0].mounted = 1; vs[0].free = 8ull << 30;
    snprintf(vs[1].path, sizeof vs[1].path, "/storage/1234-ABCD/Android/data/a.b/files"); vs[1].mounted = 1; vs[1].removable = 1; vs[1].free = 300ull << 30;
    int none[3] = {0, 0, 0}, in0[3] = {1, 0, 0}, in1[3] = {0, 1, 0};
    dm_st_pick k;
    k = dm_st_choose(vs, 2, "", none, 0); assert(k.vol == 1 && k.save && !k.missing);                // a first start: the most room
    k = dm_st_choose(vs, 1, "", none, 0); assert(k.vol == 0 && k.save);                              // no card
    k = dm_st_choose(vs, 2, "", in0, 0); assert(k.vol == 0 && k.save);                               // an install from before the choice: its data stays
    k = dm_st_choose(vs, 2, "", in1, 0); assert(k.vol == 1 && k.save);
    vs[1].free = 1ull << 30; k = dm_st_choose(vs, 2, "", none, 0); assert(k.vol == 0); vs[1].free = 300ull << 30;
    k = dm_st_choose(vs, 2, vs[1].path, in1, 0); assert(k.vol == 1 && !k.save && !k.missing);         // the choice kept
    k = dm_st_choose(vs, 2, vs[0].path, in1, 0); assert(k.vol == 0 && !k.save);                       // (internal chosen: a card's leftovers do not matter)
    k = dm_st_choose(vs, 2, vs[1].path, in1, 1); assert(k.vol == 0 && k.adopt && k.save);             // porter put a whole pack into internal storage since
    vs[1].mounted = 0; k = dm_st_choose(vs, 2, vs[1].path, in1, 0); assert(k.missing && k.vol == 0 && !k.save);   // the card taken out
    k = dm_st_choose(vs, 1, vs[1].path, none, 0); assert(k.missing);
    vs[1].mounted = 1; vs[2] = vs[1]; snprintf(vs[2].path, sizeof vs[2].path, "/storage/USB1/Android/data/a.b/files"); vs[2].free = 400ull << 30;
    k = dm_st_choose(vs, 3, "", none, 0); assert(k.vol == 2);
    // ---- kept names ----
    assert(dm_st_kept("log.txt") && dm_st_kept("saves") && dm_st_kept("sync.txt") && dm_st_kept("ctl.txt") && dm_st_kept(DM_ST_POINTER) && !dm_st_kept("RomFS") && !dm_st_kept("env.txt") && !dm_st_kept("settings.txt"));
    // ---- a move: internal (int) to the card (sd) ----
    put("int/env.txt", "DM_X=1\n"); put("int/settings.txt", "quick=1\n"); put("int/mods/main.fixups.tsv", "# REWRITE_ABI 3\n"); put("int/RomFS/a/b.bin", "0123456789");
    put("int/RomFS/c.bin", ""); put("int/nx2a-data.json", "{}"); put("int/log.txt", "the log"); put("int/saves/sync-state.txt", "s"); put("int/sync.txt", "token"); put("int/" DM_ST_POINTER, "root=x\n");
    put("sd/RomFS/stale.bin", "old");       // a leftover of a move cut short: gone before the copy
    put("sd/aoc-keep/x", "card's own");     // not a name the move has: kept
    { char p[1200]; snprintf(p, sizeof p, "%s/int/RomFS/a/b.bin", B); struct timespec t[2] = {{1600000000, 0}, {1600000000, 0}}; utimensat(AT_FDCWD, p, t, 0); }
    assert(dm_st_has_data(at("int")) && !dm_st_has_data(at("int/saves")));
    uint64_t bytes = 0; long files = 0; dm_st_size(at("int"), 1, &bytes, &files);
    assert(files == 6 && bytes == strlen("DM_X=1\n") + strlen("quick=1\n") + strlen("# REWRITE_ABI 3\n") + 10 + 0 + 2);   // (the log, saves/, sync.txt, the pointer: not counted)
    dm_mv m; memset(&m, 0, sizeof m); m.total = bytes;
    assert(dm_st_move_copy(at("int"), at("sd"), &m) == 0);
    char *t = get("sd/RomFS/a/b.bin"); assert(t && !strcmp(t, "0123456789")); free(t);
    assert(has("sd/RomFS/c.bin") && has("sd/env.txt") && has("sd/mods/main.fixups.tsv") && has("sd/nx2a-data.json") && has("sd/settings.txt"));
    assert(!has("sd/RomFS/stale.bin") && has("sd/aoc-keep/x"));
    assert(!has("sd/log.txt") && !has("sd/saves") && !has("sd/sync.txt") && !has("sd/" DM_ST_POINTER));
    { struct stat st; assert(!stat(at("sd/RomFS/a/b.bin"), &st) && st.st_mtime == 1600000000); }   // the times kept
    assert(m.done == bytes);
    dm_st_remove_moving(at("int"), at("int"));   // the old copy: only the kept names stay
    assert(!has("int/RomFS") && !has("int/mods") && !has("int/env.txt") && !has("int/nx2a-data.json"));
    assert(has("int/log.txt") && has("int/saves/sync-state.txt") && has("int/sync.txt") && has("int/" DM_ST_POINTER));
    // ---- a move cancelled part-way: the copy removed, the source whole, the target's own files kept ----
    char big[300000]; memset(big, 'x', sizeof big - 1); big[sizeof big - 1] = 0;
    for (int i = 0; i < 40; i++) { char r[64]; snprintf(r, sizeof r, "sd/RomFS/big%02d.bin", i); put(r, big); }
    put("int2/log.txt", "the log");
    memset(&m, 0, sizeof m); g_cancel = 0; g_cancel_at = 5u << 20; m.cancel = &g_cancel; m.note = note;
    assert(dm_st_move_copy(at("sd"), at("int2"), &m) == -ECANCELED);
    assert(!has("int2/RomFS") && !has("int2/env.txt") && has("int2/log.txt"));
    assert(has("sd/RomFS/big39.bin") && has("sd/env.txt"));
    // ---- the start after a move that did not end ----
    char mark[1300];
    // not committed (the choice names the old root): the partial copy goes, the source stays
    put("int2/RomFS/part.bin", "half"); put("int2/env.txt", "x");
    snprintf(mark, sizeof mark, "from=%s\nto=%s\n", at("sd"), at("int2")); put("internal/" DM_ST_MOVE, mark);
    assert(dm_st_recover(at("internal"), at("sd")) == 1);
    assert(!has("int2/RomFS") && !has("int2/env.txt") && has("int2/log.txt") && has("sd/RomFS/big00.bin") && !has("internal/" DM_ST_MOVE));
    // committed (the choice names the new root): what is left of the old copy goes, its kept names stay
    put("int/vk_pipeline_cache.bin", "written after the commit"); put("int/RomFS/x", "y");
    snprintf(mark, sizeof mark, "from=%s\nto=%s\n", at("int"), at("sd")); put("internal/" DM_ST_MOVE, mark);
    assert(dm_st_recover(at("internal"), at("sd")) == 1);
    assert(!has("int/vk_pipeline_cache.bin") && !has("int/RomFS") && has("int/log.txt") && has("int/saves/sync-state.txt") && !has("internal/" DM_ST_MOVE));
    // committed, the old root's card not in: the marker waits for it
    snprintf(mark, sizeof mark, "from=%s\nto=%s\n", at("gone"), at("int")); put("internal/" DM_ST_MOVE, mark);
    assert(dm_st_recover(at("internal"), at("int")) == 1 && has("internal/" DM_ST_MOVE));
    // neither folder there: nothing to clean, the marker stays too (a card that comes back)
    snprintf(mark, sizeof mark, "from=%s\nto=%s\n", at("gone"), at("gone2")); put("internal/" DM_ST_MOVE, mark);
    assert(dm_st_recover(at("internal"), "") == 1 && !has("internal/" DM_ST_MOVE));
    assert(dm_st_recover(at("internal"), "") == 0);   // no marker
    // ---- the choice written ----
    assert(dm_st_write(at("internal/" DM_ST_FILE), "root=/x\n") == 0); t = get("internal/" DM_ST_FILE); assert(t && !strcmp(t, "root=/x\n")); free(t);
    assert(!has("internal/" DM_ST_FILE ".new"));
    puts("storage ok");
    return 0;
}
