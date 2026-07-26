/*
 * app.js - wiring: editor -> preview -> tag.
 */
import { Panel, runScript, paint, tagSecondsNow, tagTime } from './epd.js';
import { PRESETS } from './presets.js';
import { Tag, bluetoothProblem } from './ble.js';
import * as Img from './image.js';

const $ = (id) => document.getElementById(id);

const SCRIPT_MAX = 1024;   /* CMD_SCRIPT_MAX in epd_cmdparser.c */
const LINE_MAX   = 128;    /* CMD_LINE_MAX   */

const tag = new Tag();
const panel = new Panel();

/* Which pane owns the preview and the Push button. The tag can show a
 * rendered template or an uploaded image but not both - they share one
 * framebuffer - so the tabs are a real mode switch, not just a view. */
let mode = 'template';

/* Whether we have set the tag's clock this session. The preview always runs at
 * browser time - an unsynced tag sits at 00:00, and previewing a frozen 00:00
 * looks like a broken page rather than an unsynced clock - so the sync state is
 * surfaced in the readout instead. */
let synced = false;

/* Decoded source image and the panel it currently processes down to. The
 * source is kept so the sliders can re-run the pipeline without re-decoding. */
let image = null;
let imagePanel = null;

/* Strip authoring-only lines before sending.
 *
 * The firmware stores every line it doesn't recognise as a control command,
 * comments included, so an un-stripped comment would eat into the tag's 1024
 * byte script buffer and be re-dispatched on every minute tick. Stripping here
 * keeps comments free. */
function compile(script) {
  const body = script
    .split('\n')
    .map((l) => l.trim())
    .filter((l) => l && !l.startsWith('#'))
    .join('\n');
  return body ? body + '\n' : '';
}

/* ------------------------------------------------------------------ */
/* Log                                                                 */
/* ------------------------------------------------------------------ */

function log(msg, kind = 'dim') {
  const el = $('log');
  const line = document.createElement('div');
  const t = new Date().toLocaleTimeString();
  line.className = kind;
  line.textContent = `${t}  ${msg}`;
  el.append(line);
  el.scrollTop = el.scrollHeight;
}

tag.addEventListener('log', (e) => log(e.detail.msg, e.detail.kind));
tag.addEventListener('state', () => refreshConnState());

/* ------------------------------------------------------------------ */
/* Preview                                                             */
/* ------------------------------------------------------------------ */

/* Fit the panel to the column without going below 1:1 or above 3x - past that
 * the pixel grid stops reading as a screen and starts reading as art. */
function show(p) {
  const avail = $('canvas').parentElement.clientWidth - 28;
  paint(p, $('canvas'), Math.max(1, Math.min(3, Math.floor(avail / p.width))));
  $('rotOut').textContent = String(p.rot);
  $('dims').textContent = `${p.width}×${p.height}`;
}

function showClock() {
  const tm = tagTime(tagSecondsNow());
  const hhmmss = [tm.hour, tm.min, tm.sec]
    .map((v) => String(v).padStart(2, '0')).join(':');
  $('tagClock').textContent = synced ? hhmmss : `${hhmmss} (tag not synced)`;
}

function render() {
  showClock();
  if (mode === 'image') { renderImage(); return; }

  const script = $('editor').value;
  panel.setRotation(0);
  panel.clear(1);
  const { warnings } = runScript(panel, script, tagSecondsNow());
  show(panel);
  showNotes(script, warnings);
}

function showNotes(script, warnings) {
  const notes = $('notes');
  notes.replaceChildren();

  /* Count what actually goes over the air, not what is in the box: blank lines
   * and comments are stripped before sending. */
  const bytes = new TextEncoder().encode(compile(script)).length;
  const counter = $('counter');
  counter.textContent = `${bytes} / ${SCRIPT_MAX} B`;
  counter.classList.toggle('over', bytes > SCRIPT_MAX);

  const problems = [];
  if (bytes > SCRIPT_MAX) {
    problems.push({
      err: true,
      msg: `Template is ${bytes} bytes; the tag stores ${SCRIPT_MAX} and drops `
         + 'the tail. Shorten it or the face will be cut off.',
    });
  }
  script.split('\n').forEach((l, i) => {
    if (l.length >= LINE_MAX) {
      problems.push({
        err: true,
        msg: `Line ${i + 1} is ${l.length} chars; the tag splits at ${LINE_MAX} `
           + 'and the remainder becomes a broken command.',
      });
    }
  });
  for (const w of warnings) {
    problems.push({ err: false, msg: `Line ${w.line}: ${w.msg}` });
  }

  for (const p of problems) {
    const div = document.createElement('div');
    div.className = p.err ? 'note err' : 'note';
    div.textContent = p.msg;
    notes.append(div);
  }
}

/* ------------------------------------------------------------------ */
/* Image                                                               */
/* ------------------------------------------------------------------ */

/** Current settings, read straight from the controls. */
function imageOpts() {
  return {
    fit: $('fit').value,
    dither: $('ditherSel').value,
    landscape: $('landscape').checked,
    invert: $('invert').checked,
    brightness: +$('brightness').value,
    contrast: +$('contrast').value,
    threshold: +$('threshold').value,
  };
}

function renderImage() {
  for (const id of ['brightness', 'contrast', 'threshold']) {
    $(`${id}Val`).textContent = $(id).value;
  }
  if (!image) {
    /* Show the frame the image will land in rather than a stale template, so
     * switching tabs makes it obvious the preview now belongs to this pane. */
    const { w, h, rot } = Img.surface($('landscape').checked);
    const blank = new Panel();
    blank.setRotation(rot);
    blank.clear(1);
    blank.rect(0, 0, w - 1, h - 1, 0, 1, 0);
    imagePanel = null;
    show(blank);
    return;
  }
  imagePanel = Img.process(image, imageOpts());
  show(imagePanel);
}

async function setImage(blob, name) {
  try {
    image = await Img.loadImage(blob);
    $('imgCtl').removeAttribute('disabled');
    $('drop').classList.add('loaded');
    $('drop').querySelector('b').textContent = name || 'Image loaded';
    $('drop').querySelector('span').textContent =
      `${image.width}×${image.height} — click to replace`;
    log(`Loaded ${name || 'image'} (${image.width}×${image.height}).`, 'ok');
    renderImage();
    refreshConnState();
  } catch (err) {
    log(err.message, 'err');
  }
}

/* ------------------------------------------------------------------ */
/* Actions                                                             */
/* ------------------------------------------------------------------ */

function refreshConnState() {
  const on = tag.connected;
  const paired = !!tag.device;

  $('status').classList.toggle('on', on);
  $('statusText').textContent = on ? (tag.device?.name ?? 'Connected')
                              : paired ? 'Idle — will reconnect'
                              : 'Disconnected';
  $('connect').textContent = paired ? 'Forget tag' : 'Connect';

  /* Enabled once a tag has been chosen, not only while the link is up: the
   * actions reconnect for themselves, so greying them out during a transient
   * drop would make the UI feel broken when nothing is wrong. */
  $('push').disabled = !paired || (mode === 'image' && !image);
  $('sync').disabled = !paired;
}

async function syncTime() {
  await tag.write(`TIME(${tagSecondsNow()})\n`);
  synced = true;
  log(`Clock set to ${new Date().toLocaleTimeString()}.`, 'ok');
  render();
}

async function pushTemplate() {
  const body = compile($('editor').value);
  const bytes = new TextEncoder().encode(body).length;

  if (!bytes) { log('Nothing to push - the template is empty.', 'warn'); return; }
  if (bytes > SCRIPT_MAX) {
    log(`Refusing to push ${bytes} bytes - over the tag's ${SCRIPT_MAX} byte `
      + 'buffer. It would be stored truncated.', 'err');
    return;
  }

  /* RESET() first, so a second push in the same connection replaces the
   * template instead of drawing on top of it. TIME() rides along in the same
   * batch: both are control commands the firmware applies and discards, so
   * neither ends up in the stored face. */
  const wantSync = $('autoSync').checked;
  const batch = 'RESET()\n'
              + (wantSync ? `TIME(${tagSecondsNow()})\n` : '')
              + body;

  await tag.write(batch);
  if (wantSync) synced = true;

  log(`Pushed ${bytes} bytes. The panel refreshes ~0.4 s after the last `
    + 'byte, then takes about 2 s.', 'ok');
  render();
}

async function pushImage() {
  if (!imagePanel) { log('No image loaded.', 'warn'); return; }

  /* Deliberately no TIME() here, whatever the auto-sync box says. A command
   * write is what tells the firmware the template owns the panel again, so
   * syncing the clock alongside an image would paint the image straight back
   * off the screen. Sync before switching to this tab if the clock needs it. */
  const btn = $('push');
  const label = btn.textContent;
  btn.disabled = true;

  try {
    await tag.pushImage(imagePanel.fb, {
      onProgress: (done, total) => {
        btn.textContent = `${Math.round((done / total) * 100)}%`;
      },
    });
    log('Image sent. The panel refreshes once the last byte lands, ~2 s.', 'ok');
  } finally {
    btn.textContent = label;
    refreshConnState();
  }
}

/* ------------------------------------------------------------------ */
/* Tabs                                                                */
/* ------------------------------------------------------------------ */

function setMode(next) {
  mode = next;
  const isImg = next === 'image';

  $('tabTemplate').classList.toggle('on', !isImg);
  $('tabImage').classList.toggle('on', isImg);
  $('paneTemplate').hidden = isImg;
  $('paneImage').hidden = !isImg;
  document.querySelector('details.ref').hidden = isImg;

  $('push').textContent = isImg ? 'Push image' : 'Push to tag';
  /* The auto-sync box belongs to the template push; pushImage() ignores it,
   * and leaving it visible would suggest otherwise. */
  $('autoSync').closest('label').hidden = isImg;

  refreshConnState();
  render();
}

/* ------------------------------------------------------------------ */
/* Init                                                                */
/* ------------------------------------------------------------------ */

const presetSelect = $('preset');
for (const name of Object.keys(PRESETS)) {
  presetSelect.append(new Option(name, name));
}

let lastLoaded = PRESETS['Built-in default'];

presetSelect.addEventListener('change', () => {
  const name = presetSelect.value;
  if (!name) return;
  lastLoaded = PRESETS[name];
  $('editor').value = lastLoaded;
  presetSelect.value = '';
  render();
});

$('revert').addEventListener('click', () => {
  $('editor').value = lastLoaded;
  render();
});

$('editor').addEventListener('input', render);
window.addEventListener('resize', render);

$('tabTemplate').addEventListener('click', () => setMode('template'));
$('tabImage').addEventListener('click', () => setMode('image'));

/* --- image controls ------------------------------------------------- */

for (const [value, text] of Object.entries(Img.FITS)) {
  $('fit').append(new Option(text, value));
}
for (const [value, text] of Object.entries(Img.DITHERS)) {
  $('ditherSel').append(new Option(text, value));
}

function applyImageDefaults() {
  const d = Img.DEFAULTS;
  $('fit').value = d.fit;
  $('ditherSel').value = d.dither;
  $('landscape').checked = d.landscape;
  $('invert').checked = d.invert;
  $('brightness').value = d.brightness;
  $('contrast').value = d.contrast;
  $('threshold').value = d.threshold;
}
applyImageDefaults();

for (const id of ['fit', 'ditherSel', 'landscape', 'invert',
                  'brightness', 'contrast', 'threshold']) {
  $(id).addEventListener('input', renderImage);
}
$('imgReset').addEventListener('click', () => {
  applyImageDefaults();
  renderImage();
});

$('file').addEventListener('change', (e) => {
  const f = e.target.files?.[0];
  if (f) setImage(f, f.name);
});

const drop = $('drop');
for (const type of ['dragenter', 'dragover']) {
  drop.addEventListener(type, (e) => {
    e.preventDefault();
    drop.classList.add('over');
  });
}
for (const type of ['dragleave', 'drop']) {
  drop.addEventListener(type, () => drop.classList.remove('over'));
}
drop.addEventListener('drop', (e) => {
  e.preventDefault();
  const f = e.dataTransfer?.files?.[0];
  if (f) setImage(f, f.name);
});

/* Paste anywhere on the page, not just over the drop zone - a screenshot on
 * the clipboard is the most common way an image gets here, and hunting for a
 * focus target first would be needless ceremony. */
window.addEventListener('paste', (e) => {
  if (mode !== 'image') return;
  for (const item of e.clipboardData?.items ?? []) {
    if (item.type.startsWith('image/')) {
      setImage(item.getAsFile(), 'pasted image');
      return;
    }
  }
});

/* --- connection ----------------------------------------------------- */

$('connect').addEventListener('click', async () => {
  if (tag.device) {
    await tag.disconnect();
    tag.device = null;          /* drop the pairing; Connect re-prompts */
    refreshConnState();
    log('Tag forgotten.', 'dim');
    return;
  }
  try {
    await tag.connect();
    /* Sync on connect: the tag is almost certainly sitting at its cold-boot
     * 00:00, and a clock showing the wrong time is worse than one that is
     * obviously blank. */
    await syncTime();
  } catch (err) {
    /* The user dismissing the chooser is a normal outcome, not a failure. */
    if (err.name === 'NotFoundError') log('Device chooser dismissed.', 'warn');
    else log(err.message, 'err');
  }
});

$('push').addEventListener('click', async () => {
  try {
    if (mode === 'image') await pushImage();
    else await pushTemplate();
  } catch (err) {
    log(`Push failed: ${err.message}`, 'err');
  }
});
$('sync').addEventListener('click', async () => {
  try { await syncTime(); }
  catch (err) { log(`Sync failed: ${err.message}`, 'err'); }
});

$('editor').value = lastLoaded;
refreshConnState();
render();

/* Say up front if pushing cannot work, rather than letting someone design a
 * face and only discover it when they press Connect. The editor and preview
 * are still perfectly useful without Bluetooth, so this disables the transport
 * and leaves everything else alone. */
const btProblem = bluetoothProblem();
if (btProblem) {
  log(btProblem, 'err');
  $('connect').disabled = true;
  $('statusText').textContent = 'Bluetooth unavailable';
  const banner = document.createElement('div');
  banner.className = 'note err';
  banner.textContent = btProblem;
  $('notes').before(banner);
} else {
  log('Ready. Connect a tag to push a face to it.', 'dim');
}

/* Tick the preview so {S} and the minute rollover animate. Cheap: the whole
 * render is a few thousand pixel writes. Image mode is left out - nothing in
 * it changes with the clock, and re-dithering every second would burn CPU to
 * produce identical bytes. */
setInterval(() => (mode === 'template' ? render() : showClock()), 1000);
