/* The Add to Playlist picker (patch/picker.h; docs/modules/picker.md has the design and the stock code read).
 *
 * Copyright (c) 2026 Xin. MIT licence (see LICENSE).
 * Stock's picker is playlist_page opened with PICKER_ADD | class; its row handler (@0x53d300) calls batch_add_file(class, 0xf00b, *pdeq_showlist, *SEL_VEC, the playlist's
 * id), shows msg_addplaylistsuccess and closes. Ours is the Playlists page in pick mode (patch/playlists.c); a tap there comes here with the playlist's id.
 *
 * The page reads every playlist (loadLocalClassList(0xff13) for its count and mosaic), and each read replaces *pdeq_showlist, the list the add needs. So the picker
 * copies the list (cc_copy_list) and the marks when it opens, and the add runs on the copy: the stock selection vector is filled from the marks at that moment (as
 * so_selection does for the stock picker) and batch_add_file takes the list as an argument. The browsing class, classinfo[0..1] and the navigation title that those
 * queries change are put back after each of them and when the page goes (the caller's own restore, patch/songs.c and patch/albumpage.c, then finds them as they were).
 */
#include "offsets.inc"
#include "stock.h"
#include "common.h"
#include "covercache.h"
#include "i18n.h"
#include "songops.h"
#include "picker.h"
#include "home.h"
#include "playlists.h"
#include "entries.h"

static struct {
    int on, done;
    void *list; /* the copy of *pdeq_showlist at open */
    unsigned char *sel; /* the marks at open */
    unsigned count;
    int cls; /* the browsing class of the list: batch_add_file's first argument, as the stock picker's context carries it */
    int cls0, ci[2];
    char title[NAV_TITLE_SIZE];
} pk __attribute__((section(".scratch")));

void picker_restore(void) {
    if (!pk.on) return;
    *(int *)G_CLASS_TYPE = pk.cls0;
    memcpy((void *)CLASSINFO, pk.ci, sizeof pk.ci);
    memcpy((void *)NAV_TITLE, pk.title, sizeof pk.title);
}

void picker_release(void) {
    if (!pk.on) return;
    picker_restore();
    if (pk.list) deque_destroy(pk.list);
    free(pk.sel);
    memset(&pk, 0, sizeof pk);
}

static int pk_open(int cls, void *list, unsigned count, const unsigned char *sel) {
    unsigned bytes = (count + 7) / 8;
    if (pk.on || !count || !list || deque_size(list) != count) return -1;
    pk.on = 1; /* from here picker_release undoes what is made */
    pk.list = cc_copy_list(list);
    pk.sel = calloc(bytes, 1);
    if (!pk.list || !pk.sel || deque_size(pk.list) != count) {
        picker_release();
        return -1;
    }
    memcpy(pk.sel, sel, bytes);
    pk.count = count, pk.cls = cls;
    pk.cls0 = *(int *)G_CLASS_TYPE;
    memcpy(pk.ci, (void *)CLASSINFO, sizeof pk.ci);
    memcpy(pk.title, (void *)NAV_TITLE, sizeof pk.title);
    if (playlists_pick() < 0) {
        picker_release();
        return -1;
    }
    return 1;
}

int picker_open(int cls, unsigned count, const unsigned char *sel) {
    return pk_open(cls, *(void **)PDEQ_SHOWLIST, count, sel);
}

int picker_open_one(int cls, void *rec) {
    static const unsigned char one = 1;
    void *d = rec ? so_row_deque(rec) : (void *)0;
    int r = -1;
    if (!d) return -1;
    if (deque_size(d) == 1) r = pk_open(cls, d, 1, &one);
    deque_destroy(d); /* (pk_open made its own copy) */
    return r;
}

int picker_recent(int *ids, int max) {
    unsigned n = 0, i = 0;
    char *text = m1_read_file(PICKER_RECENT_FILE, 64, &n);
    int got = 0, k;
    while (text && i < n && got < max) {
        long v = 0;
        unsigned d = 0;
        for (; i < n && text[i] >= '0' && text[i] <= '9' && d < 9; ++i, ++d) v = v * 10 + (text[i] - '0');
        if (d && (i == n || text[i] == '\n')) { /* a whole line of digits: an id */
            for (k = 0; k < got && ids[k] != (int)v; ++k) {
            }
            if (k == got) ids[got++] = (int)v;
        }
        while (i < n && text[i++] != '\n') {
        }
    }
    free(text);
    return got;
}

/* The playlist first, the others after it, at most PICKER_RECENT. */
static void pk_note(int id) {
    int ids[PICKER_RECENT], n = picker_recent(ids, PICKER_RECENT), k, kept = 1, w;
    char text[PICKER_RECENT * 12 + 1];
    w = snprintf(text, sizeof text, "%d\n", id);
    for (k = 0; k < n && kept < PICKER_RECENT; ++k)
        if (ids[k] != id) {
            w += snprintf(text + w, sizeof text - (unsigned)w, "%d\n", ids[k]);
            ++kept;
        }
    home_write_atomic(PICKER_RECENT_FILE, PICKER_RECENT_TMP, text);
}

/* Every marked song of the copy is already in the playlist (or Favourites). */
static int pk_all_in(int fav, int playlist) {
    unsigned i;
    for (i = 0; i < pk.count; ++i)
        if (pk.sel[i >> 3] & (1u << (i & 7))) {
            void *rec = deque_at(pk.list, i);
            if (!(fav ? checkFavExist(rec) : checkPlayListExist(rec, playlist))) return 0;
        }
    return 1;
}

/* The stock row handler's sequence: the selection into the stock vector, batch_add_file (duplicates skipped by checkPlayListExist; the rows of the copy get the new
 * ids), the message. A selection that cannot be made is msg_addfailed. PICKER_FAVOURITES is the stock Add button's Favourites choice (@0x5281c8):
 * batch_add_file(class, 0xf00a, list, vector, 0). The message names the target (stock's msg_addplaylistsuccess / msg_addfavsuccess did not). */
void picker_add(int playlist, const char *name) {
    int fav = playlist == PICKER_FAVOURITES, all;
    char msg[160];
    if (!pk.on || pk.done) return;
    pk.done = 1;
    all = pk_all_in(fav, playlist);
    if (!all && !so_selection(pk.count, pk.sel)) {
        pod_toast(TR("msg_addfailed"));
        return;
    }
    if (!all) batch_add_file(pk.cls, fav ? CLASS_FAV : CLASS_PLAYLISTS, pk.list, *(void **)SEL_VEC, fav ? 0 : playlist);
    if (!fav) pk_note(playlist);
    wk_subst(msg, sizeof msg, TR(all ? "Already in “%s”" : "Added to “%s”"), name ? name : "");
    pod_toast(msg);
}
