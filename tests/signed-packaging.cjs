const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const policy=require('../scripts/daily-package-policy.json'),{compareSigned,entries}=require('../scripts/inspect-daily-hap.cjs');
const root=path.resolve(__dirname,'../.test/signed-packaging-tests');fs.mkdirSync(root,{recursive:true});
// Synthetic stored ZIPs test byte-preservation rules, not real signing.
function zip(files){const local=[],central=[];let offset=0;
 for(const [name,data] of files){const n=Buffer.from(name),b=Buffer.from(data),h=Buffer.alloc(30),c=Buffer.alloc(46);
  h.writeUInt32LE(0x04034b50);h.writeUInt32LE(b.length,18);h.writeUInt32LE(b.length,22);h.writeUInt16LE(n.length,26);
  c.writeUInt32LE(0x02014b50);c.writeUInt32LE(b.length,20);c.writeUInt32LE(b.length,24);c.writeUInt16LE(n.length,28);c.writeUInt32LE(offset,42);
  local.push(h,n,b);central.push(c,n);offset+=h.length+n.length+b.length;
 }const cd=Buffer.concat(central),end=Buffer.alloc(22);end.writeUInt32LE(0x06054b50);end.writeUInt16LE(files.length,8);end.writeUInt16LE(files.length,10);end.writeUInt32LE(cd.length,12);end.writeUInt32LE(offset,16);
 return Buffer.concat([...local,cd,end]);
}
function fixture(){const dir=fs.mkdtempSync(path.join(root,'case-')),a=path.join(dir,'unsigned.hap'),b=path.join(dir,'signed.hap');
 const names=policy.nativeBase.flatMap(name=>['libs/arm64-v8a/'+name,'libs/x86_64/'+name])
  .concat(policy.arm64Runtime.map(name=>'libs/arm64-v8a/'+name),policy.rawfiles.map(name=>'resources/rawfile/'+name),policy.frameworkFiles);
 const files=names.map(name=>[name,'synthetic public bytes: '+name]);fs.writeFileSync(a,zip(files));return {a,b,files};
}
test('signer may add only the named page-info entry without changing public bytes',()=>{
 const f=fixture();fs.writeFileSync(f.b,zip([...f.files,['.pages.info','synthetic opaque signer metadata']]));
 assert.deepEqual(compareSigned(f.a,f.b),{publicEntriesUnchanged:true,publicEntriesCompared:f.files.length,addedSignerEntries:['.pages.info'],signerEntryContentRead:false});
 assert.throws(()=>entries(f.b).publicHash('.pages.info'));
});
test('signed comparison rejects extra executable code and changes to original metadata',()=>{
 const f=fixture();fs.writeFileSync(f.b,zip([...f.files,['libs/arm64-v8a/extra.so','unapproved']]));assert.throws(()=>compareSigned(f.a,f.b));
 fs.writeFileSync(f.b,zip(f.files.map(([name,data])=>[name,name==='module.json'?'changed':data])));assert.throws(()=>compareSigned(f.a,f.b));
});
test('signed comparison rejects modified original code and missing entries',()=>{
 const f=fixture();fs.writeFileSync(f.b,zip(f.files.map(([name,data])=>[name,name==='ets/modules.abc'?'changed code':data])));assert.throws(()=>compareSigned(f.a,f.b));
 fs.writeFileSync(f.b,zip(f.files.slice(1)));assert.throws(()=>compareSigned(f.a,f.b));
});
