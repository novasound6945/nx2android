// storage_android.c: the data root on internal storage or an SD card (storage.h decides and copies; this lists the volumes through JNI, keeps the choice, runs a move)
#pragma once
#include "storage.h"
#include <android/native_activity.h>

// onCreate (the main thread), before anything reads the files folder: a move cut short finished or undone, the volumes listed (Context.getExternalFilesDirs), the root
// chosen (storage.h dm_st_choose) into root, the choice and porter's pointer written. 1: the chosen root is on a card that is not in (root is internal storage, for
// nothing but the log: the start asks the player, dm_storage_recheck / dm_storage_use_internal)
int dm_storage_start(ANativeActivity *act, char *root, size_t cap);
int dm_storage_recheck(ANativeActivity *act, char *root, size_t cap);   // the main thread: the volumes listed again; 0 with root when the chosen one is there now
void dm_storage_use_internal(char *root, size_t cap);                   // the choice set to internal storage (the card that held the data is not in)
const char *dm_storage_home(void);   // the internal files folder: the log, saves/, sync.txt (they never move)
// the launcher's App settings
typedef struct {
    int on_sd;                   // the data root is the SD card's folder
    int has_sd;                  // an SD card (a removable volume) is mounted, with the app's folder on it
    uint64_t free_int, free_sd;  // their free bytes now
} dm_storage_info;
void dm_storage_info_get(dm_storage_info *o);
// A move to the other volume, on a thread of its own: 0 started (it sizes the data first and copies nothing when the target has too little room: FAIL, -ENOSPC);
// -EBUSY (one runs), -ENODEV (no such volume, or the root is there already), -1
int dm_storage_move_begin(int to_sd);
void dm_storage_move_cancel(void);
enum { DM_MV_NONE, DM_MV_RUN, DM_MV_DONE, DM_MV_FAIL, DM_MV_CANCELLED };
typedef struct { int state, to_sd, err, sizing; uint64_t done, total, need, room; char why[300]; } dm_storage_move;   // err: -errno of a failure; sizing: the data counted
// before the copy; need, room: the bytes the copy needs and the target's free bytes (-ENOSPC)
void dm_storage_move_get(dm_storage_move *o);
void dm_storage_move_clear(void);    // a failure's or cancel's outcome seen: NONE again
