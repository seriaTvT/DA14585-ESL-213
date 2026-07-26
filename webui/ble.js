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

/* A disconnect this soon after our last write is the refresh, not a fault.
 * The drop lands ~1.7 s after the last byte (400 ms flush timer, then the
 * blocking refresh outruns the supervision timeout); the window is generous
 * so a slow link still gets the benign reading. */
const REFRESH_DROP_WINDOW_MS = 8000;

export class Tag extends EventTarget {
  constructor() {
    super();
    this.device = null;
    this.char = null;
    /* Set once we know which write type the characteristic offers. The
     * firmware permits both; without-response is markedly faster because it
     * does not wait for an ATT ack per 20-byte fragment. */
    this.writeWithoutResponse = false;
    /* When our last byte went out, so a following disconnect can be told apart
     * from a real one - see the gattserverdisconnected handler. */
    this.lastWriteAt = 0;
  }

  get connected() { return !!this.device?.gatt?.connected; }

  _log(msg, kind = 'info') {
    this.dispatchEvent(new CustomEvent('log', { detail: { msg, kind } }));
  }

  _state() { this.dispatchEvent(new Event('state')); }

  async connect() {
    if (!navigator.bluetooth) {
      throw new Error(
        'Web Bluetooth unavailable. Use Chrome, Edge or Opera over ' +
        'https:// or http://localhost (not file://).');
    }

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
      /* A drop right after a push is the panel refresh, not a fault: the
       * refresh blocks the CPU for ~2 s, which outlasts the link's supervision
       * timeout, so the tag always drops us. Measured at ~1.7 s after the last
       * byte. Nothing is lost - the tag has the template by then - so this is
       * reported as routine and the next action silently reconnects. */
      const refreshing = Date.now() - this.lastWriteAt < REFRESH_DROP_WINDOW_MS;
      this._log(refreshing
        ? 'Tag dropped the link to refresh the panel (expected).'
        : 'Disconnected.', refreshing ? 'dim' : 'warn');
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
    /* Stops the disconnect handler calling this an expected refresh drop. */
    this.lastWriteAt = 0;
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
    this.lastWriteAt = Date.now();
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
      this.lastWriteAt = Date.now();
      onProgress?.(Math.min(off + chunk, bytes.length), bytes.length);
    }
    return bytes.length;
  }
}
