const test=require('node:test'),assert=require('node:assert/strict');
const {inspectInventory,inspectSourceMaps,code}=require('../scripts/inspect-daily-hap.cjs'),policy=require('../scripts/daily-package-policy.json');
function inventory(){return policy.nativeBase.flatMap(name=>['libs/arm64-v8a/'+name,'libs/x86_64/'+name])
 .concat(policy.arm64Runtime.map(name=>'libs/arm64-v8a/'+name),policy.rawfiles.map(name=>'resources/rawfile/'+name),'ets/modules.abc','module.json');}
test('daily inventory accepts the complete explicit delivery set',()=>{
  assert.deepEqual(inspectInventory(inventory()),{nativeLibraryCount:14,rawfileCount:17,strictInventoryVerified:true});
});
for(const name of ['resources/rawfile/term/xterm-addon-webgl.js','resources/rawfile/term/README.md',
 'resources/rawfile/vm/efi-virtio.rom','resources/rawfile/guide/01_Linux_Shell.html',
 'libs/arm64-v8a/libhish_linux_busybox.so','libs/arm64-v8a/libhish_linux_dash.so',
 'libs/arm64-v8a/libhish_mrsh.so','libs/arm64-v8a/libhish_probe_payload.so',
 'libs/arm64-v8a/libhish_bench_ohos.so','libs/arm64-v8a/libunreviewed.so',
  'libs/arm64-v8a/libhish_linux_tcc.so','libs/arm64-v8a/libhish_linux_git.so',
  'libs/arm64-v8a/libhish_linux_git_remote_http.so','libs/arm64-v8a/libhish_linux_curl.so',
  'libs/arm64-v8a/libhish_linux_gnu_printf.so','libs/arm64-v8a/libcrypto3.so',
  'resources/rawfile/compiler-data.bin','backup/source.cpp','backup/CLAUDE.md']){
 test('daily inventory refuses '+name,()=>assert.throws(()=>inspectInventory(inventory().concat(name))));
}
test('daily inventory refuses missing required libraries and licenses',()=>{
 for(const name of ['libs/arm64-v8a/libhish_shell.so','resources/rawfile/licenses/mksh-COPYRIGHT.txt'])
  assert.throws(()=>inspectInventory(inventory().filter(value=>value!==name)));
});
test('daily inventory rejects an alternate library ABI',()=>assert.throws(()=>inspectInventory(inventory().concat('libs/armeabi-v7a/libhish_shell.so'))));
test('daily inventory accepts only the SDK source-map location, not arbitrary maps or code',()=>{
 inspectInventory(inventory().concat('ets/sourceMaps.map'));
 for(const name of ['backup/sourceMaps.map','ets/extra.abc','extras/archive.bin'])assert.throws(()=>inspectInventory(inventory().concat(name)));
});
function sourceMaps(){return Object.fromEntries(policy.arktsSources.map(name=>[name,{version:3,sources:[name],mappings:''}]));}
test('SDK source maps contain current module locations but no embedded source text',()=>{
 assert.deepEqual(inspectSourceMaps(Buffer.from(JSON.stringify(sourceMaps()))),{sourceMapContainsSourceText:false,arktsSourceCount:18});
});
test('source maps reject backup modules and embedded source text',()=>{
 const maps=sourceMaps();maps.backup={version:3,sources:['feature/hish_main/src/main/ets/native/AuditArkChild.ets']};
 assert.throws(()=>inspectSourceMaps(Buffer.from(JSON.stringify(maps))));
 delete maps.backup;Object.values(maps)[0].sourcesContent=['unwanted source'];
 assert.throws(()=>inspectSourceMaps(Buffer.from(JSON.stringify(maps))));
});
function elf(){
 const bytes=Buffer.alloc(256);bytes.write('7f454c46','hex');bytes[4]=2;bytes[5]=1;
 bytes.writeBigUInt64LE(64n,32);bytes.writeUInt16LE(1,56);
 bytes.writeUInt32LE(1,64);bytes.writeUInt32LE(5,68);bytes.writeBigUInt64LE(256n,96);
 return bytes;
}
test('runtime code comparison tolerates only stripped section-table metadata',()=>{
 const original=elf(),stripped=Buffer.from(original);stripped.writeBigUInt64LE(4096n,40);stripped.writeUInt16LE(10,60);stripped.writeUInt16LE(9,62);
 assert.deepEqual(code(original),code(stripped));
 for(const offset of [24,58,128,255]){
  const changed=Buffer.from(original);changed[offset]^=1;assert.notDeepEqual(code(original),code(changed),'Real header/code change ignored: '+offset);
 }
});
