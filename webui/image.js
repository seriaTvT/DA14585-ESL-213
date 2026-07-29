/*
 * image.js - turn an arbitrary picture into a 1bpp panel framebuffer.
 *
 * The panel has two levels and no greys, so every interesting decision here is
 * about *how* to throw away the other 254: scale to fit, adjust the tones so
 * the interesting part of the histogram survives, then dither.
 *
 * Nothing in this file talks to the tag, and nothing knows about the DOM
 * beyond the canvas it rasterises through. The output is a Panel from epd.js,
 * which means the preview and the upload are the same bytes - see toPanel().
 */
import { Panel, activePanel } from './epd.js';

export const FITS = {
  contain: 'Contain — whole image, padded',
  cover:   'Cover — fill the frame, cropped',
  stretch: 'Stretch — fill, ignore aspect',
};

export const DITHERS = {
  'floyd-steinberg': 'Floyd–Steinberg',
  atkinson:          'Atkinson',
  ordered:           'Ordered (Bayer 8×8)',
  threshold:         'Threshold (no dither)',
};

/** Defaults chosen to make a typical photo look reasonable with no fiddling. */
export const DEFAULTS = {
  fit: 'contain',
  dither: 'floyd-steinberg',
  landscape: true,
  invert: false,
  brightness: 0,     /* -100..100, added to the 0..255 tone  */
  contrast: 0,       /* -100..100, pivoted about mid grey    */
  threshold: 128,    /* 0..255, the black/white split point  */
};

/* ------------------------------------------------------------------ */
/* Loading                                                             */
/* ------------------------------------------------------------------ */

/**
 * Decode a File/Blob into something drawable.
 *
 * createImageBitmap handles orientation and is the fast path, but it rejects
 * SVG without an intrinsic size in some browsers, so an <img> is kept as the
 * fallback rather than letting a whole format fail to load.
 */
export async function loadImage(blob) {
  try {
    return await createImageBitmap(blob);
  } catch {
    const url = URL.createObjectURL(blob);
    try {
      const img = new Image();
      await new Promise((resolve, reject) => {
        img.onload = resolve;
        img.onerror = () => reject(new Error('Not an image this browser can decode.'));
        img.src = url;
      });
      return img;
    } finally {
      URL.revokeObjectURL(url);
    }
  }
}

/* ------------------------------------------------------------------ */
/* Rasterise + tone                                                    */
/* ------------------------------------------------------------------ */

/**
 * Draw `img` into a `w` x `h` grey buffer using the given fit mode.
 *
 * Returns Float32Array luminance in 0..255. Float rather than bytes because
 * error diffusion accumulates fractional error into neighbouring pixels, and
 * rounding it at every step is exactly the banding dithering exists to avoid.
 */
export function rasterize(img, w, h, { fit = 'contain', invert = false,
                                       brightness = 0, contrast = 0 } = {}) {
  const cv = document.createElement('canvas');
  cv.width = w;
  cv.height = h;
  const ctx = cv.getContext('2d', { willReadFrequently: true });

  /* Pad with white, not black: on a white panel that reads as margin rather
   * than as a heavy border, and it matches what `contain` implies. */
  ctx.fillStyle = '#fff';
  ctx.fillRect(0, 0, w, h);
  ctx.imageSmoothingQuality = 'high';

  const iw = img.width, ih = img.height;
  if (fit === 'stretch') {
    ctx.drawImage(img, 0, 0, w, h);
  } else {
    /* contain takes the smaller scale so everything fits; cover takes the
     * larger so nothing is left blank, and the overflow is centre-cropped. */
    const pick = fit === 'cover' ? Math.max : Math.min;
    const s = pick(w / iw, h / ih);
    const dw = iw * s, dh = ih * s;
    ctx.drawImage(img, (w - dw) / 2, (h - dh) / 2, dw, dh);
  }

  const px = ctx.getImageData(0, 0, w, h).data;
  const gray = new Float32Array(w * h);

  /* The usual contrast curve: a gain pivoted about mid grey. The magic 259
   * keeps the endpoints stable at full swing. */
  const c = Math.max(-255, Math.min(255, contrast * 2.55));
  const gain = (259 * (c + 255)) / (255 * (259 - c));

  for (let i = 0, p = 0; i < gray.length; i++, p += 4) {
    /* Rec.709 luma. Applied to sRGB values without linearising: it is what
     * image editors do, and a linear-light conversion comes out visibly dark
     * once quantised to two levels. */
    let v = 0.2126 * px[p] + 0.7152 * px[p + 1] + 0.0722 * px[p + 2];

    /* Composite over white by alpha, so a transparent PNG lands on the same
     * background the padding used instead of dithering its own black. */
    const a = px[p + 3] / 255;
    v = v * a + 255 * (1 - a);

    v = gain * (v - 128) + 128 + brightness * 2.55;
    if (invert) v = 255 - v;
    gray[i] = v < 0 ? 0 : v > 255 ? 255 : v;
  }
  return gray;
}

/* ------------------------------------------------------------------ */
/* Dithering                                                           */
/* ------------------------------------------------------------------ */

/* Error-diffusion kernels: [dx, dy, weight]. Divisors are folded into the
 * weights so the loop stays a plain multiply-accumulate. */
const KERNELS = {
  'floyd-steinberg': {
    serpentine: true,
    taps: [[1, 0, 7 / 16], [-1, 1, 3 / 16], [0, 1, 5 / 16], [1, 1, 1 / 16]],
  },
  atkinson: {
    /* Atkinson passes on only 6/8 of the error. It loses shadow and highlight
     * detail, and in exchange the texture is far cleaner - which suits a 122px
     * wide panel where fine noise is the whole image. */
    serpentine: false,
    taps: [[1, 0, 1 / 8], [2, 0, 1 / 8], [-1, 1, 1 / 8],
           [0, 1, 1 / 8], [1, 1, 1 / 8], [0, 2, 1 / 8]],
  },
};

/* Normalised 8x8 Bayer matrix, values 0..63. */
const BAYER8 = (() => {
  const m = [[0]];
  for (let n = 1; n < 8; n <<= 1) {
    const out = [];
    for (let y = 0; y < n * 2; y++) out.push(new Array(n * 2));
    for (let y = 0; y < n; y++) {
      for (let x = 0; x < n; x++) {
        const v = m[y][x] * 4;
        out[y][x] = v;
        out[y][x + n] = v + 2;
        out[y + n][x] = v + 3;
        out[y + n][x + n] = v + 1;
      }
    }
    m.length = 0;
    m.push(...out);
  }
  return m;
})();

/**
 * Quantise a grey buffer to one bit per pixel.
 *
 * Returns Uint8Array where 1 = white and 0 = black, matching the panel's
 * framebuffer convention rather than the more usual ink-is-1.
 */
export function dither(gray, w, h, { dither: mode = 'floyd-steinberg',
                                     threshold = 128 } = {}) {
  const out = new Uint8Array(w * h);

  if (mode === 'threshold' || mode === 'ordered') {
    const ordered = mode === 'ordered';
    for (let y = 0; y < h; y++) {
      for (let x = 0; x < w; x++) {
        const i = y * w + x;
        /* Bayer spreads the decision point across a tile so flat areas break
         * up into a regular screen instead of one solid block. Centred on the
         * threshold so the control still means what it says. */
        const t = ordered
          ? threshold + (BAYER8[y & 7][x & 7] - 31.5) * (255 / 64)
          : threshold;
        out[i] = gray[i] >= t ? 1 : 0;
      }
    }
    return out;
  }

  const kern = KERNELS[mode] ?? KERNELS['floyd-steinberg'];
  /* Work on a copy: the error feedback mutates as it goes, and callers keep
   * the rasterised buffer to re-dither with different settings. */
  const buf = Float32Array.from(gray);

  for (let y = 0; y < h; y++) {
    const rtl = kern.serpentine && (y & 1);
    for (let k = 0; k < w; k++) {
      /* Serpentine: alternate scan direction each row, so the diffused error
       * does not consistently drift one way and streak the image. */
      const x = rtl ? w - 1 - k : k;
      const i = y * w + x;

      const old = buf[i];
      const bit = old >= threshold ? 1 : 0;
      out[i] = bit;
      const err = old - (bit ? 255 : 0);

      for (const [dx, dy, wt] of kern.taps) {
        const nx = x + (rtl ? -dx : dx);
        const ny = y + dy;
        if (nx < 0 || nx >= w || ny >= h) continue;
        buf[ny * w + nx] += err * wt;
      }
    }
  }
  return out;
}

/* ------------------------------------------------------------------ */
/* Packing                                                             */
/* ------------------------------------------------------------------ */

/**
 * Geometry of the drawing surface for an orientation.
 *
 * Landscape is rotation 3 rather than 1 so that "up" agrees with the preset
 * faces, which is the only reason to prefer one over the other.
 */
export function surface(landscape, geom = activePanel()) {
  return landscape
    ? { w: geom.h, h: geom.w, rot: 3 }
    : { w: geom.w, h: geom.h, rot: 0 };
}

/**
 * Pack a 1bpp bitmap into a Panel.
 *
 * Goes through Panel.set() rather than writing panel.fb directly, so the
 * rotation transform is the firmware's own (fb_set in epd_gfx.c) and there is
 * no second copy of it to drift. panel.fb afterwards is exactly the number of
 * bytes the selected panel's tag expects on its image characteristic.
 */
export function toPanel(bits, w, h, rot, geom = activePanel()) {
  const panel = new Panel(geom);
  panel.setRotation(rot);
  panel.clear(1);
  for (let y = 0; y < h; y++) {
    for (let x = 0; x < w; x++) {
      panel.set(x, y, bits[y * w + x]);
    }
  }
  return panel;
}

/**
 * The whole pipeline: picture in, Panel out.
 *
 * Kept as one call because every stage depends on the one before, and the
 * intermediate buffers are only a few thousand pixels - re-running the lot on
 * a slider drag is cheaper than the bookkeeping to cache it.
 */
export function process(img, opts = {}) {
  const o = { ...DEFAULTS, ...opts };
  const { w, h, rot } = surface(o.landscape);
  const gray = rasterize(img, w, h, o);
  const bits = dither(gray, w, h, o);
  return toPanel(bits, w, h, rot);
}
