/* The Add to Playlist picker: our own page in place of the stock playlist picker (docs/modules/picker.md).
 *
 * Copyright (c) 2026 Xin. MIT licence (see LICENSE).
 * The page is the Playlists page (patch/playlists.c) in pick mode; this file holds what the add needs: a copy of the list and the marks made when the picker opened,
 * and the stock batch_add_file called as the stock picker's row handler (@0x53d300) calls it.
 */
#pragma once

/* Opens the picker over the page that owns the stock list in *pdeq_showlist (count rows, marks in the bitset sel): the list is copied (the picker's own reads
 * of the playlists replace *pdeq_showlist) and so are the marks. 1: open; -1: not opened (the stock picker is the caller's fallback). The browsing class,
 * classinfo[0..1] and the navigation title the picker's stock queries change are put back when it closes. */
int picker_open(int cls, unsigned count, const unsigned char *sel);

/* picker_add's argument for the Favourites row (a playlist id is never negative). */
#define PICKER_FAVOURITES (-1)

/* The playlist's songs (or Favourites): batch_add_file on the copy and the marks, then "Added to “name”"; when every marked song is already there (checkPlayListExist /
 * checkFavExist) nothing is added and the message is "Already in “name”". A playlist (not Favourites) becomes the newest recent one. Once per picker. Playlists page only. */
void picker_add(int playlist, const char *name);

/* The recent playlists: at most PICKER_RECENT ids, newest first, one per line in PICKER_RECENT_FILE (written through PICKER_RECENT_TMP and renamed after each add).
 * picker_recent puts the saved ids in ids (no repeats, no negatives) and returns how many; whether each is still a playlist is the caller's to check. */
#define PICKER_RECENT 3
#define PICKER_RECENT_FILE "/mnt/data/m1plus/playlists-recent"
#define PICKER_RECENT_TMP "/mnt/data/m1plus/playlists-recent.tmp"
/* The picker on one record (a copy is made; Now Playing's Add to playlist): 1 when its page opened, -1 when it did not. */
int picker_open_one(int cls, void *rec);
int picker_recent(int *ids, int max);

/* The browsing record as it was when the picker opened (after each stock query the picker runs). No-op when no picker is open. Playlists page only. */
void picker_restore(void);

/* The picker's page is gone: the record put back, the copies freed. Playlists page only. */
void picker_release(void);
