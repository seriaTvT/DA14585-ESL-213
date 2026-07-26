/*
 * app.js - wiring: editor -> preview -> tag.
 */
import { Panel, runScript, paint, tagSecondsNow, tagTime } from './epd.js';
import { PRESETS } from './presets.js';
import { Tag, bluetoothProblem } from './ble.js';

const $ = (id) => document.getElementById(id);

const SCRIPT_MAX = 1024;   /* CMD_SCRIPT_MAX in epd_cmdparser.c */
const LINE_MAX   = 128;    /* CMD_LINE_MAX   */

const tag = new Tag();
const panel = new Panel();

/* Whether we have set the tag's clock this session. The preview always runs at
 * browser time - an unsynced tag sits at 00:00, and previewing a frozen 00:00
 * looks like a broken page rather than an unsynced clock - so the sync state is
 * surfaced in the readout instead. */
let synced = false;

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

function render() {
  const script = $('editor').value;
  const secs = tagSecondsNow();

  panel.setRotation(0);
  panel.clear(1);
  const { warnings } = runScript(panel, script, secs);

  /* Fit the panel to the column without going below 1:1 or above 3x - past
   * that the pixel grid stops reading as a screen and starts reading as art. */
  const avail = $('canvas').parentElement.clientWidth - 28;
  const zoom = Math.max(1, Math.min(3, Math.floor(avail / panel.width)));
  paint(panel, $('canvas'), zoom);

  const tm = tagTime(secs);
  const hhmmss = [tm.hour, tm.min, tm.sec]
    .map((v) => String(v).padStart(2, '0')).join(':');
  $('tagClock').textContent = synced ? hhmmss : `${hhmmss} (tag not synced)`;
  $('rotOut').textContent = String(panel.rot);
  $('dims').textContent = `${panel.width}×${panel.height}`;

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
   * tag drops us on every panel refresh, and the actions reconnect for
   * themselves. Greying them out for those seconds would make the UI feel
   * broken when nothing is wrong. */
  $('push').disabled = !paired;
  $('sync').disabled = !paired;
}

async function syncTime() {
  await tag.write(`TIME(${tagSecondsNow()})\n`);
  synced = true;
  log(`Clock set to ${new Date().toLocaleTimeString()}.`, 'ok');
  render();
}

async function push() {
  const body = compile($('editor').value);
  const bytes = new TextEncoder().encode(body).length;

  if (!bytes) { log('Nothing to push - the template is empty.', 'warn'); return; }
  if (bytes > SCRIPT_MAX) {
    log(`Refusing to push ${bytes} bytes - over the tag's ${SCRIPT_MAX} byte `
      + 'buffer. It would be stored truncated.', 'err');
    return;
  }

  try {
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
  } catch (err) {
    log(`Push failed: ${err.message}`, 'err');
  }
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

$('push').addEventListener('click', push);
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
 * render is a few thousand pixel writes. */
setInterval(render, 1000);
