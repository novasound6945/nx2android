// The start screen (dm/android/startscreen.c) drawn on the computer: each of the design's screens at a given size, saved as PNG (tests/test_start_screen.py builds and
// runs it; with an output folder it keeps the pictures to look at). Usage: start_screen_fixture <regular.ttf> <bold.ttf> <pixel font .nxpf> <out dir> <w> <h>
#include "startscreen.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

static void be32(uint8_t *p, uint32_t v) { p[0] = v >> 24; p[1] = v >> 16; p[2] = v >> 8; p[3] = (uint8_t)v; }
static void chunk(FILE *f, const char *t, const uint8_t *d, uint32_t n) {
    uint8_t h[8]; be32(h, n); memcpy(h + 4, t, 4); fwrite(h, 1, 8, f); if (n) fwrite(d, 1, n, f);
    uLong c = crc32(crc32(0, (const Bytef *)t, 4), d, n); uint8_t e[4]; be32(e, (uint32_t)c); fwrite(e, 1, 4, f);
}
static int write_png(const char *path, int w, int h, const uint32_t *px) {   // RGB, filter 0 each row
    size_t rn = (size_t)(w * 3 + 1) * h; uint8_t *raw = malloc(rn); uLongf zn = compressBound((uLong)rn); uint8_t *z = malloc(zn); FILE *f = fopen(path, "wb");
    if (!raw || !z || !f) return 0;
    for (int y = 0; y < h; y++) { uint8_t *r = raw + (size_t)y * (w * 3 + 1); r[0] = 0; for (int x = 0; x < w; x++) { uint32_t q = px[(size_t)y * w + x]; r[1 + 3 * x] = q & 255; r[2 + 3 * x] = q >> 8 & 255; r[3 + 3 * x] = q >> 16 & 255; } }
    compress2(z, &zn, raw, (uLong)rn, 6);
    uint8_t ih[13]; be32(ih, (uint32_t)w); be32(ih + 4, (uint32_t)h); ih[8] = 8; ih[9] = 2; ih[10] = ih[11] = ih[12] = 0;
    fwrite("\x89PNG\r\n\x1a\n", 1, 8, f); chunk(f, "IHDR", ih, 13); chunk(f, "IDAT", z, (uint32_t)zn); chunk(f, "IEND", NULL, 0);
    fclose(f); free(raw); free(z); return 1;
}

static uint8_t *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long k = ftell(f); rewind(f); uint8_t *b = malloc((size_t)k + 1);
    if (b && fread(b, 1, (size_t)k, f) != (size_t)k) { free(b); b = NULL; }
    fclose(f); if (b) *n = (size_t)k; return b;
}
static void step(stv_view *v, int i, int st, const char *name, const char *note) { v->step[i].state = st; snprintf(v->step[i].name, sizeof v->step[i].name, "%s", name); snprintf(v->step[i].note, sizeof v->step[i].note, "%s", note); }
static void head(stv_view *v, const uint32_t *icon, const char *ver) {
    memset(v, 0, sizeof *v); v->bar = -1; snprintf(v->name, sizeof v->name, "SUPER ROBOT WARS Y"); snprintf(v->version, sizeof v->version, "%s", ver);
    v->icon = icon; v->icon_w = v->icon_h = 160; v->glow = stv_icon_colour(icon, 160, 160);
}
static void save(stv_canvas *c, const char *dir, const char *name) {
    char p[1024]; snprintf(p, sizeof p, "%s/%s-%dx%d.png", dir, name, c->w, c->h);
    if (!write_png(p, c->w, c->h, c->px)) { fprintf(stderr, "cannot write %s\n", p); exit(1); }
    long lit = 0; for (int i = 0; i < c->w * c->h; i++) { uint32_t q = c->px[i]; if ((q & 255) > 200 && (q >> 8 & 255) > 200) lit++; }
    assert(lit > 200);   // text was drawn
    printf("%s\n", p);
}
int main(int argc, char **argv) {
    if (argc != 7) { fprintf(stderr, "usage: start_screen_fixture regular.ttf bold.ttf font.nxpf out_dir w h\n"); return 2; }
    size_t rn = 0, bn = 0, pn = 0; uint8_t *reg = slurp(argv[1], &rn), *bold = slurp(argv[2], &bn), *pf = slurp(argv[3], &pn);
    assert(reg && bold && pf); assert(!stv_font_set(0, reg, rn) && !stv_font_set(1, bold, bn)); stv_font_fallback(pf);
    int W = atoi(argv[5]), H = atoi(argv[6]); stv_canvas c = {malloc((size_t)W * H * 4), W, H}; assert(c.px);
    uint32_t icon[160 * 160];   // a stand-in for the game's icon: an orange tile with a dark disc
    for (int y = 0; y < 160; y++) for (int x = 0; x < 160; x++) { int dx = x - 80, dy = y - 80, in = dx * dx + dy * dy < 50 * 50;
        uint32_t r = in ? 40 : 230 - y / 4, g = in ? 30 : 120 + x / 4, b = in ? 60 : 40; icon[y * 160 + x] = 0xff000000u | b << 16 | g << 8 | r; }
    stv_view v; dm_cloud_view q;
    // 1. a usual start: the data and the shaders done, the cloud syncing
    head(&v, icon, "1.4.1 · 빌드 1");
    step(&v, 0, STV_DONE, "게임 데이터", "최신"); step(&v, 1, STV_DONE, "셰이더 준비", "완료"); step(&v, 2, STV_RUN, "클라우드 세이브", "맞추는 중…");
    snprintf(v.hint, sizeof v.hint, "곧 시작해요"); snprintf(v.foot, sizeof v.foot, "B · 동기화 없이 플레이");
    stv_draw(&c, &v, 0.3); save(&c, argv[4], "1-main");
    // 2. the first start: the game data downloading
    head(&v, icon, "1.4.1 · 빌드 1 · 처음 실행"); v.bar = 420;
    snprintf(v.bar_title, sizeof v.bar_title, "게임 데이터 받는 중"); stv_fmt_bytes(v.bar_left, sizeof v.bar_left, 7730941132ull, 18468359864ull);
    char sp[32], eta[64]; stv_fmt_speed(sp, sizeof sp, 18.4 * 1048576); stv_fmt_eta(eta, sizeof eta, 520, 1); snprintf(v.bar_right, sizeof v.bar_right, "%s · %s", sp, eta);
    char a[32], b[32], n[64]; stv_fmt_int(a, sizeof a, 3612); stv_fmt_int(b, sizeof b, 8544); snprintf(n, sizeof n, "파일 %s / %s", a, b);
    step(&v, 0, STV_RUN, "게임 데이터", n); step(&v, 1, STV_WAIT, "셰이더 준비", "대기"); step(&v, 2, STV_WAIT, "클라우드 세이브", "대기");
    snprintf(v.hint, sizeof v.hint, "와이파이에서 받는 걸 권해요 · 중간에 꺼도 받은 파일은 그대로예요");
    stv_draw(&c, &v, 0.6); save(&c, argv[4], "2-download");
    // 3. the save's conflict
    memset(&q, 0, sizeof q); q.ask = 1; q.ncards = 2; q.sel = 1; q.progress = -1; q.count_permille = -1;
    snprintf(q.title, sizeof q.title, "어느 세이브로 할까요?"); snprintf(q.line[0], sizeof q.line[0], "이 기기와 클라우드에 서로 다른 진행이 있어요");
    snprintf(q.card[0].head, sizeof q.card[0].head, "이 기기 · rp6"); snprintf(q.card[0].ago, sizeof q.card[0].ago, "9일 전"); snprintf(q.card[0].date, sizeof q.card[0].date, "11월 11일 오전 12:03"); snprintf(q.card[0].size, sizeof q.card[0].size, "1.2 MB · 파일 9개");
    snprintf(q.card[1].head, sizeof q.card[1].head, "클라우드 · nova에서 올림"); snprintf(q.card[1].ago, sizeof q.card[1].ago, "11분 전"); snprintf(q.card[1].date, sizeof q.card[1].date, "11월 20일 오후 9:29"); snprintf(q.card[1].size, sizeof q.card[1].size, "6.9 MB · 파일 37개");
    q.card[1].newer = 1; snprintf(q.badge, sizeof q.badge, "더 최근");
    snprintf(q.foot, sizeof q.foot, "고르지 않은 쪽도 백업해 둬요"); snprintf(q.foot2, sizeof q.foot2, "← → 고르기 · A 확정 · B 동기화 없이 플레이");
    head(&v, icon, ""); v.ask = &q; stv_draw(&c, &v, 0); save(&c, argv[4], "3-conflict");
    int kind = 0; assert(stv_hit(&q, W, H, W * 0.75f, H * 0.5f, &kind) == 1 && kind == 1); assert(stv_hit(&q, W, H, W * 0.25f, H * 0.5f, &kind) == 0 && kind == 1);
    assert(stv_hit(&q, W, H, 2, 2, &kind) == -1);
    // 4. the cloud cannot be reached
    memset(&q, 0, sizeof q); q.ask = 1; q.icon = DM_CLOUD_ICON_OFFLINE; q.progress = -1; q.nbtn = 2; q.sel = 0;
    snprintf(q.title, sizeof q.title, "클라우드에 연결할 수 없어요"); snprintf(q.line[0], sizeof q.line[0], "이 기기 세이브로 시작해요. 연결되면 나중에 올려요.");
    snprintf(q.btn[0], sizeof q.btn[0], "그래도 플레이"); snprintf(q.btn[1], sizeof q.btn[1], "다시 시도"); snprintf(q.count, sizeof q.count, "7초 뒤 자동으로 시작해요"); q.count_permille = 300;
    stv_draw(&c, &v, 0); save(&c, argv[4], "4-offline");
    { float sc = W / 1280.0f < H / 720.0f ? W / 1280.0f : H / 720.0f; int seen[2] = {0, 0};   // the buttons, 260 wide 20 apart around the middle
      for (int y = 0; y < H; y += 4) for (int i = 0; i < 2; i++) { int k = stv_hit(&q, W, H, W * 0.5f + (i ? 140 : -140) * sc, (float)y, &kind); if (k >= 0) { assert(k == i && kind == 2); seen[i]++; } }
      assert(seen[0] && seen[1] && seen[0] < 60 * sc / 4 + 2); }
    // 5. another device has progress the cloud has not got
    memset(&q, 0, sizeof q); q.ask = 1; q.icon = DM_CLOUD_ICON_WARN; q.progress = -1; q.nbtn = 2; q.count_permille = -1;
    snprintf(q.title, sizeof q.title, "다른 기기에 아직 안 올라온 진행이 있어요");
    snprintf(q.line[0], sizeof q.line[0], "rp6에서 11월 20일 오후 9:55에 저장한 진행이 아직 클라우드에 없어요."); snprintf(q.line[1], sizeof q.line[1], "그 기기를 인터넷에 연결해 켜면 올라가요.");
    snprintf(q.btn[0], sizeof q.btn[0], "그래도 시작"); snprintf(q.btn[1], sizeof q.btn[1], "취소 (앱 종료)");
    snprintf(q.foot, sizeof q.foot, "그래도 시작하면 나중에 두 세이브 중 하나를 고르게 돼요 · B 동기화 없이 플레이");
    stv_draw(&c, &v, 0); save(&c, argv[4], "5-other-device");
    // 6. the first start without a connection: the data has to come once
    memset(&q, 0, sizeof q); q.ask = 1; q.icon = DM_CLOUD_ICON_OFFLINE; q.progress = -1; q.nbtn = 1; q.count_permille = 400;
    snprintf(q.title, sizeof q.title, "인터넷 연결이 필요해요"); snprintf(q.line[0], sizeof q.line[0], "처음 한 번 게임 데이터를 받아야 해요. 연결되면 바로 이어서 받아요.");
    snprintf(q.btn[0], sizeof q.btn[0], "다시 시도"); snprintf(q.count, sizeof q.count, "12초 뒤 다시 확인해요");
    stv_draw(&c, &v, 0); save(&c, argv[4], "6-need-network");
    // 7. offline with the data whole: the steps go on
    head(&v, NULL, "1.4.1 · 빌드 1");
    step(&v, 0, STV_DONE, "게임 데이터", "오프라인 · 마지막 확인 11월 19일"); step(&v, 1, STV_RUN, "셰이더 준비", "1,204 / 3,980"); step(&v, 2, STV_WAIT, "클라우드 세이브", "대기");
    v.bar = 302; snprintf(v.bar_title, sizeof v.bar_title, "셰이더 준비 중"); snprintf(v.bar_left, sizeof v.bar_left, "1,204 / 3,980"); stv_fmt_eta(v.bar_right, sizeof v.bar_right, 95, 1);
    snprintf(v.hint, sizeof v.hint, "처음 한 번만 해요 · 게임 데이터나 드라이버가 바뀌면 다시 해요");
    stv_draw(&c, &v, 1.1); save(&c, argv[4], "7-offline-shaders");
    // the rate: a steady 10 MB/s reads as 10 MB/s, and the time left as what is left at that speed
    stv_rate r; stv_rate_reset(&r);
    for (int t = 0; t <= 20000; t += 250) stv_rate_feed(&r, t, t * 10485.76, 1e9 - t * 10485.76);
    assert(r.speed > 10.3e6 && r.speed < 10.7e6); assert(r.eta > 72 && r.eta < 79);   // 790 MB left at 10 MB/s
    char e[64]; stv_fmt_eta(e, sizeof e, 3, 1); assert(!strcmp(e, "약 5초 남음")); stv_fmt_eta(e, sizeof e, 61, 0); assert(!strcmp(e, "about 2 min left"));
    stv_fmt_int(e, sizeof e, 1234567); assert(!strcmp(e, "1,234,567"));
    stv_release(); free(c.px); free(reg); free(bold); free(pf);
    return 0;
}
