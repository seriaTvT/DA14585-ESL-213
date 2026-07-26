/*
 * ble.js - Web Bluetooth transport for the tag's command service.
 *
 * UUIDs mirror user_custs1_def.h. The C arrays there are little-endian, so
 * they read backwards relative to these strings:
 *
 *   DEF_CMD_SVC_UUID_128  {0xfb,0x34,...,0x10,0x1f,0x00,0x00}
 *     -> 00001f10-0000-1000-8000-00805f9b34fb
 *   DEF_CMD_CHAR_UUID_128 {0xfb,0x34,...,0x1f,0x1f,0x00,0x00}
 *     -> 00001f1f-0000-1000-8000-00805f9b34fb
 *
 * Requires a secure context: https, or http://localhost. Opening this page as
 * a file:// URL will not work - navigator.bluetooth is undefined there. See
 * the README for the one-line static server.
 */

export const CMD_SERVICE = '00001f10-0000-1000-8000-00805f9b34fb';
export const CMD_CHAR    = '00001f1f-0000-1000-8000-00805f9b34fb';

/* The image service, from the same header. These are random 128-bit UUIDs
 * rather than Bluetooth-base ones, so they look nothing like the pair above:
 *   DEF_IMG_SVC_UUID_128  {0x38,0x9a,...,0x18,0x13}
 *     -> 13187b10-eba9-a3ba-044e-83d3217d9a38
 *   DEF_IMG_CHAR_UUID_128 {0xfe,0x82,...,0x64,0x4b}
 *     -> 4b646063-6264-f3a7-8941-e65356ea82fe */
export const IMG_SERVICE = '13187b10-eba9-a3ba-044e-83d3217d9a38';
export const IMG_CHAR    = '4b646063-6264-f3a7-8941-e65356ea82fe';

export const DEVICE_NAME = 'HemaEPD-Clock';

/* EPD_BUF_SIZE: ((122 + 7) / 8) * 250. The tag refreshes when exactly this
 * many bytes have arrived, with no header and no offset in the protocol, so a
 * transfer that is short by even one byte simply never completes. */
export const IMAGE_BYTES = 16 * 250;

/* The firmware repaints EPD_FLUSH_DELAY (400 ms) after the last byte lands,
 * so a push must not stall longer than that mid-script or the tag would
 * refresh a half-written face. Nothing here waits that long, but it is the
 * budget the chunk loop is working inside. */
export const FLUSH_DELAY_MS = 400;

/* The panel refresh used to block the tag for ~2 s and drop the link about
 * 1.7 s after every push, so this file treated a disconnect there as routine.
 * The firmware now polls BUSY from a timer instead (epd_display_start /
 * epd_display_busy), and a link survives repeated refreshes - measured at
 * 150 s across three of them - so a drop is a real event again and is
 * reported plainly. ensureConnected() stays: range and interference still
 * exist, and reconnecting costs no user gesture. */

/**
 * Why Web Bluetooth is unusable here, or null if it should work.
 *
 * The two causes look identical from JS - navigator.bluetooth is simply
 * undefined either way - but they need completely different fixes, so they are
 * worth telling apart. The insecure-origin case is the one people actually hit:
 * loading the page from another device over plain http:// silently disables the
 * API, because only https:// and localhost count as secure contexts.
 */
export function bluetoothProblem() {
  if (navigator.bluetooth) return null;

  if (!window.isSecureContext) {
    return `This page is not a secure context (${location.origin}), so the `
         + 'browser disables Web Bluetooth. Reach it over https:// or as '
         + 'http://localhost - serve.py sets up the https case for phones and '
         + 'other machines on the LAN.';
  }

  /* Secure context and still no API: the browser genuinely lacks it. Safari
   * and Firefox do not implement Web Bluetooth at all, and on iOS every
   * browser is Safari underneath, so no iPhone can run this page. */
  return 'This browser does not implement Web Bluetooth. Chrome, Edge or Opera '
       + 'on desktop or Android will work; Safari and Firefox will not, which '
       + 'on iOS means no browser can.';
}

export class Tag extends EventTarget {
  constructor() {
    super();
    this.device = null;
    this.char = null;
    this.imgChar = null;
    /* Set once we know which write type the characteristic offers. The
     * firmware permits both; without-response is markedly faster because it
     * does not wait for an ATT ack per 20-byte fragment. */
    this.writeWithoutResponse = false;
  }

  get connected() { return !!this.device?.gatt?.connected; }

  _log(msg, kind = 'info') {
    this.dispatchEvent(new CustomEvent('log', { detail: { msg, kind } }));
  }

  _state() { this.dispatchEvent(new Event('state')); }

  async connect() {
    const problem = bluetoothProblem();
    if (problem) throw new Error(problem);

    this._log('Requesting device…');
    this.device = await navigator.bluetooth.requestDevice({
      /* Filter by name rather than by service: the tag does not advertise its
       * 128-bit service UUIDs (they do not fit alongside the name in a 31-byte
       * advertisement), so a services filter would match nothing. */
      filters: [{ namePrefix: 'HemaEPD' }],
      optionalServices: [CMD_SERVICE, IMG_SERVICE],
    });

    this.device.addEventListener('gattserverdisconnected', () => {
      this.char = null;
      this.imgChar = null;
      this._log('Disconnected.', 'warn');
      this._state();
    });

    this._log(`Connecting to ${this.device.name}…`);
    await this._attach();

    this._log(`Connected (${this.writeWithoutResponse
      ? 'write without response' : 'write with response'}).`, 'ok');
  }

  /** Open the GATT link and resolve the command characteristic. */
  async _attach() {
    const server = await this.device.gatt.connect();
    const service = await server.getPrimaryService(CMD_SERVICE);
    this.char = await service.getCharacteristic(CMD_CHAR);
    this.writeWithoutResponse = this.char.properties.writeWithoutResponse;

    /* The image service is optional on purpose: an older tag that predates it
     * should still be usable for templates rather than failing to connect at
     * all. pushImage() is what reports its absence, at the point it matters. */
    try {
      const imgSvc = await server.getPrimaryService(IMG_SERVICE);
      this.imgChar = await imgSvc.getCharacteristic(IMG_CHAR);
    } catch {
      this.imgChar = null;
    }

    this._state();
  }

  /**
   * Reconnect if the tag has dropped us, so the caller can just act.
   *
   * Web Bluetooth only needs the chooser for the *first* connection; the
   * BluetoothDevice stays usable afterwards, so this costs no user gesture.
   * Retried because a refresh in progress will refuse the connection until the
   * panel is done.
   */
  async ensureConnected({ tries = 4, delayMs = 900 } = {}) {
    if (this.connected && this.char) return;
    if (!this.device) throw new Error('No tag chosen yet - press Connect.');

    for (let i = 1; i <= tries; i++) {
      try {
        await this._attach();
        this._log('Reconnected.', 'ok');
        return;
      } catch (err) {
        if (i === tries) {
          throw new Error(`Could not reconnect after ${tries} tries: `
                        + err.message);
        }
        this._log(`Reconnect attempt ${i} failed, retrying…`, 'dim');
        await new Promise((r) => setTimeout(r, delayMs));
      }
    }
  }

  async disconnect() {
    if (this.device?.gatt?.connected) this.device.gatt.disconnect();
    this.char = null;
    this._state();
  }

  /**
   * Send a script, fragmented to fit the ATT payload.
   *
   * The firmware reassembles lines across writes (it buffers until '\n'), so
   * the split point does not matter and a fixed 20-byte chunk is fine: that is
   * MTU-3 at the default 23-byte MTU, which is what the tag negotiates.
   */
  async write(text, opts = {}) {
    await this.ensureConnected();
    const bytes = new TextEncoder().encode(text);
    await this._stream(this.char, bytes, opts);
    return bytes.length;
  }

  /**
   * Send a full framebuffer to the image characteristic.
   *
   * The tag counts bytes and refreshes on the 4000th, so this is all-or-
   * nothing: there is no header, no offset and no way to resume. A transfer
   * that dies partway leaves the top of the framebuffer overwritten, which the
   * firmware handles by staying in clock mode and repainting over it on the
   * next minute tick - so a failure here is ugly for a moment, not sticky.
   */
  async pushImage(fb, { onProgress } = {}) {
    await this.ensureConnected();

    if (!this.imgChar) {
      throw new Error('This tag has no image service - it is running firmware '
                    + 'from before image push existed.');
    }
    if (fb.length !== IMAGE_BYTES) {
      throw new Error(`Framebuffer is ${fb.length} bytes; the tag waits for `
                    + `exactly ${IMAGE_BYTES} and would never refresh.`);
    }

    await this._stream(this.imgChar, fb, { onProgress });
    return fb.length;
  }

  /** Fragment `bytes` across `char`, one ATT payload at a time. */
  async _stream(char, bytes, { chunk = 20, onProgress } = {}) {
    /* Prefer without-response: it does not wait for an ATT ack per fragment,
     * which is worth little over the ~9 writes a template takes but turns a
     * 200-fragment image from tens of seconds into a few. */
    const withoutResp = this.writeWithoutResponse
                     && char.properties.writeWithoutResponse
                     && !!char.writeValueWithoutResponse;

    for (let off = 0; off < bytes.length; off += chunk) {
      const slice = bytes.slice(off, off + chunk);
      /* writeValue() is the deprecated spelling, still the only one in some
       * shipping builds; the explicit pair is preferred where present. */
      if (withoutResp) {
        await char.writeValueWithoutResponse(slice);
      } else if (char.writeValueWithResponse) {
        await char.writeValueWithResponse(slice);
      } else {
        await char.writeValue(slice);
      }
      onProgress?.(Math.min(off + chunk, bytes.length), bytes.length);
    }
  }
}
