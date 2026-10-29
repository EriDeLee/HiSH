const test=require('node:test'),assert=require('node:assert/strict');
const {hash,matches}=require('../scripts/linux/source-runtime.cjs');
const input=Buffer.from('source'),script='builder-hash';
const manifest={status:'built',commit:'pinned',patches:[],archive:{sha512:'pinned'},inputs:[['loader.h',hash(input)]]};
manifest.fingerprint=hash(JSON.stringify({commit:manifest.commit,patches:manifest.patches,archive:manifest.archive,inputs:manifest.inputs,script}));
test('source cache requires matching recipe and all input bytes',()=>{
 assert(matches(manifest,script,()=>input));assert(!matches(manifest,'different',()=>input));assert(!matches(manifest,script,()=>Buffer.from('changed')));
 assert(!matches({...manifest,status:'failed'},script,()=>input));
});
