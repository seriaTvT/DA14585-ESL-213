#!/usr/bin/env python3
"""Generate an e2 studio project for hema_epd_clock from the validated
prox_reporter e2studio project.

Point DA1458X_SDK at your own unpacked SDK6 tree (the directory that contains
`projects/` and `sdk/`), e.g.

    DA1458X_SDK=~/SDK_6.0.22.1401/DA145xx_SDK/6.0.22.1401 \\
        python3 tools/gen_e2studio_project.py

The SDK is not redistributable, so it is not vendored in this repo - download
it from Renesas and accept their licence yourself. See BUILD_AND_FLASH.md.
"""
import os, re, shutil

# Repo-relative: this script lives in <project>/tools/.
PROJ = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

SDK = os.environ.get('DA1458X_SDK')
if not SDK:
    raise SystemExit('set DA1458X_SDK to your unpacked SDK6 tree '
                     '(the dir containing projects/ and sdk/)')
SDK = os.path.expanduser(SDK)

SRC = f'{SDK}/projects/target_apps/ble_examples/prox_reporter/e2studio'
DST = f'{PROJ}/e2studio'

if not os.path.isdir(SRC):
    raise SystemExit(f'not found: {SRC}\n'
                     'DA1458X_SDK should point at .../DA145xx_SDK/<version>')

os.makedirs(DST, exist_ok=True)
for f in ('.project', '.cproject', 'makefile.targets'):
    shutil.copy(f'{SRC}/{f}', f'{DST}/{f}')

U1 = '$%7BPARENT-1-PROJECT_LOC%7D'   # -> hema_epd_clock/
U5 = '$%7BPARENT-5-PROJECT_LOC%7D'   # -> SDK root

# ---------------------------------------------------------------- .project
proj = open(f'{DST}/.project').read()
proj = proj.replace('<name>prox_reporter</name>', '<name>hema_epd_clock</name>', 1)

# Drop prox_reporter's own user_* file links (keep the virtual folders).
# Targeted removal - do NOT split on the link blocks, or the trailing
# </linkedResources></projectDescription> rides along with the last one removed.
before = proj.count('<link>')
proj = re.sub(
    r'\t\t<link>\s*<name>(?:user_app|user_config|user_platform)/[^<]*</name>'
    r'.*?</link>\n', '', proj, flags=re.S)
print(f'  dropped {before - proj.count("<link>")} prox_reporter user links')
assert '</linkedResources>' in proj, 'lost closing tag'

def link(name, uri):
    return (f'\t\t<link>\n\t\t\t<name>{name}</name>\n\t\t\t<type>1</type>\n'
            f'\t\t\t<locationURI>{uri}</locationURI>\n\t\t</link>\n')

def vfolder(name):
    return (f'\t\t<link>\n\t\t\t<name>{name}</name>\n\t\t\t<type>2</type>\n'
            f'\t\t\t<locationURI>virtual:/virtual</locationURI>\n\t\t</link>\n')

new = ''
# two new virtual folders
new += vfolder('user_profile') + vfolder('user_epd')

# our application sources
for f in ('user_empty_peripheral_template.c', 'user_empty_peripheral_template.h'):
    new += link(f'user_app/{f}', f'{U1}/src/{f}')

cfg_dir = f'{PROJ}/src/config'
for f in sorted(os.listdir(cfg_dir)):
    new += link(f'user_config/{f}', f'{U1}/src/config/{f}')

new += link('user_platform/user_periph_setup.c', f'{U1}/src/platform/user_periph_setup.c')

for f in ('user_custs1_def.c', 'user_custs1_def.h',
          'user_custs_config.c', 'user_custs_config.h'):
    new += link(f'user_profile/{f}', f'{U1}/src/custom_profile/{f}')

for f in ('epd_cmdparser.c', 'epd_cmdparser.h', 'epd_gfx.c', 'epd_gfx.h',
          'epd_ssd1680.c', 'epd_ssd1680.h'):
    new += link(f'user_epd/{f}', f'{U1}/src/epd/{f}')

# SDK sources the custom (custs1) profile needs and prox_reporter lacks
for f in ('app_customs.c', 'app_customs_common.c', 'app_customs_task.c'):
    new += link(f'sdk_app/{f}', f'{U5}/sdk/app_modules/src/app_custs/{f}')
new += link('sdk_profiles/custom_common.c',
            f'{U5}/sdk/ble_stack/profiles/custom/custom_common.c')
for f in ('custs1.c', 'custs1_task.c'):
    new += link(f'sdk_profiles/{f}',
                f'{U5}/sdk/ble_stack/profiles/custom/custs/src/{f}')

# custs1.c calls attm_svc_create_db_128() to build a 128-bit-UUID service DB.
# prox_reporter only uses 16-bit SIG profiles, so it never links this.
new += link('sdk_ble/attm_db_128.c',
            f'{U5}/sdk/ble_stack/host/att/attm/attm_db_128.c')

# systick_wait() - our EPD driver's millisecond delays.
new += link('sdk_driver/systick.c', f'{U5}/sdk/platform/driver/systick/systick.c')

proj = proj.replace('\t</linkedResources>', new + '\t</linkedResources>', 1)
open(f'{DST}/.project', 'w').write(proj)

# --------------------------------------------------------------- .cproject
cp = open(f'{DST}/.cproject').read()
cp = cp.replace('prox_reporter', 'hema_epd_clock')

# include paths: add src/epd + src/custom_profile alongside src/config
inc_anchor = ('<listOptionValue builtIn="false" '
              'value="&quot;${ProjDirPath}/../src/config&quot;"/>')
assert inc_anchor in cp, 'include anchor not found'
inc_add = inc_anchor
for d in ('src/epd', 'src/custom_profile'):
    inc_add += ('\n\t\t\t\t\t\t\t\t\t<listOptionValue builtIn="false" '
                f'value="&quot;${{ProjDirPath}}/../{d}&quot;"/>')
cp = cp.replace(inc_anchor, inc_add)

# link order list: add objects for every source we introduced
obj_anchor = ('<listOptionValue builtIn="false" '
              'value="&quot;.\\user_app\\user_proxr.o&quot;"/>')
assert obj_anchor in cp, 'object anchor not found'
objs = [r'.\user_app\user_empty_peripheral_template.o',
        r'.\user_profile\user_custs1_def.o',
        r'.\user_profile\user_custs_config.o',
        r'.\user_epd\epd_ssd1680.o',
        r'.\user_epd\epd_gfx.o',
        r'.\user_epd\epd_cmdparser.o',
        r'.\sdk_app\app_customs.o',
        r'.\sdk_app\app_customs_common.o',
        r'.\sdk_app\app_customs_task.o',
        r'.\sdk_profiles\custom_common.o',
        r'.\sdk_profiles\custs1.o',
        r'.\sdk_profiles\custs1_task.o',
        r'.\sdk_ble\attm_db_128.o',
        r'.\sdk_driver\systick.o']
obj_add = '\n'.join('\t' * 9 + '<listOptionValue builtIn="false" '
                    f'value="&quot;{o}&quot;"/>' for o in objs)
cp = cp.replace(obj_anchor, obj_add.lstrip('\t'))   # replaces user_proxr.o entry

# source folders
se_anchor = ('<entry flags="VALUE_WORKSPACE_PATH|RESOLVED" '
             'kind="sourcePath" name="user_platform"/>')
assert se_anchor in cp, 'sourceEntries anchor not found'
se_add = se_anchor
for n in ('user_profile', 'user_epd'):
    se_add += ('\n\t\t\t\t\t\t<entry flags="VALUE_WORKSPACE_PATH|RESOLVED" '
               f'kind="sourcePath" name="{n}"/>')
cp = cp.replace(se_anchor, se_add)

open(f'{DST}/.cproject', 'w').write(cp)
print('generated ->', DST)
print('  .project  links:', proj.count('<link>'))
print('  .cproject size :', len(cp))
