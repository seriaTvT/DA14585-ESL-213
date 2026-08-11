"""Build webui/faces_data.js from the face files in webui/faces/.

    python3 tools/genfaces.py           # report what would be built
    python3 tools/genfaces.py --emit    # write the bundle
    python3 tools/genfaces.py --check   # exit non-zero if the bundle is stale

Same shape as tools/genfont.py, and for the same reason: editable sources, one
generated artifact, and a --check the tests run so the two cannot drift.

WHY A BUNDLE RATHER THAN FETCHING THE FILES
    A static server cannot list a directory, so loading the faces at runtime
    would need a manifest listing them - one more file to fall out of step
    with the directory it describes. Bundling costs a regeneration step and
    removes that class of bug entirely; webui/serve.py runs it on startup, so
    editing a face stays edit-and-refresh.

    It also keeps the faces importable from Node exactly as from the browser,
    which is what lets webui/test.mjs render every one of them against the
    firmware's own C.

THE FILE FORMAT
    A face file is UTF-8, and every line that is not a command is a '#'
    comment - which the DSL already ignores and compile() already strips, so
    the prose costs the tag nothing.

        # name: Calendar 中文        <- required
        # category: Localised       <- required, groups the gallery
        # order: 80                 <- optional, sorts within a category
        #
        # Any other '#' line is prose and is kept in the file, not shipped.

        # --- panel: high
        ROTATE(270)
        ...

        # --- panel: low
        ROTATE(270)
        ...

    Both panels are required. They are written separately rather than scaled
    because the small panel is not a smaller version of the same layout - see
    the note at the top of any of the two-panel faces.
"""
import argparse
import json
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FACES = ROOT / 'webui' / 'faces'
OUT = ROOT / 'webui' / 'faces_data.js'

PANELS = ('high', 'low')

# Gallery order. A category not listed here still works and sorts last, so
# adding one is a one-line change here rather than a schema migration.
CATEGORY_ORDER = ['Basics', 'Clocks', 'Calendars', 'Localised', 'Sensors',
                  'Utility']

META_KEYS = {'name', 'category', 'order'}
PANEL_RE = re.compile(r'^#\s*-*\s*panel:\s*(\w+)', re.I)
META_RE = re.compile(r'^#\s*(\w+):\s*(.*)$')


def parse(path):
    meta, panels, panel = {}, {}, None

    for lineno, raw in enumerate(path.read_text(encoding='utf-8').splitlines(), 1):
        line = raw.rstrip()

        m = PANEL_RE.match(line)
        if m:
            panel = m.group(1).lower()
            if panel not in PANELS:
                sys.exit(f'{path.name}:{lineno}: unknown panel {panel!r} - '
                         f'expected one of {", ".join(PANELS)}')
            if panel in panels:
                sys.exit(f'{path.name}:{lineno}: panel {panel} appears twice')
            panels[panel] = []
            continue

        if not line.strip():
            continue

        if line.lstrip().startswith('#'):
            # Metadata only before the first panel section; after that a '#'
            # line is prose about the layout it sits in.
            if panel is None:
                km = META_RE.match(line)
                if km and km.group(1).lower() in META_KEYS:
                    meta[km.group(1).lower()] = km.group(2).strip()
            continue

        if panel is None:
            sys.exit(f'{path.name}:{lineno}: a command before any '
                     f'"# --- panel:" section - it would belong to no panel')
        panels[panel].append(line)

    for key in ('name', 'category'):
        if not meta.get(key):
            sys.exit(f'{path.name}: missing "# {key}:"')
    missing = [p for p in PANELS if p not in panels]
    if missing:
        sys.exit(f'{path.name}: no section for panel {", ".join(missing)} - '
                 f'a face has to say what it looks like on both')
    for p in PANELS:
        if not panels[p]:
            sys.exit(f'{path.name}: the {p} section draws nothing')

    try:
        order = int(meta.get('order', 1000))
    except ValueError:
        sys.exit(f'{path.name}: "# order:" must be a whole number, '
                 f'got {meta["order"]!r}')

    return {
        'slug': path.stem,
        'name': meta['name'],
        'category': meta['category'],
        'order': order,
        # The trailing newline matters: the firmware's own DEFAULT_FACE ends
        # with one, and a test diffs the two byte for byte.
        'scripts': {p: '\n'.join(panels[p]) + '\n' for p in PANELS},
    }


def load():
    files = sorted(FACES.glob('*.face'))
    if not files:
        sys.exit(f'no .face files in {FACES}')

    faces = [parse(f) for f in files]

    seen = {}
    for f in faces:
        if f['name'] in seen:
            sys.exit(f'two faces are both called {f["name"]!r}: '
                     f'{seen[f["name"]]}.face and {f["slug"]}.face')
        seen[f['name']] = f['slug']

    def key(f):
        cat = f['category']
        rank = CATEGORY_ORDER.index(cat) if cat in CATEGORY_ORDER else len(CATEGORY_ORDER)
        return (rank, f['order'], f['name'])

    return sorted(faces, key=key)


def emit(faces):
    cats = []
    for f in faces:
        if f['category'] not in cats:
            cats.append(f['category'])

    rows = []
    for f in faces:
        rows.append(
            f'  {{\n'
            f'    slug: {json.dumps(f["slug"])},\n'
            f'    name: {json.dumps(f["name"], ensure_ascii=False)},\n'
            f'    category: {json.dumps(f["category"], ensure_ascii=False)},\n'
            f'    scripts: {{\n'
            + ''.join(f'      {p}: {json.dumps(f["scripts"][p], ensure_ascii=False)},\n'
                      for p in PANELS)
            + f'    }},\n'
            f'  }},')

    return f'''// Generated by tools/genfaces.py from webui/faces/ - do not edit.
// Add or change a face there and re-run `python3 tools/genfaces.py --emit`.
//
// webui/serve.py regenerates this on startup, so editing a face during
// development is edit-and-refresh; the committed copy is what makes the
// editor work for anyone who has not run the generator.

/** Every built-in face, in gallery order. */
export const FACES = [
{chr(10).join(rows)}
];

/** Category names in gallery order, derived from the faces themselves. */
export const CATEGORIES = {json.dumps(cats, ensure_ascii=False)};

/** The shape the app and the tests have always consumed: panel -> name ->
 *  script. Built from FACES so the two cannot disagree. */
export const PRESETS = {{
{chr(10).join(f'  {p}: Object.fromEntries(FACES.map((f) => [f.name, f.scripts.{p}])),' for p in PANELS)}
}};
'''


def main():
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument('--emit', action='store_true', help='write the bundle')
    ap.add_argument('--check', action='store_true',
                    help='exit non-zero if the bundle on disk is stale')
    args = ap.parse_args()

    faces = load()
    text = emit(faces)

    if args.check:
        if not OUT.exists() or OUT.read_text(encoding='utf-8') != text:
            sys.exit(f'{OUT.relative_to(ROOT)} does not match webui/faces/ - '
                     f'run `python3 tools/genfaces.py --emit`')
        print('faces_data.js is up to date')
        return

    by_cat = {}
    for f in faces:
        by_cat.setdefault(f['category'], []).append(f)
    for cat, group in by_cat.items():
        print(f'  {cat:<10} {len(group):2d}  '
              f'{", ".join(f["name"] for f in group)}')
    total = sum(len(f['scripts'][p]) for f in faces for p in PANELS)
    print(f'\n  {len(faces)} faces, {total} B of script across both panels')

    if args.emit:
        OUT.write_text(text, encoding='utf-8')
        print(f'wrote {OUT.relative_to(ROOT)}')
    else:
        print('\n(dry run - pass --emit to write the bundle)')


if __name__ == '__main__':
    main()
