"""Add our source files to the e2 studio build tree's makefiles.

    python3 tools/register_sources.py <build-dir>

The build tree holds one `subdir.mk` per virtual folder, listing the files
that folder compiles. e2 studio wrote them once and nothing regenerates them,
so a new .c file is invisible to the build until they are edited - and the
failure is a link error naming a symbol, not a message about a missing file.
Adding epd_font_data.c cost exactly that detour, hence this.

The build tree is not tracked by git, so this runs from tools/build.sh on
every build rather than once by hand - the same reasoning as ensure_tag_defs()
there. It is idempotent and silent when there is nothing to do.

WHAT IT WILL AND WILL NOT TOUCH
    Only `user_*` folders. Each one's existing C_SRCS entries say which source
    directory it covers, so there is no list of files here to fall out of step
    with gen_e2studio_project.py - the build tree describes itself and we add
    whatever .c is in that directory and missing from the makefile.

    That "compile every .c in the directory" rule is true of our own folders
    and false of the `sdk_*` ones, which cherry-pick a few files out of a much
    larger SDK tree. Those are left alone.
"""
import os
import re
import sys


def rule_for(text, obj, src):
    """A build rule for `obj`, cloned from whichever one this file already
    has. The recipes are written in terms of $@ and $<, so a clone needs no
    edit beyond its target - and cloning rather than composing means the
    compile line keeps whatever tools/build.sh has patched into it, including
    $(TAG_DEFS)."""
    m = re.search(r'^(user_\S+\.o): (\S+)\n((?:\t.*\n|\n)*?)(?=^\S|\Z)',
                  text, re.M)
    if not m:
        sys.exit(f'{obj}: no existing rule to copy - the subdir.mk layout is '
                 f'not what this expects, so add the file by hand')
    return f'{obj}: {src}\n{m.group(3)}'


def add_to_list(text, var, entry):
    """Append `entry` to a `VAR += \\`-continued list.

    The last entry of such a list carries no trailing backslash, so the new
    one has to take that role over rather than simply being appended.

    Note the [^\\\\\\n] rather than [^\\\\]: a negated character class matches
    newlines, so the looser form swallows the line ending and the blank line
    after it, and the list ends up terminating one entry early - which make
    accepts silently and which costs a link error to notice."""
    m = re.search(rf'^{var} \+= \\\n((?:.*\\\n)*)(.*[^\\\n])\n', text, re.M)
    if not m:
        sys.exit(f'{var}: list not found - add {entry} by hand')
    return text[:m.start()] + f'{var} += \\\n{m.group(1)}{m.group(2)}\\\n' \
                              f'{entry}\n' + text[m.end():]


def register(mk):
    text = open(mk).read()

    srcs = re.findall(r'^(/\S+\.c) ?\\?$', text, re.M)
    if not srcs:
        return []
    srcdir = os.path.dirname(srcs[0])
    if any(os.path.dirname(s) != srcdir for s in srcs):
        return []          # spans directories: not one of ours, leave it be

    folder = os.path.basename(os.path.dirname(mk))
    listed = {os.path.basename(s) for s in srcs}
    missing = sorted(f for f in os.listdir(srcdir)
                     if f.endswith('.c') and f not in listed)
    if not missing:
        return []

    for f in missing:
        stem = f[:-2]
        src = f'{srcdir}/{f}'
        text = add_to_list(text, 'C_SRCS', f'{src} ')
        text = add_to_list(text, 'C_DEPS', f'./{folder}/{stem}.d ')
        text = add_to_list(text, 'OBJS', f'./{folder}/{stem}.o ')
        text = text.rstrip('\n') + '\n\n' + \
            rule_for(text, f'{folder}/{stem}.o', src)

    open(mk, 'w').write(text)
    return [f'{folder}/{f}' for f in missing]


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__.strip().split('\n')[2].strip())
    build = sys.argv[1]

    added = []
    for folder in sorted(os.listdir(build)):
        mk = os.path.join(build, folder, 'subdir.mk')
        if folder.startswith('user_') and os.path.isfile(mk):
            added += register(mk)

    if added:
        print(f'registered {len(added)} new source file(s): '
              f'{", ".join(added)}')


if __name__ == '__main__':
    main()
