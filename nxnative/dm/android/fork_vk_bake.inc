// This fork's: the driver's own shader cache on disk, and the files of the bake that fills it at port time (the baking itself: gfx_vk_gpl.inc gpl_bake).
//
// Mesa (the Turnip the APK carries) keeps compiled shaders in a database on disk that it reads on demand: one index in memory, each entry read when a pipeline asks
// for it, keyed by what the pipeline is made from. Android leaves it off. It is turned on here, before the driver loads, when the data folder has DM_ROOT/mesa: the
// folder a port's bake made (DM_VK_GPL_BAKE=1: every shader part of the bundle compiled once, on a device with the same driver and GPU) and the data pack carries.
// A part made later, at a draw or when the game sets its program, then comes out of that file where it would have compiled: FINAL FANTASY RESONANCE (73,512 shaders,
// too many to prepare at the first start) compiled 10-40 ms a part on a Retroid Pocket 6, and the game sets a program only 10-250 ms before its first draw.
// The database is the driver's own: its folder is mesa_shader_cache_sf/<the driver's cache UUID>/<the GPU's name>, so another driver or GPU finds nothing there and
// compiles as before. nx2a.foz / nx2a_idx.foz is the baked one, read only (MESA_DISK_CACHE_READ_ONLY_FOZ_DBS); what the driver compiles besides goes to its own
// foz_cache.foz next to it. DM_VK_MESA_CACHE=0 (start-up): left off. Rows of env.txt with Mesa's own names win over what is set here
#include <dirent.h>
#define MESA_BAKED "nx2a"
// each driver folder under DM_ROOT/mesa (mesa_shader_cache_sf/<cache UUID>/<GPU name>, made by the driver when it starts)
static void mesa_dirs(void (*fn)(const char *dir, void *u), void *u) {
    const char *root = getenv("DM_ROOT"); char a[1200], b[1500], c[1800]; if (!root) return;
    snprintf(a, sizeof a, "%s/mesa/mesa_shader_cache_sf", root);
    DIR *da = opendir(a); if (!da) return;
    for (struct dirent *ea; (ea = readdir(da)); ) { if (ea->d_name[0] == '.') continue;
        snprintf(b, sizeof b, "%s/%s", a, ea->d_name); DIR *db = opendir(b); if (!db) continue;
        for (struct dirent *eb; (eb = readdir(db)); ) { if (eb->d_name[0] == '.') continue;
            struct stat st; snprintf(c, sizeof c, "%s/%s", b, eb->d_name); if (!stat(c, &st) && S_ISDIR(st.st_mode)) fn(c, u); }
        closedir(db); }
    closedir(da);
}
static long long mesa_size(const char *dir, const char *name) { char p[2000]; struct stat st; snprintf(p, sizeof p, "%s/%s", dir, name); return stat(p, &st) ? -1 : (long long)st.st_size; }
static int mesa_mv(const char *dir, const char *from, const char *to) {
    char a[2000], b[2000]; snprintf(a, sizeof a, "%s/%s", dir, from); snprintf(b, sizeof b, "%s/%s", dir, to);
    if (!rename(a, b)) return 1;
    VKLOG("[RED] the driver's shader cache: %s could not be renamed to %s (%s)", a, to, strerror(errno)); return 0;
}
// a bake that starts: an earlier bake's database becomes the driver's own again, so the bake goes on from it (the parts in it are found, not compiled again);
// the little the driver compiled besides since is dropped
static void mesa_bake_resume(const char *dir, void *u) {
    (void)u; if (mesa_size(dir, MESA_BAKED ".foz") <= 0) return;
    char p[2000]; snprintf(p, sizeof p, "%s/foz_cache.foz", dir); unlink(p); snprintf(p, sizeof p, "%s/foz_cache_idx.foz", dir); unlink(p);
    if (mesa_mv(dir, MESA_BAKED ".foz", "foz_cache.foz") && mesa_mv(dir, MESA_BAKED "_idx.foz", "foz_cache_idx.foz")) VKLOG("the driver's shader cache: the bake goes on from the one in %s", dir);
}
static void mesa_sum(const char *dir, void *u) { long long a = mesa_size(dir, "foz_cache.foz"), b = mesa_size(dir, "foz_cache_idx.foz"); *(long long *)u += (a > 0 ? a : 0) + (b > 0 ? b : 0); }
static void mesa_bake_name(const char *dir, void *u) {
    long long a = mesa_size(dir, "foz_cache.foz"), b = mesa_size(dir, "foz_cache_idx.foz"); if (a <= 0 || b <= 0) return;
    if (mesa_mv(dir, "foz_cache.foz", MESA_BAKED ".foz") && mesa_mv(dir, "foz_cache_idx.foz", MESA_BAKED "_idx.foz")) { *(long long *)u += a + b;
        VKLOG("the driver's shader cache: baked into %s/" MESA_BAKED ".foz (%.1f MB) and " MESA_BAKED "_idx.foz (%.1f MB)", dir, a / 1048576.0, b / 1048576.0); }
}
long long vk_mesa_cache_bytes(void);
long long vk_mesa_cache_bytes(void) { long long n = 0; mesa_dirs(mesa_sum, &n); return n; }   // the driver's own database so far (the bake's progress line)
// the bake is done (gfx_vk_gpl.inc gpl_bake): once the driver's writer has caught up (the files stop growing for 3 s; 5 minutes at most), its database takes the
// baked one's name. The driver keeps the files open, so what it compiles in the rest of this run still lands in them, which does no harm. The bytes baked, 0 for none
long long vk_mesa_bake_done(void);
long long vk_mesa_bake_done(void) {
    long long last = -1, n = 0; int same = 0;
    for (int i = 0; i < 600 && same < 6; i++) { long long now = vk_mesa_cache_bytes(); same = now == last ? same + 1 : 0; last = now; usleep(500000); }
    mesa_dirs(mesa_bake_name, &n);
    if (!n) VKLOG("[RED] the driver's shader cache: nothing was baked (no mesa_shader_cache_sf/<driver>/<GPU>/foz_cache.foz with entries under DM_ROOT/mesa: is the driver Mesa's, DM_VK_DRIVER?)");
    return n;
}
// 1 when the data folder holds a bake of this driver (its cache UUID's folder has an nx2a.foz): a shader part then costs a read where it cost a compile, and a
// draw makes the parts it lacks itself, where it would have been left out while they compiled in the background (gfx_vk_gpl.inc gpl_parts_async). Once the device is made
int vk_mesa_baked(void);
int vk_mesa_baked(void) {
    static int v = -1; if (v >= 0) return v;
    const char *root = getenv("DM_ROOT"), *e = getenv("DM_VK_MESA_CACHE"); char a[1300], c[1800], hex[33]; long long n = 0;
    if (!root || (e && !atoi(e)) || ((e = getenv("DM_VK_GPL_BAKE")) && atoi(e))) return v = 0;
    for (int i = 0; i < 16; i++) snprintf(hex + i * 2, 3, "%02x", V.props.pipelineCacheUUID[i]);
    snprintf(a, sizeof a, "%s/mesa/mesa_shader_cache_sf/%s", root, hex);
    DIR *d = opendir(a);
    if (d) { for (struct dirent *de; (de = readdir(d)); ) { if (de->d_name[0] == '.') continue; snprintf(c, sizeof c, "%s/%s", a, de->d_name); long long sz = mesa_size(c, MESA_BAKED ".foz"); if (sz > 0) n += sz; }
             closedir(d); }
    if (n) VKLOG("the driver's shader cache: a port's bake for this driver is there (%.1f MB): shader parts are read from it, and a draw makes its parts itself", n / 1048576.0);
    else { struct stat st; snprintf(a, sizeof a, "%s/mesa/mesa_shader_cache_sf", root);
           if (!stat(a, &st)) VKLOG("the driver's shader cache: no bake for this driver (cache UUID %s) under %s: shader parts compile as before", hex, a); }
    return v = n > 0;
}
// The driver makes its cache folders for the owner only (0700): a bake could not be pulled off the device (adb's shell is in the folder's group, not its owner),
// nor could porter install push one into them. At each start the folders under mesa/ get the group's access the app's other folders have, and the files its read
// and write (a handful of entries: three levels and two or four files).
static void mesa_open_group(const char *dir, int depth) {
    DIR *d = opendir(dir); if (!d) return; chmod(dir, 02770); struct dirent *en;
    while ((en = readdir(d))) { if (en->d_name[0] == '.' && (!en->d_name[1] || (en->d_name[1] == '.' && !en->d_name[2]))) continue;
        char p[1400]; struct stat st; if (snprintf(p, sizeof p, "%s/%s", dir, en->d_name) >= (int)sizeof p || lstat(p, &st)) continue;
        if (S_ISDIR(st.st_mode)) { if (depth < 5) mesa_open_group(p, depth + 1); } else if (S_ISREG(st.st_mode) && (st.st_mode & 0660) != 0660) chmod(p, (st.st_mode & 07777) | 0660); }
    closedir(d);
}
static void vk_mesa_cache_env(void) {   // (vk_init, before the driver is loaded)
    const char *root = getenv("DM_ROOT"), *e = getenv("DM_VK_MESA_CACHE"); char dir[1100]; struct stat st;
    if (!root || (e && !atoi(e))) return;
    int bake = (e = getenv("DM_VK_GPL_BAKE")) && atoi(e);
    snprintf(dir, sizeof dir, "%s/mesa", root);
    if (bake) { mkdir(dir, 0770); mesa_dirs(mesa_bake_resume, NULL); }
    if (stat(dir, &st) || !S_ISDIR(st.st_mode)) return;
    mesa_open_group(dir, 0);
    setenv("MESA_SHADER_CACHE_DISABLE", "false", 0); setenv("MESA_SHADER_CACHE_DIR", dir, 0); setenv("MESA_DISK_CACHE_SINGLE_FILE", "1", 0);
    if (!bake) setenv("MESA_DISK_CACHE_READ_ONLY_FOZ_DBS", MESA_BAKED, 0);
    VKLOG("the driver's shader cache on disk: %s (%s)", dir, bake ? "a bake: every shader part of the bundle is compiled into it now" : "read on demand; " MESA_BAKED ".foz there is a port's bake");
}
