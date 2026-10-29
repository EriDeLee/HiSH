const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const assert = require('node:assert/strict');
const source = fs.readFileSync(path.join(__dirname,
  '../feature/hish_main/src/main/resources/rawfile/term/term.js'), 'utf8');
const helperStart = source.indexOf('function strToUint8Array(');
const helperEnd = source.indexOf('\n}', helperStart) + 2;
const start = source.indexOf('exports.writeBase64 ='), end = source.indexOf('exports.copy =', start);
assert.ok(helperStart > 0 && helperEnd > helperStart && start > 0 && end > start);
const chunks = [], pasted = [], modes = [], exportsObject = {};
const term = {
  modes: { applicationCursorKeysMode: false },
  write(bytes, callback) {
    chunks.push(Buffer.from(bytes));
    this.modes.applicationCursorKeysMode = true;
    callback();
  },
  paste(text) { pasted.push(text); }
};
vm.runInNewContext(source.slice(helperStart, helperEnd) + '\n' + source.slice(start, end), {
  exports: exportsObject, term, decoder: new TextDecoder('utf8'), Uint8Array,
  native: { setApplicationMode: mode => modes.push(mode) }, console,
  atob: value => Buffer.from(value, 'base64').toString('latin1')
});
const literal = Buffer.from(String.raw`C:\tmp\new\test\x1b\u4e2d`);
exportsObject.writeBase64(literal.toString('base64'), false);
assert.deepEqual(chunks[0], literal);
assert.deepEqual(modes, [true]);
const unicode = Buffer.from('中文🙂');
exportsObject.writeBase64(unicode.subarray(0, 2).toString('base64'), true);
exportsObject.writeBase64(unicode.subarray(2).toString('base64'), true);
assert.deepEqual(Buffer.concat(chunks.slice(1)), unicode);
const clipboard = '中文🙂\n' + String.raw`literal\n\x1b`;
exportsObject.pasteBase64(Buffer.from(clipboard).toString('base64'));
assert.deepEqual(pasted, [clipboard]);
console.log('PASS: 3 terminal codec checks (literal bytes, UTF-8 frames, clipboard Unicode)');
