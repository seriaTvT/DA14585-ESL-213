"""Build every font the firmware can draw, from one command.

    python3 tools/genfont.py                 # report what would be built
    python3 tools/genfont.py --show 年月日ab   # preview glyphs as ASCII art
    python3 tools/genfont.py --emit          # write the C and JS tables

There are three fonts and three kinds of source, but one output and one
structure, because the previous arrangement - each font its own table shape,
hand-pasted into two files - is what let the panel and the preview drift apart.

    5x7     tools/font5.py    ASCII art, hand-drawn. The general-purpose face.
    16x24   tools/font16.py   ASCII art, hand-drawn. Digits and ':' only.
    16x16   tools/glyphs.txt  a character list, rendered from Noto Sans CJK.

All three end up as an epd_font_t: a codepoint-sorted index into a shared byte
array, column-major with the LSB at the top row. Adding a fourth font means
adding a source and a row to FONTS, not a new lookup path in epd_gfx.c.

WHY THE CJK FONT IS A LIST AND NOT A FONT
    A full Chinese font at 16x16 is 120 KB. The DA14585 runs its image from
    96 KiB of SysRAM, so that is not a tight fit but an impossible one, and
    the nRF52811 has ~70 KiB of free flash for everything. Listing the
    characters the faces actually draw keeps the whole table around 3 KB and
    needs no external storage at all.

CHINESE AND JAPANESE TOGETHER
    Mostly free: the two languages differ by codepoint far more than by
    shape, and 时/時 or 电/電 are separate characters that coexist in one
    table with nothing to decide.

    Where they share a codepoint, Noto CJK carries two designs and picks
    between them by per-language cmap. We ship bitmaps, so one has to be
    chosen at build time; '@lang' in glyphs.txt does that per section, and
    the report names every character where the choice was real.

    Do not judge that from the rendered pixels. A one-row shift in a
    horizontal stroke changes 28 pixels of a 256-pixel cell without changing
    the shape - 六 differs more between the faces (36 px) than 直 does
    (34 px), yet 直 is the textbook genuine difference and 六 is the same
    glyph rasterised one row down. The cmap is the only signal that means
    anything, so that is what the report uses.

LICENCE
    Noto Sans CJK is SIL Open Font License 1.1, which permits embedding
    rendered bitmaps with no notice obligation on the firmware image. Do not
    repoint NOTO at SimSun, MS YaHei or the fonts the vendor's own tools
    default to - those are Microsoft-licensed and cannot ship here. The two
    hand-drawn fonts stay hand-drawn for the same reason.
"""
import argparse
import sys
import unicodedata
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / 'tools'))

import font5                                                    # noqa: E402
import font16                                                   # noqa: E402

MANIFEST = ROOT / 'tools' / 'glyphs.txt'
C_OUT = ROOT / 'firmware' / 'hema_epd_clock' / 'src' / 'epd' / 'epd_font_data.c'
H_OUT = C_OUT.with_suffix('.h')
JS_OUT = ROOT / 'webui' / 'font_data.js'

NOTO = '/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc'

# Face indices within the collection. The proportional faces carry the CJK
# designs; the Mono ones give ASCII an exactly 8.0 px advance, which is what
# makes the half-width cell come out square against the full-width one instead
# of 8.88 px and a fractional gap.
FACE = {'jp': 0, 'sc': 2, 'tc': 3}
FACE_MONO = {'jp': 5, 'sc': 7, 'tc': 8}
DEFAULT_LANG = 'sc'

CJK_CELL = 16
CJK_HALF = 8
# Baseline row, from the font's own sTypoAscender/upem = 880/1000. Fixed for
# every glyph rather than per-glyph centring: centring each cell on its own ink
# is what makes a row of them sit at visibly different heights.
CJK_BASELINE = round(CJK_CELL * 880 / 1000)

ASCII = [chr(c) for c in range(0x20, 0x7F)]


# --------------------------------------------------------------------------
# sources
# --------------------------------------------------------------------------

def from_art(art, w, h, who):
    """Hand-drawn art -> {char: columns of booleans}, column-major, LSB = top.

    Short glyphs are padded at the top and narrow rows on the right, so every
    glyph in a font shares the cell's last row as its baseline. font16.py has
    the long version of why: centring instead gave each glyph a baseline of
    its own, and a '4' and a '0' sat at different depths in one string."""
    out = {}
    for ch, block in art.items():
        rows = block.strip('\n').split('\n')
        if len(rows) > h:
            sys.exit(f'{who}: {ch!r} is {len(rows)} rows, max {h}')
        if any(len(r) > w for r in rows):
            sys.exit(f'{who}: {ch!r} is {max(map(len, rows))} wide, max {w}')
        rows = ['.' * w] * (h - len(rows)) + [r.ljust(w, '.') for r in rows]
        out[ch] = [[rows[y][x] == '#' for y in range(h)] for x in range(w)]
    return out


def read_manifest(path):
    """-> ([(char, lang, section)], [section names]) in file order."""
    entries, sections, lang, section = [], [], DEFAULT_LANG, '(top)'
    for lineno, raw in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        if line.startswith('[') and line.endswith(']'):
            section, lang = line[1:-1].strip(), DEFAULT_LANG
            sections.append(section)
            continue
        if line.startswith('@lang'):
            parts = line.split(None, 1)
            lang = parts[1].strip().lower() if len(parts) > 1 else ''
            if lang not in FACE:
                sys.exit(f'{path}:{lineno}: @lang must be one of '
                         f'{", ".join(sorted(FACE))}, got {lang!r}')
            continue
        for ch in line:
            if ch.isspace():
                continue
            if ord(ch) < 0x80:
                sys.exit(f'{path}:{lineno}: {ch!r} is ASCII, which every font '
                         f'carries already - remove it from the manifest')
            entries.append((ch, lang, section))
    return entries, sections


def resolve(entries):
    """One entry per codepoint. A codepoint wanted in two languages is a
    conflict for the manifest to settle, not something to silently pick."""
    chosen, conflicts = {}, []
    for ch, lang, section in entries:
        prev = chosen.get(ch)
        if prev is None:
            chosen[ch] = (lang, section)
        elif prev[0] != lang:
            conflicts.append((ch, prev, (lang, section)))
    return chosen, conflicts


# --------------------------------------------------------------------------
# Noto rendering
# --------------------------------------------------------------------------

_faces, _cmaps = {}, {}


def _pil():
    try:
        from PIL import Image, ImageDraw, ImageFont
    except ImportError as e:
        sys.exit(f'the 16x16 font needs pillow: {e}')
    return Image, ImageDraw, ImageFont


def render_noto(ch, lang, width):
    """-> `width` columns of CJK_CELL booleans, top first."""
    Image, ImageDraw, ImageFont = _pil()
    idx = (FACE_MONO if width == CJK_HALF else FACE)[lang]
    if idx not in _faces:
        _faces[idx] = ImageFont.truetype(NOTO, CJK_CELL, index=idx)
    img = Image.new('L', (width, CJK_CELL), 0)
    ImageDraw.Draw(img).text((0, CJK_BASELINE), ch, font=_faces[idx], fill=255,
                             anchor='ls')
    px = img.load()
    return [[px[x, y] >= 128 for y in range(CJK_CELL)] for x in range(width)]


def cmap(lang):
    if lang not in _cmaps:
        try:
            from fontTools.ttLib import TTCollection
        except ImportError as e:
            sys.exit(f'the language report needs fonttools: {e}')
        _cmaps[lang] = TTCollection(NOTO).fonts[FACE[lang]].getBestCmap()
    return _cmaps[lang]


def divergent(chars):
    """Characters Noto draws differently in Chinese and Japanese, by cmap -
    exact, and deliberately not a pixel comparison. See the module docstring."""
    cn, jp = cmap('sc'), cmap('jp')
    return [ch for ch in chars
            if cn.get(ord(ch)) is not None
            and jp.get(ord(ch)) is not None
            and cn.get(ord(ch)) != jp.get(ord(ch))]


def pixel_delta(ch):
    """How much that difference shows at 16x16. Informational only: a large
    number can still be a one-row shift, so this ranks, it does not decide."""
    a, b = render_noto(ch, 'sc', CJK_CELL), render_noto(ch, 'jp', CJK_CELL)
    return sum(x != y for ca, cb in zip(a, b) for x, y in zip(ca, cb))


# --------------------------------------------------------------------------
# packing
# --------------------------------------------------------------------------

class Font:
    def __init__(self, name, cid, height, glyphs):
        self.name, self.cid, self.height = name, cid, height
        self.bpc = (height + 7) // 8            # bytes per column
        self.glyphs = dict(sorted(glyphs.items(), key=lambda kv: ord(kv[0])))
        self.index = []                          # [(cp, offset, width)]
        self.blob = bytearray()

    def pack(self, columns):
        out = bytearray()
        for col in columns:
            for b in range(self.bpc):
                byte = 0
                for i in range(8):
                    row = b * 8 + i
                    if row < self.height and col[row]:
                        byte |= 1 << i
                out.append(byte)
        return bytes(out)

    def build(self):
        seen = {}
        for ch, columns in self.glyphs.items():
            bits = self.pack(columns)
            # Identical bitmaps are common - blanks especially - so share them.
            if bits not in seen:
                seen[bits] = len(self.blob)
                self.blob += bits
            self.index.append((ord(ch), seen[bits], len(columns)))
        return self

    @property
    def size(self):
        return len(self.index) * 5 + len(self.blob)


def build_all(chosen):
    f5 = from_art(font5.ART, 5, 7, 'tools/font5.py')
    f16 = from_art(font16.ART, font16.W, font16.H, 'tools/font16.py')
    cjk = {ch: render_noto(ch, DEFAULT_LANG, CJK_HALF) for ch in ASCII}
    cjk.update({ch: render_noto(ch, lang, CJK_CELL)
                for ch, (lang, _) in chosen.items()})
    return [Font('5x7', 'EPD_FONT_5X7', 7, f5).build(),
            Font('16x24', 'EPD_FONT_16X24', font16.H, f16).build(),
            Font('16x16 CJK', 'EPD_FONT_CJK16', CJK_CELL, cjk).build()]


# --------------------------------------------------------------------------
# emit
# --------------------------------------------------------------------------

BANNER = ('Generated by tools/genfont.py - do not edit.\n'
          'Sources: tools/font5.py, tools/font16.py, tools/glyphs.txt.\n'
          'Change one of those and re-run `python3 tools/genfont.py --emit`.')


def label(cp):
    ch = chr(cp)
    if ch == '*/':
        return '?'
    return ch if ch.isprintable() and not ch.isspace() and ch != '/' else ' '


def wrap(values, per_line, fmt, indent):
    return '\n'.join(
        indent + ', '.join(fmt(v) for v in values[i:i + per_line]) + ','
        for i in range(0, len(values), per_line))


def emit_c(fonts):
    parts = ['/* ' + BANNER.replace('\n', '\n * ') + ' */\n',
             '#include "epd_font_data.h"\n']
    for i, f in enumerate(fonts):
        tag = f'F{i}'
        idx = '\n'.join(
            f'    {{ 0x{cp:04X}, {off:5d}, {w:2d} }},  /* {label(cp)} */'
            for cp, off, w in f.index)
        parts.append(
            f'/* --- {f.name} ------------------------------------------- */\n'
            f'static const epd_glyph_t {tag}_INDEX[] = {{\n{idx}\n}};\n\n'
            f'static const uint8_t {tag}_BITS[{len(f.blob)}] = {{\n'
            f'{wrap(list(f.blob), 12, lambda b: f"0x{b:02X}", "    ")}\n}};\n')
    table = '\n'.join(
        f'    [{f.cid}] = {{ F{i}_INDEX, {len(f.index)}, F{i}_BITS, '
        f'{f.height}, {f.bpc} }},'
        for i, f in enumerate(fonts))
    parts.append(f'const epd_font_t EPD_FONTS[EPD_FONT_COUNT] = {{\n'
                 f'{table}\n}};\n')
    return '\n'.join(parts)


def emit_h(fonts):
    ids = '\n'.join(f'#define {f.cid:<16} {i}   /* {f.name} */'
                    for i, f in enumerate(fonts))
    return f'''/* {BANNER.replace(chr(10), chr(10) + " * ")} */

#ifndef _EPD_FONT_DATA_H_
#define _EPD_FONT_DATA_H_

#include <stdint.h>

{ids}
#define EPD_FONT_COUNT   {len(fonts)}

/* One glyph. `off` indexes its font's bits[], which is column-major with the
 * LSB at the top row and `bpc` bytes per column. `w` is the cell width in
 * pixels and varies within a font: the 16x16 face stores ASCII at 8 px so a
 * string can mix half- and full-width cells on one baseline. */
typedef struct {{
    uint16_t cp;
    uint16_t off;
    uint8_t  w;
}} epd_glyph_t;

/* `index` is sorted by codepoint, so lookup bisects - see epd_font_find(). */
typedef struct {{
    const epd_glyph_t *index;
    uint16_t           count;
    const uint8_t     *bits;
    uint8_t            h;
    uint8_t            bpc;
}} epd_font_t;

extern const epd_font_t EPD_FONTS[EPD_FONT_COUNT];

#endif /* _EPD_FONT_DATA_H_ */
'''


def emit_js(fonts):
    parts = ['// ' + BANNER.replace('\n', '\n// '),
             '//\n// Mirrors epd_font_data.c. The preview and the panel have to'
             ' agree about what\n// a character looks like, so both come from '
             'one run of the generator.\n']
    names = []
    for i, f in enumerate(fonts):
        idx = wrap(f.index, 6, lambda t: f'[{t[0]},{t[1]},{t[2]}]', '  ')
        bits = wrap(list(f.blob), 16, str, '  ')
        parts.append(f'const F{i} = {{\n'
                     f'  h: {f.height}, bpc: {f.bpc},\n'
                     f'  index: [\n{idx}\n  ],\n'
                     f'  bits: Uint8Array.from([\n{bits}\n  ]),\n}};\n')
        names.append(f'F{i}')
    ids = '\n'.join(f'export const {f.cid} = {i};' for i, f in enumerate(fonts))
    parts.append(f'{ids}\n\nexport const FONTS = [{", ".join(names)}];\n')
    return '\n'.join(parts)


# --------------------------------------------------------------------------
# report
# --------------------------------------------------------------------------

def show(chars, chosen, fonts):
    by_cp = [{cp: (off, w) for cp, off, w in f.index} for f in fonts]
    for ch in chars:
        for f, table in zip(fonts, by_cp):
            hit = table.get(ord(ch))
            if hit is None:
                continue
            off, w = hit
            print(f'--- {ch} U+{ord(ch):04X}  {f.name}  {w}x{f.height}  '
                  f'{unicodedata.name(ch, "?")}')
            for y in range(f.height):
                print('    ' + ''.join(
                    '#' if (f.blob[off + x * f.bpc + (y >> 3)] >> (y & 7)) & 1
                    else '.' for x in range(w)))


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--emit', action='store_true', help='write the tables')
    ap.add_argument('--check', action='store_true',
                    help='exit non-zero if the tables on disk are stale')
    ap.add_argument('--show', metavar='CHARS', help='preview glyphs and exit')
    args = ap.parse_args()

    entries, sections = read_manifest(MANIFEST)
    chosen, conflicts = resolve(entries)
    if conflicts:
        for ch, (la, sa), (lb, sb) in conflicts:
            print(f'conflict: {ch} U+{ord(ch):04X} wanted as {la} in [{sa}] '
                  f'and as {lb} in [{sb}]', file=sys.stderr)
        sys.exit('a codepoint is stored once - settle it in glyphs.txt')

    fonts = build_all(chosen)

    if args.show:
        show(args.show, chosen, fonts)
        return

    # Checked before the report so a stale tree fails loudly and says which
    # file, rather than printing a size table that describes neither copy.
    if args.check:
        stale = [p.relative_to(ROOT)
                 for p, text in ((C_OUT, emit_c(fonts)),
                                 (H_OUT, emit_h(fonts)),
                                 (JS_OUT, emit_js(fonts)))
                 if not p.exists() or p.read_text(encoding='utf-8') != text]
        if stale:
            for p in stale:
                print(f'stale: {p}', file=sys.stderr)
            sys.exit('the generated tables do not match their sources - '
                     'run `python3 tools/genfont.py --emit`')
        print('generated tables are up to date')
        return

    total = 0
    for f in fonts:
        print(f'  {f.name:<10} {len(f.index):4d} glyphs  '
              f'{len(f.index) * 5:5d} B index + {len(f.blob):5d} B bitmaps '
              f'= {f.size:5d} B')
        total += f.size
    print(f'  {"total":<10} {total:>44d} B')
    print(f'\n  glyphs.txt: {len(sections)} sections, {len(chosen)} characters')

    div = divergent(chosen)
    if div:
        print(f'\n  {len(div)} of {len(chosen)} have separate Chinese and '
              f'Japanese designs in Noto:')
        for ch in sorted(div, key=pixel_delta, reverse=True):
            lang, section = chosen[ch]
            print(f'    {ch} U+{ord(ch):04X}  built as {lang}  '
                  f'({pixel_delta(ch):3d}/256 px apart)  [{section}]')
        print('  Each is stored in the language its section asked for. Move a '
              'character to\n  a section with a different @lang if a face '
              'needs the other design.')

    if args.emit:
        C_OUT.write_text(emit_c(fonts), encoding='utf-8')
        H_OUT.write_text(emit_h(fonts), encoding='utf-8')
        JS_OUT.write_text(emit_js(fonts), encoding='utf-8')
        for p in (C_OUT, H_OUT, JS_OUT):
            print(f'wrote {p.relative_to(ROOT)}')
    else:
        print('\n(dry run - pass --emit to write the tables)')


if __name__ == '__main__':
    main()
