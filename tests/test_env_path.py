"""An env.txt path into another package's files folder read as the app's own (nxnative/dm/android/env_path.h), without a device: a data pack ported under the old package,
read by an APK that porter apk-update --package renamed."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


DM = Path(__file__).resolve().parents[1] / 'nxnative/dm'


class EnvPath(unittest.TestCase):
    def test_other_package_read_as_own(self):
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc:
            self.skipTest('a C compiler is needed for the env path check')
        harness = r'''
#include <assert.h>
#include "env_path.h"
#define OWN "/storage/emulated/0/Android/data/kr.wook.srw.srwvswitchns/files"
static const char *own(const char *v) { static char b[300]; return dm_env_own_path(v, OWN, b, sizeof b) ? b : NULL; }
int main(void) {
    const char *r;
    // the pack's own files under the package it was ported with
    r = own("/sdcard/Android/data/org.nx2a.t0100ca400e300000/files/standard.ttf"); assert(r && !strcmp(r, OWN "/standard.ttf"));
    r = own("/sdcard/Android/data/org.nx2a.t0100ca400e300000/files/aoc"); assert(r && !strcmp(r, OWN "/aoc"));
    r = own("/sdcard/Android/data/org.nx2a.t0100ca400e300000/files"); assert(r && !strcmp(r, OWN));
    r = own("/storage/emulated/0/Android/data/org.nx2a.t01006c900cc60000/files/RomFS-overlay/a b.bin"); assert(r && !strcmp(r, OWN "/RomFS-overlay/a b.bin"));
    // the app's own folder (either spelling), anything else, and what does not fit stay as they are
    assert(!own("/sdcard/Android/data/kr.wook.srw.srwvswitchns/files/standard.ttf"));
    assert(!own(OWN "/standard.ttf"));
    assert(!own("/sdcard/Android/data/org.nx2a.t0100ca400e300000/filesx/standard.ttf"));   // not the files folder
    assert(!own("/sdcard/Android/data/org.nx2a.t0100ca400e300000/cache/x"));
    assert(!own("/sdcard/Android/data//files/x"));                                          // no package
    assert(!own("/sdcard/Android/data/org.nx2a.t0100ca400e300000"));                        // the package's folder, not its files
    assert(!own("Android/data/org.nx2a.t0100ca400e300000/files/x"));                         // not absolute
    assert(!own("/sdcard/roms/switch/x.nsp") && !own("1") && !own("") && !own("libvulkan.purple.so"));
    char small[20]; assert(!dm_env_own_path("/sdcard/Android/data/a.b/files/x", OWN, small, sizeof small));
    // own_root that is no files folder: nothing is rewritten
    char b[300]; assert(!dm_env_own_path("/sdcard/Android/data/a.b/files/x", "/data/user/0/a.c/files", b, sizeof b));
    assert(!dm_env_own_path("/sdcard/Android/data/a.b/files/x", OWN "/sub", b, sizeof b));
    // the data on an SD card (storage_android.c): the pack's internal paths of its own package read as the card's, another package's too; a path on the card stays
#define SD "/storage/6AA8-165A/Android/data/kr.wook.srw.srwvswitchns/files"
    assert(dm_env_own_path("/sdcard/Android/data/kr.wook.srw.srwvswitchns/files/aoc", SD, b, sizeof b) && !strcmp(b, SD "/aoc"));
    assert(dm_env_own_path(OWN "/standard.ttf", SD, b, sizeof b) && !strcmp(b, SD "/standard.ttf"));
    assert(dm_env_own_path("/storage/self/primary/Android/data/kr.wook.srw.srwvswitchns/files", SD, b, sizeof b) && !strcmp(b, SD));
    assert(dm_env_own_path("/data/media/0/Android/data/kr.wook.srw.srwvswitchns/files/x", SD, b, sizeof b) && !strcmp(b, SD "/x"));
    assert(dm_env_own_path("/sdcard/Android/data/org.nx2a.t0100ca400e300000/files/x", SD, b, sizeof b) && !strcmp(b, SD "/x"));
    assert(!dm_env_own_path(SD "/RomFS", SD, b, sizeof b));                                                       // the data's own folder
    assert(!dm_env_own_path("/storage/6AA8-165A/Android/data/kr.wook.srw.srwvswitchns/files/RomFS", OWN, b, sizeof b));   // --romfs-at sd: the card's RomFS stays
    assert(!dm_env_own_path("/storage/emulated/10x/Android/data/kr.wook.srw.srwvswitchns/files/x", SD, b, sizeof b));     // not internal storage's spelling
    // dm_env_moved_path: a card's path of the app's own that is gone, read in the data's folder when that has it
    assert(!dm_env_moved_path(SD "/RomFS", OWN, b, sizeof b));   // (neither there)
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src, exe = Path(tmp) / 'envpath.c', Path(tmp) / 'envpath'
            src.write_text(harness)
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', f'-I{DM / "android"}', str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_moved_path_on_disk(self):   # the card's RomFS row after the data moved back to internal storage: read there once the card's folder is gone
        cc = shutil.which('cc') or shutil.which('clang')
        if not cc: self.skipTest('a C compiler is needed')
        with tempfile.TemporaryDirectory() as tmp:
            own = Path(tmp) / 'storage/emulated/0/Android/data/a.b/files'; card = Path(tmp) / 'storage/AB-CD/Android/data/a.b/files'
            (own / 'RomFS').mkdir(parents=True)
            harness = f'''
#include <assert.h>
#include <sys/stat.h>
#include "env_path.h"
int main(void) {{
    char b[600];
    assert(dm_env_moved_path("{card}/RomFS", "{own}", b, sizeof b) && !strcmp(b, "{own}/RomFS"));
    assert(!dm_env_moved_path("{card}/aoc", "{own}", b, sizeof b));                  // not in the data's folder either: as it is
    assert(!dm_env_moved_path("{card.parent.parent}/c.d/files/RomFS", "{own}", b, sizeof b));   // another package: dm_env_own_path's
    mkdir("{card.parent.parent.parent.parent}", 0755); mkdir("{card.parent.parent.parent}", 0755); mkdir("{card.parent.parent}", 0755); mkdir("{card.parent}", 0755); mkdir("{card}", 0755); mkdir("{card}/RomFS", 0755);
    assert(!dm_env_moved_path("{card}/RomFS", "{own}", b, sizeof b));                // the card's is there: it stays
    return 0;
}}
'''
            src, exe = Path(tmp) / 'moved.c', Path(tmp) / 'moved'
            src.write_text(harness)
            subprocess.run([cc, '-std=c11', '-Wall', '-Wextra', '-Werror', '-Wno-unused-function', f'-I{DM / "android"}', str(src), '-o', str(exe)], check=True)
            subprocess.run([str(exe)], check=True)

    def test_front_uses_it(self):   # env_file reads every env.txt row through it
        src = (DM / 'android/front_android.c').read_text(encoding='utf-8')
        self.assertIn('#include "env_path.h"', src)
        body = src[src.index('static void env_file('):]; body = body[:body.index('\n}\n')]
        self.assertIn('dm_env_own_path(eq + 1, g_root', body)


if __name__ == '__main__':
    unittest.main()
