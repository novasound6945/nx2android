// nxnative/dm/android/bgdl.h on the computer (tests/test_bgdl.py builds and drives it): each command prints one line.
// bgdl_fixture plan <rc> <net> <paused> <cancelled> <delay> <progressed>   -> "<next> <wait_s> <delay>"
// bgdl_fixture note <ko> <game> <state> <done> <total> <eta> <wait_s>       -> "<state>|<permille>|<title>|<text>|<pause>|<cancel>"
// bgdl_fixture notep <ko> <game> <state> <phase> <done> <total> <eta> <wait_s>   -> as note (bg_note_phase)
// bgdl_fixture due <now> <last> <state> <last_state> <permille> <last_permille>   -> "0" or "1"
// bgdl_fixture finish <visible> <detached> | cancel <visible> <detached> | keep <downloading> <service_on>   -> the number
#include "bgdl.h"
#include <stdlib.h>

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    const char *c = argv[1]; long long a[8] = {0};
    for (int i = 2; i < argc && i < 10; i++) a[i - 2] = strtoll(argv[i], NULL, 10);
    if (!strcmp(c, "plan") && argc == 8) { bg_plan_t p = bg_plan((int)a[0], (int)a[1], (int)a[2], (int)a[3], (int)a[4], (int)a[5]); printf("%d %d %d\n", p.next, p.wait_s, p.delay); return 0; }
    if (!strcmp(c, "note") && argc == 9) {
        bg_note_t n; bg_note(&n, atoi(argv[2]), argv[3], atoi(argv[4]), strtoull(argv[5], NULL, 10), strtoull(argv[6], NULL, 10), atof(argv[7]), atoi(argv[8]));
        printf("%d|%d|%s|%s|%s|%s\n", n.state, n.permille, n.title, n.text, n.b_pause, n.b_cancel); return 0;
    }
    if (!strcmp(c, "notep") && argc == 10) {
        bg_note_t n; bg_note_phase(&n, atoi(argv[2]), argv[3], atoi(argv[4]), atoi(argv[5]), strtoull(argv[6], NULL, 10), strtoull(argv[7], NULL, 10), atof(argv[8]), atoi(argv[9]));
        printf("%d|%d|%s|%s|%s|%s\n", n.state, n.permille, n.title, n.text, n.b_pause, n.b_cancel); return 0;
    }
    if (!strcmp(c, "due") && argc == 8) { printf("%d\n", bg_due(a[0], a[1], (int)a[2], (int)a[3], (int)a[4], (int)a[5])); return 0; }
    if (!strcmp(c, "finish") && argc == 4) { printf("%d\n", bg_finish((int)a[0], (int)a[1])); return 0; }
    if (!strcmp(c, "cancel") && argc == 4) { printf("%d\n", bg_cancel((int)a[0], (int)a[1])); return 0; }
    if (!strcmp(c, "keep") && argc == 4) { printf("%d\n", bg_keep_alive((int)a[0], (int)a[1])); return 0; }
    return 2;
}
