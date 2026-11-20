// dm/android/savesync.h on the computer: the save's archive is deterministic (the same tree gives the same bytes, whatever the files' times and the order they were made in),
// it reads back to the same tree, an archive with a path that leaves its folder (.., an absolute path, a link) is refused before anything is written, sync.txt and the
// state read back, the server's meta is read, and the start's decision (ss_decide) for each case: offline, a token refused, nothing on the server, the same save, the server
// newer with this device's save unchanged, a conflict, a first setup with a save on both, the player's choice of this device's save, a run played without the sync, another
// device's session (a save not yet up, running now, killed), and a trial build, and the start that asks no server (ss_start_offline). No game profile: make -C dm/host/tests build/t_savesync && build/t_savesync
#include "../../android/savesync.h"
#include <sys/time.h>

static int fails;
#define CHECK(c, ...) do { if (!(c)) { fails++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static char g_tmp[512];
static void put(const char *rel, const char *body) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s", g_tmp, rel); ds_mkdirs(p);
    FILE *f = fopen(p, "wb"); if (!f) { printf("cannot write %s\n", p); exit(2); } fputs(body, f); fclose(f);
}
static void mk(const char *rel) { char p[1024]; snprintf(p, sizeof p, "%s/%s", g_tmp, rel); ds_mkdirs(p); mkdir(p, 0755); }
static void touch_time(const char *rel, long t) { char p[1024]; snprintf(p, sizeof p, "%s/%s", g_tmp, rel); struct timeval tv[2] = {{t, 0}, {t, 0}}; utimes(p, tv); }
static void path(char *out, size_t cap, const char *rel) { snprintf(out, cap, "%s/%s", g_tmp, rel); }
static int tar_of(const char *save, const char *dev, char sha[65], ss_buf *keep) {
    char a[1024], b[1024], err[300]; ss_buf t; int64_t nw; path(a, sizeof a, save); path(b, sizeof b, dev);
    int r = ss_tar_tree(a, b, &t, &nw, err, sizeof err);
    if (r == 0) ss_sha256(t.p, t.n, sha); else sha[0] = 0;
    if (r < 0) printf("  (tar %s: %d %s)\n", save, r, err);
    if (keep) *keep = t; else ss_buf_free(&t);
    return r;
}
// a tar made by hand: one entry with the given name and type, then the end
static size_t raw_tar(uint8_t *out, const char *name, char type, const char *body) {
    memset(out, 0, 4096); uint8_t *h = out; size_t n = strlen(body);
    snprintf((char *)h, 100, "%s", name); ss_octal((char *)h + 100, 8, 0644); ss_octal((char *)h + 108, 8, 0); ss_octal((char *)h + 116, 8, 0);
    ss_octal((char *)h + 124, 12, n); ss_octal((char *)h + 136, 12, 0); h[156] = (uint8_t)type; memcpy(h + 257, "ustar", 6); h[263] = '0'; h[264] = '0';
    if (type == '2') snprintf((char *)h + 157, 100, "/etc/passwd");
    memset(h + 148, ' ', 8); unsigned s = 0; for (int i = 0; i < 512; i++) s += h[i]; ss_octal((char *)h + 148, 7, s); h[155] = ' ';
    memcpy(out + 512, body, n);
    return 512 + (n + 511) / 512 * 512 + 1024;
}
static int refused(const uint8_t *t, size_t n, const char *what) {
    char a[1024], b[1024], err[300]; path(a, sizeof a, "x.save"); path(b, sizeof b, "x.dev"); ss_rm_tree(a); ss_rm_tree(b);
    int r = ss_untar(t, n, a, b, err, sizeof err); struct stat st;
    int wrote = !stat(a, &st) || !stat(b, &st);
    CHECK(r == -1 && !wrote, "%s: not refused (r %d, wrote %d)", what, r, wrote);
    return r == -1;
}

static void t_tar(void) {
    // two trees with the same content, made in another order and with other times
    put("a/save/zeta.bin", "zzz"); put("a/save/alpha/one.dat", "1"); put("a/save/alpha/two.dat", "22"); mk("a/save/empty"); put("a/dev/common.bin", "device");
    put("b/save/alpha/two.dat", "22"); mk("b/save/empty"); put("b/save/zeta.bin", "zzz"); put("b/save/alpha/one.dat", "1"); put("b/dev/common.bin", "device");
    touch_time("a/save/zeta.bin", 1000000); touch_time("b/save/zeta.bin", 1700000000); touch_time("a/save/alpha/one.dat", 5);
    char ha[65], hb[65], hc[65]; ss_buf ta;
    CHECK(tar_of("a/save", "a/dev", ha, &ta) == 0 && tar_of("b/save", "b/dev", hb, NULL) == 0, "the trees are archived");
    CHECK(!strcmp(ha, hb), "the same tree gives the same archive (%s / %s)", ha, hb);
    CHECK(tar_of("a/save", "a/dev", hc, NULL) == 0 && !strcmp(ha, hc), "again the same");
    CHECK(ta.n % 512 == 0 && ta.n >= 1024, "whole blocks");
    CHECK(ss_tar_files(ta.p, ta.n) == 4, "the start screen's file count: 4 files, the folders not counted (%lld)", (long long)ss_tar_files(ta.p, ta.n));
    CHECK(ss_tar_files(ta.p, 100) == -1, "a cut archive has no count");
    put("b/save/zeta.bin", "zzZ"); tar_of("b/save", "b/dev", hb, NULL); CHECK(strcmp(ha, hb), "a changed byte changes the hash");
    put("b/save/zeta.bin", "zzz"); tar_of("b/save", "b/dev", hb, NULL); CHECK(!strcmp(ha, hb), "and back");
    CHECK(tar_of("a/save", "nodev", hc, NULL) == 0 && strcmp(ha, hc), "without the device save: another archive");
    mk("none/save"); CHECK(tar_of("none/save", "none/dev", hc, NULL) == 1, "an empty save: no local save");
    // a long name (ustar's prefix field)
    char longp[300]; snprintf(longp, sizeof longp, "c/save/%s/%s/file.bin", "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa", "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb");
    put(longp, "long"); char hl[65]; ss_buf tl; CHECK(tar_of("c/save", "c/dev", hl, &tl) == 0, "a long name is archived");
    // the archive reads back to the same tree: archived again, the same bytes
    mk("r"); mk("rl"); mk("ok");
    char a[1024], b[1024], err[300]; path(a, sizeof a, "r/save"); path(b, sizeof b, "r/dev");
    CHECK(ss_untar(ta.p, ta.n, a, b, err, sizeof err) == 0, "read back: %s", err);
    char hr[65]; CHECK(tar_of("r/save", "r/dev", hr, NULL) == 0 && !strcmp(hr, ha), "the tree read back is archived to the same bytes");
    struct stat st; char e[1024]; path(e, sizeof e, "r/save/empty"); CHECK(!stat(e, &st) && S_ISDIR(st.st_mode), "an empty folder comes back");
    path(a, sizeof a, "rl/save"); path(b, sizeof b, "rl/dev");
    CHECK(ss_untar(tl.p, tl.n, a, b, err, sizeof err) == 0 && tar_of("rl/save", "rl/dev", hr, NULL) == 0 && !strcmp(hr, hl), "a long name reads back: %s", err);
    CHECK(stat(b, &st) != 0, "no device folder when the archive has none");
    // what is refused
    uint8_t t[4096]; size_t n;
    n = raw_tar(t, "save/../escape.txt", '0', "x"); refused(t, n, "save/../");
    n = raw_tar(t, "/etc/passwd", '0', "x"); refused(t, n, "an absolute path");
    n = raw_tar(t, "save//x", '0', "x"); refused(t, n, "an empty part");
    n = raw_tar(t, "save/./x", '0', "x"); refused(t, n, "a . part");
    n = raw_tar(t, "other/x", '0', "x"); refused(t, n, "outside save/ and device/");
    n = raw_tar(t, "save/link", '2', ""); refused(t, n, "a symbolic link");
    n = raw_tar(t, "save/hard", '1', ""); refused(t, n, "a hard link");
    n = raw_tar(t, "save/fifo", '6', ""); refused(t, n, "a FIFO");
    n = raw_tar(t, "save/a\\b", '0', "x"); refused(t, n, "a backslash");
    n = raw_tar(t, "save/ok.txt", '0', "x"); t[0] ^= 1; refused(t, n, "a wrong checksum");
    n = raw_tar(t, "save/ok.txt", '0', "x"); refused(t, n - 1024, "no end");
    n = raw_tar(t, "save/big.txt", '0', "x"); ss_octal((char *)t + 124, 12, 1ull << 30); { memset(t + 148, ' ', 8); unsigned s = 0; for (int i = 0; i < 512; i++) s += t[i]; ss_octal((char *)t + 148, 7, s); t[155] = ' '; }
    refused(t, n, "a size past the end");
    // a good one made by hand is taken
    n = raw_tar(t, "save/ok.txt", '0', "hello"); path(a, sizeof a, "ok/save"); path(b, sizeof b, "ok/dev");
    CHECK(ss_untar(t, n, a, b, err, sizeof err) == 0, "a plain archive is taken: %s", err);
    ss_buf_free(&ta); ss_buf_free(&tl);
}

static void t_files(void) {
    ss_cfg c;
    CHECK(ss_cfg_text("url=https://rooms.example.org/\ntoken=abcd1234\ndevice=nova\n", &c) == 1 && !strcmp(c.url, "https://rooms.example.org") && !strcmp(c.device, "nova"), "sync.txt");
    CHECK(ss_cfg_text("# none\nurl=https://x\n", &c) == 0, "no token: off");
    CHECK(ss_cfg_text("url=\ntoken=abc\n", &c) == 0, "no url: off");
    CHECK(ss_cfg_text("url=ftp://x\ntoken=abc\n", &c) == 0, "not http: off");
    CHECK(ss_cfg_text(" url = http://10.0.0.2:8080 \r\ntoken= t \r\n", &c) == 1 && !strcmp(c.token, "t") && !strcmp(c.device, "android"), "spaces, CRLF, a default device");
    ss_state s = {0}, r; s.have = 1; s.seq = 7; snprintf(s.sha, sizeof s.sha, "%064d", 0); s.pending = 1; s.conflict = 1; s.cseq = 9; s.ctime = 1700000000; snprintf(s.cdevice, sizeof s.cdevice, "deck");
    s.force = 1; s.fbase = 9; s.last_up = 1700000100; s.offline_play = 1; s.op_seq = 7; snprintf(s.op_sha, sizeof s.op_sha, "%s", s.sha); s.mode = SS_MODE_LOCAL;
    s.relink = 1; snprintf(s.m_device, sizeof s.m_device, "nova"); s.m_time = 1700000200; s.checked = 1700000300;
    char b[1024]; ss_state_format(&s, b, sizeof b); ss_state_text(b, &r);
    CHECK(!memcmp(&s, &r, sizeof s), "the state reads back");
    CHECK(strstr(b, "off=1\n") != NULL, "this device only still writes off=1 (an older build reads it as switched off)");
    ss_state_text("seq=3\noff=1\n", &r); CHECK(r.mode == SS_MODE_LOCAL, "an older state's switch off: this device only");
    ss_state_text("seq=3\noff=0\n", &r); CHECK(r.mode == SS_MODE_AUTO, "an older state's switch on: automatic");
    ss_state_text("seq=3\noff=1\nmode=1\n", &r); CHECK(r.mode == SS_MODE_MANUAL, "mode= wins over off=");
    ss_meta m;
    const char *j = "{\"seq\": 12, \"sha256\": \"ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789\", \"size\": 4096, \"time\": 1763640000, \"device\": \"deck\", "
                    "\"gameVersion\": \"v196608\", \"build\": \"1.2\", \"extra\": [1, {\"a\": null}], \"session\": {\"device\": \"deck\", \"state\": \"open\", \"updated\": \"2026-11-20T12:00:00Z\"}}";
    CHECK(ss_meta_parse(j, strlen(j), &m) == 0 && m.seq == 12 && m.size == 4096 && m.time == 1763640000 && !strcmp(m.device, "deck") && !strncmp(m.sha, "abcdef", 6), "meta");
    CHECK(m.session && m.s_open && !strcmp(m.s_device, "deck") && m.s_updated == 1795176000, "meta's session (%lld)", (long long)m.s_updated);
    CHECK(m.files == -1, "no fileCount: not known");
    const char *jf = "{\"seq\":4,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"size\":7234567,\"fileCount\":37}";
    CHECK(ss_meta_parse(jf, strlen(jf), &m) == 0 && m.files == 37, "fileCount");
    char sz[32]; ss_fmt_size(sz, sizeof sz, m.size); CHECK(!strcmp(sz, "6.9 MB"), "size %s", sz);
    ss_fmt_size(sz, sizeof sz, 1258291); CHECK(!strcmp(sz, "1.2 MB"), "size %s", sz); ss_fmt_size(sz, sizeof sz, 655360); CHECK(!strcmp(sz, "640 KB"), "size %s", sz);
    const char *j2 = "{\"seq\":3,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\",\"time\":1763640000123,\"session\":null}";
    CHECK(ss_meta_parse(j2, strlen(j2), &m) == 0 && m.time == 1763640000 && !m.session, "milliseconds, no session");
    const char *c1 = "{\"current\": null}", *c2 = "{\"error\":\"conflict\",\"current\":{\"seq\":5,\"sha256\":\"0000000000000000000000000000000000000000000000000000000000000000\"}}";
    CHECK(ss_conflict_parse(c1, strlen(c1), &m) == 0, "409 with no save on the server");
    CHECK(ss_conflict_parse(c2, strlen(c2), &m) == 1 && m.seq == 5, "409 with the server's meta");
    char o[64]; setenv("TZ", "UTC", 1); tzset();
    ss_fmt_date(o, sizeof o, 1795208520, 1); CHECK(!strcmp(o, "11월 20일 오후 9:02"), "the date in Korean: %s", o);
    ss_fmt_date(o, sizeof o, 1795176000, 0); CHECK(!strcmp(o, "Nov 20, 12:00 PM"), "the date in English: %s", o);
    ss_fmt_ago(o, sizeof o, 1000, 1030, 1); CHECK(!strcmp(o, "방금"), "%s", o);
    ss_fmt_ago(o, sizeof o, 1000, 1000 + 180, 1); CHECK(!strcmp(o, "3분 전"), "%s", o);
    ss_fmt_ago(o, sizeof o, 1000, 1000 + 7300, 1); CHECK(!strcmp(o, "2시간 전"), "%s", o);
    ss_fmt_ago(o, sizeof o, 1000, 1000 + 90000, 1); CHECK(!strcmp(o, "어제"), "%s", o);
    ss_fmt_ago(o, sizeof o, 1000, 1000 + 3 * 86400 + 5, 1); CHECK(!strcmp(o, "3일 전"), "%s", o);
    ss_qenc(o, sizeof o, "nova deck/1"); CHECK(!strcmp(o, "nova%20deck%2F1"), "%s", o);
}

#define A "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa"
#define B "bbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbbb"
#define C "cccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccccc"
static ss_state st_of(int have, uint64_t seq, const char *sha) { ss_state s = {0}; s.have = have; s.seq = seq; snprintf(s.sha, sizeof s.sha, "%s", sha ? sha : ""); return s; }
static ss_meta meta_of(uint64_t seq, const char *sha) { ss_meta m = {0}; m.seq = seq; snprintf(m.sha, sizeof m.sha, "%s", sha); return m; }
static void t_decide(void) {
    int p; ss_state s; ss_meta m = meta_of(5, B);
    s = st_of(1, 5, A);
    CHECK(ss_decide(SS_NET_FAIL, NULL, &s, A, &p) == SS_GO_OFFLINE && !p, "offline, nothing changed");
    CHECK(ss_decide(SS_NET_FAIL, NULL, &s, C, &p) == SS_GO_OFFLINE && p, "offline, the save changed: owed");
    CHECK(ss_decide(SS_NET_UNAUTH, NULL, &s, C, &p) == SS_GO_AUTH && p, "the token refused");
    CHECK(ss_decide(SS_NET_NOTFOUND, NULL, &s, A, &p) == SS_GO_PENDING && p, "nothing on the server, a save here: it goes up");
    CHECK(ss_decide(SS_NET_NOTFOUND, NULL, &s, NULL, &p) == SS_GO_NOTHING && !p, "nothing on either");
    s = st_of(1, 5, A); m = meta_of(5, A);
    CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_UPTODATE && !p, "the same seq, the same save");
    CHECK(ss_decide(SS_NET_OK, &m, &s, C, &p) == SS_GO_PENDING && p, "the same seq, this device's save changed: it goes up");
    s = st_of(1, 5, A); m = meta_of(8, B);
    CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_DOWNLOAD && !p, "the server newer, this device's unchanged: download");
    CHECK(ss_decide(SS_NET_OK, &m, &s, C, &p) == SS_GO_CONFLICT && !p, "the server newer, this device's changed: conflict");
    CHECK(ss_decide(SS_NET_OK, &m, &s, B, &p) == SS_GO_UPTODATE, "both reached the same save");
    CHECK(ss_decide(SS_NET_OK, &m, &s, NULL, &p) == SS_GO_DOWNLOAD, "no save here: download");
    s = st_of(1, 9, A); m = meta_of(8, B);
    CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_CONFLICT, "the server went back to an older seq: conflict");
    s = st_of(0, 0, NULL); m = meta_of(3, B);
    CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_CONFLICT && !p, "a first setup with a save on both: conflict");
    CHECK(ss_decide(SS_NET_OK, &m, &s, NULL, &p) == SS_GO_DOWNLOAD, "a first setup, no save here: download");
    CHECK(ss_decide(SS_NET_NOTFOUND, NULL, &s, A, &p) == SS_GO_PENDING && p, "a first setup, nothing on the server: it goes up");
    s = st_of(1, 5, A); s.force = 1; s.fbase = 8; m = meta_of(8, B);
    CHECK(ss_decide(SS_NET_OK, &m, &s, C, &p) == SS_GO_PENDING && p, "this device's save chosen, the server did not move: it goes up");
    m = meta_of(9, B); CHECK(ss_decide(SS_NET_OK, &m, &s, C, &p) == SS_GO_CONFLICT, "this device's save chosen, the server moved again: conflict");
    // a run played without the sync (B at the start, a trial build, the game's sync off)
    s = st_of(1, 5, A); ss_mark_offline_play(&s); m = meta_of(5, A);
    CHECK(s.offline_play && s.op_seq == 5 && !strcmp(s.op_sha, A), "marked");
    CHECK(ss_decide(SS_NET_OK, &m, &s, C, &p) == SS_GO_ASK_OFFLINE && !p, "played without the sync, the save changed, the server did not: ask");
    CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_UPTODATE && !p, "played without the sync, nothing changed: no question");
    m = meta_of(6, B); CHECK(ss_decide(SS_NET_OK, &m, &s, C, &p) == SS_GO_CONFLICT, "played without the sync, the server moved too: conflict");
    CHECK(ss_decide(SS_NET_FAIL, NULL, &s, C, &p) == SS_GO_OFFLINE && !p, "played without the sync, then offline: nothing goes up by itself");
    // another device: dirty (a save not yet up) warns, a recent heartbeat is a note, anything else nothing
    int64_t now = 1795176000; m = meta_of(5, A); m.session = 1; m.s_open = 1; m.s_dirty = 1; snprintf(m.s_device, sizeof m.s_device, "deck"); m.s_updated = now - 3600;
    CHECK(ss_other_device(&m, "nova", now) == SS_OTHER_UNSYNCED, "dirty on another device an hour ago: the warning");
    m.s_updated = now - 29 * 86400; CHECK(ss_other_device(&m, "nova", now) == SS_OTHER_UNSYNCED, "dirty 29 days ago: the warning");
    m.s_updated = now - 31 * 86400; CHECK(ss_other_device(&m, "nova", now) == SS_OTHER_NONE, "dirty but older than 30 days: none");
    m.s_updated = now - 3600; m.s_open = 0; CHECK(ss_other_device(&m, "nova", now) == SS_OTHER_UNSYNCED, "closed but dirty: the warning");
    m.s_open = 1; m.s_updated = now - 30; CHECK(ss_other_device(&m, "nova", now) == SS_OTHER_UNSYNCED, "alive and dirty: the warning, not the note");
    CHECK(ss_other_device(&m, "deck", now) == SS_OTHER_NONE, "dirty on this device: none");
    m.s_dirty = 0; m.s_updated = now - 60; CHECK(ss_other_device(&m, "nova", now) == SS_OTHER_RUNNING, "alive a minute ago, clean: a note");
    m.s_updated = now - 179; CHECK(ss_other_device(&m, "nova", now) == SS_OTHER_RUNNING, "alive 179 s ago: a note");
    m.s_updated = now - 180; CHECK(ss_other_device(&m, "nova", now) == SS_OTHER_NONE, "open 3 min ago (killed, its heartbeats stopped): none");
    m.s_updated = now - 3600; CHECK(ss_other_device(&m, "nova", now) == SS_OTHER_NONE, "open an hour ago, clean (killed): none");
    CHECK(ss_other_device(&m, "deck", now) == SS_OTHER_NONE, "open on this device: none");
    m.s_updated = now - 10; m.s_open = 0; CHECK(ss_other_device(&m, "nova", now) == SS_OTHER_NONE, "closed and clean a moment ago: none");
    m.session = 0; m.s_open = 1; m.s_dirty = 1; CHECK(ss_other_device(&m, "nova", now) == SS_OTHER_NONE, "no session: none");
    // the session as the server says it: alive and dirty read; no dirty (an older server) is clean
    const char *j3 = "{\"seq\":0,\"session\":{\"device\":\"deck\",\"state\":\"alive\",\"updated\":\"2026-11-20T12:00:00.000Z\",\"dirty\":true}}";
    CHECK(ss_meta_parse(j3, strlen(j3), &m) == 0 && m.session && m.s_open && m.s_dirty && m.s_updated == 1795176000, "alive, dirty");
    const char *j4 = "{\"seq\":0,\"session\":{\"device\":\"deck\",\"state\":\"closed\",\"dirty\":false,\"updated\":\"2026-11-20T12:00:00Z\"}}";
    CHECK(ss_meta_parse(j4, strlen(j4), &m) == 0 && m.session && !m.s_open && !m.s_dirty, "closed, clean");
    const char *j5 = "{\"seq\":0,\"session\":{\"device\":\"deck\",\"state\":\"open\",\"updated\":\"2026-11-20T12:00:00Z\"}}";
    CHECK(ss_meta_parse(j5, strlen(j5), &m) == 0 && m.s_open && !m.s_dirty && ss_other_device(&m, "nova", 1795176000 + 600) == SS_OTHER_NONE, "no dirty field: clean");
    // a trial build, the game's switch, no sync.txt
    CHECK(ss_run_mode(0, 0, 0) == SS_RUN_NONE && ss_run_mode(0, 1, 1) == SS_RUN_NONE, "no sync.txt: nothing");
    CHECK(ss_run_mode(1, 1, 0) == SS_RUN_SUPPRESSED, "a trial build: suppressed");
    CHECK(ss_run_mode(1, 0, 1) == SS_RUN_SUPPRESSED, "switched off: suppressed");
    CHECK(ss_run_mode(1, 0, 0) == SS_RUN_SYNC, "a release with sync.txt: the sync");
    // the start without asking the server: no network on the device, or the data's step could not reach its server just now; else the meta is asked (and its own
    // first failure still asks the player)
    CHECK(ss_start_offline(0, 0) && ss_start_offline(0, 1), "no network on the device: the server is not asked");
    CHECK(ss_start_offline(1, 1) && ss_start_offline(-1, 1), "the data's server was not reached: the cloud's is not asked either");
    CHECK(!ss_start_offline(1, 0) && !ss_start_offline(-1, 0), "the data's server answered (or no data step): the cloud's meta is asked");
    s = st_of(1, 4, A); m = meta_of(4, A);
    CHECK(ss_start_offline(1, 1) && ss_decide(SS_NET_FAIL, &m, &s, C, &p) == SS_GO_OFFLINE && p, "skipped for the data's server: a changed save is owed, it goes up once connected");
    s = st_of(0, 0, NULL); ss_mark_offline_play(&s); m = meta_of(4, B);
    CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_CONFLICT, "a trial build played first, then the release finds a save on both: conflict");
    s = st_of(1, 4, B); ss_mark_offline_play(&s); m = meta_of(4, B);
    CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_ASK_OFFLINE, "a trial build changed the synced save: the release asks");
}

// the corner marks, sampled every 50 ms as the presents read them: uploads at the given starts, each answered after `dur` ms with the given notes
typedef struct { int busy_edges, checks, other, busy_at_end; } marks_seen;
static marks_seen run_marks(ss_notes *n, const int64_t *start, const int *note, int count, int64_t dur, int64_t until) {
    marks_seen m = {0}; int prev = SS_MARK_NONE, i = 0, up = 0;
    for (int64_t t = start[0] - 1000; t <= until; t += 50) {
        if (up && t >= start[i] + dur) { ss_up_end(n, note[i], t); up = 0; i++; }
        if (!up && i < count && t >= start[i]) { ss_up_begin(n, t); up = 1; }
        int64_t age = -1; int k = ss_mark_now(n, t, &age);
        CHECK(k == SS_MARK_NONE || age >= 0, "a mark's age (%lld at %lld)", (long long)age, (long long)t);
        if (k != prev) { if (k == SS_MARK_BUSY) m.busy_edges++; else if (k == SS_MARK_SAVED) { m.checks++; CHECK(age < 50, "the check starts with its pop (age %lld)", (long long)age); } else if (k) m.other++; }
        prev = k;
    }
    m.busy_at_end = prev == SS_MARK_BUSY; return m;
}
static void t_marks(void) {
    static const int ok[3] = {SS_NOTE_SAVED, SS_NOTE_SAVED, SS_NOTE_SAVED};
    ss_notes n = {0}; int64_t apart[3] = {2000, 12000, 22000};   // three uploads 10 s apart (each half a second): three spinners, three checks
    marks_seen m = run_marks(&n, apart, ok, 3, 500, 40000);
    CHECK(m.busy_edges == 3 && m.checks == 3 && !m.other && !m.busy_at_end, "three uploads: three spinners and three checks (%d, %d)", m.busy_edges, m.checks);
    ss_notes n3 = {0}; int64_t gap3[3] = {2000, 6000, 10000};   // the next starts 3.5 s after the last went up: not joined
    m = run_marks(&n3, gap3, ok, 3, 500, 30000);
    CHECK(m.busy_edges == 3 && m.checks == 3, "uploads 3.5 s apart: a check each (%d, %d)", m.busy_edges, m.checks);
    ss_notes n1 = {0}; int64_t close[3] = {2000, 3000, 4000};   // a save every second: one spinner turning through, one check after the last
    m = run_marks(&n1, close, ok, 3, 300, 20000);
    CHECK(m.busy_edges == 1 && m.checks == 1 && !m.other, "uploads 1 s apart: one spinner, one check (%d, %d)", m.busy_edges, m.checks);
    ss_notes nb = {0}; int64_t age; ss_up_begin(&nb, 1000); ss_up_end(&nb, SS_NOTE_SAVED, 1400);
    CHECK(ss_mark_now(&nb, 1400 + SS_JOIN_MS - 1, &age) == SS_MARK_BUSY && age == SS_JOIN_MS - 1 + 400, "the spinner turns on after it went up, its age from the upload's start");
    CHECK(ss_mark_now(&nb, 1400 + SS_JOIN_MS + 100, &age) == SS_MARK_SAVED && age == 100, "then the check");
    CHECK(ss_mark_now(&nb, 1400 + SS_JOIN_MS + SS_MARK_SAVED_MS, &age) == SS_MARK_NONE, "for its time, then nothing");
    // waiting and a conflict keep their rule: a change of state, none of a kind within 5 min
    static const int lost[3] = {SS_NOTE_WAITING, SS_NOTE_WAITING, SS_NOTE_SAVED};
    ss_notes nw = {0}; int64_t retry[3] = {2000, 62000, 122000};
    m = run_marks(&nw, retry, lost, 3, 500, 140000);
    CHECK(m.other == 1 && m.checks == 1 && m.busy_edges == 3, "no network twice, then up: one waiting note, one check (%d, %d)", m.other, m.checks);
    static const int after[2] = {SS_NOTE_WAITING, SS_NOTE_WAITING};
    int64_t again[2] = {150000, 250000}; m = run_marks(&nw, again, after, 2, 500, 260000);
    CHECK(m.other == 0, "lost again after a check, but within 5 min of the last waiting note: none (%d)", m.other);
    ss_notes nc = {0}; static const int conf[2] = {SS_NOTE_CONFLICT, SS_NOTE_NONE}; int64_t cs[2] = {2000, 10000};
    m = run_marks(&nc, cs, conf, 2, 500, 20000);
    CHECK(m.other == 1 && !m.checks && m.busy_edges == 2, "a conflict: its note once; a refused upload (401, 5xx): the spinner and no mark");
}

// while the game runs: a start without the network, the network back, the upload going up or meeting another device's newer save
static void t_ingame(void) {
    ss_state s = st_of(1, 4, B), s2; ss_meta m = meta_of(4, B); int p;
    CHECK(ss_decide(SS_NET_FAIL, &m, &s, A, &p) == SS_GO_OFFLINE && p, "an offline start with a changed save: owed");
    s.pending = 1; s2 = s;
    ss_tick_in t = {.syncs = 1, .net = 0, .was_net = 1, .sess_open = 0, .owed = 1, .now = 10000, .next_try = 70000};
    CHECK(ss_tick(&t) == 0, "known offline: nothing is tried, not even the open");
    t.was_net = 0; t.net = 1; int a = ss_tick(&t);
    CHECK(a == (SS_DO_RETRY_NOW | SS_DO_UPLOAD | SS_DO_OPEN), "the network back: the owed save goes up at once (backoff forgotten) and the session opens (%d)", a);
    t.was_net = 1; t.now = 20000; CHECK(ss_tick(&t) == SS_DO_OPEN, "the next tick, still within the backoff, an open that did not go through: tried again");
    t.sess_open = 1; t.last_hb = 20000; t.now = 50000; CHECK(ss_tick(&t) == 0, "open, no save owed due: nothing");
    t.now = 81000; CHECK(ss_tick(&t) == (SS_DO_UPLOAD | SS_DO_ALIVE), "past the backoff and a minute on: the upload and a heartbeat");
    t.last_hb = 81000; t.committed = 1; t.owed = 0; t.now = 82000; CHECK(ss_tick(&t) == SS_DO_UPLOAD, "a fresh commit goes up");
    ss_tick_in off = t; off.syncs = 0; off.was_net = 0; CHECK(ss_tick(&off) == 0, "a run without the sync (B, a trial build, switched off) sends nothing when the network comes back");
    ss_notes n = {0};
    // the upload goes up
    CHECK(ss_after_put(&s, 200, 5, A, NULL, 1795176000) == SS_NOTE_SAVED && !s.pending && s.seq == 5 && !strcmp(s.sha, A) && s.last_up == 1795176000, "200: saved, nothing owed");
    CHECK(ss_note_ok(&n, SS_NOTE_WAITING, 170000), "the network lost: waiting shows");
    CHECK(!ss_note_ok(&n, SS_NOTE_WAITING, 190000), "a network that comes and goes: no waiting note within 5 min of the last");
    CHECK(!ss_note_ok(&n, SS_NOTE_WAITING, 170000 + SS_NOTE_GAP_MS), "never the same note twice in a row");
    ss_up_end(&n, SS_NOTE_SAVED, 200000); CHECK(ss_note_ok(&n, SS_NOTE_WAITING, 170000 + SS_NOTE_GAP_MS), "after a check, waiting again 5 min on");
    // or another device's newer save is there: kept for the next start, no retry, one note
    ss_meta cur = meta_of(6, C); snprintf(cur.device, sizeof cur.device, "nova"); cur.time = 1795170000;
    CHECK(ss_after_put(&s2, 409, 0, A, &cur, 0) == SS_NOTE_CONFLICT && s2.conflict && s2.pending && s2.cseq == 6 && !strcmp(s2.cdevice, "nova"), "409: a conflict, kept");
    ss_notes n2 = {0}; CHECK(ss_note_ok(&n2, SS_NOTE_CONFLICT, 1000) && !ss_note_ok(&n2, SS_NOTE_CONFLICT, 400000), "the conflict's note once");
    t_marks();
    t.conflict = 1; t.owed = 1; t.committed = 1; t.now = 900000; CHECK(!(ss_tick(&t) & SS_DO_UPLOAD), "with a conflict nothing goes up by itself");
    m = meta_of(6, C); CHECK(ss_decide(SS_NET_OK, &m, &s2, A, &p) == SS_GO_CONFLICT, "the next start: the cards");
    CHECK(ss_after_put(&s2, 0, 0, A, NULL, 0) == SS_NOTE_WAITING && s2.pending, "no answer: waiting, owed");
}

// the modes: which events send the save, the upload thread's ticks, this device only and back, manual's question (17), the question after a run without the sync (16),
// and the state after Back to the cloud save
static void t_modes(void) {
    CHECK(ss_uploads(SS_MODE_AUTO, SS_EV_SAVE) && ss_uploads(SS_MODE_AUTO, SS_EV_LEAVE) && ss_uploads(SS_MODE_AUTO, SS_EV_ASKED), "automatic: a save, the app leaving and the player's ask all go up");
    CHECK(!ss_uploads(SS_MODE_MANUAL, SS_EV_SAVE) && !ss_uploads(SS_MODE_MANUAL, SS_EV_LEAVE) && ss_uploads(SS_MODE_MANUAL, SS_EV_ASKED), "manual: only what the player asks for");
    CHECK(!ss_uploads(SS_MODE_LOCAL, SS_EV_SAVE) && !ss_uploads(SS_MODE_LOCAL, SS_EV_LEAVE) && !ss_uploads(SS_MODE_LOCAL, SS_EV_ASKED), "this device only: nothing, ever");
    ss_tick_in t = {.syncs = 1, .mode = SS_MODE_MANUAL, .net = 1, .was_net = 1, .sess_open = 1, .committed = 1, .now = 10000, .last_hb = 9000};
    CHECK(ss_tick(&t) == SS_DO_CHECK, "manual: a commit is looked at (owed or not), not sent (%d)", ss_tick(&t));
    t.committed = 0; t.owed = 1; t.next_try = 0; CHECK(ss_tick(&t) == 0, "manual: an owed save is not tried again by itself");
    t.was_net = 0; CHECK(!(ss_tick(&t) & SS_DO_UPLOAD), "manual: nor when the network comes back");
    t.was_net = 1; t.kick = 1; CHECK(ss_tick(&t) == SS_DO_UPLOAD, "manual: Upload now sends it");
    t.kick = 0; t.committed = 1; t.net = 0; CHECK(ss_tick(&t) == SS_DO_CHECK, "manual offline: the commit is still looked at (the corner's mark)");
    t.mode = SS_MODE_AUTO; t.net = 1; CHECK(ss_tick(&t) == SS_DO_UPLOAD, "automatic: the commit goes up");
    t.syncs = 0; CHECK(ss_tick(&t) == 0, "this device only (the run does not sync): nothing");
    int p; ss_state s; ss_meta m;
    // this device only, a test played, then automatic again: the next check is a first connection, so the test save is asked about, never sent over the cloud's
    s = st_of(1, 4, B); ss_set_mode(&s, SS_MODE_LOCAL);
    CHECK(s.mode == SS_MODE_LOCAL && s.offline_play && s.op_seq == 4, "to this device only: the run is marked played without the sync");
    ss_set_mode(&s, SS_MODE_AUTO);
    CHECK(s.mode == SS_MODE_AUTO && s.relink && !s.seq && !s.sha[0] && !s.offline_play && !s.pending, "back to automatic: the last sync forgotten, relink");
    m = meta_of(4, B); CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_CONFLICT && !p, "back from this device only with a test save: the cards, nothing sent");
    m = meta_of(6, C); CHECK(ss_decide(SS_NET_OK, &m, &s, B, &p) == SS_GO_CONFLICT && !p, "the cloud moved meanwhile, this device kept the old save: the cards too (not a download over it)");
    m = meta_of(4, B); CHECK(ss_decide(SS_NET_OK, &m, &s, B, &p) == SS_GO_UPTODATE, "the same save on both: nothing to ask");
    CHECK(ss_decide(SS_NET_OK, &m, &s, NULL, &p) == SS_GO_DOWNLOAD, "no save here: the cloud's");
    CHECK(ss_decide(SS_NET_NOTFOUND, &m, &s, A, &p) == SS_GO_PENDING && p, "nothing on the server: this device's goes up (nothing is overwritten)");
    s = st_of(1, 4, B); ss_set_mode(&s, SS_MODE_MANUAL); CHECK(!s.relink && s.seq == 4 && !s.offline_play, "automatic to manual: nothing else changes");
    ss_set_mode(&s, SS_MODE_AUTO); CHECK(!s.relink && s.seq == 4, "and back");
    // manual: a save not sent; the next start asks (17), unless the cloud moved too (the conflict's cards)
    s = st_of(1, 4, B); s.mode = SS_MODE_MANUAL; m = meta_of(4, B);
    CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_ASK_MANUAL && !p, "manual, changed, the cloud did not move: ask (upload / keep)");
    m = meta_of(6, C); CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_CONFLICT, "manual, changed, the cloud moved: the conflict's cards instead");
    m = meta_of(6, C); CHECK(ss_decide(SS_NET_OK, &m, &s, B, &p) == SS_GO_DOWNLOAD, "manual, unchanged, the cloud moved: the cloud's, as automatic");
    m = meta_of(4, B); CHECK(ss_decide(SS_NET_OK, &m, &s, B, &p) == SS_GO_UPTODATE, "manual, unchanged: up to date");
    CHECK(ss_decide(SS_NET_NOTFOUND, &m, &s, A, &p) == SS_GO_ASK_MANUAL, "manual, nothing on the server: asked before it goes up");
    CHECK(ss_decide(SS_NET_FAIL, &m, &s, A, &p) == SS_GO_OFFLINE && p, "manual offline: owed (the corner's mark), sent only when asked");
    s.force = 1; s.fbase = 4; CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_ASK_MANUAL, "manual, this device's chosen earlier but not sent: asked again");
    // a run with Play without the sync (the launcher, or B on a card) changed the save: the next start asks (16), in automatic and in manual; offline nothing is asked
    s = st_of(1, 4, B); ss_mark_offline_play(&s); m = meta_of(4, B);
    CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_ASK_OFFLINE && !p, "after a test play: ask (back to the cloud's / upload / keep)");
    s.mode = SS_MODE_MANUAL; CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_ASK_OFFLINE, "the same in manual");
    CHECK(ss_decide(SS_NET_FAIL, &m, &s, A, &p) == SS_GO_OFFLINE && !p, "offline: no question, nothing owed");
    CHECK(ss_local_state(&s, A) == SS_LOCAL_PLAYED, "the launcher says so (played without the sync)");
    CHECK(ss_decide(SS_NET_OK, &m, &s, B, &p) == SS_GO_UPTODATE, "a test play that saved nothing: nothing to ask");
    // Back to the cloud save: this device's goes to the backup slot, the cloud's is the save, the state is the cloud's
    ss_after_restore(&s, 4, B, B);
    CHECK(s.seq == 4 && !strcmp(s.sha, B) && !s.pending && !s.offline_play && !s.conflict && !s.relink, "after Back to the cloud save: synced");
    CHECK(ss_decide(SS_NET_OK, &m, &s, B, &p) == SS_GO_UPTODATE && ss_local_state(&s, B) == SS_LOCAL_SAME, "the next start: up to date");
    s = st_of(1, 4, B); s.relink = 1; ss_after_restore(&s, 6, C, C); CHECK(!s.relink && s.seq == 6, "a restore ends a relink too");
    ss_after_restore(&s, 6, A, C); CHECK(s.pending, "a device save kept from before: what is there now goes up");
    s = st_of(1, 4, B); CHECK(ss_local_state(&s, A) == SS_LOCAL_OWED && ss_local_state(&s, B) == SS_LOCAL_SAME && ss_local_state(&s, NULL) == SS_LOCAL_UNKNOWN, "the launcher's This device row");
    s.mode = SS_MODE_LOCAL; CHECK(ss_local_state(&s, A) == SS_LOCAL_ONLY, "this device only");
    s.mode = SS_MODE_AUTO; s.conflict = 1; CHECK(ss_local_state(&s, A) == SS_LOCAL_CONFLICT, "a conflict");
    char o[64]; int64_t now = 1795176000;   // (local time: only the words are checked)
    ss_fmt_when(o, sizeof o, now - 60, now, 1); CHECK(!strncmp(o, "오늘 ", 7) || !strncmp(o, "어제 ", 7), "%s", o);
    ss_fmt_when(o, sizeof o, now - 10 * 86400, now, 1); CHECK(strstr(o, "월 ") != NULL, "%s", o);
    ss_fmt_when(o, sizeof o, now - 60, now, 0); CHECK(!strncmp(o, "Today ", 6) || !strncmp(o, "Yesterday ", 10), "%s", o);
}

int main(void) {
    snprintf(g_tmp, sizeof g_tmp, "%s/t_savesync.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid()); ss_rm_tree(g_tmp); mkdir(g_tmp, 0755);
    t_tar(); t_files(); t_decide(); t_ingame(); t_modes();
    ss_rm_tree(g_tmp);
    printf("%s: %d failed\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
