/* Record labels: each file's label tag, read from the file and kept on the card by path hash (docs/modules/labels.md).
 *
 * The stock database has no label column, so the label is read from the file itself on the shared worker: FLAC and Ogg (Vorbis, Opus)
 * Vorbis comments LABEL, ORGANIZATION, PUBLISHER; MP3 ID3v2.2-2.4 TXXX:LABEL, TPUB (TPB); MP4 (M4A)
 * ----:com.apple.iTunes:LABEL, ORGANIZATION, PUBLISHER. The reader uses only open, read, lseek and close,
 * reads at most LB_READ bytes of one block or frame and skips the rest (pictures) by seeking. The table follows the durations table
 * (patch/durations.h): entries {key, size, label} sorted by key, a changed size read again, a pass that drops the files nobody named.
 */
#pragma once
#include "covercache.h"

#define LB_FILE CARD_DIR "/labels"
#define LB_TMP CARD_DIR "/labels.tmp"
#define LB_MAX 128 /* a label's bytes with its NUL; a longer one is cut at a character boundary */
#define LB_READ 65536 /* the most of one comment block, Ogg packet or ID3 frame read */
#define LB_MAX_ENTRIES 200000 /* a table of more is refused (and rebuilt) */
#define LB_MAX_LABELS 8192 /* distinct labels; a file with a new label past this reads as none */

typedef struct {
    unsigned key; /* lb_key(path) */
    unsigned size; /* the file's size when read */
    char label[LB_MAX]; /* "" = read, no label (not read again until the size changes) */
} lb_add_t;

unsigned lb_key(const char *path);
/* The worker's read: path's label into out (trimmed, at most size - 1 bytes, UTF-8); its length, 0 when it has none or cannot be read. */
int lb_probe(const char *path, char *out, unsigned size);
/* The table is read on the first acquire and freed on the last release. */
void lb_acquire(void);
void lb_release(void);
/* path's label when its entry has this size ("" = it has none); NULL when there is no entry or the size differs (it must be read). */
const char *lb_file(const char *path, unsigned size);
/* A track record's label: as lb_file for its file's size; NULL when not read yet (*size, when given, the size to read it with). */
const char *lb_song(void *rec, unsigned *size);
/* The entries merged in (a key already there replaced, the last of equal keys wins) and the file written; 0, or -1 when it could not be written. */
int lb_merge(const lb_add_t *add, unsigned n);
/* A pass over the library, as dur_pass_*: the entries nobody named dropped (nothing when no song was named). */
void lb_pass_begin(void);
void lb_pass_seen(const char *path);
unsigned lb_pass_end(void);
int lb_has(const char *path); /* an entry for path, any size */
/* The distinct labels some entry names (acquired): lb_count of them, lb_name(i) for 0 <= i < lb_count, in no particular order. */
unsigned lb_count(void);
const char *lb_name(unsigned i);
/* path's label (acquired), any size; NULL when it has none or no entry. */
const char *lb_path(const char *path);
/* The two labels are one (ASCII case aside): the Labels page lists them once and a label's albums take either. */
int lb_same(const char *a, const char *b);
/* The labels (acquired) as a new deque of class rows (REC_TYPE 4, REC_ID -3 - i, REC_TITLE lb_name(i), copied): the Labels page's rows and Search's. NULL when there is none,
 * more than max, or no memory. */
void *lb_rows(unsigned max);
void lb_clear(void); /* the file deleted and the table in memory emptied */
unsigned lb_epoch(void); /* counts the clears */
