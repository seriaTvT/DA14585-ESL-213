/*
 * gallery.js - pick a face by looking at it.
 *
 * The dropdown this replaces asked people to choose between seventeen names
 * for pictures. Names are a poor handle for a layout: 'Classic' and 'Classic
 * big clock' differ in a way you can see instantly and describe only slowly.
 *
 * THUMBNAILS ARE RENDERED, NOT STORED
 * Each card runs the face through the same renderer the live preview uses, at
 * the same instant the preview is showing. Storing images instead would put a
 * second copy of every face in the repo, free to drift from the script it
 * claims to depict - which is exactly the bug the two hand-pasted font tables
 * used to have, and the reason they are generated now. A face renders in well
 * under a millisecond, so there is nothing to buy by caching it.
 */
import { Panel, runScript, paint, PANELS } from './epd.js';

/** Rendered at 1:1 and scaled by CSS, so a card stays crisp at any size. */
function thumbnail(script, panelKey, env) {
  const p = new Panel(PANELS[panelKey]);
  p.setRotation(0);
  p.clear(1);
  const canvas = document.createElement('canvas');
  try {
    runScript(p, script, env.secs, env);
  } catch {
    /* A face that throws still gets a card - blank, and still loadable, since
     * the editor is where you would go to fix it. */
  }
  paint(p, canvas, 1);
  canvas.className = 'thumb';
  return canvas;
}

function card(face, panelKey, env, { onPick, onDelete }) {
  const el = document.createElement('button');
  el.className = 'face';
  el.type = 'button';
  el.append(thumbnail(face.script, panelKey, env));

  const label = document.createElement('span');
  label.className = 'face-name';
  label.textContent = face.name;
  el.append(label);

  el.addEventListener('click', () => onPick(face));

  if (onDelete) {
    /* Nested inside the card for layout, but a button inside a button is
     * invalid HTML and does not receive clicks in every browser - so this is a
     * span with a role, and it stops the click reaching the card. */
    const del = document.createElement('span');
    del.className = 'face-del';
    del.setAttribute('role', 'button');
    del.setAttribute('tabindex', '0');
    del.title = `Delete “${face.name}”`;
    del.textContent = '×';
    const go = (e) => { e.stopPropagation(); e.preventDefault(); onDelete(face); };
    del.addEventListener('click', go);
    del.addEventListener('keydown', (e) => {
      if (e.key === 'Enter' || e.key === ' ') go(e);
    });
    el.append(del);
  }
  return el;
}

/**
 * Fill `root` with grouped, thumbnailed cards.
 *
 * `groups` is [{ title, faces: [{name, script}], deletable }] so the caller
 * decides what a section means - built-in categories and saved faces are the
 * same shape and differ only in whether a card can be removed.
 */
export function renderGallery(root, groups, panelKey, env, handlers) {
  root.replaceChildren();

  let shown = 0;
  for (const group of groups) {
    if (!group.faces.length) continue;
    shown += group.faces.length;

    const head = document.createElement('h4');
    head.className = 'face-group';
    head.textContent = group.title;
    const count = document.createElement('span');
    count.textContent = String(group.faces.length);
    head.append(count);
    root.append(head);

    const grid = document.createElement('div');
    grid.className = 'face-grid';
    for (const face of group.faces) {
      grid.append(card(face, panelKey, env, {
        onPick: handlers.onPick,
        onDelete: group.deletable ? handlers.onDelete : null,
      }));
    }
    root.append(grid);
  }

  if (!shown) {
    const empty = document.createElement('p');
    empty.className = 'face-empty';
    empty.textContent = 'Nothing matches that.';
    root.append(empty);
  }
  return shown;
}

/** Case-insensitive substring match on the name, for the filter box. */
export function filterFaces(faces, query) {
  const q = query.trim().toLowerCase();
  if (!q) return faces;
  return faces.filter((f) => f.name.toLowerCase().includes(q));
}
