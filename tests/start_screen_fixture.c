// The start screen (dm/android/startscreen.c) drawn on the computer: each of the design's screens at a given size, saved as PNG (tests/test_start_screen.py builds and
// runs it; with an output folder it keeps the pictures to look at). Usage: start_screen_fixture <regular.ttf> <bold.ttf> <pixel font .nxpf> <out dir> <w> <h>
#include "startscreen.h"
#include "savesync.h"   // the link page's address and its linked line (ss_link_page_url, ss_link_host, ss_link_foot)
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
// The touch areas against the picture: each of the n targets answers a touch somewhere (scanned every 2 px), all inside the window, none overlapping; and where a
// target can be chosen (sel not NULL), what changes when it is chosen (its edge, its fill, the ring 6 design px out) is where it answers a touch, give or take that ring
static const stv_view *TV; static const dm_cloud_view *TQ;
static int hit_at(int W, int H, float x, float y) { int k = 0; return TQ ? stv_hit(TQ, W, H, x, y, &k) : stv_page_hit(TV, W, H, x, y); }
// ids: what each target answers (NULL: 0 .. n - 1), the only answers there may be
static void touch_check_ids(stv_canvas *c, stv_view *v, const int *ids, int n, int *sel, int nsel) {
    int W = c->w, H = c->h, bx[8][4]; assert(n <= 8);
    for (int i = 0; i < n; i++) { bx[i][0] = bx[i][1] = 1 << 30; bx[i][2] = bx[i][3] = -1; }
    for (int y = 0; y < H; y += 2) for (int x = 0; x < W; x += 2) { int h = hit_at(W, H, x + 0.5f, y + 0.5f), k = -1; if (h < 0) continue;
        for (int i = 0; i < n; i++) if ((ids ? ids[i] : i) == h) k = i;
        assert(k >= 0);
        if (x < bx[k][0]) bx[k][0] = x; if (y < bx[k][1]) bx[k][1] = y; if (x > bx[k][2]) bx[k][2] = x; if (y > bx[k][3]) bx[k][3] = y; }
    for (int i = 0; i < n; i++) {
        assert(bx[i][2] >= 0 && bx[i][0] > 4 && bx[i][1] > 4 && bx[i][2] < W - 4 && bx[i][3] < H - 4);
        for (int j = 0; j < i; j++) assert(bx[i][2] < bx[j][0] || bx[j][2] < bx[i][0] || bx[i][3] < bx[j][1] || bx[j][3] < bx[i][1]);
    }
    if (!sel) return;
    float sc = (W / 1280.0f < H / 720.0f ? W / 1280.0f : H / 720.0f) * 1.3f; int keep = *sel; size_t np = (size_t)W * H;
    uint32_t *a = malloc(np * 4); assert(a);
    for (int i = 0; i < nsel; i++) {
        *sel = -1; stv_draw(c, v, 0); memcpy(a, c->px, np * 4); *sel = ids ? ids[i] : i; stv_draw(c, v, 0);
        int d[4] = {1 << 30, 1 << 30, -1, -1};
        for (int y = 0; y < H; y++) for (int x = 0; x < W; x++) if (a[(size_t)y * W + x] != c->px[(size_t)y * W + x]) {
            if (x < d[0]) d[0] = x; if (y < d[1]) d[1] = y; if (x > d[2]) d[2] = x; if (y > d[3]) d[3] = y; }
        int out = (int)(8 * sc) + 3;
        assert(d[2] >= 0);
        assert(d[0] <= bx[i][0] + 2 && d[1] <= bx[i][1] + 2 && d[2] >= bx[i][2] - 2 && d[3] >= bx[i][3] - 2);   // the touch area within what is drawn
        assert(d[0] >= bx[i][0] - out && d[1] >= bx[i][1] - out && d[2] <= bx[i][2] + out && d[3] <= bx[i][3] + out);   // and what is drawn no wider than it and its ring
    }
    free(a); *sel = keep; stv_draw(c, v, 0);
}
static void touch_check(stv_canvas *c, stv_view *v, int n, int *sel, int nsel) { touch_check_ids(c, v, NULL, n, sel, nsel); }
// the QR code is sharp: the box around its pure black pixels holds nothing but pure black and pure white, its modules are whole pixels (the finder's 7-module run on its
// first row a multiple of 7), and 4 modules of pure white are around it
static void qr_check(const stv_canvas *c) {
    int x0 = c->w, y0 = c->h, x1 = -1, y1 = -1;
    for (int y = 0; y < c->h; y++) for (int x = 0; x < c->w; x++) if ((c->px[(size_t)y * c->w + x] & 0xffffff) == 0) { if (x < x0) x0 = x; if (x > x1) x1 = x; if (y < y0) y0 = y; if (y > y1) y1 = y; }
    assert(x1 > x0 + 50 && y1 > y0 + 50 && x1 - x0 == y1 - y0);
    int run = 0; while (x0 + run <= x1 && (c->px[(size_t)y0 * c->w + x0 + run] & 0xffffff) == 0) run++;
    assert(run % 7 == 0); int m = run / 7; assert(m >= 2 && (x1 - x0 + 1) % m == 0);
    for (int y = y0 - 4 * m; y <= y1 + 4 * m; y++) for (int x = x0 - 4 * m; x <= x1 + 4 * m; x++) {
        uint32_t q = c->px[(size_t)y * c->w + x] & 0xffffff; int in = x >= x0 && x <= x1 && y >= y0 && y <= y1;
        assert(q == 0xffffff || (in && q == 0));
    }
    printf("  QR %d modules of %d px\n", (x1 - x0 + 1) / m, m);
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
    TQ = &q; touch_check(&c, &v, 2, &q.sel, 2);
    // 4. the cloud cannot be reached
    memset(&q, 0, sizeof q); q.ask = 1; q.icon = DM_CLOUD_ICON_OFFLINE; q.progress = -1; q.nbtn = 2; q.sel = 0;
    snprintf(q.title, sizeof q.title, "클라우드에 연결할 수 없어요"); snprintf(q.line[0], sizeof q.line[0], "이 기기 세이브로 시작해요. 연결되면 나중에 올려요.");
    snprintf(q.btn[0], sizeof q.btn[0], "그래도 플레이"); snprintf(q.btn[1], sizeof q.btn[1], "다시 시도"); snprintf(q.count, sizeof q.count, "7초 뒤 자동으로 시작해요"); q.count_permille = 300;
    stv_draw(&c, &v, 0); save(&c, argv[4], "4-offline");
    touch_check(&c, &v, 2, &q.sel, 2);
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
    // 8. the files on the device checked by their hashes (a start without the launcher): the bytes so far as the progress block
    head(&v, icon, "1.0.3 · 빌드 2");
    step(&v, 0, STV_RUN, "게임 데이터", "파일 확인 중 · 1.2 / 3.1 GB"); step(&v, 1, STV_WAIT, "셰이더 준비", "대기"); step(&v, 2, STV_WAIT, "클라우드 세이브", "대기");
    v.bar = 387; snprintf(v.bar_title, sizeof v.bar_title, "파일 확인 중"); stv_fmt_bytes(v.bar_left, sizeof v.bar_left, 1288490188ull, 3328599654ull); stv_fmt_eta(v.bar_right, sizeof v.bar_right, 33, 1);
    assert(!strcmp(v.bar_left, "1.2 / 3.1 GB"));
    snprintf(v.hint, sizeof v.hint, "기기에 있는 게임 데이터가 맞는지 확인해요");
    stv_draw(&c, &v, 0.5); save(&c, argv[4], "8-checking-files");
    // 12. the launcher, the start ready: Start chosen (A starts the game)
    stv_launcher *ln = &v.ln;
    head(&v, icon, "1.4.1 · 빌드 3"); v.screen = STV_SCREEN_LAUNCHER;
    static const char *const ST[3] = {"게임 데이터 최신", "셰이더 준비 완료", "클라우드 세이브 최신"};
    for (int i = 0; i < 3; i++) { ln->status[i].state = STV_DONE; snprintf(ln->status[i].text, sizeof ln->status[i].text, "%s", ST[i]); }
    snprintf(ln->item[0], sizeof ln->item[0], "동기화 없이 플레이"); snprintf(ln->note[0], sizeof ln->note[0], "테스트 · 이번 판은 올리지 않아요");
    snprintf(ln->item[1], sizeof ln->item[1], "클라우드 세이브"); snprintf(ln->note[1], sizeof ln->note[1], "자동 · nova에서 3분 전");
    snprintf(ln->item[2], sizeof ln->item[2], "앱 설정"); snprintf(ln->note[2], sizeof ln->note[2], "데이터 6.2 GB · 바로 시작 꺼짐");
    snprintf(ln->start, sizeof ln->start, "게임 시작"); snprintf(ln->keys, sizeof ln->keys, "↑↓ 고르기 · A 선택"); ln->sel = 3;
    stv_draw(&c, &v, 0); save(&c, argv[4], "12-launcher");
    TQ = NULL; TV = &v; touch_check(&c, &v, 4, &ln->sel, 4); assert(stv_page_hit(&v, W, H, 2, 2) == -1);
    // 12 with a long name: smaller to 80 %, then two lines at a space (side by side: the column of 340)
    snprintf(v.name, sizeof v.name, "A열차로 가자 시작해요 관광지 개발"); stv_draw(&c, &v, 0); save(&c, argv[4], "18-launcher-long-name");
    snprintf(v.name, sizeof v.name, "슈퍼로봇대전Y"); stv_draw(&c, &v, 0); save(&c, argv[4], "19-launcher-korean-name");
    snprintf(v.name, sizeof v.name, "The Legend of Heroes: Trails through Daybreak II"); stv_draw(&c, &v, 0); save(&c, argv[4], "20-launcher-two-line-name");
    snprintf(v.name, sizeof v.name, "SUPER ROBOT WARS Y");
    // 13. the shaders not ready yet: Start (chosen, as it is at first) and Play without the sync are off, Start says what runs and fills with its progress
    ln->status[1].state = STV_RUN; snprintf(ln->status[1].text, sizeof ln->status[1].text, "셰이더 준비 중 · 62%%");
    ln->start_off = 1; ln->start_permille = 620; snprintf(ln->start_sub, sizeof ln->start_sub, "셰이더 준비 중 · 62%%"); ln->item_off[0] = 1; v.anim = 1;
    stv_draw(&c, &v, 0.4); save(&c, argv[4], "13-launcher-preparing"); assert(stv_animated(&v));
    { stv_canvas d = {malloc((size_t)W * H * 4), W, H}; assert(d.px); ln->start_off = 0; ln->item_off[0] = 0; stv_draw(&d, &v, 0.4);   // off and on differ in Start and the item
      long diff = 0; for (size_t i = 0; i < (size_t)W * H; i++) diff += d.px[i] != c.px[i]; assert(diff > 1000); free(d.px); }
    ln->start_off = 1; ln->item_off[0] = 1;
    ln->sel = 1; stv_draw(&c, &v, 0.4); save(&c, argv[4], "13a-launcher-preparing-item"); ln->sel = 3;   // (an item chosen: Start off without its ring)
    // 13b. the files on the device checked (no list of them on the device): the top line counts the bytes, Start its share
    ln->status[0].state = STV_RUN; snprintf(ln->status[0].text, sizeof ln->status[0].text, "파일 확인 중 · 1.2 / 3.1 GB");
    ln->status[1].state = STV_WAIT; snprintf(ln->status[1].text, sizeof ln->status[1].text, "셰이더 준비 대기");
    ln->start_permille = 387; snprintf(ln->start_sub, sizeof ln->start_sub, "파일 확인 중 · 38%%");
    stv_draw(&c, &v, 0.7); save(&c, argv[4], "13b-launcher-checking-files");
    // 13c. the cloud's early check beside the data's (the device link, the server's meta): Cloud save off ("확인 중…") until it ends
    ln->status[0].state = STV_RUN; snprintf(ln->status[0].text, sizeof ln->status[0].text, "게임 데이터 확인 중");
    ln->status[2].state = STV_RUN; snprintf(ln->status[2].text, sizeof ln->status[2].text, "클라우드 세이브 확인 중");
    ln->item_off[1] = 1; char keep1[96]; snprintf(keep1, sizeof keep1, "%s", ln->note[1]); snprintf(ln->note[1], sizeof ln->note[1], "확인 중…");
    ln->start_permille = -1; snprintf(ln->start_sub, sizeof ln->start_sub, "서버 확인 중");
    stv_draw(&c, &v, 0.2); save(&c, argv[4], "13c-launcher-cloud-checking");
    ln->sel = 1; stv_draw(&c, &v, 0.2); save(&c, argv[4], "13d-launcher-cloud-checking-chosen"); ln->sel = 3;
    // 13e. the early check ended while the data is still checked: Cloud save on, what the start's check will do in the top line
    ln->item_off[1] = 0; snprintf(ln->note[1], sizeof ln->note[1], "%s", keep1);
    ln->status[2].state = STV_WAIT; snprintf(ln->status[2].text, sizeof ln->status[2].text, "클라우드 세이브 · 시작할 때 받아요");
    stv_draw(&c, &v, 0.2); save(&c, argv[4], "13e-launcher-cloud-checked");
    for (int i = 0; i < 3; i++) { ln->status[i].state = STV_DONE; snprintf(ln->status[i].text, sizeof ln->status[i].text, "%s", ST[i]); }
    ln->start_off = 0; ln->item_off[0] = 0;
    // 14. the cloud save's page
    memset(&v.ln, 0, sizeof v.ln); v.anim = 0; v.screen = STV_SCREEN_CLOUD; stv_cloudpage *cp = &v.cp;
    snprintf(cp->title, sizeof cp->title, "클라우드 세이브"); snprintf(cp->label[0], sizeof cp->label[0], "동기화 방식"); snprintf(cp->label[1], sizeof cp->label[1], "지금 상태");
    static const char *const MN[3] = {"자동", "수동", "이 기기만"}, *const MD[3] = {"저장하면 바로 올리고, 게임을 끌 때도 올려요", "올리기는 내가 정해요. 시작할 때 올릴지 물어봐요", "받지도 올리지도 않아요. 이 기기 세이브로만 테스트해요"};
    for (int i = 0; i < 3; i++) { snprintf(cp->mode[i], sizeof cp->mode[i], "%s", MN[i]); snprintf(cp->desc[i], sizeof cp->desc[i], "%s", MD[i]); }
    snprintf(cp->note, sizeof cp->note, "이 기기만에서 다시 자동·수동으로 바꾸면, 처음 연결할 때처럼 어느 세이브로 할지 물어봐요.");
    static const char *const CK[4] = {"클라우드", "이 기기", "크기", "계정"}, *const CV[4] = {"nova · 오늘 오전 6:41", "클라우드와 같아요", "6.9 MB · 파일 37개", "기기 이름 rp6"};
    cp->nrow = 4; for (int i = 0; i < 4; i++) { snprintf(cp->key[i], sizeof cp->key[i], "%s", CK[i]); snprintf(cp->val[i], sizeof cp->val[i], "%s", CV[i]); } cp->col[1] = STV_COL_GOOD;
    snprintf(cp->up, sizeof cp->up, "지금 올리기"); snprintf(cp->up_note, sizeof cp->up_note, "올릴 진행 없음"); cp->up_off = 1;
    snprintf(cp->back, sizeof cp->back, "클라우드 세이브로 되돌리기"); snprintf(cp->back_note, sizeof cp->back_note, "A 길게 · 이 기기 세이브는 백업 1칸에"); cp->hold = 380;
    snprintf(cp->keys, sizeof cp->keys, "↑↓←→ 고르기 · A 선택 · B 돌아가기"); cp->sel = 0; cp->checked = 0;
    stv_draw(&c, &v, 0); save(&c, argv[4], "14-cloud-settings");
    touch_check(&c, &v, 5, &cp->sel, 5);
    { stv_view e; memset(&e, 0, sizeof e); e.screen = STV_SCREEN_CLOUD; e.cp.nrow = 4;   // datasync_android.c's lnr_hit: a view with nothing but its page
      for (int y = 0; y < H; y += 7) for (int x = 0; x < W; x += 7) assert(stv_page_hit(&e, W, H, x, y) == stv_page_hit(&v, W, H, x, y)); }
    // 21. the cloud save's page, not linked: Link this device at the top of the right column (chosen), the card under it, no Upload now / Back
    { stv_view u = v; stv_cloudpage *up = &u.cp; u.screen = STV_SCREEN_CLOUD; up->link = 1; up->sel = 5; up->modes_off = 1; up->checked = 0; up->hold = 0;
      snprintf(up->lbtn, sizeof up->lbtn, "기기 연결하기"); snprintf(up->lnote, sizeof up->lnote, "휴대폰으로 QR · 코드");
      snprintf(up->note, sizeof up->note, "휴대폰으로 한 번 연결하면 이 기기의 다른 게임도 함께 연결돼요.");
      static const char *const UV[4] = {"-", "확인 전", "6.9 MB · 파일 37개", "연결 안 됨"}; for (int i = 0; i < 4; i++) snprintf(up->val[i], sizeof up->val[i], "%s", UV[i]); up->col[1] = STV_COL_DIM;
      stv_draw(&c, &u, 0); save(&c, argv[4], "21-cloud-unlinked");
      TQ = NULL; TV = &u; static const int ids[4] = {5, 0, 1, 2}; touch_check_ids(&c, &u, ids, 4, &up->sel, 1);   // (the modes cannot be chosen: off)
      stv_view e; memset(&e, 0, sizeof e); e.screen = STV_SCREEN_CLOUD; e.cp.nrow = 4; e.cp.link = 1;   // lnr_hit's view: its page and cp.link only
      for (int y = 0; y < H; y += 7) for (int x = 0; x < W; x += 7) assert(stv_page_hit(&e, W, H, x, y) == stv_page_hit(&u, W, H, x, y));
    // 22. linked: Unlink this app under the card (pressed once: red, the next press unlinks)
      up->link = 2; up->sel = 5; up->modes_off = 0; snprintf(up->lbtn, sizeof up->lbtn, "이 앱 연결 끊기"); snprintf(up->lnote, sizeof up->lnote, "다른 게임과 기기 전체는 웹에서 끊을 수 있어요");
      static const char *const LV[4] = {"nova · 오늘 오전 6:41", "클라우드와 같아요", "6.9 MB · 파일 37개", "기기 이름 Retroid Pocket Nova"}; for (int i = 0; i < 4; i++) snprintf(up->val[i], sizeof up->val[i], "%s", LV[i]); up->col[1] = STV_COL_GOOD;
      snprintf(up->note, sizeof up->note, "이 기기만에서 다시 자동·수동으로 바꾸면, 처음 연결할 때처럼 어느 세이브로 할지 물어봐요.");
      stv_draw(&c, &u, 0); save(&c, argv[4], "22-cloud-linked");
      static const int ids2[6] = {0, 1, 2, 3, 4, 5}; touch_check_ids(&c, &u, ids2, 6, &up->sel, 6);
      e.cp.link = 2; for (int y = 0; y < H; y += 7) for (int x = 0; x < W; x += 7) assert(stv_page_hit(&e, W, H, x, y) == stv_page_hit(&u, W, H, x, y));
      up->larm = 1; snprintf(up->lnote, sizeof up->lnote, "A 한 번 더 누르면 끊어요"); stv_draw(&c, &u, 0); save(&c, argv[4], "22b-cloud-unlink-armed"); }
    // 23. the device link: the QR code, the address, the code and the time left (LinkCode.dc.html)
    { stv_view u; head(&u, icon, ""); u.screen = STV_SCREEN_LINK; stv_linkpage *lp = &u.lk;
      lp->state = STV_LINK_CODE; snprintf(lp->title, sizeof lp->title, "휴대폰으로 이 기기를 연결하세요");
      // (the address from the server's link/start answer, as savecloud_android.c link_thread makes it: its url, the scheme dropped; none in it: <server>/link)
      { const char *JB = "{\"linked\":false,\"code\":\"K7Q-4MP\",\"poll\":\"p0ll\",\"expiresIn\":600,\"url\":\"https://saves.example.com/link\"}"; ss_link sl; char lu[700];
        assert(ss_link_read(200, JB, strlen(JB), &sl) == SS_LK_CODE);
        ss_link_page_url(lu, sizeof lu, sl.url, "https://other.example.com"); ss_link_qr(lp->qr, sizeof lp->qr, lu, sl.code); ss_link_host(lp->host, sizeof lp->host, lu); snprintf(lp->code, sizeof lp->code, "%s", sl.code);
        assert(!strcmp(lp->host, "saves.example.com/link") && !strcmp(lp->qr, "https://saves.example.com/link?code=K7Q-4MP"));
        char h2[160]; ss_link_page_url(lu, sizeof lu, "", "http://lan.example.com:8080"); ss_link_host(h2, sizeof h2, lu); assert(!strcmp(h2, "lan.example.com:8080/link")); }
      snprintf(lp->step1, sizeof lp->step1, "1. 휴대폰 카메라로 QR을 찍거나, 아래 주소로 들어가요"); snprintf(lp->step2, sizeof lp->step2, "2. 카카오로 로그인하고 이 코드를 확인해요");
      snprintf(lp->status, sizeof lp->status, "휴대폰에서 연결을 누르길 기다리는 중 · 9:42 남음"); lp->nbtn = 1; lp->sel = -1; snprintf(lp->btn[0], sizeof lp->btn[0], "취소"); snprintf(lp->keys, sizeof lp->keys, "B 취소");
      stv_draw(&c, &u, 0.2); save(&c, argv[4], "23-link-code"); qr_check(&c);
      TQ = NULL; TV = &u; touch_check(&c, &u, 1, &lp->sel, 1);
      stv_view e; memset(&e, 0, sizeof e); e.screen = STV_SCREEN_LINK; e.lk.state = lp->state; e.lk.nbtn = lp->nbtn;
      for (int y = 0; y < H; y += 7) for (int x = 0; x < W; x += 7) assert(stv_page_hit(&e, W, H, x, y) == stv_page_hit(&u, W, H, x, y));
    // 24. the code ran out: faded, New code (A) and Cancel
      lp->state = STV_LINK_EXPIRED; lp->nbtn = 2; lp->sel = 0; snprintf(lp->btn[0], sizeof lp->btn[0], "새 코드"); snprintf(lp->btn[1], sizeof lp->btn[1], "취소");
      snprintf(lp->status, sizeof lp->status, "코드 시간이 지났어요 · A 새 코드"); snprintf(lp->keys, sizeof lp->keys, "← → 고르기 · A 선택 · B 취소");
      stv_draw(&c, &u, 0); save(&c, argv[4], "24-link-expired"); touch_check(&c, &u, 2, &lp->sel, 2);
      e.lk.state = lp->state; e.lk.nbtn = 2; for (int y = 0; y < H; y += 7) for (int x = 0; x < W; x += 7) assert(stv_page_hit(&e, W, H, x, y) == stv_page_hit(&u, W, H, x, y));
    // 25. linked (LinkDone.dc.html): the check, the device's name, Continue
      memset(lp, 0, sizeof *lp); lp->state = STV_LINK_DONE; snprintf(lp->title, sizeof lp->title, "연결됐어요"); snprintf(lp->line, sizeof lp->line, "이제 저장하면 클라우드에 올라가요.");
      lp->nrow = 2; snprintf(lp->key[0], sizeof lp->key[0], "기기 이름"); snprintf(lp->val[0], sizeof lp->val[0], "Retroid Pocket Nova"); snprintf(lp->key[1], sizeof lp->key[1], "이 게임"); snprintf(lp->val[1], sizeof lp->val[1], "SUPER ROBOT WARS Y");
      ss_link_foot(lp->foot, sizeof lp->foot, "saves.example.com/link", 1); assert(!strcmp(lp->foot, "기기 이름은 웹 saves.example.com/saves 에서 바꾸거나 연결을 끊을 수 있어요")); lp->nbtn = 1; lp->sel = 0; snprintf(lp->btn[0], sizeof lp->btn[0], "계속");
      stv_draw(&c, &u, 0); save(&c, argv[4], "25-link-done"); touch_check(&c, &u, 1, &lp->sel, 1);
      e.lk.state = lp->state; e.lk.nbtn = 1; e.lk.nrow = 2; e.lk.foot[0] = 'x'; for (int y = 0; y < H; y += 7) for (int x = 0; x < W; x += 7) assert(stv_page_hit(&e, W, H, x, y) == stv_page_hit(&u, W, H, x, y));
    // 26. no code came (the server not reached)
      memset(lp, 0, sizeof *lp); lp->state = STV_LINK_FAIL; snprintf(lp->title, sizeof lp->title, "연결 코드를 받지 못했어요"); snprintf(lp->line, sizeof lp->line, "인터넷 연결을 확인하고 다시 해 주세요.");
      lp->nbtn = 2; lp->sel = 0; snprintf(lp->btn[0], sizeof lp->btn[0], "다시 시도"); snprintf(lp->btn[1], sizeof lp->btn[1], "취소"); snprintf(lp->keys, sizeof lp->keys, "← → 고르기 · A 선택 · B 취소");
      stv_draw(&c, &u, 0); save(&c, argv[4], "26-link-fail"); touch_check(&c, &u, 2, &lp->sel, 2); }
    TV = &v;   // (the pages after these: the launcher's view again)
    // 15. the app's settings
    v.screen = STV_SCREEN_APP; stv_apppage *ap = &v.ap;
    snprintf(ap->title, sizeof ap->title, "앱 설정"); snprintf(ap->label[0], sizeof ap->label[0], "시작"); snprintf(ap->label[1], sizeof ap->label[1], "게임 데이터");
    snprintf(ap->quick, sizeof ap->quick, "바로 시작"); snprintf(ap->quick_desc[0], sizeof ap->quick_desc[0], "켜면 이 런처 없이 바로 게임을 시작해요."); snprintf(ap->quick_desc[1], sizeof ap->quick_desc[1], "켤 때 −를 누르고 있으면 런처가 다시 열려요.");
    static const char *const AK[3] = {"게임 데이터", "셰이더 캐시", "마지막 확인"}, *const AV[3] = {"6.2 GB · 최신", "312 MB", "오늘 오전 6:41"};
    ap->nrow = 3; for (int i = 0; i < 3; i++) { snprintf(ap->key[i], sizeof ap->key[i], "%s", AK[i]); snprintf(ap->val[i], sizeof ap->val[i], "%s", AV[i]); }
    snprintf(ap->verify, sizeof ap->verify, "데이터 다시 확인"); snprintf(ap->clear, sizeof ap->clear, "셰이더 캐시 비우기");
    snprintf(ap->note, sizeof ap->note, "데이터 다시 확인은 파일을 하나씩 검사해 망가진 것만 다시 받아요."); snprintf(ap->keys, sizeof ap->keys, "↑↓ 고르기 · A 선택 · B 돌아가기"); ap->sel = -1;
    snprintf(ap->loc, sizeof ap->loc, "데이터 위치"); snprintf(ap->loc_val, sizeof ap->loc_val, "내부 저장소");
    snprintf(ap->loc_desc[0], sizeof ap->loc_desc[0], "내부 저장소 · 여유 7.9 GB"); snprintf(ap->loc_desc[1], sizeof ap->loc_desc[1], "SD 카드 · 여유 328.4 GB");
    stv_draw(&c, &v, 0); save(&c, argv[4], "15-app-settings");
    touch_check(&c, &v, 4, &ap->sel, 4);
    // 15a. the data on the SD card; no card: the place dimmed
    snprintf(ap->loc_val, sizeof ap->loc_val, "SD 카드"); ap->sel = 3; stv_draw(&c, &v, 0); save(&c, argv[4], "15a-app-settings-on-sd");
    snprintf(ap->loc_val, sizeof ap->loc_val, "내부 저장소"); snprintf(ap->loc_desc[1], sizeof ap->loc_desc[1], "SD 카드 없음"); ap->loc_off = 1; stv_draw(&c, &v, 0); save(&c, argv[4], "15e-app-settings-no-sd");
    ap->loc_off = 0; ap->sel = -1;
    // 15b-d. the move: asked, under way, done (datasync_android.c move_view's words)
    { dm_cloud_view mq; memset(&mq, 0, sizeof mq); mq.ask = 1; mq.urgent = 1; mq.progress = -1; mq.count_permille = -1; mq.nbtn = 2; mq.sel = 0;
      snprintf(mq.title, sizeof mq.title, "게임 데이터를 SD 카드로 옮길까요?");
      snprintf(mq.line[0], sizeof mq.line[0], "게임 데이터 6.2 GB를 복사하고 원래 자리의 것은 지워요 (328.4 GB 여유). 다 옮기면 앱이 닫혀요.");
      snprintf(mq.line[1], sizeof mq.line[1], "세이브는 내부 저장소에 그대로 있어요. SD 카드는 내부 저장소보다 느려 불러오기가 길어질 수 있어요.");
      snprintf(mq.btn[0], sizeof mq.btn[0], "옮기기"); snprintf(mq.btn[1], sizeof mq.btn[1], "취소");
      v.ask = &mq; stv_draw(&c, &v, 0); save(&c, argv[4], "15b-app-move-confirm"); TQ = &mq; touch_check(&c, &v, 2, &mq.sel, 2); TQ = NULL;
      memset(&mq, 0, sizeof mq); mq.ask = 1; mq.urgent = 1; mq.progress = -1; mq.nbtn = 1;
      snprintf(mq.title, sizeof mq.title, "SD 카드로 옮기는 중"); snprintf(mq.line[0], sizeof mq.line[0], "다 옮기면 앱이 닫혀요. 옮기는 동안은 게임을 시작할 수 없어요.");
      snprintf(mq.line[1], sizeof mq.line[1], "취소하면 복사한 것을 지우고 원래 자리에 그대로 둬요. 세이브는 내부 저장소에 그대로예요.");
      snprintf(mq.count, sizeof mq.count, "2.6 / 6.2 GB · 42%%"); mq.count_permille = 420; snprintf(mq.btn[0], sizeof mq.btn[0], "취소");
      stv_draw(&c, &v, 0); save(&c, argv[4], "15c-app-moving"); TQ = &mq; touch_check(&c, &v, 1, NULL, 0); TQ = NULL;
      memset(&mq, 0, sizeof mq); mq.ask = 1; mq.urgent = 1; mq.progress = -1; mq.count_permille = -1; mq.nbtn = 1;
      snprintf(mq.title, sizeof mq.title, "옮겼어요"); snprintf(mq.line[0], sizeof mq.line[0], "이제 게임 데이터를 SD 카드에서 읽어요. 앱이 닫히면 다시 열어 주세요."); snprintf(mq.btn[0], sizeof mq.btn[0], "닫기");
      stv_draw(&c, &v, 0); save(&c, argv[4], "15d-app-moved");
      // 27. the card that holds the data is not in (datasync_android.c storage_view)
      memset(&mq, 0, sizeof mq); mq.ask = 1; mq.urgent = 1; mq.icon = DM_CLOUD_ICON_WARN; mq.progress = -1; mq.count_permille = -1; mq.nbtn = 2; mq.sel = 0;
      snprintf(mq.title, sizeof mq.title, "SD 카드를 찾을 수 없어요");
      snprintf(mq.line[0], sizeof mq.line[0], "게임 데이터가 SD 카드에 있어요. 카드를 꽂거나 데이터 위치를 내부 저장소로 바꿔 주세요.");
      snprintf(mq.line[1], sizeof mq.line[1], "내부 저장소로 바꾸면 게임 데이터를 다시 받아요. 세이브는 그대로예요.");
      snprintf(mq.btn[0], sizeof mq.btn[0], "다시 확인"); snprintf(mq.btn[1], sizeof mq.btn[1], "내부 저장소로 바꾸기"); snprintf(mq.foot, sizeof mq.foot, "아직 SD 카드가 없어요");
      stv_draw(&c, &v, 0); save(&c, argv[4], "27-sd-missing"); TQ = &mq; touch_check(&c, &v, 2, &mq.sel, 2); TQ = NULL;
      v.ask = NULL; }
    { stv_view e; memset(&e, 0, sizeof e); e.screen = STV_SCREEN_APP;
      for (int y = 0; y < H; y += 7) for (int x = 0; x < W; x += 7) assert(stv_page_hit(&e, W, H, x, y) == stv_page_hit(&v, W, H, x, y)); }
    // 16. the last run was played without the sync
    memset(&q, 0, sizeof q); q.ask = 1; q.style = DM_CLOUD_STYLE_LIST; q.progress = -1; q.count_permille = -1; q.nbtn = 3; q.sel = 0;
    snprintf(q.title, sizeof q.title, "지난번에 동기화 없이 한 플레이가 있어요"); snprintf(q.line[0], sizeof q.line[0], "테스트로 하셨다면 클라우드 세이브로 되돌리세요");
    q.nrow = 2; snprintf(q.row_k[0], sizeof q.row_k[0], "이 기기 (테스트 플레이)"); snprintf(q.row_v[0], sizeof q.row_v[0], "오늘 오전 2:10 · 7.1 MB");
    snprintf(q.row_k[1], sizeof q.row_k[1], "클라우드"); snprintf(q.row_v[1], sizeof q.row_v[1], "nova · 어제 오후 11:32 · 6.9 MB");
    snprintf(q.btn[0], sizeof q.btn[0], "클라우드 세이브로 되돌리기"); snprintf(q.btn[1], sizeof q.btn[1], "이 기기 것 올리기"); snprintf(q.btn[2], sizeof q.btn[2], "그대로 두기");
    snprintf(q.bnote[1], sizeof q.bnote[1], "클라우드 세이브는 백업해 둬요"); snprintf(q.bnote[2], sizeof q.bnote[2], "이번에도 올리지 않고 플레이");
    snprintf(q.foot, sizeof q.foot, "되돌린 테스트 세이브는 이 기기 백업 1칸에 남아요"); snprintf(q.foot2, sizeof q.foot2, "↑↓ 고르기 · A 선택");
    head(&v, icon, ""); v.ask = &q; stv_draw(&c, &v, 0); save(&c, argv[4], "16-after-test");
    TQ = &q; touch_check(&c, &v, 3, &q.sel, 3);
    // 17. manual: progress not uploaded
    memset(&q, 0, sizeof q); q.ask = 1; q.style = DM_CLOUD_STYLE_PAIR; q.progress = -1; q.count_permille = -1; q.nbtn = 2;
    snprintf(q.title, sizeof q.title, "이 기기에 안 올린 진행이 있어요"); snprintf(q.line[0], sizeof q.line[0], "수동 모드 · 올릴지 정해 주세요");
    q.nrow = 2; snprintf(q.row_k[0], sizeof q.row_k[0], "이 기기"); snprintf(q.row_v[0], sizeof q.row_v[0], "오늘 오전 7:05 · 7.0 MB");
    snprintf(q.row_k[1], sizeof q.row_k[1], "클라우드"); snprintf(q.row_v[1], sizeof q.row_v[1], "이 기기에서 올림 · 오늘 오전 6:41");
    snprintf(q.btn[0], sizeof q.btn[0], "A · 올리기"); snprintf(q.btn[1], sizeof q.btn[1], "B · 그대로 두기"); snprintf(q.foot, sizeof q.foot, "클라우드 세이브 설정에서 자동으로 바꿀 수 있어요");
    stv_draw(&c, &v, 0); save(&c, argv[4], "17-manual-pending");
    touch_check(&c, &v, 2, NULL, 0); assert(stv_hit(&q, W, H, W * 0.5f, 2, &kind) == -1);
    // the rate: a steady 10 MB/s reads as 10 MB/s, and the time left as what is left at that speed
    stv_rate r; stv_rate_reset(&r);
    for (int t = 0; t <= 20000; t += 250) stv_rate_feed(&r, t, t * 10485.76, 1e9 - t * 10485.76);
    assert(r.speed > 10.3e6 && r.speed < 10.7e6); assert(r.eta > 72 && r.eta < 79);   // 790 MB left at 10 MB/s
    char e[64]; stv_fmt_eta(e, sizeof e, 3, 1); assert(!strcmp(e, "약 5초 남음")); stv_fmt_eta(e, sizeof e, 61, 0); assert(!strcmp(e, "about 2 min left"));
    stv_fmt_int(e, sizeof e, 1234567); assert(!strcmp(e, "1,234,567"));
    stv_release(); free(c.px); free(reg); free(bold); free(pf);
    return 0;
}
