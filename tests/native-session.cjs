const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const repo = path.resolve(__dirname, '..');
const cli = process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME || path.resolve(repo, '../command-line-tools');
const ts = require(path.join(cli, 'codelinter/linter/arkPerfCheck/node_modules/typescript'));
const sourcePath = path.join(repo, 'feature/hish_main/src/main/ets/native/NativeTerminalSession.ets');
const source = fs.readFileSync(sourcePath, 'utf8');
function compile(text) {
  return ts.transpileModule(text, { compilerOptions: {
    target: ts.ScriptTarget.ES2021, module: ts.ModuleKind.CommonJS
  } }).outputText;
}
function fixture(pty = true) {
  const output = [], events = [], sent = [], modes = [], timers = new Map();
  const native = {
    limit: Infinity, frames: [], closed: [], reads: 0,
    mode: { rawInput: false, echoInput: true, convertEol: !pty },
    async start() { return { id: 17, pid: 900, pty }; },
    read(id) {
      assert.equal(id, 17); this.reads++;
      const frame = this.frames.shift() || { data: new ArrayBuffer(0), running: true };
      for(const key of ['rawInput','echoInput','convertEol','controlChars','localFlags']) if(key in frame) this.mode[key] = frame[key];
      return { ...this.mode, ...frame };
    },
    write(id, data) {
      assert.equal(id, 17);
      const bytes = new Uint8Array(data), count = Math.min(bytes.length, this.limit);
      assert.ok(bytes.length <= 65536);
      sent.push(...bytes.subarray(0, count));
      return count;
    },
    resize(id, columns, rows) { assert.deepEqual([id, columns, rows], [17, 120, 40]); },
    close(id) { this.closed.push(id); events.push('close'); }
  };
  const exports = {};
  vm.runInNewContext(compile(source), {
    exports, Uint8Array, ArrayBuffer, Promise,
    require(name) {
      if (name === 'libhish_main.so') return {
        start: (...args) => native.start(...args), read: (...args) => native.read(...args),
        write: (...args) => native.write(...args), resize: (...args) => native.resize(...args),
        close: (...args) => native.close(...args)
      };
      if (name === '@kit.ArkTS') return { util: {
        TextEncoder: { create: () => ({ encodeInto: text => new TextEncoder().encode(text) }) }
      } };
      throw new Error('Unexpected import ' + name);
    },
    setInterval(callback) { timers.set(1, callback); return 1; },
    clearInterval(id) { timers.delete(id); }
  });
  return {
    native, output, events, sent, modes, timers,
    async open() {
      return exports.NativeTerminalSession.open('/private/files/native', 80, 24, 'shell', data => {
        events.push('data'); output.push(Buffer.from(data));
      }, () => events.push('exit'), value => modes.push(value));
    },
    tick() { for (const callback of [...timers.values()]) callback(); },
    text() { return Buffer.concat(output).toString('utf8'); }
  };
}

async function main() {
  let count = 0;
  const f = fixture();
  const session = await f.open();
  f.native.limit = 0;
  session.send('中文 test\r'); f.tick();
  assert.equal(f.sent.length, 0);
  f.native.limit = 2;
  for (let i = 0; i < 20; i++) f.tick();
  assert.deepEqual(Buffer.from(f.sent), Buffer.from('中文 test\r'));
  assert.equal(f.text(), ''); // PTY owns echo; the application must not duplicate it.
  count++;
  session.resize(120, 40); count++;

  const encoded = Buffer.from('中文🙂');
  f.native.frames.push(
    { data: Uint8Array.from(encoded.subarray(0, 2)).buffer, running: true },
    { data: Uint8Array.from(encoded.subarray(2)).buffer, running: false });
  f.tick(); f.tick(); f.tick();
  assert.equal(f.text(), '中文🙂');
  assert.deepEqual(f.events, ['data', 'data', 'close', 'exit']);
  session.close(); session.close();
  assert.deepEqual(f.native.closed, [17]);
  assert.equal(f.timers.size, 0); count++;

  const paste = fixture(), large = await paste.open();
  large.send('x'.repeat(150000));
  paste.tick(); paste.tick(); paste.tick();
  assert.equal(paste.sent.length, 150000); large.close(); count++;

  const p = fixture(false), pipe = await p.open();
  pipe.send('echo 你x\x7f好🙂\r'); pipe.send('\n');
  p.tick(); p.tick();
  assert.deepEqual(Buffer.from(p.sent), Buffer.from('echo 你好🙂\n'));
  assert.match(p.text(), /好🙂\r\n$/); count++;
  p.sent.length = 0;
  pipe.send('\x1b['); pipe.send('A'); pipe.send('\x1bOA'); pipe.send('next\r'); p.tick();
  assert.deepEqual(Buffer.from(p.sent), Buffer.from('next\n')); count++;
  p.sent.length = 0;
  pipe.send('cancel this'); pipe.send('\x03'); p.tick();
  assert.deepEqual(p.sent, [3]);
  assert.match(p.text(), /\^C\r\n$/); count++;
  for(const [key,label] of [['\x1a','^Z'],['\x1c','^\\']]){
    p.sent.length=0;pipe.send('discard pending line');pipe.send(key);p.tick();
    assert.deepEqual(p.sent,[key.charCodeAt(0)]);assert.ok(p.text().endsWith(label+'\r\n'));count++;
  }
  p.sent.length = 0;
  pipe.send('\x04'); p.tick(); assert.deepEqual(p.sent, [4]); pipe.close(); count++;
  const edit = fixture(false), editor = await edit.open();
  editor.send('discard🙂\x15kept word\x17tail\r');edit.tick();
  assert.equal(Buffer.from(edit.sent).toString(),'kept tail\n');count++;
  edit.sent.length=0;editor.send('partial\x04');edit.tick();
  assert.equal(Buffer.from(edit.sent).toString(),'partial');count++;
  edit.sent.length=0;editor.send('\x04next\r');edit.tick();edit.tick();
  assert.equal(Buffer.from(edit.sent).toString(),'\x04next\n');editor.close();count++;
  const custom = fixture(false), customSession = await custom.open();
  const controls = new Uint8Array(32);controls[0]=2;controls[1]=28;controls[2]=127;controls[3]=21;controls[4]=5;controls[10]=26;controls[14]=23;
  custom.native.frames.push({ data: new ArrayBuffer(0), running: true, controlChars: controls.buffer, localFlags: 0x800b });custom.tick();
  customSession.send('cancel\x02');custom.tick();assert.deepEqual(custom.sent,[2]);count++;
  custom.sent.length=0;customSession.send('\x03\r');custom.tick();assert.deepEqual(custom.sent,[3,10]);count++;
  custom.sent.length=0;customSession.send('\x05');custom.tick();assert.deepEqual(custom.sent,[5]);customSession.close();count++;
  const longEdit=fixture(false), longEditor=await longEdit.open();longEditor.send('x'.repeat(12000)+'🙂');
  const frames=longEdit.output.length;longEditor.send('\x15');
  assert.equal(longEdit.output.length-frames,1);longEditor.send('echo ready\r');longEdit.tick();
  assert.equal(Buffer.from(longEdit.sent).toString(),'echo ready\n');longEditor.close();count++;

  // ash disables canonical input while retaining ONLCR; vi disables both.
  const raw = fixture(false), rawSession = await raw.open();
  raw.native.frames.push({ data: new ArrayBuffer(0), running: true, rawInput: true, echoInput: false, convertEol: true });
  raw.tick(); rawSession.send('\x1b[A\x03'); raw.tick();
  assert.deepEqual(raw.sent, [27, 91, 65, 3]);
  assert.equal(raw.text(), ''); assert.deepEqual(raw.modes, [true]);
  raw.sent.length=0;
  for(let i=0;i<10;i++){rawSession.send('中文\t\x7f\x1b[D\r');raw.tick();}
  assert.deepEqual(Buffer.from(raw.sent),Buffer.from('中文\t\x7f\x1b[D\r'.repeat(10)));
  assert.equal(raw.text(),'');count++;
  raw.native.frames.push({ data: new ArrayBuffer(0), running: true, rawInput: true, echoInput: false, convertEol: false });
  raw.tick(); assert.deepEqual(raw.modes, [true, false]);
  raw.native.frames.push({ data: new ArrayBuffer(0), running: true, rawInput: false, echoInput: true, convertEol: true });
  raw.tick(); raw.sent.length = 0; rawSession.send('pwd\r'); raw.tick();
  assert.deepEqual(Buffer.from(raw.sent), Buffer.from('pwd\n'));
  assert.deepEqual(raw.modes, [true, false, true]); rawSession.close(); count++;

  const failed = fixture();
  failed.native.start = async () => { throw new Error('system rejected native process'); };
  await assert.rejects(failed.open(), /system rejected/);
  assert.equal(failed.timers.size, 0); assert.deepEqual(failed.native.closed, []); count++;
  const broken = fixture(), brokenSession = await broken.open();
  broken.native.read = () => { throw new Error('channel error'); };
  broken.tick(); broken.tick();
  assert.match(broken.text(), /channel error/);
  assert.deepEqual(broken.native.closed, [17]);
  assert.equal(broken.events.filter(item => item === 'exit').length, 1);
  brokenSession.close(); count++;

  // Run the actual view lifecycle methods, excluding only ArkUI's rendering DSL.
  const view = fs.readFileSync(path.join(repo, 'feature/hish_main/src/main/ets/components/WebTerminal.ets'), 'utf8');
  const start = view.indexOf('  private nativeSession:'), end = view.indexOf('  // 性能优化：输出缓冲区', start);
  assert.ok(start >= 0 && end > start);
  const openings = [], viewOutput = [], viewExports = {};
  const harness = `class Harness {
    webviewController = { runJavaScript() {} }; flushTimeoutID = undefined; pendingChunks = [];
    getUIContext() { return { getHostContext: () => ({ getApplicationContext: () => ({ filesDir: '/private/files' }) }) }; }
    onData(data: ArrayBuffer) { collect(data); }
    ${view.slice(start, end)}
  }; exports.Harness = Harness;`;
  vm.runInNewContext(compile(harness), {
    exports: viewExports, collect: data => viewOutput.push(Buffer.from(data).toString('utf8')),
    NativeTerminalSession: { open(...args) { return new Promise((resolve, reject) => openings.push({ args, resolve, reject })); } },
    util: { TextEncoder: { create: () => ({ encodeInto: text => new TextEncoder().encode(text) }) } },
    clearTimeout() {}, Promise, diagnosticsEnabled: () => false,
    AppStorage: { get: () => '' }
  });
  const v = new viewExports.Harness(), closed = [];
  const first = v.startNativeSession('shell'), second = v.startNativeSession('probe');
  openings[1].resolve({ close() { closed.push('second'); } }); await second;
  openings[0].resolve({ close() { closed.push('first'); } }); await first;
  assert.deepEqual(closed, ['first']);
  const before = viewOutput.length;
  openings[0].args[4](Buffer.from('stale').buffer); openings[0].args[5]();
  assert.equal(viewOutput.length, before); count++;
  v.aboutToDisappear(); assert.deepEqual(closed, ['first', 'second']); count++;
  const pending = v.startNativeSession('shell'); v.aboutToDisappear();
  openings[2].resolve({ close() { closed.push('pending'); } }); await pending;
  assert.equal(closed.at(-1), 'pending'); count++;

  const keyExports = {};
  vm.runInNewContext(compile(fs.readFileSync(path.join(repo,
    'feature/hish_main/src/main/ets/lib/terminalVirtualKeys.ets'), 'utf8')), { exports: keyExports });
  assert.equal(keyExports.applyVirtualKeyModifiers('c', true, false).data, '\x03');
  assert.equal(keyExports.applyVirtualKeyModifiers('c', true, false).shouldResetCtrl, true);
  assert.equal(keyExports.applyVirtualKeyModifiers('\x1b[A', true, false).data, '\x1b[A'); count++;
  assert.equal(keyExports.applyVirtualKeyModifiers('C', true, true).data, '\x03');
  assert.equal(keyExports.applyVirtualKeyModifiers(' ', true, false).data, '\x00');
  assert.equal(keyExports.applyVirtualKeyModifiers('?', true, false).data, '\x7f'); count++;
  assert.equal(keyExports.applyVirtualKeyModifiers('ß', true, false).data, 'ß');
  assert.equal(keyExports.applyVirtualKeyModifiers('c', false, true).data, 'C');
  const shifted = keyExports.applyVirtualKeyModifiers('a', false, true);
  assert.equal(shifted.data, 'A');
  assert.equal(shifted.shouldResetShift, true);
  assert.equal(keyExports.applyVirtualKeyModifiers('b', false, !shifted.shouldResetShift).data, 'b');
  assert.equal(keyExports.applyVirtualKeyModifiers('', false, true).shouldResetShift, false);
  assert.equal(keyExports.applyVirtualKeyModifiers('\x1b[A', false, true).shouldResetShift, true);
  const alt = keyExports.applyVirtualKeyModifiers('c', false, false, true);
  assert.equal(alt.data, '\x1bc'); assert.equal(alt.shouldResetAlt, true);
  assert.equal(keyExports.applyVirtualKeyModifiers('c', true, false, true).data, '\x1b\x03');
  assert.equal(keyExports.applyVirtualKeyModifiers('', false, false, true).shouldResetAlt, false); count++;
  console.log(`PASS: ${count} native transport, lifecycle and virtual-key checks`);
}
main().catch(error => { console.error(error); process.exitCode = 1; });
