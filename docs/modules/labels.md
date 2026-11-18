# Record labels

**Sources:** `patch/labels.c`, `patch/labels.h`, the `CC_LABELS` mode in `patch/covercache.c`, the
label stage in `patch/homecover.c`. **Tests:** `test/pod/labels.py`.

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
| MP4 (M4A) | iTunes freeform `----:…:LABEL`, `ORGANIZATION`, `PUBLISHER` in `moov/udta/meta/ilst` (or `moov/meta`); boxes are walked by seeking, so `moov` after `mdat` and cover art cost nothing |

Tag names compare case-insensitively. ID3 text in encodings 0–3 becomes UTF-8. Compressed,
encrypted or unsynchronised frames are skipped, and a frame the file cuts short is not taken. The
value is trimmed, control characters become spaces, and it is cut to `LB_MAX - 1` (127) bytes at a
character boundary. DSF and WAV are not read (no label). An MP4 box past 4 GB stops the walk.

## Table

`/mnt/mmc/.m1plus/labels` (`LB_FILE`, written through `LB_TMP` and renamed):

- A 20-byte header: magic `M1LB`, entry count, label count, pool bytes, FNV-1a of the body.
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
