/* Record labels: the tag reader and the table (patch/labels.h, docs/modules/labels.md).
 *
 * The file: a 20 byte header {magic, count, labels, pool bytes, FNV-1a of the body}, then the body: count {key, size, label} entries sorted by
 * key (label 0 = none, else 1 + an index into the pool), then the pool: the labels, each with its NUL, every one named by some entry. A file
 * whose magic, size, order, indexes, pool or checksum is wrong is ignored (and replaced by the next write).
 */
#include "offsets.inc"
#include "stock.h"
#include "common.h"
#include "covercache.h"
#include "labels.h"
#include "fnv.h"

#define P(w, off) (*(void **)((char *)(w) + (off)))

#define LB_MAGIC 0x324c314du /* "M1L2" (was "M1LB" before WAV and AIFF were read: those tables are rebuilt) */
#define LB_HDR 20

typedef struct {
    unsigned key, size, lab;
} lb_ent_t;

static struct {
    lb_ent_t *tab;
    unsigned n, users;
    char **names; /* nlab labels, strdup'd; an entry's lab is 1 + its index */
    unsigned nlab, cap;
    unsigned char *seen;
    unsigned named;
} lb __attribute__((section(".scratch")));

static unsigned lb_clears __attribute__((section(".scratch")));

unsigned lb_key(const char *path) {
    unsigned k = cc_fnv(FNV_SEED, path);
    return k ? k : 1;
}

/* ------------------------------------------------------------------------------------------------------------------------------------------
 * The reader. Runs on the worker: no globals, nothing but open, read, lseek, close, calloc and free.
 */

static unsigned le32(const unsigned char *b) { return b[0] | b[1] << 8 | b[2] << 16 | (unsigned)b[3] << 24; }
static unsigned be32(const unsigned char *b) { return (unsigned)b[0] << 24 | b[1] << 16 | b[2] << 8 | b[3]; }
static unsigned sync32(const unsigned char *b) {
    return (b[0] & 0x7f) << 21 | (b[1] & 0x7f) << 14 | (b[2] & 0x7f) << 7 | (b[3] & 0x7f);
}

/* n bytes at pos into buf; the bytes got. */
static unsigned lb_rd(int fd, unsigned pos, void *buf, unsigned n) {
    unsigned got = 0;
    int r = 1;
    if (lseek(fd, (long)pos, 0) != (long)pos) return 0;
    while (r > 0 && got < n) {
        r = read(fd, (char *)buf + got, n - got);
        if (r > 0) got += (unsigned)r;
    }
    return got;
}

/* src (n bytes, or to a NUL) into out trimmed, controls as spaces, cut at a character boundary to size - 1; its length. */
static int lb_put(char *out, unsigned size, const unsigned char *src, unsigned n) {
    unsigned i = 0, k, o;
    for (k = 0; k < n && src[k]; ++k);
    n = k;
    while (i < n && src[i] <= ' ') ++i;
    while (n > i && src[n - 1] <= ' ') --n;
    if (n - i > size - 1) {
        n = i + size - 1;
        while (n > i && (src[n] & 0xc0) == 0x80)
            --n; /* src[n] is where the cut falls: a continuation byte is backed off */
        while (n > i && src[n - 1] <= ' ') --n;
    }
    for (o = 0; i < n; ++i) out[o++] = (char)(src[i] < ' ' ? ' ' : src[i]);
    out[o] = 0;
    return (int)o;
}

static int lb_eq(const unsigned char *a, const char *b, unsigned n) {
    while (n--)
        if (*a++ != (unsigned char)*b++) return 0;
    return 1;
}

static int lb_keyis(const unsigned char *s, unsigned n, const char *k) {
    unsigned i;
    for (i = 0; i < n && k[i]; ++i)
        if ((s[i] | 0x20) != (k[i] | 0x20)) return 0;
    return i == n && !k[i];
}

/* A Vorbis comment (FLAC block 4, Vorbis and Opus header packets): LABEL, else ORGANIZATION, else PUBLISHER. */
static int lb_vorbis(const unsigned char *b, unsigned n, char *out, unsigned size) {
    unsigned p, cnt, i, best = 0;
    int len = 0;
    if (n < 8 || le32(b) > n - 8) return 0;
    p = 4 + le32(b);
    cnt = le32(b + p);
    p += 4;
    for (i = 0; i < cnt && n - p >= 4; ++i) {
        unsigned l = le32(b + p), e, rank;
        p += 4;
        if (l > n - p) l = n - p; /* cut by LB_READ: what is there is read */
        for (e = 0; e < l && b[p + e] != '='; ++e);
        rank = e >= l                               ? 0
               : lb_keyis(b + p, e, "LABEL")        ? 3
               : lb_keyis(b + p, e, "ORGANIZATION") ? 2
               : lb_keyis(b + p, e, "PUBLISHER")    ? 1
                                                    : 0;
        if (rank > best) {
            int got = lb_put(out, size, b + p + e + 1, l - e - 1);
            if (got) best = rank, len = got;
        }
        p += l;
    }
    return len;
}

static int lb_flac(int fd, unsigned pos, unsigned char *buf, char *out, unsigned size) {
    unsigned char h[4];
    unsigned i;
    for (i = 0; i < 256; ++i) {
        unsigned len;
        if (lb_rd(fd, pos, h, 4) != 4) return 0;
        len = (unsigned)h[1] << 16 | h[2] << 8 | h[3];
        if ((h[0] & 0x7f) == 4)
            return lb_vorbis(buf, lb_rd(fd, pos + 4, buf, len < LB_READ ? len : LB_READ), out, size);
        if (h[0] & 0x80) return 0; /* the last block, and no comment */
        pos += 4 + len;
    }
    return 0;
}

/* The second packet of the first logical stream, put together from its pages' segments: Vorbis "\3vorbis" or Opus "OpusTags", then a comment. */
static int lb_ogg(int fd, unsigned char *buf, char *out, unsigned size) {
    unsigned char h[27], seg[255], *page = calloc(1, 255 * 255);
    unsigned pos = 0, got = 0, pk = 0, i;
    int len = 0;
    for (i = 0; page && i < 64 && pk < 2; ++i) {
        unsigned ns, total = 0, s, off = 0;
        if (lb_rd(fd, pos, h, 27) != 27 || h[0] != 'O' || h[1] != 'g' || h[2] != 'g' || h[3] != 'S') break;
        ns = h[26];
        if (lb_rd(fd, pos + 27, seg, ns) != ns) break;
        for (s = 0; s < ns; ++s) total += seg[s];
        if (lb_rd(fd, pos + 27 + ns, page, total) != total) break;
        for (s = 0; s < ns && pk < 2; ++s) {
            if (pk == 1) {
                unsigned c = seg[s] < LB_READ - got ? seg[s] : LB_READ - got;
                memcpy(buf + got, page + off, c);
                got += c;
            }
            off += seg[s];
            if (seg[s] < 255) ++pk;
        }
        pos += 27 + ns + total;
    }
    free(page);
    if (got >= 7 && buf[0] == 3 && lb_eq(buf + 1, "vorbis", 6))
        len = lb_vorbis(buf + 7, got - 7, out, size);
    else if (got >= 8 && lb_eq(buf, "OpusTags", 8))
        len = lb_vorbis(buf + 8, got - 8, out, size);
    return len;
}

static unsigned lb_utf8(unsigned char *o, unsigned c) {
    if (c < 0x80) return o[0] = (unsigned char)c, 1;
    if (c < 0x800) return o[0] = (unsigned char)(0xc0 | c >> 6), o[1] = (unsigned char)(0x80 | (c & 0x3f)), 2;
    if (c < 0x10000)
        return o[0] = (unsigned char)(0xe0 | c >> 12), o[1] = (unsigned char)(0x80 | (c >> 6 & 0x3f)),
               o[2] = (unsigned char)(0x80 | (c & 0x3f)), 3;
    return o[0] = (unsigned char)(0xf0 | c >> 18), o[1] = (unsigned char)(0x80 | (c >> 12 & 0x3f)),
           o[2] = (unsigned char)(0x80 | (c >> 6 & 0x3f)), o[3] = (unsigned char)(0x80 | (c & 0x3f)), 4;
}

/* One ID3 string of encoding enc (0 Latin-1, 1 UTF-16 with a BOM, 2 UTF-16BE, 3 UTF-8) from b (n bytes) as UTF-8 into t (cap bytes with its NUL),
 * to its terminator; the bytes it took, the terminator included. */
static unsigned lb_id3str(unsigned enc, const unsigned char *b, unsigned n, unsigned char *t, unsigned cap) {
    unsigned p = 0, o = 0, big = enc == 2;
    if (enc == 1 && n >= 2 && (b[0] == 0xfe || b[0] == 0xff) && (b[0] ^ b[1]) == 1) big = b[0] == 0xfe, p = 2;
    while (p < n) {
        unsigned c;
        if (enc == 1 || enc == 2) {
            if (n - p < 2) break;
            c = big ? (unsigned)b[p] << 8 | b[p + 1] : (unsigned)b[p + 1] << 8 | b[p];
            p += 2;
            if (!c) break;
            if (c >= 0xd800 && c < 0xdc00 && n - p >= 2) {
                unsigned d = big ? (unsigned)b[p] << 8 | b[p + 1] : (unsigned)b[p + 1] << 8 | b[p];
                if (d >= 0xdc00 && d < 0xe000) c = 0x10000 + ((c - 0xd800) << 10) + (d - 0xdc00), p += 2;
            }
        } else {
            c = b[p++];
            if (!c) break;
        }
        if (enc == 3 || c < 0x80) {
            if (o + 1 < cap) t[o++] = (unsigned char)c; /* UTF-8 is copied as it is */
        } else if (o + 5 < cap)
            o += lb_utf8(t + o, c);
    }
    t[o] = 0;
    return p;
}

/* An ID3v2.2-2.4 tag at base (h: its 10 byte header): TXXX:LABEL, else TPUB (v2.2 TXX, TPB). *end gets the tag's end (a FLAC may follow). */
static int lb_id3(int fd, unsigned base, const unsigned char *h, unsigned char *buf, char *out, unsigned size,
                  unsigned *end) {
    unsigned ver = h[3], fl = h[5], pos = base + 10, hl = ver == 2 ? 6 : 10, i, best = 0;
    unsigned char f[10], t[LB_MAX * 2];
    int len = 0;
    *end = base + 10 + sync32(h + 6);
    if (fl & 0x10) *end += 10; /* (a v2.4 footer) */
    if (ver < 2 || ver > 4) return 0;
    if (ver >= 3 && fl & 0x40) { /* an extended header */
        if (lb_rd(fd, pos, f, 4) != 4) return 0;
        pos += ver == 3 ? 4 + be32(f) : sync32(f);
    }
    for (i = 0; i < 1024 && pos + hl <= base + 10 + sync32(h + 6); ++i) {
        unsigned fs, rank, txxx, n, p;
        if (lb_rd(fd, pos, f, hl) != hl || !f[0]) break; /* padding */
        fs = ver == 2 ? (unsigned)f[3] << 16 | f[4] << 8 | f[5] : ver == 3 ? be32(f + 4) : sync32(f + 4);
        txxx = ver == 2 ? lb_eq(f, "TXX", 3) : lb_eq(f, "TXXX", 4);
        rank = txxx ? 2 : (ver == 2 ? lb_eq(f, "TPB", 3) : lb_eq(f, "TPUB", 4)) ? 1 : 0;
        if (ver == 3 && f[9] & 0xe0) rank = 0; /* compressed, encrypted or grouped: not read */
        if (ver == 4 && f[9] & 0x4f) rank = 0;
        n = fs < LB_READ ? fs : LB_READ;
        if (rank > best && fs >= 2 &&
            lb_rd(fd, pos + hl, buf, n) == n) { /* (a frame the file cuts short is not taken) */
            p = 1;
            if (txxx) {
                p += lb_id3str(buf[0], buf + 1, n - 1, t, sizeof t);
                if (!lb_keyis(t, (unsigned)strlen((char *)t), "LABEL")) p = n;
            }
            if (p < n) {
                int got;
                lb_id3str(buf[0], buf + p, n - p, t, sizeof t);
                got = lb_put(out, size, t, sizeof t);
                if (got) best = rank, len = got;
            }
        }
        pos += hl + fs;
    }
    return len;
}

/* A RIFF WAVE (le: little-endian sizes) or AIFF/AIFC FORM file: the ID3v2 tag in its top-level "id3 " or "ID3 " chunk. Chunks are walked by
 * seeking (at most 256 looked at), so the audio costs nothing; a chunk past 2 GB stops the walk. */
static int lb_chunks(int fd, int le, unsigned char *buf, char *out, unsigned size) {
    unsigned char h[10];
    unsigned pos = 12, i, n, end;
    for (i = 0; i < 256 && lb_rd(fd, pos, h, 8) == 8; ++i) {
        n = le ? le32(h + 4) : be32(h + 4);
        if ((lb_eq(h, "id3 ", 4) || lb_eq(h, "ID3 ", 4)) && n >= 10) {
            if (lb_rd(fd, pos + 8, h, 10) != 10 || !lb_eq(h, "ID3", 3)) return 0;
            return lb_id3(fd, pos + 8, h, buf, out, size, &end);
        }
        if (n > 0x7ffffff0u - pos) return 0;
        pos += 8 + n + (n & 1); /* (chunks are padded to an even size) */
    }
    return 0;
}

/* The child box of type t in [pos, end) (at most 256 looked at): *at its start, *len its size; 0 when there is none. A 64-bit size past 4 GB and
 * a box that overruns end stop the walk; size 0 runs to end. */
static int lb_box(int fd, unsigned pos, unsigned end, const char *t, unsigned *at, unsigned *len) {
    unsigned char h[16];
    unsigned i;
    for (i = 0; i < 256 && pos < end && end - pos >= 8; ++i) {
        unsigned n, hdr = 8;
        if (lb_rd(fd, pos, h, 16) < 8) return 0;
        n = be32(h);
        if (n == 1) {
            if (be32(h + 8)) return 0;
            n = be32(h + 12), hdr = 16;
        } else if (!n)
            n = end - pos;
        if (n < hdr || n > end - pos) return 0;
        if (lb_eq(h + 4, t, 4)) return *at = pos, *len = n, 1;
        pos += n;
    }
    return 0;
}

/* An MP4 (M4A) file: the freeform iTunes items ----:com.apple.iTunes:LABEL, else :ORGANIZATION, else :PUBLISHER, under moov/udta/meta/ilst. */
static int lb_mp4(int fd, unsigned char *buf, char *out, unsigned size) {
    unsigned char h[8];
    unsigned at, n, end, i, pos, best = 0;
    int len = 0;
    if (!lb_box(fd, 0, 0xffffffffu, "moov", &at, &n)) return 0;
    end = at + n;
    if (!(lb_box(fd, at + 8, end, "udta", &i, &n) && lb_box(fd, i + 8, i + n, "meta", &at, &n)) &&
        !lb_box(fd, at + 8, end, "meta", &at, &n))
        return 0; /* udta/meta (iTunes), else meta in moov */
    end = at + n;
    pos = at + 8;
    if (lb_rd(fd, pos, h, 8) != 8) return 0;
    if (!lb_eq(h + 4, "hdlr", 4)) pos += 4; /* the full box's version and flags (QuickTime's meta has none) */
    if (!lb_box(fd, pos, end, "ilst", &at, &n)) return 0;
    end = at + n;
    for (pos = at + 8, i = 0; i < 1024 && pos < end && end - pos >= 8; ++i) {
        unsigned in, p, rank = 0, dat = 0, dn = 0;
        if (lb_rd(fd, pos, h, 8) != 8) break;
        in = be32(h);
        if (in < 8 || in > end - pos) break;
        if (lb_eq(h + 4, "----", 4) && in <= LB_READ && lb_rd(fd, pos, buf, in) == in) {
            for (p = 8; in - p >= 8;) {
                unsigned cn = be32(buf + p);
                if (cn < 8 || cn > in - p) break;
                if (lb_eq(buf + p + 4, "name", 4) && cn >= 12)
                    rank = lb_keyis(buf + p + 12, cn - 12, "LABEL")          ? 3
                           : lb_keyis(buf + p + 12, cn - 12, "ORGANIZATION") ? 2
                           : lb_keyis(buf + p + 12, cn - 12, "PUBLISHER")    ? 1
                                                                             : 0;
                else if (lb_eq(buf + p + 4, "data", 4) && cn >= 16 && !dat)
                    dat = p, dn = cn;
                p += cn;
            }
            if (rank > best && dat) {
                unsigned ty = be32(buf + dat + 8) & 0xffffff;
                unsigned char t[LB_MAX * 2];
                int got = 0;
                if (ty == 1 || ty == 0)
                    got = lb_put(out, size, buf + dat + 16, dn - 16);
                else if (ty == 2) {
                    lb_id3str(2, buf + dat + 16, dn - 16, t, sizeof t);
                    got = lb_put(out, size, t, sizeof t);
                }
                if (got) best = rank, len = got;
            }
        }
        pos += in;
    }
    return len;
}

int lb_probe(const char *path, char *out, unsigned size) {
    unsigned char h[10], *buf;
    unsigned end;
    int fd, len = 0;
    if (!out || size < 2) return 0;
    out[0] = 0;
    if (!path || !*path) return 0;
    buf = calloc(1, LB_READ);
    fd = buf ? open(path, O_RDONLY, 0) : -1;
    if (fd < 0) {
        free(buf);
        return 0;
    }
    if (lb_rd(fd, 0, h, 10) == 10) {
        if (lb_eq(h, "fLaC", 4))
            len = lb_flac(fd, 4, buf, out, size);
        else if (lb_eq(h, "OggS", 4))
            len = lb_ogg(fd, buf, out, size);
        else if (lb_eq(h, "ID3", 3)) {
            len = lb_id3(fd, 0, h, buf, out, size, &end);
            if (!len && lb_rd(fd, end, h, 4) == 4 && lb_eq(h, "fLaC", 4)) len = lb_flac(fd, end + 4, buf, out, size);
        } else if (lb_eq(h + 4, "ftyp", 4))
            len = lb_mp4(fd, buf, out, size);
        else if (lb_eq(h, "RIFF", 4) && lb_rd(fd, 8, h, 4) == 4 && lb_eq(h, "WAVE", 4))
            len = lb_chunks(fd, 1, buf, out, size);
        else if (lb_eq(h, "FORM", 4) && lb_rd(fd, 8, h, 4) == 4 && (lb_eq(h, "AIFF", 4) || lb_eq(h, "AIFC", 4)))
            len = lb_chunks(fd, 0, buf, out, size);
    }
    close(fd);
    free(buf);
    if (!len) out[0] = 0;
    return len;
}

/* ------------------------------------------------------------------------------------------------------------------------------------------
 * The table.
 */

static unsigned lb_sum(const unsigned char *p, unsigned n, unsigned h) {
    unsigned i;
    for (i = 0; i < n; ++i) h = (h ^ p[i]) * FNV_PRIME;
    return h;
}

static void lb_free_names(void) {
    unsigned i;
    for (i = 0; i < lb.nlab; ++i) free(lb.names[i]);
    free(lb.names);
    lb.names = (char **)0;
    lb.nlab = lb.cap = 0;
}

/* The index + 1 of label s, added when new; 0 for "" or when the table of labels is full. */
static unsigned lb_intern(const char *s) {
    unsigned i;
    char *d;
    if (!s || !*s) return 0;
    for (i = 0; i < lb.nlab; ++i)
        if (!strcmp(lb.names[i], s)) return i + 1;
    for (i = 0; i < lb.nlab; ++i)
        if (!cc_nocase(lb.names[i], s))
            return i + 1; /* "Warp Records" and "WARP RECORDS" are one label: the first spelling kept */
    if (lb.nlab >= LB_MAX_LABELS) return 0;
    if (lb.nlab == lb.cap) {
        unsigned cap = lb.cap ? lb.cap * 2 : 64;
        char **v = calloc(cap, sizeof *v);
        if (!v) return 0;
        if (lb.nlab) memcpy(v, lb.names, lb.nlab * sizeof *v);
        free(lb.names);
        lb.names = v;
        lb.cap = cap;
    }
    d = strdup(s);
    if (!d) return 0;
    lb.names[lb.nlab] = d;
    return ++lb.nlab;
}

static void lb_read(void) {
    long size = m1_card_mounted() ? cc_file_size(LB_FILE) : 0;
    unsigned char *buf;
    unsigned *h, n, nl, pb, i, ok = 1;
    int fd, got = 0, r = 1;
    lb.tab = (lb_ent_t *)0;
    lb.n = 0;
    if (size < LB_HDR || size > LB_HDR + (long)LB_MAX_ENTRIES * 12 + (long)LB_MAX_LABELS * LB_MAX) return;
    buf = calloc(1, (unsigned)size + 1);
    fd = buf ? open(LB_FILE, O_RDONLY, 0) : -1;
    if (fd < 0) {
        free(buf);
        return;
    }
    while (r > 0 && got < size) {
        r = read(fd, buf + got, (unsigned)(size - got));
        if (r > 0) got += r;
    }
    close(fd);
    h = (unsigned *)buf, n = h[1], nl = h[2], pb = h[3];
    if (got != size || h[0] != LB_MAGIC || n > LB_MAX_ENTRIES || nl > LB_MAX_LABELS ||
        (unsigned)size != LB_HDR + n * 12 + pb || h[4] != lb_sum(buf + LB_HDR, (unsigned)size - LB_HDR, FNV_SEED)) {
        free(buf);
        return;
    }
    {
        const lb_ent_t *e = (const lb_ent_t *)(buf + LB_HDR);
        const char *pool = (const char *)(buf + LB_HDR + n * 12), *s = pool;
        for (i = 0; ok && i < n; ++i)
            if ((i && e[i - 1].key >= e[i].key) || e[i].lab > nl) ok = 0;
        for (i = 0; ok && i < nl; ++i) { /* nl strings, each with its NUL, filling the pool exactly */
            unsigned l = 0;
            while (s + l < pool + pb && s[l]) ++l;
            if (s + l >= pool + pb || !l || lb_intern(s) != i + 1) ok = 0;
            s += l + 1;
        }
        if (ok && s != pool + pb) ok = 0;
        if (ok && n) {
            lb.tab = calloc(n, sizeof *lb.tab);
            if (lb.tab) memcpy(lb.tab, e, n * sizeof *e), lb.n = n;
        }
        if (!ok || !lb.tab) lb_free_names();
    }
    free(buf);
}

void lb_acquire(void) {
    if (lb.users++ == 0) lb_read();
}

void lb_release(void) {
    if (!lb.users || --lb.users) return;
    free(lb.tab);
    free(lb.seen);
    lb_free_names();
    memset(&lb, 0, sizeof lb);
}

unsigned lb_epoch(void) { return lb_clears; }

void lb_clear(void) {
    if (m1_card_mounted()) unlink(LB_FILE), unlink(LB_TMP);
    ++lb_clears;
    free(lb.seen);
    lb.seen = (unsigned char *)0;
    lb.n = 0;
    lb_free_names();
}

static int lb_find(unsigned key) {
    unsigned lo = 0, hi = lb.n;
    while (lo < hi) {
        unsigned mid = lo + (hi - lo) / 2;
        if (lb.tab[mid].key < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    return lo < lb.n && lb.tab[lo].key == key ? (int)lo : -1;
}

const char *lb_file(const char *path, unsigned size) {
    int at = path && *path ? lb_find(lb_key(path)) : -1;
    if (at < 0 || lb.tab[at].size != size) return (const char *)0;
    return lb.tab[at].lab ? lb.names[lb.tab[at].lab - 1] : "";
}

const char *lb_song(void *rec, unsigned *size) {
    const char *path = rec ? (const char *)P(rec, REC_PATH) : (const char *)0;
    long sz = path && *path ? cc_file_size(path) : 0;
    if (sz <= 0 || sz > 0x7fffffff) return (const char *)0;
    if (size) *size = (unsigned)sz;
    return lb_file(path, (unsigned)sz);
}

int lb_has(const char *path) { return path && *path && lb_find(lb_key(path)) >= 0; }

unsigned lb_count(void) { return lb.nlab; }

const char *lb_name(unsigned i) { return i < lb.nlab ? lb.names[i] : (const char *)0; }

const char *lb_path(const char *path) {
    int at = path && *path ? lb_find(lb_key(path)) : -1;
    return at >= 0 && lb.tab[at].lab ? lb.names[lb.tab[at].lab - 1] : (const char *)0;
}

int lb_same(const char *a, const char *b) {
    unsigned x, y;
    if (!a || !b) return 0;
    do {
        x = (unsigned char)*a++, y = (unsigned char)*b++;
        if (x - 'A' < 26) x += 32;
        if (y - 'A' < 26) y += 32;
    } while (x == y && x);
    return x == y;
}

/* The labels no entry names dropped and the entries renumbered, then the whole table written through LB_TMP. */
static int lb_write(void) {
    unsigned *map = lb.nlab ? calloc(lb.nlab, sizeof *map) : (unsigned *)0, i, k = 0, pb = 0, h[5];
    unsigned char *body;
    int r;
    if (lb.nlab && !map) return -1;
    for (i = 0; i < lb.n; ++i)
        if (lb.tab[i].lab) map[lb.tab[i].lab - 1] = 1;
    for (i = 0; i < lb.nlab; ++i)
        if (map[i]) {
            lb.names[k] = lb.names[i];
            map[i] = ++k;
        } else
            free(lb.names[i]);
    lb.nlab = k;
    for (i = 0; i < lb.n; ++i)
        if (lb.tab[i].lab) lb.tab[i].lab = map[lb.tab[i].lab - 1];
    free(map);
    for (i = 0; i < lb.nlab; ++i) pb += (unsigned)strlen(lb.names[i]) + 1;
    body = calloc(1, lb.n * 12 + pb + 1);
    if (!body) return -1;
    if (lb.n) memcpy(body, lb.tab, lb.n * 12);
    for (i = 0, k = lb.n * 12; i < lb.nlab; ++i) {
        unsigned l = (unsigned)strlen(lb.names[i]) + 1;
        memcpy(body + k, lb.names[i], l);
        k += l;
    }
    h[0] = LB_MAGIC, h[1] = lb.n, h[2] = lb.nlab, h[3] = pb, h[4] = lb_sum(body, k, FNV_SEED);
    r = m1_card_write(CARD_DIR, LB_FILE, LB_TMP, h, LB_HDR, body, k);
    free(body);
    return r;
}

int lb_merge(const lb_add_t *add, unsigned n) {
    lb_ent_t *a, *out;
    unsigned i = 0, j = 0, k = 0;
    if (!lb.users || !add || !n) return 0;
    a = calloc(n, sizeof *a);
    out = calloc(lb.n + n + 1, sizeof *out);
    if (!a || !out) {
        free(a);
        free(out);
        return -1;
    }
    for (i = 0; i < n; ++i) a[i].key = add[i].key, a[i].size = add[i].size, a[i].lab = lb_intern(add[i].label);
    /* (an insertion order sort keeps equal keys in order: the last of them wins below) */
    for (i = 1; i < n; ++i) {
        lb_ent_t t = a[i];
        for (j = i; j && a[j - 1].key > t.key; --j) a[j] = a[j - 1];
        a[j] = t;
    }
    i = j = 0;
    while (i < lb.n || j < n) {
        lb_ent_t e;
        if (j >= n || (i < lb.n && lb.tab[i].key < a[j].key))
            e = lb.tab[i++];
        else {
            e = a[j++];
            if (i < lb.n && lb.tab[i].key == e.key) ++i;
        }
        if (k && out[k - 1].key == e.key)
            out[k - 1] = e;
        else
            out[k++] = e;
    }
    free(a);
    free(lb.tab);
    free(lb.seen);
    lb.seen = (unsigned char *)0;
    lb.tab = out;
    lb.n = k;
    return lb_write();
}

void lb_pass_begin(void) {
    free(lb.seen);
    lb.seen = lb.n ? calloc((lb.n + 7) / 8, 1) : (unsigned char *)0;
    lb.named = 0;
}

void lb_pass_seen(const char *path) {
    int at;
    if (!path || !*path) return;
    ++lb.named;
    at = lb.seen ? lb_find(lb_key(path)) : -1;
    if (at >= 0) lb.seen[at >> 3] |= (unsigned char)(1 << (at & 7));
}

unsigned lb_pass_end(void) {
    unsigned i, k = 0, gone;
    if (!lb.seen || !lb.named) {
        free(lb.seen);
        lb.seen = (unsigned char *)0;
        return 0;
    }
    for (i = 0; i < lb.n; ++i)
        if (lb.seen[i >> 3] >> (i & 7) & 1) lb.tab[k++] = lb.tab[i];
    gone = lb.n - k;
    free(lb.seen);
    lb.seen = (unsigned char *)0;
    if (gone) {
        lb.n = k;
        lb_write();
    }
    return gone;
}

void *lb_rows(unsigned max) {
    unsigned i, n = lb_count();
    void *q = n && n <= max ? _create_deque("stSongInfo") : (void *)0;
    if (!q) return (void *)0;
    deque_init(q);
    for (i = 0; i < n; ++i) {
        unsigned rec[REC_SIZE / 4];
        memset(rec, 0, sizeof rec);
        I(rec, REC_ID) = -3 - (int)i;
        *(const char **)((char *)rec + REC_TITLE) = lb_name(i);
        I(rec, REC_TYPE) = 4;
        _deque_push_back(q, rec); /* (the stock copy: the record and its strings) */
    }
    if (!deque_size(q)) {
        deque_destroy(q);
        return (void *)0;
    }
    return q;
}

/* The entry of dir that is a folder (dir 1) or a regular file named one of names[0..nn) (ASCII case aside; an earlier name wins) as dir/entry into out; 1 when found. */
static int lb_entry(const char *dir, int folder, const char *const *names, int nn, char *out, unsigned size) {
    void *d = opendir(dir);
    char *ent;
    int best = nn, r;
    if (!d) return 0;
    while ((ent = readdir(d)) != (char *)0) {
        const char *name = ent + 11; /* struct dirent: d_type at 10, d_name at 11 */
        if (ent[10] != (folder ? 4 : 8)) continue;
        for (r = 0; r < best; ++r)
            if (!cc_nocase(name, names[r])) {
                if ((unsigned)snprintf(out, size, "%s/%s", dir, name) < size) best = r;
                break;
            }
    }
    closedir(d);
    return best < nn;
}

void lb_extras(const char *label, char *logo, unsigned lsize, char *about, unsigned asize) {
    static const char *const pics[3] = {"logo.jpg", "logo.jpeg", "logo.png"}, *const txt[1] = {"about.txt"};
    char dir[LB_MAX + sizeof LB_DIR + 1], path[sizeof dir + 16];
    int fd, n = 0, r, i, o;
    if (lsize) logo[0] = 0;
    if (asize) about[0] = 0;
    if (!label || !*label || !lb_entry(LB_DIR, 1, &label, 1, dir, sizeof dir)) return;
    if (lsize > 8 && lb_entry(dir, 0, pics, 3, path, sizeof path) && strlen(path) + 8 <= lsize)
        snprintf(logo, lsize, "file://%s", path);
    if (asize < 2 || !lb_entry(dir, 0, txt, 1, path, sizeof path) || (fd = open(path, O_RDONLY, 0)) < 0) return;
    while (n < (int)asize - 1 && (r = read(fd, about + n, asize - 1 - (unsigned)n)) > 0) n += r;
    close(fd);
    for (i = o = 0; i < n; ++i) { /* control characters (line ends) as one space; no leading or trailing space */
        char c = (unsigned char)about[i] < 32 ? ' ' : about[i];
        if (c == ' ' && (!o || about[o - 1] == ' ')) continue;
        about[o++] = c;
    }
    if (o) { /* a character the size cut is dropped */
        int k = o - 1;
        unsigned char c;
        while (k > 0 && ((unsigned char)about[k] & 0xc0) == 0x80) --k;
        c = (unsigned char)about[k];
        if (c >= 0xc0 && o - k < (c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : 2)) o = k;
    }
    while (o && about[o - 1] == ' ') --o;
    about[o] = 0;
}
