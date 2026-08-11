/*
 * store.js - what survives a reload: the draft in the editor, and saved faces.
 *
 * Kept out of app.js so it can be exercised from Node with no DOM, which is
 * also why the backing store is injectable rather than reached for directly.
 *
 * Everything here treats storage as allowed to fail. Private-browsing modes
 * throw from getItem/setItem rather than returning null, and a page that dies
 * because it could not save a draft would be worse than one that quietly does
 * not save it - so a failed write is reported to the caller and never thrown.
 */

const DRAFT_KEY = 'hema.draft';
const FACES_KEY = 'hema.faces';

/** Storage that keeps nothing, for Node and for private mode. */
function memoryStore() {
  const m = new Map();
  return {
    getItem: (k) => (m.has(k) ? m.get(k) : null),
    setItem: (k, v) => m.set(k, String(v)),
    removeItem: (k) => m.delete(k),
  };
}

let backing = null;

/** Point the module at a different store. Returns the previous one. */
export function useStore(next) {
  const prev = backing;
  backing = next;
  return prev;
}

function store() {
  if (backing) return backing;
  try {
    /* Touch it rather than trust it: Safari's private mode exposes
     * localStorage and throws on write, so presence proves nothing. */
    const probe = '__hema_probe__';
    globalThis.localStorage.setItem(probe, '1');
    globalThis.localStorage.removeItem(probe);
    backing = globalThis.localStorage;
  } catch {
    backing = memoryStore();
  }
  return backing;
}

function read(key, fallback) {
  try {
    const raw = store().getItem(key);
    return raw === null ? fallback : JSON.parse(raw);
  } catch {
    /* Unparseable means a half-written or hand-edited value. Returning the
     * fallback loses it, which is better than throwing on every page load
     * with no way for the user to get back to a working editor. */
    return fallback;
  }
}

function write(key, value) {
  try {
    store().setItem(key, JSON.stringify(value));
    return true;
  } catch {
    return false;               /* quota, or private mode */
  }
}

/* ------------------------------------------------------------------ */
/* The draft - whatever is in the editor right now                     */
/* ------------------------------------------------------------------ */

/**
 * Remember the editor's contents.
 *
 * Stored with the panel it was written against, because a face is written for
 * one geometry: restoring a 250 px layout into a 212 px session would put the
 * author in front of a face that does not fit and no note saying why.
 */
export function saveDraft(panel, script) {
  return write(DRAFT_KEY, { panel, script, at: Date.now() });
}

/** The stored draft for `panel`, or null. */
export function loadDraft(panel) {
  const d = read(DRAFT_KEY, null);
  if (!d || typeof d.script !== 'string') return null;
  if (panel && d.panel !== panel) return null;
  return d;
}

export function clearDraft() {
  try { store().removeItem(DRAFT_KEY); } catch { /* nothing to undo */ }
}

/* ------------------------------------------------------------------ */
/* Saved faces                                                         */
/* ------------------------------------------------------------------ */

/** Every saved face, newest first. */
export function listFaces() {
  const list = read(FACES_KEY, []);
  return Array.isArray(list) ? list.filter(ok) : [];
}

function ok(f) {
  return f && typeof f.name === 'string' && typeof f.script === 'string'
    && typeof f.panel === 'string';
}

/**
 * Save under `name`, replacing any face of that name on the same panel.
 *
 * Keyed by name *and* panel rather than by name alone: the two panels want
 * genuinely different layouts, so "Kitchen tag" written for one is not a draft
 * of the other and overwriting across them would lose work silently.
 */
export function saveFace(name, panel, script) {
  name = name.trim();
  if (!name) return { ok: false, reason: 'A face needs a name.' };

  const list = listFaces();
  const at = list.findIndex((f) => f.name === name && f.panel === panel);
  const entry = { name, panel, script, at: Date.now() };
  const replaced = at >= 0;
  if (replaced) list[at] = entry; else list.unshift(entry);

  if (!write(FACES_KEY, list)) {
    return { ok: false, reason: 'Storage is full or unavailable.' };
  }
  return { ok: true, replaced };
}

export function deleteFace(name, panel) {
  const list = listFaces().filter((f) => !(f.name === name && f.panel === panel));
  return write(FACES_KEY, list);
}

/** A name not yet used on this panel, e.g. "Calendar 2". */
export function freeName(base, panel) {
  const taken = new Set(listFaces().filter((f) => f.panel === panel)
                                   .map((f) => f.name));
  if (!taken.has(base)) return base;
  for (let n = 2; ; n++) {
    const candidate = `${base} ${n}`;
    if (!taken.has(candidate)) return candidate;
  }
}

/* ------------------------------------------------------------------ */
/* Moving faces between machines                                       */
/* ------------------------------------------------------------------ */

export const EXPORT_VERSION = 1;

export function exportFaces(faces = listFaces()) {
  return JSON.stringify(
    { format: 'hema-faces', version: EXPORT_VERSION, faces }, null, 2);
}

/**
 * Merge an exported bundle in. Existing faces of the same name and panel are
 * replaced; everything else is added.
 *
 * Rejects rather than guesses on anything it does not recognise: importing is
 * the one place a user hands this page a file from elsewhere, and silently
 * accepting half a bundle would leave them with a gallery that is wrong in a
 * way they did not ask about.
 */
export function importFaces(text) {
  let data;
  try {
    data = JSON.parse(text);
  } catch {
    return { ok: false, reason: 'That is not a JSON file.' };
  }
  if (!data || data.format !== 'hema-faces' || !Array.isArray(data.faces)) {
    return { ok: false, reason: 'That is not a face bundle from this page.' };
  }
  if (data.version > EXPORT_VERSION) {
    return { ok: false,
             reason: `That bundle is version ${data.version}; this page reads `
                   + `up to ${EXPORT_VERSION}.` };
  }
  const incoming = data.faces.filter(ok);
  if (!incoming.length) {
    return { ok: false, reason: 'That bundle contains no readable faces.' };
  }

  const list = listFaces();
  let added = 0, replaced = 0;
  for (const f of incoming) {
    const entry = { name: f.name, panel: f.panel, script: f.script,
                    at: Date.now() };
    const at = list.findIndex((x) => x.name === f.name && x.panel === f.panel);
    if (at >= 0) { list[at] = entry; replaced++; } else { list.unshift(entry); added++; }
  }
  if (!write(FACES_KEY, list)) {
    return { ok: false, reason: 'Storage is full or unavailable.' };
  }
  return { ok: true, added, replaced };
}
