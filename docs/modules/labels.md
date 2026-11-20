# Record labels

**Sources:** `patch/labels.c`, `patch/labels.h`, the `CC_LABELS` mode in `patch/covercache.c`, the
label stage in `patch/homecover.c`, the Labels page in `patch/artists.c` and `patch/albums.c`
(the label folder's logo and about text).
**Tests:** `test/pod/labels.py`, `test/pod/artists.py` (`label_page_tests`).

The stock database has no label column, so each file's record label is read from its tags on the
shared worker during the Home pass ([homecover](homecover.md#record-labels)) and kept on the card.

## Reader

`lb_probe(path, out, size)` uses only `open`, `read`, `lseek` and `close`, reads at most `LB_READ`
(64 KB) of one block, packet or frame and seeks past the rest (pictures).

| Format | Tags, best first |
|---|---|
| FLAC | Vorbis comment `LABEL`, `ORGANIZATION`, `PUBLISHER` (block type 4, at most 256 blocks walked) |
| Ogg Vorbis, Opus | the same, from the comment packet reassembled over at most 64 pages |
| MP3 (ID3v2.2–2.4) | `TXXX:LABEL` (`TXX`), then `TPUB` (`TPB`); FLAC after an ID3 tag is read when the tag has none |
| WAV, AIFF, AIFC | the ID3v2 tag in the top-level `id3 ` or `ID3 ` chunk, read as for MP3; chunks are walked by seeking (at most 256), so the audio costs nothing |
| MP4 (M4A) | iTunes freeform `----:…:LABEL`, `ORGANIZATION`, `PUBLISHER` in `moov/udta/meta/ilst` (or `moov/meta`); boxes are walked by seeking, so `moov` after `mdat` and cover art cost nothing |

Tag names compare case-insensitively. ID3 text in encodings 0–3 becomes UTF-8. Compressed,
encrypted or unsynchronised frames are skipped, and a frame the file cuts short is not taken. The
value is trimmed, control characters become spaces, and it is cut to `LB_MAX - 1` (127) bytes at a
character boundary. DSF, RF64 and WAV `LIST`/`INFO` tags are not read (no label). An MP4 box past 4 GB and a
WAV or AIFF chunk past 2 GB stop the walk.

## Table

`/mnt/mmc/.m1plus/labels` (`LB_FILE`, written through `LB_TMP` and renamed):

- A 20-byte header: magic `M1L2`, entry count, label count, pool bytes, FNV-1a of the body.
- Entries `{key, size, lab}`, sorted by `key = lb_key(path)` (FNV-1a). `lab` 0 means read with no
  label; otherwise it is 1 + the label's index.
- The pool: the label names, NUL-terminated. Every name is used by some entry.

A file fails validation and is rebuilt when any of these hold:

- The magic, sizes or checksum are wrong.
- The keys are out of order.
- An index is out of range.
- The pool is malformed.
- It has more than `LB_MAX_ENTRIES` entries.

Labels are deduplicated: first by exact match, then case-insensitively, so the first spelling seen is
kept. There are at most `LB_MAX_LABELS` distinct labels; a new label past that limit reads as none.

- `lb_file(path, size)` / `lb_song(rec, &size)`: the label, `""` when the file has none, or NULL
  when the file is not read yet or its size changed.
- `lb_merge` replaces entries with equal keys (the last one wins), drops unused labels and writes the file.
- `lb_pass_begin` / `lb_pass_seen` / `lb_pass_end` prune as the durations table does. An empty pass
  prunes nothing.
- `lb_count` / `lb_name(i)` list the distinct labels.
- `lb_clear` deletes the file and bumps `lb_epoch`, which resets the Home pass's complete mark.
- `lb_path(path)`: the label of the file's entry of any size; NULL when it has none or no entry.
  `lb_same(a, b)`: ASCII case-insensitive equality (0 when either is NULL).

## Labels page

Menu item 25 `Labels` (`ACT_ARTISTS` with `AR_LABELS` 64; Library group after Composers) opens
the Artists page on our own class `CLASS_LABELS` (`0x4c42`, never given to a stock query). The page
holds the table (`lb_acquire` at open, `lb_release` on destroy).

- The rows are the table's labels (`lb_rows(max)`: a new deque of class rows, `REC_ID` -3 - index, the name in
  `REC_TITLE`), sorted by the name keys as Artists are; Name A-Z / Z-A in `labels-sort`. The title is
  "Labels"; with no label read yet the page says "No labels". The card has Search Labels (the
  asset's `vs_label6` retitled; [search](search.md) scoped to `SEARCH_LABELS`) and Name. The rows are
  read at open, not again while the page is up.
- A tap is `albums_open_label(name)`: the Albums page in the saved mode on the library albums
  (`cc_albums`) whose first track's file has the label (`lb_path` of `REC_PATH`, `lb_same`), in
  library order, the label as the title. An album with tracks from several labels is filed under
  its first track's. The card has the views and orders but no Search; the Cover Flow button is
  hidden and insensitive. An album tap opens its library row (`cc_open_album(NULL, 0, lrow[row])`).
  The list is found again on foreground; an empty one says "No albums".

## Label folder

`lb_extras(label, logo, lsize, about, asize)` reads the label's folder under `/mnt/mmc/Labels`
(`LB_DIR`). The folder's name matches the label in any ASCII case.

- **Logo:** the folder's `logo.jpg`, `logo.jpeg` or `logo.png` (any case; the earlier name wins),
  returned as a `file://` URL.
- **About text:** the first `LB_ABOUT - 1` bytes of `about.txt`.
  - Control characters and line ends become one space.
  - Runs of spaces collapse; the ends are trimmed.
  - A UTF-8 character cut by the size is dropped.
- Either is empty when missing.

A label's Albums page reads the folder once at open (`al_label_head` in `patch/albums.c`). Under
the large title it shows:

- the logo, decoded once, fitted whole into `AL_LB_LOGO` (96) px at `SG_TITLE_X`;
- under the logo, the about text in `label_row_sub`'s style, wrapped to `SG_TITLE_W`, at most
  `AL_LB_LINES` (8) lines of `AL_LB_PITCH` (22) px, with an ellipsis ending a cut last line.

`AL_LB_GAP` (12) px follows each part. The rows move down by the room both take. A part that is
missing, or a logo that does not decode, takes no room. The header is painted by the page's own
`EVT_AFTER_PAINT`, so a label with no albums shows only "No albums". Paragraph breaks in
`about.txt` are not kept.
