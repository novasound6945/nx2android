// dm/android/savesync.h on the computer: the save's archive is deterministic (the same tree gives the same bytes, whatever the files' times and the order they were made in),
// it reads back to the same tree, an archive with a path that leaves its folder (.., an absolute path, a link) is refused before anything is written, sync.txt and the
// state read back, the server's meta is read, and the start's decision (ss_decide) for each case: offline, a token refused, nothing on the server, the same save, the server
// newer with this device's save unchanged, a conflict, a first setup with a save on both, the player's choice of this device's save, a run played without the sync, another
// device still running the game, and a trial build. No game profile: make -C dm/host/tests build/t_savesync && build/t_savesync
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
    s.force = 1; s.fbase = 9; s.last_up = 1700000100; s.offline_play = 1; s.op_seq = 7; snprintf(s.op_sha, sizeof s.op_sha, "%s", s.sha); s.off = 1;
    char b[1024]; ss_state_format(&s, b, sizeof b); ss_state_text(b, &r);
    CHECK(!memcmp(&s, &r, sizeof s), "the state reads back");
    ss_meta m;
    const char *j = "{\"seq\": 12, \"sha256\": \"ABCDEF0123456789abcdef0123456789abcdef0123456789abcdef0123456789\", \"size\": 4096, \"time\": 1763640000, \"device\": \"deck\", "
                    "\"gameVersion\": \"v196608\", \"build\": \"1.2\", \"extra\": [1, {\"a\": null}], \"session\": {\"device\": \"deck\", \"state\": \"open\", \"updated\": \"2026-11-20T12:00:00Z\"}}";
    CHECK(ss_meta_parse(j, strlen(j), &m) == 0 && m.seq == 12 && m.size == 4096 && m.time == 1763640000 && !strcmp(m.device, "deck") && !strncmp(m.sha, "abcdef", 6), "meta");
    CHECK(m.session && m.s_open && !strcmp(m.s_device, "deck") && m.s_updated == 1795176000, "meta's session (%lld)", (long long)m.s_updated);
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
    // another device still running the game
    int64_t now = 1795176000; m = meta_of(5, A); m.session = 1; m.s_open = 1; snprintf(m.s_device, sizeof m.s_device, "deck"); m.s_updated = now - 3600;
    CHECK(ss_other_running(&m, "nova", now), "open on another device an hour ago: the warning");
    CHECK(!ss_other_running(&m, "deck", now), "open on this device: none");
    m.s_updated = now - 8 * 86400; CHECK(!ss_other_running(&m, "nova", now), "open but older than a week: none");
    m.s_updated = now - 60; m.s_open = 0; CHECK(!ss_other_running(&m, "nova", now), "closed: none");
    m.session = 0; m.s_open = 1; CHECK(!ss_other_running(&m, "nova", now), "no session: none");
    // a trial build, the game's switch, no sync.txt
    CHECK(ss_run_mode(0, 0, 0) == SS_RUN_NONE && ss_run_mode(0, 1, 1) == SS_RUN_NONE, "no sync.txt: nothing");
    CHECK(ss_run_mode(1, 1, 0) == SS_RUN_SUPPRESSED, "a trial build: suppressed");
    CHECK(ss_run_mode(1, 0, 1) == SS_RUN_SUPPRESSED, "switched off: suppressed");
    CHECK(ss_run_mode(1, 0, 0) == SS_RUN_SYNC, "a release with sync.txt: the sync");
    s = st_of(0, 0, NULL); ss_mark_offline_play(&s); m = meta_of(4, B);
    CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_CONFLICT, "a trial build played first, then the release finds a save on both: conflict");
    s = st_of(1, 4, B); ss_mark_offline_play(&s); m = meta_of(4, B);
    CHECK(ss_decide(SS_NET_OK, &m, &s, A, &p) == SS_GO_ASK_OFFLINE, "a trial build changed the synced save: the release asks");
}

int main(void) {
    snprintf(g_tmp, sizeof g_tmp, "%s/t_savesync.%d", getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp", (int)getpid()); ss_rm_tree(g_tmp); mkdir(g_tmp, 0755);
    t_tar(); t_files(); t_decide();
    ss_rm_tree(g_tmp);
    printf("%s: %d failed\n", fails ? "FAIL" : "ok", fails);
    return fails != 0;
}
