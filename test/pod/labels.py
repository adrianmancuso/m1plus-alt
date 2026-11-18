"""Record labels (patch/labels.c): the tag reader over synthetic FLAC, Ogg Vorbis, Opus and MP3 (ID3v2.2-2.4) files, and the table file (format,
lookup, merge, a changed size, labels shared and dropped, prune, corrupt files, clear). The compiled payload runs under Unicorn."""
import struct
from harness import *   # noqa: F401,F403  (test/patch.py initialises the harness before loading a module)

LBF, LBT = '/mnt/mmc/.m1plus/labels', '/mnt/mmc/.m1plus/labels.tmp'
MAGIC = 0x424c314d
LB_MAX = 128


def key(path): return fnv(O['FNV_SEED'], path) or 1


def fn(m, name, *args, count=40000000):
    return m.call(PSYMS[name], a0=args[0] if args else 0, a1=args[1] if len(args) > 1 else 0, a2=args[2] if len(args) > 2 else 0, a3=args[3] if len(args) > 3 else 0, count=count)


# ---- synthetic files ---------------------------------------------------------------------------------------------------------------------
def vorbis(*fields, vendor=b'reference libFLAC 1.4.3'):
    body = b''.join(struct.pack('<I', len(f)) + f for f in (x.encode() if isinstance(x, str) else x for x in fields))
    return struct.pack('<I', len(vendor)) + vendor + struct.pack('<I', len(fields)) + body


def flac(*fields, picture=0, comment=True):
    def block(t, data, last=False): return bytes([t | (0x80 if last else 0)]) + len(data).to_bytes(3, 'big') + data
    out = b'fLaC' + block(0, b'\0' * 34, last=not picture and not comment)
    if picture: out += block(6, b'\xaa' * picture, last=not comment)
    if comment: out += block(4, vorbis(*fields), last=True)
    return out + b'\xff\xf8' + b'\0' * 64   # (a frame)


def ogg_pages(packets, per_page=255):
    """Pages carrying packets, at most per_page segments in a page (a packet longer than that crosses pages)."""
    segs = []
    for p in packets:
        n = len(p)
        segs += [(p[i:i + 255], 255) for i in range(0, n - n % 255, 255)] + [(p[n - n % 255:], n % 255)]
    out, seq = b'', 0
    for i in range(0, len(segs), per_page):
        chunk = segs[i:i + per_page]
        out += b'OggS' + bytes([0, 2 if seq == 0 else 0]) + b'\0' * 8 + struct.pack('<III', 1234, seq, 0) + bytes([len(chunk)]) + bytes(l for _, l in chunk) + b''.join(d for d, _ in chunk)
        seq += 1
    return out


def ogg_vorbis(*fields, per_page=255, pad=0):
    return ogg_pages([b'\x01vorbis' + b'\0' * 23, b'\x03vorbis' + vorbis(*fields, vendor=b'Xiph' + b'v' * pad) + b'\x01', b'\x05vorbis' + b'\0' * 40], per_page)


def opus(*fields):
    return ogg_pages([b'OpusHead' + b'\0' * 11, b'OpusTags' + vorbis(*fields, vendor=b'libopus')])


def syncsafe(n): return bytes([(n >> 21) & 0x7f, (n >> 14) & 0x7f, (n >> 7) & 0x7f, n & 0x7f])


def text(enc, s):
    if enc == 0: return s.encode('latin-1') + b'\0'
    if enc == 1: return b'\xff\xfe' + s.encode('utf-16-le') + b'\0\0'
    if enc == 2: return s.encode('utf-16-be') + b'\0\0'
    return s.encode() + b'\0'


def frame(ver, fid, data, flags=0):
    if ver == 2: return fid.encode() + len(data).to_bytes(3, 'big') + data
    return fid.encode() + (syncsafe(len(data)) if ver == 4 else struct.pack('>I', len(data))) + struct.pack('>H', flags) + data


def tpub(ver, s, enc=0): return frame(ver, 'TPB' if ver == 2 else 'TPUB', bytes([enc]) + text(enc, s)[:-1 if enc in (0, 3) else -2])


def txxx(ver, desc, s, enc=0): return frame(ver, 'TXX' if ver == 2 else 'TXXX', bytes([enc]) + text(enc, desc) + text(enc, s))


def box(t, body): return struct.pack('>I', 8 + len(body)) + t.encode('latin-1') + body


def freeform(name, value, ty=1, mean='com.apple.iTunes'):
    """An iTunes ---- item: mean, name, data (type ty: 1 UTF-8, 2 UTF-16BE)."""
    v = value.encode('utf-16-be') if ty == 2 else value.encode()
    return box('----', box('mean', b'\0' * 4 + mean.encode()) + box('name', b'\0' * 4 + name.encode()) + box('data', struct.pack('>II', ty, 0) + v))


def m4a(*items, mdat=b'\0' * 64, moov_last=False, fullbox=True, cover=0, udta=True):
    """An M4A: ftyp, mdat, moov/udta/meta/ilst with items (and a cover of `cover` bytes), moov after mdat when moov_last."""
    ilst = (box('covr', box('data', struct.pack('>II', 13, 0) + b'\xaa' * cover)) if cover else b'') + box('\xa9nam', box('data', struct.pack('>II', 1, 0) + b'Song')) + b''.join(items)
    meta = box('meta', (b'\0' * 4 if fullbox else b'') + box('hdlr', b'\0' * 8 + b'mdirappl' + b'\0' * 9) + box('ilst', ilst))
    moov = box('moov', box('mvhd', b'\0' * 100) + (box('udta', meta) if udta else meta))
    head, data = box('ftyp', b'M4A \0\0\0\0M4A mp42isom'), box('mdat', mdat)
    return head + (data + moov if moov_last else moov + data)


def id3(ver, *frames, padding=64, after=b'\xff\xfb\x90\x00' + b'\0' * 400):
    body = b''.join(frames) + b'\0' * padding
    return b'ID3' + bytes([ver, 0, 0]) + syncsafe(len(body)) + body + after


def utf8(m, a): return bytes(m.u.mem_read(a, 512)).split(b'\0')[0].decode() if a else None


def probe(m, path):
    out = m.alloc(LB_MAX)
    m.u.mem_write(out, b'\xee' * LB_MAX)
    n = fn(m, 'lb_probe', m.cstr(path), out, LB_MAX)
    return n, utf8(m, out)


# ---- the table file -------------------------------------------------------------------------------------------------------------------------
def parse(data):
    """{key: (size, label)} of a table file, or None when it is not valid (the payload's own checks)."""
    if len(data) < 20: return None
    magic, n, nl, pb, check = struct.unpack_from('<5I', data)
    if magic != MAGIC or len(data) != 20 + 12 * n + pb or check != fnv_plain(data[20:]): return None
    ents = [struct.unpack_from('<III', data, 20 + 12 * i) for i in range(n)]
    names = data[20 + 12 * n:].split(b'\0')[:-1]
    if len(names) != nl or any(ents[i][0] >= ents[i + 1][0] for i in range(n - 1)) or any(e[2] > nl for e in ents): return None
    if len(set(e[2] for e in ents if e[2])) != nl: return None   # every label in the pool is named
    return {k: (s, names[l - 1].decode() if l else '') for k, s, l in ents}


def table(rows, names, magic=MAGIC, check=None, sort=True):
    """The file's bytes for [(path, size, label index)] and the pool's names."""
    ents = [(key(p), s, l) for p, s, l in rows]
    if sort: ents.sort()
    pool = b''.join(n.encode() + b'\0' for n in names)
    body = b''.join(struct.pack('<III', *e) for e in ents) + pool
    return struct.pack('<5I', magic, len(ents), len(names), len(pool), fnv_plain(body) if check is None else check) + body


def merge(m, rows):
    a = m.alloc(136 * len(rows) + 4)
    m.u.mem_write(a, b''.join(struct.pack('<II', key(p), s) + l.encode().ljust(LB_MAX, b'\0') for p, s, l in rows))
    r = fn(m, 'lb_merge', a, len(rows))
    return r - (1 << 32) if r >= 1 << 31 else r


def lookup(m, path, size):
    p = fn(m, 'lb_file', m.cstr(path), size)
    return utf8(m, p)


def names(m):
    return sorted(utf8(m, fn(m, 'lb_name', i)) for i in range(fn(m, 'lb_count')))


# ---------------------------------------------------------------------------------------------------------------------------------------------
def probe_tests(ctx):
    """1. The reader: each format's label field, which field wins, text encodings, trimming and cutting, files without one, bounded reads."""
    expect = ctx.expect
    m = Machine()
    cases = {
        'FLAC LABEL': (flac('TITLE=One', 'LABEL=Warp Records', 'ARTIST=Aphex Twin'), 'Warp Records'),
        'FLAC lower-case key': (flac('label=Ninja Tune'), 'Ninja Tune'),
        'FLAC ORGANIZATION': (flac('ORGANIZATION=Blue Note'), 'Blue Note'),
        'FLAC PUBLISHER': (flac('PUBLISHER=Sub Pop'), 'Sub Pop'),
        'FLAC LABEL beats ORGANIZATION and PUBLISHER, in any order': (flac('PUBLISHER=P', 'LABEL=L', 'ORGANIZATION=O'), 'L'),
        'FLAC ORGANIZATION beats PUBLISHER': (flac('PUBLISHER=P', 'ORGANIZATION=O'), 'O'),
        'FLAC an empty LABEL falls back': (flac('LABEL=  ', 'PUBLISHER=P'), 'P'),
        'FLAC the first of two LABELs': (flac('LABEL=First', 'LABEL=Second'), 'First'),
        'FLAC trimmed, controls as spaces': (flac('LABEL=  ECM\tRecords \n'), 'ECM Records'),
        'FLAC UTF-8 kept': (flac('LABEL=Éditions Mego'), 'Éditions Mego'),
        'FLAC the comment after a 300 KB picture': (flac('LABEL=4AD', picture=300000), '4AD'),
        'FLAC no LABEL': (flac('TITLE=x', 'ARTIST=y'), ''),
        'FLAC no comment block': (flac(comment=False), ''),
        'FLAC a field without =': (flac('LABEL', 'LABELX=no', 'LABEL=yes'), 'yes'),
        'Ogg Vorbis': (ogg_vorbis('LABEL=Domino'), 'Domino'),
        'Ogg Vorbis, the comment across three pages': (ogg_vorbis('ARTIST=a', 'LABEL=Rough Trade', per_page=2, pad=700), 'Rough Trade'),
        'Opus': (opus('ORGANIZATION=XL Recordings'), 'XL Recordings'),
        'ID3v2.3 TPUB': (id3(3, frame(3, 'TIT2', b'\0Song'), tpub(3, 'Factory')), 'Factory'),
        'ID3v2.3 TPUB after a 200 KB APIC': (id3(3, frame(3, 'APIC', b'\0image/jpeg\0\3\0' + b'\xaa' * 200000), tpub(3, 'Mute')), 'Mute'),
        'ID3v2.4 TXXX:LABEL beats TPUB': (id3(4, tpub(4, 'Publisher'), txxx(4, 'LABEL', 'Label')), 'Label'),
        'ID3v2.4 TXXX:label, any case': (id3(4, txxx(4, 'label', 'Hyperdub', enc=3)), 'Hyperdub'),
        'ID3v2.4 another TXXX is not the label': (id3(4, txxx(4, 'CATALOGNUMBER', 'WARP123'), tpub(4, 'Warp')), 'Warp'),
        'ID3v2.3 Latin-1 to UTF-8': (id3(3, tpub(3, 'Société', enc=0)), 'Société'),
        'ID3v2.3 UTF-16 with a BOM': (id3(3, tpub(3, 'Ünder 𝄞', enc=1)), 'Ünder 𝄞'),
        'ID3v2.4 UTF-16BE': (id3(4, tpub(4, 'Big Ender', enc=2)), 'Big Ender'),
        'ID3v2.4 UTF-8': (id3(4, tpub(4, 'ディスク', enc=3)), 'ディスク'),
        'ID3v2.4 the first of several values': (id3(4, frame(4, 'TPUB', b'\3One\0Two')), 'One'),
        'ID3v2.4 a compressed frame is not read': (id3(4, frame(4, 'TPUB', b'\0Squeezed', flags=0x0008)), ''),
        'ID3v2.2 TPB': (id3(2, frame(2, 'TT2', b'\0Song'), tpub(2, 'Rephlex')), 'Rephlex'),
        'ID3v2.2 TXX:LABEL': (id3(2, tpub(2, 'P'), txxx(2, 'LABEL', 'L')), 'L'),
        'MP3 no label': (id3(3, frame(3, 'TIT2', b'\0Song')), ''),
        'FLAC behind an ID3 tag': (id3(3, frame(3, 'TIT2', b'\0x'), after=flac('LABEL=Planet Mu')), 'Planet Mu'),
        'ID3 label wins over the FLAC behind it': (id3(3, tpub(3, 'Outer'), after=flac('LABEL=Inner')), 'Outer'),
        'M4A LABEL': (m4a(freeform('LABEL', 'INA-GRM')), 'INA-GRM'),
        'M4A label, any case': (m4a(freeform('label', 'Warp')), 'Warp'),
        'M4A LABEL beats ORGANIZATION and PUBLISHER': (m4a(freeform('PUBLISHER', 'P'), freeform('LABEL', 'L'), freeform('ORGANIZATION', 'O')), 'L'),
        'M4A PUBLISHER': (m4a(freeform('PUBLISHER', 'Sub Pop')), 'Sub Pop'),
        'M4A moov after a large mdat': (m4a(freeform('LABEL', 'Kompakt'), mdat=b'\0' * 400000, moov_last=True), 'Kompakt'),
        'M4A after a 300 KB cover': (m4a(freeform('LABEL', 'Editions Mego'), cover=300000), 'Editions Mego'),
        'M4A UTF-16 data': (m4a(freeform('LABEL', 'スペクトル', ty=2)), 'スペクトル'),
        'M4A UTF-8 kept': (m4a(freeform('LABEL', 'Éditions')), 'Éditions'),
        'M4A QuickTime meta (no version)': (m4a(freeform('LABEL', 'QT'), fullbox=False), 'QT'),
        'M4A meta directly in moov': (m4a(freeform('LABEL', 'Direct'), udta=False), 'Direct'),
        'M4A another freeform is not the label': (m4a(freeform('CATALOGNUMBER', 'X1'), freeform('iTunNORM', 'y')), ''),
        'M4A a binary data item is not taken': (m4a(freeform('LABEL', 'bin', ty=0x15)), ''),
        'M4A no label': (m4a(), ''),
        'a WAV': (b'RIFF' + b'\0' * 100, ''),
        'an empty file': (b'', ''),
        'garbage': (b'\xff' * 50, ''),
    }
    for what, (data, want) in cases.items():
        path = '/mnt/mmc/Music/' + str(abs(hash(what))) + '.x'
        m.fs[path] = data
        n, got = probe(m, path)
        expect(got == want and n == len(want.encode()), f'{what}: {want!r} (got {got!r}, {n})')
    expect(probe(m, '/mnt/mmc/none.flac') == (0, ''), 'a missing file has none')

    # the comment is cut to LB_MAX - 1 bytes at a character boundary
    long = 'é' * 100   # 200 bytes
    m.fs['/l.flac'] = flac('LABEL=' + long)
    n, got = probe(m, '/l.flac')
    expect(n == 126 and got == 'é' * 63, f'a 200 byte label is cut to 126 bytes, not through a character ({n})')
    m.fs['/l2.flac'] = flac('LABEL=' + 'a' * 127 + 'é')
    expect(probe(m, '/l2.flac') == (127, 'a' * 127), 'cut before a character that does not fit whole')

    # truncated and lying sizes never read past what is there
    good = flac('LABEL=Kranky')
    for cut in (4, 8, 40, 46, 60, len(good) - 70):
        m.fs['/t.flac'] = good[:cut]
        n, got = probe(m, '/t.flac')
        expect(got in ('', 'Kranky'[:max(0, n)]) and n <= 6, f'a FLAC cut at {cut} bytes reads safely ({got!r})')
    m.fs['/v.flac'] = b'fLaC' + bytes([0x84]) + (1000).to_bytes(3, 'big') + struct.pack('<I', 0xfffffff0) + b'x' * 20
    expect(probe(m, '/v.flac') == (0, ''), 'a vendor length past the block is refused')
    m.fs['/c.flac'] = b'fLaC' + bytes([0x84]) + (30).to_bytes(3, 'big') + struct.pack('<II', 0, 1000) + struct.pack('<I', 0xffffff00) + b'LABEL=Cut'
    n, got = probe(m, '/c.flac')
    expect(got == 'Cut', f'a field longer than the block is read to the block\'s end ({got!r})')
    m.fs['/i.mp3'] = b'ID3\4\0\0' + syncsafe(1 << 27) + b'TPUB' + syncsafe(1000) + b'\0\0' + b'\0' + b'A' * 10
    expect(probe(m, '/i.mp3')[1] == '', 'an ID3 tag that claims more than the file: the frame cut short is not taken')

    good = m4a(freeform('LABEL', 'Raster'), moov_last=True)   # the moov (and its label) at the end, so every cut is through it
    for cut in (8, 30, 60, len(good) // 2, len(good) - 4):
        m.fs['/t.m4a'] = good[:cut]
        n, got = probe(m, '/t.m4a')
        expect(got == '', f'an M4A cut at {cut} bytes reads safely, without the label ({got!r})')
    lie = bytearray(good); i = lie.index(b'ilst') - 4; lie[i:i + 4] = struct.pack('>I', 0xfffffff0)
    m.fs['/l.m4a'] = bytes(lie)
    expect(probe(m, '/l.m4a') == (0, ''), 'an M4A box that claims more than its parent is refused')
    m.fs['/z.m4a'] = box('ftyp', b'M4A ') + struct.pack('>I', 0) + b'free' + b'\0' * 32
    expect(probe(m, '/z.m4a') == (0, ''), 'an M4A whose last box runs to the end, no moov')
    m.fs['/w.m4a'] = box('ftyp', b'M4A ') + struct.pack('>I', 1) + b'mdat' + struct.pack('>II', 1, 16) + b'\0' * 16 + box('moov', b'')
    expect(probe(m, '/w.m4a') == (0, ''), 'an mdat past 4 GB stops the walk')

    # bounded: the picture in front of the comment is skipped by seeking, not read
    m = Machine()
    m.fs['/p.flac'] = flac('LABEL=Thrill Jockey', picture=5_000_000)
    probe(m, '/p.flac')
    seeks = [c for c in m.calls if c[0] == 'lseek' and c[1] == '/p.flac']
    expect(len([c for c in m.calls if c[0] == 'read']) <= 12 and any(c[2] > 5_000_000 for c in seeks), 'a 5 MB picture is seeked over, a handful of reads')
    expect(not [c for c in m.calls if c[0] == 'open' and c[1] == '/p.flac' and c[2] & 3], 'the file is opened read-only')
    expect(len([c for c in m.calls if c[0] == 'open']) == len([c for c in m.calls if c[0] == 'close']), 'every open is closed')


def table_tests(ctx):
    """2. The table: merge, lookup by path and size, shared labels, persistence, prune, corrupt files, clear."""
    expect = ctx.expect
    m = Machine()
    fn(m, 'lb_acquire')
    r = merge(m, [('/a.flac', 10, 'Warp'), ('/b.flac', 20, ''), ('/c.flac', 30, 'WARP'), ('/d.flac', 40, 'Ninja Tune')])
    expect(r == 0 and ('rename', LBT, LBF) in m.calls and ('fsync', LBT) in m.calls and LBT not in m.fs, 'a merge writes the file through the tmp file, synced and renamed')
    expect(lookup(m, '/a.flac', 10) == 'Warp' and lookup(m, '/c.flac', 30) == 'Warp', 'one label in any case is one label, the first spelling kept')
    expect(lookup(m, '/b.flac', 20) == '', 'a file read without a label is known to have none ("")')
    expect(lookup(m, '/a.flac', 11) is None and lookup(m, '/z.flac', 1) is None and lookup(m, '', 1) is None, 'a changed size, an unknown path, an empty path: not read yet')
    expect(names(m) == ['Ninja Tune', 'Warp'], f'two distinct labels ({names(m)})')
    t = parse(m.fs[LBF])
    expect(t == {key('/a.flac'): (10, 'Warp'), key('/b.flac'): (20, ''), key('/c.flac'): (30, 'Warp'), key('/d.flac'): (40, 'Ninja Tune')}, f'the file holds the entries ({t})')

    # a re-read replaces the entry; a label no entry names leaves the pool
    merge(m, [('/d.flac', 41, 'Warp'), ('/e.flac', 5, 'Kranky'), ('/e.flac', 6, 'Kranky 2')])
    expect(lookup(m, '/d.flac', 41) == 'Warp' and lookup(m, '/d.flac', 40) is None, 'the new size and label replace the old')
    expect(lookup(m, '/e.flac', 6) == 'Kranky 2', 'the last of two entries for one file wins')
    expect(names(m) == ['Kranky 2', 'Warp'] and parse(m.fs[LBF]) is not None, f'labels nobody names are dropped from memory and the file ({names(m)})')
    fn(m, 'lb_release')

    # read back by the next acquire
    fn(m, 'lb_acquire')
    expect(lookup(m, '/a.flac', 10) == 'Warp' and lookup(m, '/e.flac', 6) == 'Kranky 2' and lookup(m, '/b.flac', 20) == '', 'the next acquire reads the table back')
    expect(names(m) == ['Kranky 2', 'Warp'], 'with its labels')

    # a pass drops the files nobody named, and their labels
    fn(m, 'lb_pass_begin')
    for p in ('/a.flac', '/b.flac', '/new.flac'): fn(m, 'lb_pass_seen', m.cstr(p))
    gone = fn(m, 'lb_pass_end')
    expect(gone == 3 and names(m) == ['Warp'] and lookup(m, '/e.flac', 6) is None, f'three files not named are dropped, Kranky 2 with them ({gone})')
    expect(parse(m.fs[LBF]) == {key('/a.flac'): (10, 'Warp'), key('/b.flac'): (20, '')}, 'and the file written')
    fn(m, 'lb_pass_begin'); gone = fn(m, 'lb_pass_end')
    expect(gone == 0 and lookup(m, '/a.flac', 10) == 'Warp', 'a pass that named nothing drops nothing (a library that did not load)')

    # clear
    fn(m, 'lb_clear')
    expect(LBF not in m.fs and lookup(m, '/a.flac', 10) is None and fn(m, 'lb_count') == 0, 'clear deletes the file and empties the table')
    fn(m, 'lb_release')

    # lookups by bisection
    m = Machine().count()
    paths = [f'/mnt/mmc/lib/{i:04d}.flac' for i in range(2000)]
    labels = [f'Label {i % 37}' for i in range(2000)]
    fn(m, 'lb_acquire')
    merge(m, list(zip(paths, range(1000, 3000), labels)))
    expect(all(lookup(m, p, 1000 + i) == labels[i] for i, p in enumerate(paths)), '2000 entries all found')
    expect(fn(m, 'lb_count') == 37, '37 labels')
    before = m.icount; lookup(m, paths[1234], 2234)
    expect(m.icount - before < 2000, f'a lookup is a bisection ({m.icount} instructions)')
    fn(m, 'lb_release')

    # corrupt files are ignored and replaced by the next merge
    rows, pool = [('/a.flac', 10, 1), ('/b.flac', 20, 2)], ['A', 'B']
    bad = {
        'wrong magic': table(rows, pool, magic=1),
        'wrong checksum': table(rows, pool, check=1),
        'unsorted': table(sorted(rows, key=lambda r: -key(r[0])), pool, sort=False),
        'a label index past the pool': table([('/a.flac', 10, 3)], pool),
        'a pool short of its count': table(rows, pool)[:-2],
        'an empty name in the pool': table([('/a.flac', 10, 1)], ['']),
        'garbage': b'\xff' * 64,
    }
    for what, data in bad.items():
        m = Machine(); m.fs[LBF] = data
        fn(m, 'lb_acquire')
        expect(lookup(m, '/a.flac', 10) is None and fn(m, 'lb_count') == 0, f'{what}: ignored')
        merge(m, [('/z.flac', 1, 'Z')])
        expect(parse(m.fs[LBF]) == {key('/z.flac'): (1, 'Z')}, f'{what}: the next merge writes a valid file')
        fn(m, 'lb_release')
    m = Machine(); m.fs[LBF] = table(rows, pool)
    fn(m, 'lb_acquire')
    expect(lookup(m, '/a.flac', 10) == 'A' and lookup(m, '/b.flac', 20) == 'B', 'a valid file written by the test reads')
    fn(m, 'lb_release')

    # the card out: nothing read, nothing written
    m = Machine(); m.card_mounted = False; m.fs[LBF] = table(rows, pool)
    fn(m, 'lb_acquire')
    expect(lookup(m, '/a.flac', 10) is None, 'no card: no table')
    r = merge(m, [('/z.flac', 1, 'Z')])
    expect(r == -1 and parse(m.fs[LBF]) == {key('/a.flac'): (10, 'A'), key('/b.flac'): (20, 'B')}, 'no card: the merge writes nothing (-1)')
    fn(m, 'lb_release')

    # a song record
    m = Machine(); m.fs['/mnt/mmc/x.flac'] = b'\0' * 777
    fn(m, 'lb_acquire'); merge(m, [('/mnt/mmc/x.flac', 777, 'Erased Tapes')])
    rec = m.record(path='/mnt/mmc/x.flac'); sz = m.alloc(4)
    p = fn(m, 'lb_song', rec, sz)
    expect(p and utf8(m, p) == 'Erased Tapes' and m.word(sz) == 777, 'a record\'s label by its file\'s size')
    m.fs['/mnt/mmc/x.flac'] = b'\0' * 778
    expect(fn(m, 'lb_song', rec, sz) == 0 and m.word(sz) == 778, 'a retagged file (a new size) is not read yet, with the size to read it with')
    fn(m, 'lb_release')


SECTIONS = [probe_tests, table_tests]
