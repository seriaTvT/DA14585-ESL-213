/*
 * app.js - wiring: editor -> preview -> tag.
 */
import { Panel, runScript, paint, tagSecondsNow, tagTime, EPOCH_2000,
         PANELS, activePanel, setActivePanel } from './epd.js';
import { FACES, CATEGORIES } from './faces_data.js';
import * as Store from './store.js';
import { renderGallery, filterFaces } from './gallery.js';
import { Tag, bluetoothProblem, FLUSH_DELAY_MS } from './ble.js';
import * as Img from './image.js';

const $ = (id) => document.getElementById(id);

const SCRIPT_MAX = 3072;   /* CMD_SCRIPT_MAX in epd_cmdparser.c */
const LINE_MAX   = 128;    /* CMD_LINE_MAX   */

const tag = new Tag();

/* Rebuilt when the panel changes - a Panel is fixed to the geometry it was
 * constructed with, so switching panels means a new one rather than a resize. */
let panel = new Panel();

/* Which pane owns the preview and the Push button. The tag can show a
 * rendered template or an uploaded image but not both - they share one
 * framebuffer - so the tabs are a real mode switch, not just a view. */
let mode = 'template';

/* The clock everything here runs on, held as an offset from the browser's own
 * rather than as an instant: the tag is handed one number and counts on from
 * it, so an offset ticks forward exactly the way its clock will. Zero is the
 * browser's time, which is the case until a custom one is entered.
 *
 * The preview never runs at the tag's *actual* time - an unsynced tag sits at
 * 00:00, and previewing a frozen 00:00 looks like a broken page rather than an
 * unsynced clock - so what the tag believes is surfaced in the readout instead.
 */
let clockSkew = 0;

/* The skew the tag was last given, or null while it has never been set. Not a
 * boolean any more: with a custom time in play, "synced" also has to mean
 * synced *to what*, or the readout would vouch for a clock the tag was never
 * sent. */
let sentSkew = null;

/* Decoded source image and the panel it currently processes down to. The
 * source is kept so the sliders can re-run the pipeline without re-decoding. */
let image = null;
let imagePanel = null;

/* Strip authoring-only lines before sending.
 *
 * The firmware stores every line it doesn't recognise as a control command,
 * comments included, so an un-stripped comment would eat into the tag's
 * script buffer and be re-dispatched on every minute tick. Stripping here
 * keeps comments free. */
function compile(script) {
  const kept = [];
  const from = [];              /* kept[i] came from editor line from[i] */

  script.split('\n').forEach((raw, i) => {
    const l = raw.trim();
    if (!l || l.startsWith('#')) return;
    kept.push(l);
    from.push(i + 1);
  });

  return { body: kept.length ? kept.join('\n') + '\n' : '', from };
}

/**
 * Editor line for a line number the tag reported, or null if it cannot be
 * placed.
 *
 * The tag counts lines of the script it stored, and that is a different
 * numbering from the one in the editor's margin: comments and blank lines are
 * stripped here before sending, and the firmware applies RESET()/TIME() on
 * arrival without storing them, so everything after them shifts up again.
 * Reporting the tag's number raw would point confidently at the wrong line,
 * which is worse than pointing at nothing.
 *
 * `dropped` is how many leading control lines this push prepended.
 */
function editorLine(tagLine, dropped) {
  if (!tagLine) return null;
  const { from } = compile($('editor').value);
  const idx = tagLine - 1 + dropped;
  return idx >= 0 && idx < from.length ? from[idx] : null;
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
 * the pixel grid stops reading as a screen and starts reading as art.
 *
 * The zoom comes from a fixed reference panel rather than from the one being
 * shown, so choosing a panel does not resize the preview. Sizing it from its
 * own width made the *smaller* panel render much the larger of the two: the
 * zoom is a clamped integer, and at a column width in the 640-750 px range
 * floor(avail/212) is 3 where floor(avail/250) is still 2, so the low-res
 * frame came out 636 px wide against the high-res one's 500.
 *
 * With a common reference the zoom is equal, one panel pixel is one preview
 * pixel of the same size on both, and the low-res frame is simply the 15%
 * narrower that it actually is. Rotation still sets the scale, since a
 * portrait frame has a genuinely different width to fill. */
const ZOOM_REF = PANELS.high;

function show(p) {
  const avail = $('canvas').parentElement.clientWidth - 28;
  const refW = (p.rot & 1) ? ZOOM_REF.h : ZOOM_REF.w;
  paint(p, $('canvas'), Math.max(1, Math.min(3, Math.floor(avail / refW))));
  $('rotOut').textContent = String(p.rot);
  $('dims').textContent = `${p.width}×${p.height}`;
}

/** Tag-seconds the preview is drawn at, and what Sync sends - see clockSkew. */
function previewSeconds() { return tagSecondsNow() + clockSkew; }

/* A tag instant as text. Built from tagTime() rather than from a Date because
 * the tag has no timezone: putting the same number through the browser's locale
 * would be a second clock, and the two disagree by hours. */
function stamp(secs, withDate = false) {
  const tm = tagTime(secs);
  const p = (v) => String(v).padStart(2, '0');
  const hhmmss = `${p(tm.hour)}:${p(tm.min)}:${p(tm.sec)}`;
  return withDate ? `${tm.year}-${p(tm.month)}-${p(tm.day)} ${hhmmss}` : hhmmss;
}

function showClock() {
  /* Three states, and the difference between the last two matters: a custom
   * time that has not been sent is the one case where this readout and the tag
   * disagree on purpose. */
  const note = sentSkew === null ? ' (tag not synced)'
             : sentSkew !== clockSkew ? ' (not sent to the tag)'
             : '';
  /* The date only when a custom time is in play. It is usually the whole point
   * of setting one, and hiding it behind HH:MM:SS shows the wrong half. */
  $('tagClock').textContent = stamp(previewSeconds(), clockSkew !== 0) + note;
}

/* A preview field as a number, or undefined when it is blank - which is how
 * you see what a build that takes no such reading shows. Shared with the
 * gallery, whose thumbnails have to render from the same values or a card
 * would disagree with the preview it came from. */
const num = (id) => {
  const raw = $(id).value;
  return raw === '' ? undefined : parseInt(raw, 10);
};

function render() {
  showClock();
  if (mode === 'image') { renderImage(); return; }

  const script = $('editor').value;
  panel.setRotation(0);
  panel.clear(1);
  /* The preview's own temperature, not the tag's: nothing here can read the
   * panel's sensor, and the tag substitutes its own {T} when it renders. This
   * exists so a face using {T} can be laid out and its width judged - a
   * two-digit reading and a negative one are different widths, which is
   * exactly the kind of thing a preview is for. Blank the field to see what a
   * build with no temperature reading shows, which is the literal "{T}". */
  /* Battery reads the same way and for the same reasons: the tag measures its
   * own cell, and a blank field is how you see what a face shows on a build
   * that takes no reading - the literal "{BAT}". Percent and millivolts are
   * separate fields because they are separate variables; a face is free to
   * show a bar from one and a diagnostic from the other. */
  const { warnings, every } = runScript(panel, script, previewSeconds(), {
    temp: num('previewTemp'),
    battPct: num('previewBatt'),
    battMv: num('previewVcc'),
  });
  show(panel);
  showNotes(script, warnings, every);
}

/* How often the tag will repaint, in words. The preview renders a single
 * instant, so EVERY() is the one command with no visible effect here at all -
 * without this the editor would give no sign it had been read. */
function everyNote(every) {
  if (every <= 1) return null;
  if (every === 60)   return 'Repaints hourly, on the hour.';
  if (every === 1440) return 'Repaints once a day, at midnight.';
  const unit = every % 60 === 0 ? `${every / 60} hours` : `${every} minutes`;
  return `Repaints every ${unit}, on the boundary - not from when it was sent.`;
}

function showNotes(script, warnings, every = 1) {
  const notes = $('notes');
  notes.replaceChildren();

  /* Count what actually goes over the air, not what is in the box: blank lines
   * and comments are stripped before sending. */
  const bytes = new TextEncoder().encode(compile(script).body).length;
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

  const note = everyNote(every);
  if (note) {
    problems.push({ err: false, msg: note });
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
  const secs = previewSeconds();
  await tag.write(`TIME(${secs})\n`);
  sentSkew = clockSkew;
  /* Report what was sent rather than what the wall clock says, since with a
   * custom time in play those are different and only one of them is the tag's. */
  log(`Clock set to ${stamp(secs, true)}.`, 'ok');
  render();
}

async function pushTemplate() {
  const { body } = compile($('editor').value);
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
              + (wantSync ? `TIME(${previewSeconds()})\n` : '')
              + body;

  await tag.write(batch);
  if (wantSync) sentSkew = clockSkew;

  log(`Pushed ${bytes} bytes. The panel refreshes ~0.4 s after the last `
    + 'byte, then takes about 2 s.', 'ok');
  render();
  /* RESET() is always prepended and TIME() sometimes is; neither is stored, so
   * both shift the tag's line numbering relative to the editor's. */
  reportTagStatus(0);
}

/**
 * Ask the tag what it made of the face, once it has had time to render it.
 *
 * The preview's own warnings come from a port of the parser; this comes from
 * the parser. When they disagree the tag is right by definition, and saying so
 * is more useful than either one alone - a face can look correct in the
 * preview and still have a line the tag skipped.
 *
 * Deliberately not awaited by pushScript(): a tag too old to have the status
 * characteristic, or a link that drops during the refresh, must not turn a
 * successful push into a failed one.
 */
async function reportTagStatus(dropped) {
  /* The flush timer is 400 ms and the refresh about 2 s; the report is written
   * at the start of the render, so waiting for the flush is enough. */
  await new Promise((r) => setTimeout(r, FLUSH_DELAY_MS + 400));
  let st;
  try {
    st = await tag.readStatus();
  } catch (err) {
    log(`Could not read the tag's render status: ${err.message}`, 'dim');
    return;
  }
  if (!st) return;                      /* firmware predates the status char */

  if (st.code === 0 && !st.count) {
    /* Report the interval the *tag* came back with, not the one the preview
     * worked out. They should agree, and saying so is how you find out they
     * do - a face whose EVERY() the tag never parsed looks identical here
     * otherwise, and would only give itself away an hour later. */
    const every = st.every > 1 ? `, repainting every ${st.every} min` : '';
    log(`The tag rendered all ${st.scriptLen} bytes with no complaints${every}.`,
        'ok');
    return;
  }
  const ln = editorLine(st.line, dropped);
  const where = ln ? ` at line ${ln}`
              : st.line ? ` at stored line ${st.line}` : '';
  const more = st.count > 1 ? ` (and ${st.count - 1} more)` : '';
  log(`The tag reports: ${st.message}${where}${more}.`, 'warn');
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

/* --- panel ---------------------------------------------------------- */

/* Which panel the tag has is a property of the hardware, not of the session, so
 * it is remembered: an operator with one kind of tag should not have to reset
 * it on every visit, and picking it wrong is the one mistake here that produces
 * garbage rather than a message (see PANELS in epd.js). */
const PANEL_KEY = 'hema.panel';

const panelSelect = $('panel');
for (const p of Object.values(PANELS)) {
  panelSelect.append(new Option(p.label, p.key));
}

let lastLoaded;

/** The built-in face of that name for the active panel. */
function builtin(name) {
  const f = FACES.find((x) => x.name === name);
  return f && f.scripts[activePanel().key];
}

/* The faces belong to the panel - they are written per panel rather than
 * scaled - so switching panels re-reads them rather than filtering. */
function loadPanelPresets({ replaceEditor }) {
  /* Only replace what the author is looking at when the *panel* changed and the
   * editor still holds the other panel's default. Anything they have edited or
   * deliberately loaded is theirs, and silently swapping it out on a panel
   * change would be the rudest thing this page could do. */
  const untouched = replaceEditor
    && (!lastLoaded || $('editor').value === lastLoaded);

  lastLoaded = builtin('Built-in default');
  if (untouched) $('editor').value = lastLoaded;
}

panelSelect.addEventListener('change', () => {
  setActivePanel(panelSelect.value);
  try { localStorage.setItem(PANEL_KEY, panelSelect.value); } catch { /* private mode */ }

  panel = new Panel();
  imagePanel = null;      /* stale geometry; renderImage() rebuilds it */
  loadPanelPresets({ replaceEditor: true });
  /* Re-stamp the draft with the panel now on screen. Without this the buffer
   * would keep the old panel's label and be refused on the next reload, which
   * looks exactly like the autosave having failed. */
  Store.saveDraft(activePanel().key, $('editor').value);
  log(`Panel set to ${activePanel().label}.`, 'ok');
  render();
});

let storedPanel = null;
try { storedPanel = localStorage.getItem(PANEL_KEY); } catch { /* private mode */ }
if (storedPanel && PANELS[storedPanel]) {
  setActivePanel(storedPanel);
  panel = new Panel();
}
panelSelect.value = activePanel().key;

loadPanelPresets({ replaceEditor: false });

/* Restore whatever was being written, but only for this panel: a draft is a
 * layout for one geometry, and dropping a 250 px face into a 212 px session
 * would show the author something that does not fit with nothing saying why.
 * The built-in default stays the starting point otherwise. */
const draft = Store.loadDraft(activePanel().key);
if (draft && draft.script.trim() && draft.script !== lastLoaded) {
  $('editor').value = draft.script;
}

/* ------------------------------------------------------------------ */
/* The face gallery                                                    */
/* ------------------------------------------------------------------ */

const gallery = $('gallery');

/** Saved faces for the active panel, newest first. */
function savedFaces() {
  return Store.listFaces().filter((f) => f.panel === activePanel().key);
}

function galleryGroups(query) {
  const key = activePanel().key;
  const groups = [{
    title: 'Saved',
    deletable: true,
    faces: filterFaces(savedFaces(), query),
  }];
  for (const cat of CATEGORIES) {
    groups.push({
      title: cat,
      deletable: false,
      faces: filterFaces(
        FACES.filter((f) => f.category === cat)
             .map((f) => ({ name: f.name, script: f.scripts[key] })),
        query),
    });
  }
  return groups;
}

function drawGallery() {
  const key = activePanel().key;
  /* Thumbnails render at the instant the preview is showing, and with the same
   * readings, so a card is what pushing that face right now would put on the
   * glass - not a picture of it at some other time. */
  const env = {
    secs: previewSeconds(),
    temp: num('previewTemp'),
    battPct: num('previewBatt'),
    battMv: num('previewVcc'),
  };
  const shown = renderGallery($('galBody'), galleryGroups($('galFilter').value),
                              key, env, {
    onPick: (face) => {
      lastLoaded = face.script;
      $('editor').value = face.script;
      gallery.close();
      render();
      log(`Loaded “${face.name}”.`, 'ok');
    },
    onDelete: (face) => {
      if (!confirm(`Delete the saved face “${face.name}”?`)) return;
      Store.deleteFace(face.name, key);
      drawGallery();
      log(`Deleted “${face.name}”.`);
    },
  });
  const saved = savedFaces().length;
  $('galFoot').textContent =
    `${shown} shown · ${saved} saved · thumbnails are rendered live at the `
    + `previewed time, on the ${activePanel().label} panel.`;
}

$('browse').addEventListener('click', () => {
  $('galFilter').value = '';
  drawGallery();
  gallery.showModal();
  $('galFilter').focus();
});
$('galClose').addEventListener('click', () => gallery.close());
$('galFilter').addEventListener('input', drawGallery);

$('saveFace').addEventListener('click', () => {
  const key = activePanel().key;
  const suggested = Store.freeName('My face', key);
  const name = prompt('Save this face as:', suggested);
  if (name === null) return;

  const res = Store.saveFace(name, key, $('editor').value);
  if (!res.ok) { log(res.reason, 'err'); return; }
  lastLoaded = $('editor').value;
  log(`${res.replaced ? 'Replaced' : 'Saved'} “${name.trim()}”.`, 'ok');
});

$('galExport').addEventListener('click', () => {
  const faces = Store.listFaces();
  if (!faces.length) { log('No saved faces to export.', 'warn'); return; }
  const blob = new Blob([Store.exportFaces(faces)], { type: 'application/json' });
  const a = document.createElement('a');
  a.href = URL.createObjectURL(blob);
  a.download = 'hema-faces.json';
  a.click();
  URL.revokeObjectURL(a.href);
  log(`Exported ${faces.length} saved face(s).`, 'ok');
});

$('galImport').addEventListener('click', () => $('galFile').click());
$('galFile').addEventListener('change', async (e) => {
  const file = e.target.files?.[0];
  e.target.value = '';                 /* so the same file can be re-chosen */
  if (!file) return;

  const res = Store.importFaces(await file.text());
  if (!res.ok) { log(res.reason, 'err'); return; }
  drawGallery();
  log(`Imported ${res.added} new and replaced ${res.replaced} face(s).`, 'ok');
});

$('revert').addEventListener('click', () => {
  $('editor').value = lastLoaded;
  render();
});

/* Autosave, so a reload does not cost the face you were writing.
 *
 * Debounced only lightly: a localStorage write of a few kilobytes is cheap,
 * and the failure this guards against is the tab going away without warning,
 * which no amount of batching gets to negotiate with. */
let draftTimer = null;
function saveDraftSoon() {
  clearTimeout(draftTimer);
  draftTimer = setTimeout(
    () => Store.saveDraft(activePanel().key, $('editor').value), 400);
}

$('editor').addEventListener('input', () => { saveDraftSoon(); render(); });
/* Belt and braces for the case the timer never fires. */
addEventListener('pagehide', () => {
  clearTimeout(draftTimer);
  Store.saveDraft(activePanel().key, $('editor').value);
});
/* Same re-render path as the editor: changing what {T} stands for changes the
 * drawing, and a face using it should reflow as you type a different reading. */
for (const id of ['previewTemp', 'previewBatt', 'previewVcc']) {
  $(id).addEventListener('input', render);
}
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

/* --- clock ---------------------------------------------------------- */

/* TIME() parses a signed 32-bit count of seconds from 2000-01-01 and ignores
 * anything that is not positive - see handle_line() in epd_cmdparser.c. The
 * field carries the same pair as min/max, so the browser flags an out-of-range
 * date too; this is what catches one typed past it anyway. */
const TIME_MIN = 1;
const TIME_MAX = 0x7fffffff;

const customTime = $('customTime');
const clockAt = $('clockAt');

/* datetime-local carries a wall-clock time with no zone attached, which is
 * exactly what the tag wants: it has no notion of a timezone, so local time is
 * sent as though it were UTC and read back the same way (see tagSecondsNow()).
 * Parsing this field as UTC is that same convention running the other way,
 * which is why neither direction applies an offset. */
function fieldSeconds() {
  const v = clockAt.value;
  if (!v) return null;              /* empty, or mid-edit and not yet a time */
  const ms = Date.parse(v.length === 16 ? `${v}:00Z` : `${v}Z`);
  return Number.isNaN(ms) ? null : Math.floor(ms / 1000) - EPOCH_2000;
}

/** The field's value for a tag instant - fieldSeconds() inverted. */
function secondsField(secs) {
  return new Date((secs + EPOCH_2000) * 1000).toISOString().slice(0, 19);
}

function readClockField() {
  const secs = fieldSeconds();
  if (secs === null) return;        /* half-typed: leave the clock as it was */
  if (secs < TIME_MIN || secs > TIME_MAX) {
    log(`${stamp(secs, true)} is outside the tag's clock, which counts seconds `
      + 'from 2000-01-01 in a signed 32-bit field. The tag would ignore it.',
        'err');
    return;
  }
  clockSkew = secs - tagSecondsNow();
}

/* Bring the clock into line with the controls. Renders nothing itself, so it
 * can run at init too: a reload restores the checkbox but not the state behind
 * it, and once the browser has had its say the markup is no longer the
 * authority on whether a custom time is in play. */
function applyCustomTime() {
  clockAt.disabled = !customTime.checked;
  if (!customTime.checked) { clockSkew = 0; return; }
  /* Seeded with the time already on show, so ticking the box changes nothing by
   * itself: the field is a starting point to edit, not a jump elsewhere. */
  if (!clockAt.value) clockAt.value = secondsField(previewSeconds());
  readClockField();
}

customTime.addEventListener('change', () => { applyCustomTime(); render(); });
clockAt.addEventListener('input', () => { readClockField(); render(); });
applyCustomTime();

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
    /* Not synced on connect unless asked. Connecting is how you inspect a tag,
     * and a connection that silently rewrote its clock made two ordinary things
     * impossible: reading what time a tag actually thinks it is, and leaving a
     * deliberately-set time - a custom one, or one from another host - alone
     * across a reconnect. The actions here reconnect on their own, so that
     * write was not even a once-per-session event. */
    if ($('syncOnConnect').checked) await syncTime();
    else if (sentSkew === null) {
      log('Connected without setting the clock - a tag that has been power '
        + 'cut reads 00:00 on 2000-01-01 until something does. Press Sync '
        + 'time, or push a face with "sync clock on push" ticked.', 'dim');
    }
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
