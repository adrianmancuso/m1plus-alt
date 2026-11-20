# Add to Playlist picker

**Sources:** `patch/picker.c`, `patch/picker.h` (the add and the copies), `patch/playlists.c` (pick mode),
`patch/songops.c` (`so_add`), `PICKER_PINS` in `tools/pod.py`. **Tests:** `test/pod/picker.py`.
**Preview:** `tools/preview/preview_picker.py` (`out/preview/picker.png`).

Our own picker replaces the stock one. Add on the Songs page and the album page (`so_add`) opens it,
and Now Playing's More > Add to playlist opens it on the playing track (`picker_open_one`,
[playmore](playmore.md)); the stock `playlist_page` in picker mode is only the fallback.

## Choice: a page, not a sheet

Our Playlists page in list mode, titled "Add to Playlist". A glass sheet (the Playlists "..." card, the Songs Sort card) is for a handful of fixed rows; the picker
lists up to 100 playlists with mosaics and counts, so it needs the scrolling list, the mosaic loader and the row painter the Playlists page already has. One
code path, the same rows the user knows, nothing new to keep in step. The design is the page's: neutral greys, 64 px rows (52 px mosaic, name, count), the
pressed row `#333334`.

- Navbar: Back (glass chevron) and the title "Add to Playlist" (the Songs pill's text, short in all 13 languages; its label is widened to `SG_SELALL_X`, as
  in select mode). The "..." button and Now Playing are hidden and insensitive; the page is always the list (the saved grid / list choice is
  the Playlists page's own and is not written). The playlists after the two pinned rows follow the Playlists page's saved order (`playlists-sort`: Name A–Z by default, or Z–A); the picker has no card to change it.
- Row 0: "New playlist" (the stock key `playlist_create`, the New playlist keyboard's title), a white plus on a dark rounded square (`pl_plus`), no count. A tap opens our keyboard
  (`keyboard_open`, as the Playlists New); its Done creates the playlist (`createPlayListItem`: -1 full and -2 exists say the stock message and leave the picker
  open) and adds to it.
- Row 1: "Favorites" (our own key, the Favorites row of the Extras page, translated in all 13 languages), a white filled star (24 px alpha bitmap made at open by `pl_make_star`, 4 x 4 samples a pixel; the same glyph as the Favorites icon, which the repo draws as a star everywhere, not a heart) on the New row's dark rounded square, no count. A tap adds to Favourites and closes, as a playlist row does. `pl_fixed()` is 2 in pick mode (the rows that are no playlist).
- Then up to 3 recent playlists (`PICKER_RECENT`), newest first, with the subtitle "Recent · N tracks" (just "Recent" until read) and no heading. They are
  also in the list below. `pl_v()` counts the rows before that list (2 + the recent rows); `pick.rslot` maps a recent row to its playlist, found once per load.
- Then the playlists, the name and "N tracks" once read (the page's loader, one playlist a tick). A tap adds and closes.
- Back, or the system back, closes with nothing added; there is no tap-outside (it is a page). Now Playing is not offered.

## Recent playlists

`/mnt/data/m1plus/playlists-recent` (`PICKER_RECENT_FILE`): playlist ids (`REC_DISC`) as text, one per line, newest first. `picker_recent` reads at most 64
bytes and keeps whole lines of up to 9 digits, without repeats, at most 3. An id that is no shown playlist (deleted, past `PL_MAX`) is not shown. After an add
to a playlist (a tap or New playlist; Favorites is not recorded), `pk_note` writes that id first and up to two of the others, through `PICKER_RECENT_TMP` and a
rename (`home_write_atomic`).

## What it does with a tap

Stock's row handler in picker mode calls `batch_add_file(class, 0xf00b, *pdeq_showlist, *SEL_VEC, playlist id)`
(the id is the row's `REC_DISC`; it skips songs already in the playlist), shows `msg_addplaylistsuccess`
in `dialog/msginfo_dialog` and closes the picker's window. `picker_add` does the same, in the same
order, with its own message, which names the target: "Added to “name”". When every marked song is already there (`checkPlayListExist`, or
`checkFavExist` for Favorites) nothing is added and the message is "Already in “name”". The prologue of `batch_add_file` and the handler's class, call and 5th-argument words are pinned
(`PICKER_PINS`). Stock returns nothing the handler tests, so the message cannot report a failed add.

**Favourites** is the stock Add button's other choice (`dialog/batchadd_dialog` returns 1): `batch_add_file(g_class_type, 0xf00a, *pdeq_showlist, *SEL_VEC, 0)`, then the message `msg_addfavsuccess` in `dialog/msginfo_dialog` (ours says "Added to “Favorites”"). Stock stays on its page afterwards; ours closes the picker, as for a playlist. With nothing selected stock shows `msg_addfailed` first: ours never opens with an empty selection (`so_add`). `picker_add(PICKER_FAVOURITES)` does it on the same copies, with class `0xf00a` and 0 for the id. The words are pinned in `PICKER_PINS`.

`picker_open_one(cls, rec)` opens the picker on a one-row copy of a record with that row marked (Now Playing).

**The copies.** The Playlists page reads every playlist to show its count and mosaic (`loadLocalClassList(0xff13)`), and each read replaces `*pdeq_showlist`, the list
the add needs. So `picker_open` copies the list (`cc_copy_list`: the page's `count` rows must be the stock list's, else -1) and the marks (the page's bitset)
when it opens; `picker_add` fills the stock selection vector from the copy of the marks (`so_selection`, as the stock-picker path did) and passes the copy to
`batch_add_file`. The call writes the new ids into the copy's records (stock writes them into the shown list's); the caller reads its list again anyway.

**The browsing record.** Those queries also set the browsing class, `classinfo[0..1]` and the navigation title. The picker saves them at open and puts them back
after every query it runs (`picker_restore`: after each playlist scan, the playlist list, the creating query) and when the page is gone (`picker_release`, from
the page's destroy), so the caller's own restore on its to-foreground (which `so_add` still sets up: `title_save`, `*picker`) finds them as they were, and its
reload of the list uses them. The stock picker's side effects (class `0xf00b`, the saved title) do not happen: stock's page never runs.

**Over our Playlists page's Songs page.** The Playlists page keeps its state in one struct (`pl`). When the picker opens while the Playlists page is below (Playlists >
a playlist's Songs page > Add), the state is copied aside (`calloc`) and `pl` is the picker's; the page's destroy puts the copy back (the Playlists page below
was in the background all along: its loader stopped, to-foreground reads it again).

**Fallback.** `picker_open` returns -1 and `so_add` runs the stock path exactly as before (the selection into the stock vector, `playlist_page` with `PICKER_ADD |
class`) when: the stock list is not the one the marks are of, the copies cannot be made, the window `m1plus_playlists` does not open, the asset lacks a widget,
or a picker is already open. The SD file `/mnt/mmc/m1plus-off` disables the whole payload (then stock's page is the only one).

## Tests

`test/pod/picker.py` covers opening, the Favorites row, a tap, New playlist, Back, the fallbacks, the
stash over an open Playlists page, the recent rows and file, the messages and `picker_open_one`. `test/pod/songs.py` and `albumpage.py` refuse the window
`m1plus_playlists` where they assert the stock picker fallback.
