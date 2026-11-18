# Home cover build

**Sources:** `patch/homecover.c`, `patch/durations.c`, `patch/labels.c`, `patch/covlist.c`, `cache_pill` in
`tools/pod_ui.py`, `HC_*` in `patch/offsets.inc`. **Tests:** `test/pod/homecover.py`,
`test/pod/durations.py`, `test/pod/labels.py`.

The Home page ([menu](menu.md)) builds the album covers that have no cache file in the background,
with the shared worker ([covercache](covercache.md)), and shows a progress pill at the top while it
runs: `Preparing covers 120/840` with a bar and a stop button. The same pass measures track durations
and fetches artist photos. **No hook and nothing at boot:** `home_menu_opened` calls
`homecover_start` (Home instance only, beside `recent_start` / `dailymix_start`), which looks up two
widgets, registers `EVT_DESTROY` / `EVT_WINDOW_TO_BG` / `EVT_WINDOW_TO_FG` and arms a one-shot
`HC_START_MS` (5 s, never in the boot window) only when work is expected. A refused registration or
timer leaves Home as it was.

## The pill

- A window child of the menu asset, `pod_cache_pill`: a capsule (fill `#1C1C1E`) at the top of Home.
  The asset has it on the Normal menu too, hidden and insensitive; only Home shows it.
- A grey 15 px label, a 4 px progress bar (track `#FFFFFF33`, fill in the accent role: a level off
  white under Neutral; same pair as the Podcasts refresh card) and the stop button `pod_cache_x`. No
  accent tint on the rest.
- The fill's width follows `done / total` and changes only when the counter moved. The label text is
  `TR("Preparing covers")` plus `done/total`, rebuilt at each poll that saw the counter move.
- Showing it makes room: the stock list and its scroll view move down and shrink (`HC_H`), then
  `menu_arrange` clamps the scroll offset; hiding it puts them back. The first row is never covered.
- Hidden = `visible=false` and `sensitive=false` together. Shown only when the worker runs with at
  least one job, so a library whose albums all have a picture or marker never shows it. Hidden again
  at done == total, on background and on destroy.
- A static sync icon in the status bar shows on every page while any set runs ([lineout](lineout.md)).
- **The x** (`hc_on_stop`) sets `hc_off` and runs `hc_end`: timers go, a half-scanned copy is freed,
  durations done so far are saved, a set Home owns is stopped (never joined here), the pill hides.
  While `hc_off` is set Home starts nothing, also for a Home window opened later (a static: a reboot
  clears it). Settings > Library > Rebuild Covers clears it (`homecover_rebuild_asked`). **The x
  writes nothing**, so after a reboot the pill comes back. A build another page owns is never touched
  by the x.

## The check and the stamp

Whether Home has work is decided before anything is queried or scanned (`hc_expect`), from the stamp
and the database file's state.

**The stamp** `/mnt/mmc/.m1plus/cache-stamp` (on the card like the cache) has one line per part:
`covers <albums> <hash>`, `tracks <albums> <hash>`, `artists <albums> <hash>` (decimal count, 8 hex
digits). The signature is the stock Albums count and the order-independent sum of `cc_album_key`
(`hc_lib_of`). A line is written only when its part ran to the end (`hc_stamp`, atomic
`m1_card_write`). A part that did not (card full, no thread, held worker, a save error) leaves its
line as it was. A stamp from before artist photos has no artists line, so the pass runs once by
itself: no migration code.

| Case | Line written? |
|---|---|
| covers: every album has a file or marker (also an empty library) | covers |
| covers: set ended by **stall** (the hung album has its `.bad`, others unlooked at) | none; the next foreground scans again and skips it |
| covers: **truncated** pass (more missing than `CC_MAX_JOBS` 4000, or a path copy failed) | none; chains the next chunk ([Chunks](#chunks)) |
| **failed list copy** (`cc_list_failed`) | none; the pill hides, Home asks again |
| tracks: table complete or finished measuring (also no songs) | tracks |
| **no songs although there are albums** (stock reports a failed query as an empty list) | none; the pill hides, Home asks again |
| artists: set finished unstalled, untruncated, every artist fitted | artists (also when nothing was queued) |
| **empty album list** (may be a failed query) | `covers 0 00000000` etc., never trusted: `lib.sigok` stays 0, so every foreground asks again silently at `HC_START_MS`. A still empty library matches its stamp: no pill |

Clear Cover Cache drops the covers line (`homecover_stamp_drop(1)`), Clear Track Durations the tracks
line (`(2)`); no line left removes the file. **The database file's state** (`hc_dbsig`: `__xstat` of
`database.db` and its `-wal` and `-journal`) is kept from the last signature, in memory.

On Home open and every foreground (not `hc_off`), `hc_expect` answers without a query or per-album
`access`:

- a missing stamp line, a pending `REBUILD_FILE` or `AREBUILD_FILE`: **work certain**; no card:
  nothing;
- the database changed since the signature was taken (a play changes it too), or none taken since
  boot, with both lines present: **maybe work**. A silent 5 s probe asks the library once; equal to
  the stamp: done, nothing shown; different: the pill appears and the scan runs;
- otherwise nothing: no pill, no timer, no query.

Work certain shows the pill **at once** in its waiting state: `Checking library…`, or `Rebuilding
covers…` / `Rebuilding artist photos…` while a request is pending, no count, with animated dots
(`hc.dots`, 400 ms; the label's trailing ellipsis is dropped and 1 to 3 ASCII dots show). The 5 s
one-shot is armed as before; the x in this state just removes it. At 5 s `hc_on_start` queries the
albums once and compares the signature with the stamp. A worker held by another page at 5 s hides the
waiting pill (the next foreground offers again). **Returning to Home** with the pill up when it left
arms the one-shot at `HC_RESUME_MS` (1 s).

**Known limit:** a song added to an existing album changes neither the album count nor the key sum,
so the signature stays; its duration is measured by the album page, not Home. After a reboot the
first Home open, and each one after a database change such as a play, costs one stock album query at
5 s even when nothing changed (no pill).

## The build

- `hc_on_start` (page-scoped timer): if the worker is held (`cc_build_busy`) it does nothing and the
  next foreground offers again. Otherwise it copies the library's albums (`cc_albums`, one stock
  query; `recent_rekey` corrects Recently Played's cover keys from it, [recent](recent.md)), opens a
  `CC_MISSING` set and scans in steps of `HC_BATCH` 64 albums with `cc_build_near_step`, re-arming
  `HC_SCAN_MS` 20 ms later so input is served between steps. After the last step the copy is freed
  and `cc_build_start` runs: nothing missing, no card, under 16 MB or no thread frees the set and
  shows nothing.
- `hc_order` moves the Recently Played shelf's albums to the front, most recent first, so the shelf's
  placeholders are the first covers made.
- `hc_on_poll` (500 ms) reads `cc_build_done`; when it moved, rewrites the text and bar and calls
  `recent_refresh` so shelf tiles that showed the placeholder try again. At done == total the set is
  stopped and the pill hidden. A Rebuild Covers request is deleted then only if the set did not stall
  and the scan reached the end of the list; a stalled one stays for the next foreground, which skips
  the hung album. The Daily Mix card's art is its own render and the Made for You tiles pick the new
  covers up on the next foreground.
- An orphaned worker still inside its last album is retried every `HC_POLL_MS`.
- `EVT_WINDOW_TO_BG`: `hc_end` removes the timer, frees a half-scanned copy, stops a set Home owns
  (only then) and hides the pill, so the worker never runs beside Cover Flow's or the Albums page's
  own build. `EVT_WINDOW_TO_FG` runs the check again. `EVT_DESTROY` ends it likewise (an earlier
  window's destroy is ignored).

## Chunks

A library with more missing covers than one set holds (`CC_MAX_JOBS`) is built in chunks, each
resuming where the last scan stopped, never from the first album (the worker leaves no file for an
album whose track is gone, so a rescan from 0 would queue the same albums for ever).

- `cc_build_near_step` stops at the first album the set cannot take and leaves `*cursor` on it;
  `cc_build_truncated` tells the caller.
- The resume record `rs` (static, outlives the window) holds the library signature, the mode
  (missing / rebuild), the cursor, a position-sensitive hash of the cover keys (`lib.oh`) and the
  shelf's rows. It is written when a truncated chunk's set finished, so a chunk Home leaves is redone
  from its start. The next chunk is chained at once; it queries again and resumes only if signature,
  mode and order hash match, else starts from 0.
- The scan runs in the stored order, not the Albums page's display order (the Artist order would sort
  the whole library on the UI thread at every chunk), with the shelf's albums first. When the shelf's
  order cannot be allocated the pass is deferred rather than scanned in another order.
- Only a scan that reaches the end with no stall writes the covers line, clears the record and
  deletes a Rebuild Covers request. Starting a truncated chunk drops a present covers line so a
  matching line can never skip an interrupted chain. Rebuild asked again and Clear Cover Cache clear
  the record. The durations start after the last chunk.

## Track durations

After the cover phase (or at once when no cover is missing) the same pill shows `Measuring tracks
n/m` while the worker measures the songs the durations table lacks ([album-page](album-page.md#album-length)).

- `hc_dur_begin` runs when the page is in front, Home owns no set and the worker is free: `cc_songs`
  copies the stock Songs list, a `CC_DURATIONS` set is opened (up to `CC_MAX_LEN_JOBS` 30000; a longer
  library runs more passes) and `hc_on_dscan` walks it (`HC_LBATCH` 1024 songs looked up, `HC_LSTAT`
  64 files `stat`ed per step). A song is queued when its path has no table entry (CUE tracks with both ends and repeated paths
  are not). Home does not
  `stat` songs already in the table, so a replaced file is re-measured when its album page opens.
- **Prune:** each pass names the paths of the in-memory song list; unnamed entries are dropped and the
  table rewritten. An empty list prunes nothing.
- `hc_on_dpoll` (500 ms) updates the pill and saves finished jobs every `HC_LSAVE` 500 songs. At the
  end the rest is saved and the set stopped. Background, destroy and `hc_end` save what finished and
  stop the worker (the job in flight is lost); the next foreground resumes.
- A pass that finds every song in the table is not repeated until the library signature changes or
  Clear Track Durations is used, and it writes the tracks line.
- The set needs the card (the table is `/mnt/mmc/.m1plus/durations`; without it the pass does not
  start) but not 16 MB free.

## Record labels

After the durations the same pill shows `Reading labels n/m` while the worker reads the label tag of
the songs the labels table lacks ([labels](labels.md)). The stage runs as the durations one
(`hc_len_begin(1)`, a `CC_LABELS` set of up to `CC_MAX_LBL_JOBS` 4000, the same batching, saving,
prune and resume); CUE images are read once like any file. The tracks line is written only when both
stages find every song in their tables, and a missing `/mnt/mmc/.m1plus/labels` makes it stale.

## Rebuild Covers

Settings > Library > Rebuild Covers (`ACT_REBUILD`, `rebuild_ask`) asks in the
[confirm dialog](ui-rules.md#confirm-dialog); Yes writes the request file
`/mnt/data/m1plus/covers-rebuild` (`REBUILD_FILE`, `home_write_atomic`, survives a reboot), clears
the stop flag and toasts "Covers will rebuild on Home" (a failed write: "Storage unavailable"). When
`hc_on_start` begins a scan it checks the file:

- present: the set opens in `CC_REBUILD` mode (every album with a track path, the shelf's first) and
  the pill reads `Rebuilding covers n/m`;
- absent: the normal missing-only build.

The request is removed only when the scan reached the end and the set completed (no truncation, no
stall), or at once when no album has a track path. A library over `CC_MAX_JOBS` rebuilds in chunks,
each in `CC_REBUILD` mode from the cursor the last stopped at. Interruption (background, destroy, a
held worker, no card or space, no thread) leaves the file, and the next foreground starts again from
the unfinished chunk; the worker replaces each file atomically, so a restart only repeats work.

Its opposite is Clear Cover Cache: it deletes the cache and this request; the next Home open shows
the waiting pill (the covers line is dropped) and queues every album as missing.

## Rebuild Artist Photos

Settings > Library > Rebuild Artist Photos (`arebuild_ask`) asks in the same dialog ("Rebuild all
artist photos? This may take a few minutes."); Yes writes `AREBUILD_FILE`
(`/mnt/data/m1plus/artists-rebuild`), calls `homecover_rebuild_asked` and toasts "Artist photos will
rebuild on Home" (a failed write: "Storage unavailable"). `hc_expect` sees work (the waiting pill reads `Rebuilding artist photos…`, covers
first when both are pending; the pill counts `Rebuilding artist photos n/m`) and `hc_on_start`
forces every artist; the covers' request, `.bad`
files and cache are not touched. Before the worker starts, a complete walk saves the purity map
(`ap_lib_save`); a pass without a snapshot removes it. Details: [artists](artists.md).

## Artist photos

After the cover steps of the last chunk, `hc_on_start` walks the same album copy again
(`hc.astage`, 64 albums a step) and calls `cc_build_add_artist` for every album. The artists without
a result (all of them under a rebuild request) become jobs after the cover jobs in the same set, so
the pill counts `Preparing covers n/m`, then `Preparing artist photos n/m`. The set's capacity is
twice the album count (at most `CC_MAX_JOBS`).

**The artists line is separate from the covers line** (`hc_stale` bit 8). It is written when the set
finished unstalled, the cover pass was not truncated and every artist fitted (a library over
`AP_LIB_MAX` albums has no artist photos and is stamped at once so it does not rerun). A pass with
only the artists line stale skips the cover scan altogether (`hc.cur` = count) so the set holds only
artist jobs. A stalled or truncated pass writes no artists line; a library change makes both stale.
Clear Cover Cache and Clear Track Durations do not drop it. The request is deleted like Rebuild
Covers'. Discovery rules and the cache: [artists](artists.md#artist-photos).

## Cache page

Settings > Developer > Tools > **Cache** opens `m1plus_cache`, a read-only cards page of the About
kind (`ACT_INFO` rows: label and grey right-aligned value, no chevron). Three sections:

- **Covers.** Card 1: **Albums**, **Covers** (`r<key>-168.jpg` with size > 0), **No art** (empty
  markers), **Missing** (a track path and no file, by the `CC_MISSING` rule), **Old files** (anything
  not `r<8 hex>-168.jpg`), **Size**. Card 2: **Status**: Idle, Preparing or Rebuilding n/m (Home's
  set), Building (another page's), Rebuild requested, Clearing. Card 3: **Clear Cover Cache**.
- **Artist Photos:** **Photos** (`a<key>-44.jpg` with a picture) and **None** (empty ones), counted
  after the covers folder ([artists](artists.md)).
- **Track Durations:** **Measured Tracks** (`INFO_DUR`: `<count> · <size>` from the table header and the
  file's size, read once as the page opens and after a clear; no card, no file or a bad header: `0`) and **Clear Track Durations** (`ACT_CLEARDUR`
  44, glyph `trash`): "Delete all measured track durations? They will be measured again on Home." in
  the confirm dialog; Yes calls `dur_clear` (deletes the file and its `.tmp`, empties a held table,
  counts the clear so Home reads the library again) and toasts "Track durations cleared". Durations
  themselves: [album-page](album-page.md#album-length).

Counting never blocks and runs only while the page is open (`homecover_cache_open`): the folder is
read `CP_N` 64 entries per 20 ms tick, then `cc_albums` is loaded once and checked 64 albums per
tick; until a count is known the value is an ellipsis. A 500 ms poll refreshes the page when Status
changed. Background and destroy remove the timers; no folder shows zeros (every album missing, Status Idle).

**The two lists.** **No art** and **Missing** are value rows (`ACT_CCLIST`): the chevron shows, and
the row takes a tap, only when the count is counted and above 0. A tap opens a plain list
(`patch/covlist.c`, `m1plus_covlist`, title No Art / Missing Covers): the Albums page's list mode
without the cover (title 21 px, artist 18 px grey, 64 px rows), in the Albums order. A tap on a row
opens that album with `cc_open_album`; Back returns to the list, then to Cache. The rows are
collected by the count's album scan (at most `CC_MAX_JOBS` each) and copied into the list page.
Nothing to show: "No albums". The No art count is the folder's empty markers (including ones no album
has), the list the albums that have one, so they can differ.

### Clear Cover Cache

`ACT_CLEARCACHE`, glyph `trash`. `clear_ask` first refuses with the toast "Covers are being made, try
later" when the shared worker is held (nothing is stopped: a set may belong to Albums or Cover Flow).
Otherwise the confirm dialog asks "Delete all cached covers? They will be made again on Home."; Yes
calls `homecover_clear`: it removes a pending Rebuild Covers request and deletes every regular file
in `/mnt/mmc/.m1plus/covers` (pictures, markers, `.bad`, `.inflight`, `.tmp`, old-format files). Cover
Flow's position file lives outside that folder and is kept ([coverflow](coverflow.md)). Deletion is
32 unlinks per 20 ms tick of a self-ending timer, so nothing blocks; leaving Settings mid-way lets it
finish, and a build starting meanwhile stops it quietly. The toast "Cover cache cleared" shows at the
end.
