/*
 * crop.js - choosing which part of a picture reaches the panel.
 *
 * `cover` centre-crops, which on this hardware throws away most of the
 * picture and never asks: the frame is 250x122, close enough to 2:1 that
 * almost any photograph loses the majority of its height. Which band survives
 * is the whole decision, and it was the one thing the pipeline did not let
 * anyone make.
 *
 * The geometry lives here as pure functions, separate from the pointer
 * handling, so the part that can be wrong in a way you would not notice - a
 * rectangle that drifts off the image after a zoom, or an aspect that is a
 * pixel out - is tested rather than eyeballed.
 *
 * All rectangles are in SOURCE IMAGE pixels: {x, y, w, h}. The display is a
 * scaled view of the same thing, and keeping one coordinate system means the
 * scale factor appears exactly twice, at the edges of the DOM controller.
 */

/** The largest `aspect`-shaped rectangle that fits inside w x h. */
export function maxRect(imgW, imgH, aspect) {
  let w = imgW;
  let h = w / aspect;
  if (h > imgH) {
    h = imgH;
    w = h * aspect;
  }
  return { x: (imgW - w) / 2, y: (imgH - h) / 2, w, h };
}

/**
 * Slide `rect` back inside the image without changing its size.
 *
 * Size first, position second: a rectangle larger than the image cannot be
 * placed legally at all, so it is shrunk to fit before it is moved. Clamping
 * position alone would leave it hanging off two edges at once.
 */
export function clamp(rect, imgW, imgH) {
  let { x, y, w, h } = rect;
  if (w > imgW) { h *= imgW / w; w = imgW; }
  if (h > imgH) { w *= imgH / h; h = imgH; }
  x = Math.min(Math.max(x, 0), imgW - w);
  y = Math.min(Math.max(y, 0), imgH - h);
  return { x, y, w, h };
}

/**
 * A crop rectangle at `zoom`, centred on (cx, cy) in image pixels.
 *
 * zoom 1 is the largest rectangle that fits - the same framing `cover` would
 * choose - and larger numbers crop tighter. There is no zoom below 1 because
 * it would mean padding, which is what `contain` is for.
 */
export function rectFor(imgW, imgH, aspect, zoom, cx, cy) {
  const base = maxRect(imgW, imgH, aspect);
  const z = Math.max(1, zoom);
  const w = base.w / z;
  const h = base.h / z;
  return clamp({ x: cx - w / 2, y: cy - h / 2, w, h }, imgW, imgH);
}

/** Move by a delta in image pixels, staying inside the image. */
export function move(rect, dx, dy, imgW, imgH) {
  return clamp({ ...rect, x: rect.x + dx, y: rect.y + dy }, imgW, imgH);
}

/** Re-zoom about the rectangle's own centre, so the framing holds still. */
export function zoomTo(rect, imgW, imgH, aspect, zoom) {
  return rectFor(imgW, imgH, aspect,
                 zoom, rect.x + rect.w / 2, rect.y + rect.h / 2);
}

/** How far `rect` is zoomed in, the inverse of rectFor's `zoom`. */
export function zoomOf(rect, imgW, imgH, aspect) {
  const base = maxRect(imgW, imgH, aspect);
  return base.w / rect.w;
}

/* ------------------------------------------------------------------ */
/* The interactive part                                                */
/* ------------------------------------------------------------------ */

/**
 * Draw the source with everything outside `rect` dimmed.
 *
 * The masked-out area is dimmed rather than hidden: what you are choosing is
 * a part of a whole, and seeing the rest is how you tell whether you framed
 * the right band.
 */
export function drawStage(canvas, img, rect, scale) {
  const w = Math.round(img.width * scale);
  const h = Math.round(img.height * scale);
  canvas.width = w;
  canvas.height = h;

  const ctx = canvas.getContext('2d');
  ctx.clearRect(0, 0, w, h);
  ctx.drawImage(img, 0, 0, w, h);

  const r = {
    x: rect.x * scale, y: rect.y * scale,
    w: rect.w * scale, h: rect.h * scale,
  };

  ctx.save();
  /* Punch the selection out of a full-canvas veil, so the dimming is one fill
   * rather than four rectangles that have to meet exactly at the corners. */
  ctx.beginPath();
  ctx.rect(0, 0, w, h);
  ctx.rect(r.x, r.y, r.w, r.h);
  ctx.fillStyle = 'rgba(0, 0, 0, 0.55)';
  ctx.fill('evenodd');
  ctx.restore();

  ctx.strokeStyle = '#fff';
  ctx.lineWidth = 1;
  ctx.strokeRect(r.x + 0.5, r.y + 0.5, r.w - 1, r.h - 1);

  /* Thirds, the usual framing aid. Inside the selection only. */
  ctx.strokeStyle = 'rgba(255, 255, 255, 0.35)';
  ctx.beginPath();
  for (let i = 1; i < 3; i++) {
    const x = Math.round(r.x + (r.w * i) / 3) + 0.5;
    const y = Math.round(r.y + (r.h * i) / 3) + 0.5;
    ctx.moveTo(x, r.y); ctx.lineTo(x, r.y + r.h);
    ctx.moveTo(r.x, y); ctx.lineTo(r.x + r.w, y);
  }
  ctx.stroke();
}

/**
 * Make `canvas` draggable, reporting a new rectangle as the pointer moves.
 *
 * Returns a teardown function. Pointer events rather than mouse events so a
 * touchscreen works, and pointer capture so a drag that leaves the canvas
 * still tracks - releasing outside the element is otherwise a stuck drag.
 */
export function attachDrag(canvas, getState, onChange) {
  let from = null;

  const down = (e) => {
    const { rect } = getState();
    if (!rect) return;
    canvas.setPointerCapture(e.pointerId);
    from = { px: e.clientX, py: e.clientY, rect };
    canvas.classList.add('dragging');
  };

  const move_ = (e) => {
    if (!from) return;
    const { img, scale } = getState();
    /* The selection is what is drawn and what the pointer is on, so it
     * follows the pointer. Inverting this - dragging the image under a fixed
     * frame - is the other common convention and reads as broken when the
     * thing under your finger is the box. */
    onChange(move(from.rect,
                  (e.clientX - from.px) / scale,
                  (e.clientY - from.py) / scale,
                  img.width, img.height));
  };

  const up = (e) => {
    if (!from) return;
    from = null;
    canvas.classList.remove('dragging');
    if (canvas.hasPointerCapture(e.pointerId)) {
      canvas.releasePointerCapture(e.pointerId);
    }
  };

  canvas.addEventListener('pointerdown', down);
  canvas.addEventListener('pointermove', move_);
  canvas.addEventListener('pointerup', up);
  canvas.addEventListener('pointercancel', up);

  return () => {
    canvas.removeEventListener('pointerdown', down);
    canvas.removeEventListener('pointermove', move_);
    canvas.removeEventListener('pointerup', up);
    canvas.removeEventListener('pointercancel', up);
  };
}
