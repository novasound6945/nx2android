// This fork's pause menu letters (gfx_vk_present.c, included there once before ov_menu_panel): half as large again as upstream's. The panel is rasterized at the
// window's resolution (a texel a window pixel: ov_panel_copy), its letters are three times the scale upstream's half-resolution raster gives them (ovm_ref_scale:
// upstream's sizing, the largest at which every row fits), and the rows make room for them: the controls grow to hold their words (ovm_geo), and the panel grows
// wider where the screen has room (overlay_layout.h ov_menu_rect, dm_menu_row_need); only where that is not enough do the letters get smaller
//
// the letters' size of the half-resolution raster (rh: a row's height in window pixels, rh / 2 texels there): upstream's ov_menu_panel sizing (0.30 of a row,
// smaller until every row fits beside its control in a panel OV_MENU_W window heights wide)
static int ovm_ref_scale(float rh) {
    float u = rh / OV_MENU_ROW, h = rh / 2, rw = (OV_MENU_W - 2 * OV_MENU_PAD) * u / 2, gap = h * 0.35f, sh = h * 0.40f, sw = sh * 1.85f;
    int is = (int)fmaxf(1, roundf(h * 0.30f / PF_CAP));
    for (; is > 1; is--) {   // the widest row: its label, a gap and its control
        float need = 0;
        for (int i = 0; i < OVM_RESUME; i++) {
            float lw = cv_text_w(ovm_label(i), is); const char *btn = ovm_button(i);
            float cw = ov_val_n(i) ? ov_val_w(i, is) + 2 * sh
                     : i == OVM_MODE ? 2 * (fmaxf(cv_text_w(ovm_mode(0), is), cv_text_w(ovm_mode(1), is)) + sh)
                     : i == OVM_PAD  ? 2 * (fmaxf(cv_text_w(ovm_pad(0), is),  cv_text_w(ovm_pad(1), is))  + sh)
                     : btn ? cv_text_w(btn, is) + 2 * sh
                     : sw + gap + fmaxf(cv_text_w(TR("ON", "开"), is), cv_text_w(TR("OFF", "关"), is));
            need = fmaxf(need, i == OVM_OPACITY ? fmaxf(lw / 0.38f, cv_text_w("80%", is) / 0.14f) : lw + gap + cw + h * 0.3f);
        }
        float tabw = 0; for (int k = 0; k < OVM_TABS; k++) tabw = fmaxf(tabw, cv_text_w(OVM_TAB_NAMES[k], is));
        if (need <= rw && tabw < rw / OVM_TABS - sh) break;
    }
    return is;
}
// a row's parts at letter scale is (rh: the row's height): the gap after a label, a switch's height (also a segment's and a button's padding) and width, a segment
// bar's or a pill's height, a button's, the tabs' bar's. As upstream's (of rh) while the letters fit in them; taller for larger letters (their capitals x 1.55)
typedef struct { float gap, sh, sw, hh, bh, th; } ovm_geo;
static ovm_geo ovm_geo_of(float rh, int is) {
    float ch = (float)(PF_CAP * is) * 1.55f; ovm_geo g;
    g.gap = rh * 0.35f; g.sh = fminf(rh * 0.6f, fmaxf(rh * 0.40f, PF_CAP * is)); g.sw = g.sh * 1.85f;
    g.hh = fminf(rh * 0.9f, fmaxf(rh * 0.46f, ch)); g.bh = fminf(rh * 0.9f, fmaxf(rh * 0.60f, ch)); g.th = fminf(rh * 0.9f, fmaxf(rh * 0.62f, ch));
    return g;
}
static int ovm_has_row(int it) {   // the item has a row in a tab (the audio output's only where the app has outputs to choose: ov_menu_item)
    for (int t = 0; t < OVM_TABS; t++) for (int k = 0; k < OVM_PER_TAB; k++) if (OVM_TAB_ITEMS[t][k] == it) return it != OVM_AUDIO || dm_menu_audio_available();
    return 0;
}
// the width the rows need at letter scale is: the widest row (its label, a gap, its control, the margins), and the tabs' names in their quarters
static float ovm_need(float rh, int is) {
    ovm_geo g = ovm_geo_of(rh, is); float need = 0;
    for (int k = 0; k < OVM_TABS; k++) need = fmaxf(need, OVM_TABS * (cv_text_w(OVM_TAB_NAMES[k], is) + g.sh));
    for (int i = 0; i < OVM_RESUME; i++) {
        if (!ovm_has_row(i)) continue;
        float lw = cv_text_w(ovm_label(i), is), cw; const char *btn = ovm_button(i);
        if (i == OVM_OPACITY) { need = fmaxf(need, fmaxf(lw / 0.38f, (cv_text_w("80%", is) + rh * 0.40f) / 0.16f)); continue; }   // (the track: 0.40 to 0.84 of the row, the knob past its end, the value at the right)
        if (ov_val_n(i)) cw = ov_val_w(i, is) + 2 * g.sh;
        else if (i == OVM_MODE) cw = 2 * (fmaxf(cv_text_w(ovm_mode(0), is), cv_text_w(ovm_mode(1), is)) + g.sh);
        else if (i == OVM_PAD) cw = 2 * (fmaxf(cv_text_w(ovm_pad(0), is), cv_text_w(ovm_pad(1), is)) + g.sh);
        else if (btn) cw = cv_text_w(btn, is) + 2 * g.sh;
        else cw = g.sw + g.gap + fmaxf(cv_text_w(TR("ON", "开"), is), cv_text_w(TR("OFF", "关"), is));
        need = fmaxf(need, rh * 0.15f + lw + g.gap + cw + rh * 0.15f);
    }
    return need;
}
// the menu's letters for a row rh high and rw wide (0: as wide as they need): three times the half-resolution raster's (half as large again on screen), smaller only
// where a line (the font's 12 rows: capitals and descenders) would take more than 0.85 of a row (a small window's short rows), or the widest row would not fit in rw
static int ovm_letters(float rh, float rw) {
    int is = 3 * ovm_ref_scale(rh);
    while (is > 1 && PF_CAP * is * 4 > rh * 0.85f * 3) is--;
    while (rw > 0 && is > 1 && ovm_need(rh, is) > rw) is--;
    return is;
}
float dm_menu_row_need(float rh) { return ovm_need(rh, ovm_letters(rh, 0)); }   // overlay_layout.h ov_menu_rect: the panel as wide as its rows need
