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

export const DEVICE_NAME = 'HemaEPD-Clock';

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
      optionalServices: [CMD_SERVICE],
    });

    this.device.addEventListener('gattserverdisconnected', () => {
      this.char = null;
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
  async write(text, { chunk = 20, onProgress } = {}) {
    await this.ensureConnected();

    const bytes = new TextEncoder().encode(text);
    for (let off = 0; off < bytes.length; off += chunk) {
      const slice = bytes.slice(off, off + chunk);
      /* writeValue() is the deprecated spelling, still the only one in some
       * shipping builds; the explicit pair is preferred where present. */
      if (this.writeWithoutResponse && this.char.writeValueWithoutResponse) {
        await this.char.writeValueWithoutResponse(slice);
      } else if (this.char.writeValueWithResponse) {
        await this.char.writeValueWithResponse(slice);
      } else {
        await this.char.writeValue(slice);
      }
      onProgress?.(Math.min(off + chunk, bytes.length), bytes.length);
    }
    return bytes.length;
  }
}
