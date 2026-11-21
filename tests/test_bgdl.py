# The background download's decisions (nxnative/dm/android/bgdl.h, built on the computer: tests/bgdl_fixture.c): what a failed try waits for (no timed retries without
# a network), the notification's words and bar, how often it is posted, and what the end and Cancel do with the app off the screen. The numbers are the Java service's
# (java_service/org/nx2a/DownloadService.java ST_* / CMD_*): checked against its source here. python -m unittest tests.test_bgdl
import atexit, os, re, shutil, subprocess, tempfile, unittest
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
ANDROID = REPO / 'nxnative/dm/android'
JAVA = ANDROID / 'java_service/org/nx2a/DownloadService.java'
CC = next((w for w in map(shutil.which, [n for n in (os.environ.get('CC'), 'cc', 'clang', 'gcc') if n]) if w), None)
DS_ERR_NET, DS_ERR_SERVER, DS_ERR_PACKAGE, DS_ERR_SPACE, DS_ERR_LOCAL = 3, 4, 5, 6, 9   # datasync.h
TIMER, NET, RESUME, CANCEL = 0, 1, 2, 3
RUN, PAUSED, WAIT_NET, WAIT, DONE = 1, 2, 3, 4, 5

_exe = []
def fx(*args):
    if not _exe:
        d = Path(tempfile.mkdtemp(prefix='nx2a_bgdl_')); atexit.register(shutil.rmtree, d, ignore_errors=True); exe = d / 'fx'
        r = subprocess.run([CC, '-O1', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-std=gnu11', '-I', str(ANDROID), str(Path(__file__).with_name('bgdl_fixture.c')),
                            '-lm', '-o', str(exe)], capture_output=True, text=True)
        assert r.returncode == 0 and not r.stderr.strip(), r.stderr   # the header builds without a warning
        _exe.append(exe)
    return subprocess.run([str(_exe[0]), *map(str, args)], capture_output=True, text=True, check=True).stdout.rstrip('\n')


@unittest.skipUnless(CC, 'no C compiler')
class Plan(unittest.TestCase):
    def plan(self, rc, net, paused=0, cancelled=0, delay=2, progressed=0): return tuple(map(int, fx('plan', rc, net, paused, cancelled, delay, progressed).split()))

    def test_no_network_waits_for_it(self):   # no timer at all: the network's callback (or the player) starts the next try
        self.assertEqual(self.plan(DS_ERR_NET, 0, delay=16), (NET, 0, 2))
        self.assertEqual(self.plan(DS_ERR_NET, 0, progressed=1), (NET, 0, 2))

    def test_server_not_reached_backs_off(self):   # a network, but no answer: 2, 4, 8 ... 60 s; a try that downloaded something starts from 2 again
        d, seen = 2, []
        for _ in range(7): n, w, d = self.plan(DS_ERR_NET, 1, delay=d); self.assertEqual(n, TIMER); seen.append(w)
        self.assertEqual(seen, [2, 4, 8, 16, 32, 60, 60])
        self.assertEqual(self.plan(DS_ERR_NET, 1, delay=60, progressed=1), (TIMER, 2, 4))
        self.assertEqual(self.plan(DS_ERR_NET, -1, delay=8), (TIMER, 8, 16))   # the network not known: as before, timed
        self.assertEqual(self.plan(DS_ERR_SERVER, 0, delay=4), (TIMER, 4, 8))  # an HTTP error is an answer: timed even with the network said gone

    def test_what_only_the_player_fixes(self):   # a full disk, another app's pack, a missing install: every 30 s after the first try
        for rc in (DS_ERR_SPACE, DS_ERR_PACKAGE, DS_ERR_LOCAL): self.assertEqual(self.plan(rc, 1), (TIMER, 2, 30)); self.assertEqual(self.plan(rc, 1, delay=30), (TIMER, 30, 30))

    def test_pause_and_cancel_first(self):
        self.assertEqual(self.plan(DS_ERR_NET, 0, paused=1), (RESUME, 0, 2))
        self.assertEqual(self.plan(DS_ERR_NET, 1, paused=1, cancelled=1)[0], CANCEL)

    def test_finish_cancel_keep(self):
        self.assertEqual([int(fx('finish', v, d)) for v, d in ((1, 0), (0, 0), (0, 1), (1, 1))], [0, 1, 2, 2])   # GO, WAIT, EXIT, EXIT
        self.assertEqual([int(fx('cancel', v, d)) for v, d in ((1, 0), (0, 0), (0, 1))], [0, 1, 1])                # PAUSE on the screen, else EXIT
        self.assertEqual([int(fx('keep', a, b)) for a, b in ((1, 1), (1, 0), (0, 1))], [1, 0, 0])


@unittest.skipUnless(CC, 'no C compiler')
class Note(unittest.TestCase):
    GB = 1 << 30
    def note(self, ko, state, done=0, total=0, eta=-1, wait=0, game='슈퍼로봇대전 Y'):
        s, p, title, text, bp, bc = fx('note', ko, game, state, done, total, eta, wait).split('|'); return int(s), int(p), title, text, bp, bc

    def test_running(self):
        self.assertEqual(self.note(1, RUN, 7 * self.GB, 17 * self.GB, 25 * 60), (RUN, 411, '슈퍼로봇대전 Y', '게임 데이터 받는 중 · 7.0 / 17.0 GB · 약 25분 남음', '일시정지', '취소'))
        self.assertEqual(self.note(0, RUN, 300 << 20, 600 << 20, 3700)[3], 'Downloading the game data · 300 / 600 MB · about 1 h 2 min left')
        self.assertEqual(self.note(1, RUN, 0, 0)[1:4], (-1, '슈퍼로봇대전 Y', '게임 데이터 받는 중'))   # the size not known yet: a moving bar, no numbers
        self.assertEqual(self.note(1, RUN, 20 * self.GB, 17 * self.GB)[1], 1000)                      # never past the end

    def test_waiting_paused_done(self):
        self.assertEqual(self.note(1, PAUSED, self.GB, 2 * self.GB)[1:], (500, '슈퍼로봇대전 Y', '일시정지됨 · 1.0 / 2.0 GB', '이어 받기', '취소'))
        self.assertEqual(self.note(1, WAIT_NET, self.GB, 2 * self.GB)[1:], (-1, '슈퍼로봇대전 Y', '연결을 기다리는 중 · 연결되면 이어서 받아요', '일시정지', '취소'))
        self.assertEqual(self.note(0, WAIT, self.GB, 2 * self.GB, wait=16)[3], 'Trying again in 16 s')
        self.assertEqual(self.note(1, DONE, 2 * self.GB, 2 * self.GB)[1:], (-1, '슈퍼로봇대전 Y', '다운로드 완료 · 눌러서 시작', '', ''))   # no buttons: the tap starts it
        self.assertEqual(self.note(1, RUN, game='')[2], '게임 데이터')

    def test_korean_words_are_in_the_start_font(self):   # the notification uses the system's font, but the same words show on the start screen's Noto subset (2,350 syllables)
        hangul = {chr(c) for c in range(0xAC00, 0xD7A4)}
        ks = {bytes([a, b]).decode('euc-kr') for a in range(0xB0, 0xC9) for b in range(0xA1, 0xFF)}
        src = (ANDROID / 'bgdl.h').read_text(encoding='utf-8')
        self.assertFalse({ch for ch in src if ch in hangul} - ks)

    def test_due(self):
        due = lambda *a: int(fx('due', *a))
        self.assertEqual(due(5000, 0, RUN, 0, 0, -1), 1)            # the first
        self.assertEqual(due(5100, 5000, PAUSED, RUN, 10, 10), 1)  # a new state at once
        self.assertEqual(due(5500, 5000, RUN, RUN, 11, 10), 0)     # at most once a second
        self.assertEqual(due(6100, 5000, RUN, RUN, 11, 10), 1)
        self.assertEqual(due(9000, 5000, RUN, RUN, 10, 10), 0)     # nothing changed
        self.assertEqual(due(15000, 5000, RUN, RUN, 10, 10), 1)    # the time left, every 10 s


class JavaNumbers(unittest.TestCase):   # bgdl.h's enums are the service's constants
    def test_same_numbers(self):
        h = (ANDROID / 'bgdl.h').read_text(encoding='utf-8'); j = JAVA.read_text(encoding='utf-8')
        for prefix, names in (('ST', ('RUN', 'PAUSED', 'WAIT_NET', 'WAIT', 'DONE')), ('CMD', ('PAUSE', 'RESUME', 'CANCEL', 'NET_UP', 'NET_DOWN', 'STOPPED'))):
            m = re.search(r'enum \{ BG_%s_\w+ = (\d+), ([^}]*)\}' % prefix, h); first = int(m.group(1))
            order = [x.strip().removeprefix(f'BG_{prefix}_') for x in m.group(0)[len('enum {'):-1].split(',')]
            order = [x.split('=')[0].strip() for x in order]
            for i, n in enumerate(order):
                if n in names: self.assertRegex(j, rf'static final int {prefix}_{n} = {first + i};', n)


if __name__ == '__main__':
    unittest.main()
