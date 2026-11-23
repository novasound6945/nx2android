// The pause menu's Exit game (menu_exit.h), gfx_vk_present.c's part (fork_draw_android.inc includes it, before ov_menu_panel). The button row r (its line drawn already): Exit game
// (left) beside Resume (right), each centred in its half (front_android.c menu_touch splits the row at the same midline). Exit game is grey, a quieter word than
// Resume; held, its half fills red from the left and reads Hold to exit (a tap shows that for a moment), then Exiting... while the cloud save goes up. The highlight
// (selected: the row is the controller's): the half a controller has chosen (Resume when the menu opens)
static void menu_exit_row(const ov_canvas *cv, const float r[4], int selected, int is) {
    float rh = r[3], rw = r[2], cy = r[1] + rh / 2, f; int chosen, ex = dm_menu_exit(&f, &chosen), half = selected ? (chosen ? 0 : 1) : -1;
    if (half >= 0) cv_rrect(cv, r[0] + half * (rw / 2), r[1] + rh * 0.08f, r[0] + (half + 1) * (rw / 2), r[1] + rh * 0.92f, rh * 0.14f, C_HI);
    if (ex == OV_EXIT_HOLDING || ex == OV_EXIT_GOING) {
        static const uint8_t track[4] = {255, 59, 48, 60}, red[4] = {255, 59, 48, 255};
        float y0 = r[1] + rh * 0.12f, y1 = r[1] + rh * 0.88f, rad = rh * 0.14f, w = rw / 2 - rh * 0.1f, fx = fmaxf(2 * rad, w * f);
        cv_rrect(cv, r[0] + rh * 0.05f, y0, r[0] + rh * 0.05f + w, y1, rad, track);
        cv_rrect(cv, r[0] + rh * 0.05f, y0, r[0] + rh * 0.05f + fx, y1, rad, red);
    }
    const char *const w[2] = {ex == OV_EXIT_GOING ? TR("Exiting…", "正在退出…") : ex != OV_EXIT_IDLE ? TR("Hold to exit", "长按退出") : TR("Exit game", "退出游戏"), ovm_label(OVM_RESUME)};
    for (int k = 0; k < 2; k++) { int s = is; while (s > 1 && cv_text_w(w[k], s) > rw / 2 - rh * 0.3f) s--;   // the letters shrink only if the half would not hold them
        cv_text(cv, r[0] + k * (rw / 2) + (rw / 2 - cv_text_w(w[k], s)) / 2, cy, w[k], s, k ? C_ACCENT : ex == OV_EXIT_IDLE ? C_DIM : C_WHITE); }
}
