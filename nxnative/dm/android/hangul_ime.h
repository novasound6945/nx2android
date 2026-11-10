// Hangul (two-set, dubeolsik) composition for the text entry's keyboard (front_android.c tx_jamo / tx_del): compatibility jamo (U+3131..U+3163) in, the syllable
// being composed out. The caller keeps that syllable as its text's last character and replaces it as the jamo come; a finished syllable is plain text.
// No Vulkan or Android here: tests/test_hangul_ime.py runs it on the computer.
// Rules: initial, medial, final; the compound vowels (oa oae oi uo ue ui eui) and the compound finals (gs nj nh lg lm lb ls lt lp lh bs); a vowel after a final
// takes the final (a compound's second consonant) as its initial; the tense consonants are typed with shift (g + g stays two), and only kk and ss are finals;
// a consonant no final takes, a lone consonant or a lone vowel starts a new syllable; a lone jamo shows as itself. Backspace takes one jamo away.
#ifndef HANGUL_IME_H
#define HANGUL_IME_H
#include <stdint.h>
#define HG_CONS 0x3131   // the 30 compatibility consonants, then the 21 vowels (in the medials' order)
#define HG_VOW 0x314F
#define HG_SYL 0xAC00
typedef struct { int l, v, t; } hg_state;   // initial 0..18 or -1, medial 0..20 or -1, final 1..27 or 0 (none)
static const int8_t HG_CHO_OF[30] = {0, 1, -1, 2, -1, -1, 3, 4, 5, -1, -1, -1, -1, -1, -1, -1, 6, 7, 8, -1, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18};   // consonant → initial, -1 none
static const int8_t HG_JONG_OF[30] = {1, 2, 3, 4, 5, 6, 7, 0, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 0, 18, 19, 20, 21, 22, 0, 23, 24, 25, 26, 27};   // consonant → final, 0 none
static const uint8_t HG_CHO_CP[19] = {0, 1, 3, 6, 7, 8, 16, 17, 18, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29};   // initial → consonant (from HG_CONS)
static const uint8_t HG_JONG_CP[28] = {0, 0, 1, 2, 3, 4, 5, 6, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 19, 20, 21, 22, 23, 25, 26, 27, 28, 29};   // final → consonant
static const uint8_t HG_JPAIR[11][3] = {{3, 1, 9}, {5, 4, 12}, {6, 4, 18}, {9, 8, 0}, {10, 8, 6}, {11, 8, 7}, {12, 8, 9}, {13, 8, 16}, {14, 8, 17}, {15, 8, 18}, {18, 17, 9}};   // compound final, its final, its second as an initial
static const uint8_t HG_VPAIR[7][3] = {{9, 8, 0}, {10, 8, 1}, {11, 8, 20}, {14, 13, 4}, {15, 13, 5}, {16, 13, 20}, {19, 18, 20}};   // compound vowel, its first, its second

static inline void hg_reset(hg_state *s) { s->l = s->v = -1; s->t = 0; }
static inline int hg_active(const hg_state *s) { return s->l >= 0 || s->v >= 0; }   // a syllable is being composed
static inline int hg_is_jamo(uint32_t c) { return c >= HG_CONS && c <= 0x3163; }
static inline uint32_t hg_cur(const hg_state *s) {   // the syllable being composed, a lone jamo as itself; 0 when none
    if (s->l >= 0 && s->v >= 0) return HG_SYL + (uint32_t)((s->l * 21 + s->v) * 28 + s->t);
    return s->l >= 0 ? HG_CONS + HG_CHO_CP[s->l] : s->v >= 0 ? HG_VOW + (uint32_t)s->v : 0;
}
static inline int hg_jpair(int t, int l) { for (int i = 0; i < 11; i++) if (HG_JPAIR[i][1] == t && HG_JPAIR[i][2] == l) return HG_JPAIR[i][0]; return 0; }
static inline int hg_vpair(int a, int b) { for (int i = 0; i < 7; i++) if (HG_VPAIR[i][1] == a && HG_VPAIR[i][2] == b) return HG_VPAIR[i][0]; return -1; }
static inline int hg_start(hg_state *s, uint32_t j, uint32_t *out) {   // a new syllable from jamo j
    hg_reset(s);
    if (j >= HG_VOW) s->v = (int)(j - HG_VOW); else s->l = HG_CHO_OF[j - HG_CONS];   // a consonant no initial takes (a compound) stays as itself, composing nothing
    *out = j; return 2;
}
// Jamo j typed: out[0] replaces the syllable being composed (or is added when none was); a return of 2 adds out[1] after it as the new syllable; 0: j is no jamo
static inline int hg_feed(hg_state *s, uint32_t j, uint32_t out[2]) {
    if (!hg_is_jamo(j)) return 0;
    if (!hg_active(s)) { hg_start(s, j, &out[0]); return 1; }
    out[0] = hg_cur(s);
    if (j < HG_VOW) {   // a consonant: the syllable's final, its compound final, or the next syllable's initial
        int k = (int)(j - HG_CONS);
        if (s->l >= 0 && s->v >= 0) {
            int t = !s->t ? HG_JONG_OF[k] : HG_CHO_OF[k] >= 0 ? hg_jpair(s->t, HG_CHO_OF[k]) : 0;
            if (t) { s->t = t; out[0] = hg_cur(s); return 1; }
        }
        return hg_start(s, j, &out[1]);
    }
    int v = (int)(j - HG_VOW);
    if (s->v >= 0 && !s->t) {   // a vowel after a vowel: the compound, or a lone vowel
        int c = hg_vpair(s->v, v);
        if (c >= 0) { s->v = c; out[0] = hg_cur(s); return 1; }
        return hg_start(s, j, &out[1]);
    }
    if (s->v < 0) { s->v = v; out[0] = hg_cur(s); return 1; }   // the initial's medial
    int l = HG_CHO_OF[HG_JONG_CP[s->t]], t = 0;   // after a final: it (or a compound's second) moves to the new syllable
    for (int i = 0; i < 11; i++) if (HG_JPAIR[i][0] == s->t) { l = HG_JPAIR[i][2]; t = HG_JPAIR[i][1]; }
    s->t = t; out[0] = hg_cur(s); hg_reset(s); s->l = l; s->v = v; out[1] = hg_cur(s); return 2;
}
// Backspace: 0 nothing is being composed (the caller erases a character), 1 the syllable is now *out, 2 the syllable is gone
static inline int hg_back(hg_state *s, uint32_t *out) {
    if (!hg_active(s)) return 0;
    if (s->t) { int t = 0; for (int i = 0; i < 11; i++) if (HG_JPAIR[i][0] == s->t) t = HG_JPAIR[i][1]; s->t = t; }
    else if (s->v >= 0) { int v = -1; for (int i = 0; i < 7; i++) if (HG_VPAIR[i][0] == s->v) v = HG_VPAIR[i][1]; s->v = v; }
    else s->l = -1;
    if (!hg_active(s)) { hg_reset(s); return 2; }
    *out = hg_cur(s); return 1;
}
#endif
