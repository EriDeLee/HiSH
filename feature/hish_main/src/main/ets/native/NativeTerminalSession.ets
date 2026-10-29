import { start as startNative, read as readNative, write as writeNative,
  resize as resizeNative, close as closeNative } from 'libhish_main.so';
import type { NativeSessionInfo } from 'libhish_main.so';
import { util } from '@kit.ArkTS';

/** xterm 与原生进程间的字节通道；不含命令解析或命令输出替身。 */
export class NativeTerminalSession {
  private readonly id: number;
  private readonly pty: boolean;
  private readonly onData: (data: ArrayBuffer) => void;
  private readonly onExit: () => void;
  private readonly onMode: ((convertEol: boolean) => void) | null;
  private rawInput: boolean = false;
  private echoInput: boolean = true;
  private convertEol: boolean | null = null;
  private readonly encoder: util.TextEncoder = util.TextEncoder.create('utf-8');
  private pending: Uint8Array[] = [];
  private offset: number = 0;
  private timer: number = -1;
  private closed: boolean = false;
  private pipeLine: string = '';
  private escapeSequence: string = '';
  private skipLineFeed: boolean = false;
  private controls: Uint8Array = new Uint8Array([3, 28, 127, 21, 4, 0, 1, 0, 17, 19, 26, 0, 18, 15, 23]);
  private localFlags: number = 0x800b;

  private constructor(id: number, pty: boolean, onData: (data: ArrayBuffer) => void, onExit: () => void,
    onMode: ((convertEol: boolean) => void) | null = null) {
    this.id = id;
    this.pty = pty;
    this.onData = onData;
    this.onExit = onExit;
    this.onMode = onMode;
    this.timer = setInterval(() => { this.tick(); }, 20);
  }

  static async open(root: string, columns: number, rows: number, mode: string,
    onData: (data: ArrayBuffer) => void, onExit: () => void,
    onMode: ((convertEol: boolean) => void) | null = null): Promise<NativeTerminalSession> {
    const pending: Promise<NativeSessionInfo> = startNative(root, columns, rows, mode);
    const info = await pending;
    return new NativeTerminalSession(info.id, info.pty, onData, onExit, onMode);
  }

  send(text: string): void {
    if (this.closed) { return; }
    if (this.pty || this.rawInput) {
      this.pending.push(this.encoder.encodeInto(text));
      if (!this.pty && this.echoInput) { this.echo(text); }
    } else {
      this.acceptPipeInput(text);
    }
  }

  // Dynamic Linux compatibility libraries publish termios state through the native channel.
  private acceptPipeInput(text: string): void {
    for (let index = 0; index < text.length; index++) {
      const value = text[index];
      if (value === '\n' && this.skipLineFeed) { this.skipLineFeed = false; continue; }
      this.skipLineFeed = value === '\r';
      if (this.escapeSequence.length > 0) {
        this.escapeSequence += value;
        if (this.escapeSequence.length > 32 ||
          (this.escapeSequence.length === 2 && value !== '[' && value !== 'O') ||
          (this.escapeSequence.length > 2 && /[A-Za-z~]$/.test(this.escapeSequence))) {
          this.escapeSequence = '';
        }
        continue;
      }
      if (value === '\x1b') { this.escapeSequence = value; continue; }
      if (value === '\r' || value === '\n') {
        this.pending.push(this.encoder.encodeInto(this.pipeLine + '\n'));
        this.pipeLine = '';
        this.echo('\r\n');
      } else if ((this.localFlags & 1) !== 0 && (this.control(value, 0) || this.control(value, 1) || this.control(value, 10))) {
        this.pipeLine = '';
        this.pending = [new Uint8Array([value.charCodeAt(0)])];
        this.offset = 0;
        this.echo('^' + String.fromCharCode(value.charCodeAt(0) + 64) + '\r\n');
      } else if (this.control(value, 4)) {
        // Nonempty EOF delivers the current line without a newline. Empty EOF
        // ends one read; the native adapter keeps the input channel usable.
        this.pending.push(this.pipeLine.length > 0 ? this.encoder.encodeInto(this.pipeLine) : new Uint8Array([value.charCodeAt(0)]));
        this.pipeLine = '';
      } else if (this.control(value, 3)) {
        this.eraseCharacters(this.pipeLine.length);
      } else if ((this.localFlags & 0x8000) !== 0 && this.control(value, 14)) {
        let end = this.pipeLine.length;
        while (end > 0 && /\s/.test(this.pipeLine[end - 1])) { end--; }
        while (end > 0 && !/\s/.test(this.pipeLine[end - 1])) { end--; }
        this.eraseCharacters(this.pipeLine.length - end);
      } else if (this.control(value, 2) || value === '\b') {
        if (this.pipeLine.length === 0) { continue; }
        const last = this.pipeLine.charCodeAt(this.pipeLine.length - 1);
        const count = last >= 0xdc00 && last <= 0xdfff && this.pipeLine.length > 1 ? 2 : 1;
        this.pipeLine = this.pipeLine.substring(0, this.pipeLine.length - count);
        this.echo('\b \b');
      } else if (value !== '\x00') {
        // Keep surrogate pairs together when encoding for xterm.
        const code = text.charCodeAt(index);
        let character = value;
        if (code >= 0xd800 && code <= 0xdbff && index + 1 < text.length) {
          character += text[++index];
        }
        this.pipeLine += character;
        this.echo(character);
      }
    }
  }

  private control(value: string, index: number): boolean {
    return this.controls[index] !== 0 && value.charCodeAt(0) === this.controls[index];
  }

  private eraseCharacters(units: number): void {
    let start = Math.max(0, this.pipeLine.length - units);
    const first = this.pipeLine.charCodeAt(start);
    if (start > 0 && first >= 0xdc00 && first <= 0xdfff) { start--; }
    let columns = 0;
    for (let index = start; index < this.pipeLine.length; index++) {
      const code = this.pipeLine.charCodeAt(index);
      if (code >= 0xd800 && code <= 0xdbff && index + 1 < this.pipeLine.length) { index++; }
      columns++;
    }
    this.pipeLine = this.pipeLine.substring(0, start);
    if (columns > 0) { this.echo('\b \b'.repeat(columns)); }
  }

  private echo(text: string): void {
    if (this.echoInput) { this.onData(this.encoder.encodeInto(text).buffer as ArrayBuffer); }
  }

  private flushInput(): void {
    if (this.closed || this.pending.length === 0) { return; }
    const data = this.pending[0].subarray(this.offset, this.offset + 65536);
    const written = writeNative(this.id, data.buffer.slice(data.byteOffset,
      data.byteOffset + data.byteLength) as ArrayBuffer);
    this.offset += written;
    if (this.offset === this.pending[0].length) {
      this.pending.shift();
      this.offset = 0;
    }
  }

  private tick(): void {
    if (this.closed) { return; }
    try {
      const output = readNative(this.id);
      const raw = output.rawInput === true;
      this.echoInput = output.echoInput !== false;
      if (output.controlChars !== undefined && output.controlChars.byteLength >= 15) {
        this.controls = new Uint8Array(output.controlChars);
        this.localFlags = output.localFlags;
      }
      if (raw !== this.rawInput) {
        this.rawInput = raw;
        this.pipeLine = '';
        this.escapeSequence = '';
      }
      // Noncanonical input does not imply OPOST is disabled (ash keeps it on).
      const convertEol = output.convertEol === true;
      if (convertEol !== this.convertEol) {
        this.convertEol = convertEol;
        if (this.onMode !== null) { this.onMode(convertEol); }
      }
      if (output.data.byteLength > 0) { this.onData(output.data); }
      if (!output.running) {
        this.close();
        this.onExit();
        return;
      }
      this.flushInput();
    } catch (err) { this.fail(err as Error); }
  }

  private fail(error: Error): void {
    this.onData(this.encoder.encodeInto('\r\n原生通道错误: ' + error.message + '\r\n').buffer as ArrayBuffer);
    this.close();
    this.onExit();
  }

  resize(columns: number, rows: number): void {
    if (!this.closed) { resizeNative(this.id, columns, rows); }
  }

  close(): void {
    if (this.closed) { return; }
    this.closed = true;
    clearInterval(this.timer);
    this.timer = -1;
    this.pending = [];
    this.pipeLine = '';
    closeNative(this.id);
  }
}
