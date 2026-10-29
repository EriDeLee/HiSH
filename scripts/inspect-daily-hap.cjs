// Inspect public ZIP entries only. Never extract signing/profile entries.
const fs=require('node:fs'),zlib=require('node:zlib'),assert=require('node:assert/strict');
const {hash}=require('./linux/source-runtime.cjs');
const policy=require('./daily-package-policy.json');
function inspectInventory(names){
 const files=names.filter(name=>!name.endsWith('/'));
 for(const name of files)assert(!name.includes('\\')&&!name.startsWith('/')&&!name.split('/').includes('..'),'Unsafe HAP path: '+name);
 const expected=policy.nativeBase.flatMap(name=>['libs/arm64-v8a/'+name,'libs/x86_64/'+name])
   .concat(policy.arm64Runtime.map(name=>'libs/arm64-v8a/'+name));
 assert.deepEqual(files.filter(name=>name.startsWith('libs/')).sort(),expected.sort(),'Unexpected or missing daily native library');
 const rawfiles=files.filter(name=>name.startsWith('resources/rawfile/'));
 assert.deepEqual(rawfiles.sort(),policy.rawfiles.map(name=>'resources/rawfile/'+name).sort(),'Unexpected or missing rawfile (backup code/docs must stay outside the package)');
 assert.deepEqual(files.filter(name=>name!=='ets/sourceMaps.map'&&/\.(md|rst|c|cc|cpp|h|sh|ps1|cjs|ts|ets|map)$/i.test(name)),[],'Development source/document leaked into HAP');
 const allowed=new Set([...expected,...policy.rawfiles.map(name=>'resources/rawfile/'+name),...policy.frameworkFiles,'ets/sourceMaps.map']);
 assert.deepEqual(files.filter(name=>!allowed.has(name)),[],'Unreviewed HAP entry');
 return {nativeLibraryCount:expected.length,rawfileCount:rawfiles.length,strictInventoryVerified:true};
}
function inspectSourceMaps(bytes){
 const maps=JSON.parse(bytes.toString('utf8')),sources=[];
 for(const map of Object.values(maps)){
  assert(Array.isArray(map.sources),'Missing source-map source list');
  assert(!Object.hasOwn(map,'sourcesContent'),'Embedded source text must not be packaged');
  sources.push(...map.sources);
 }
 assert.deepEqual(sources.sort(),[...policy.arktsSources].sort(),'Unexpected/missing ArkTS source record (diagnostic or retired module)');
 return {sourceMapContainsSourceText:false,arktsSourceCount:sources.length};
}
function entries(file){
 const bytes=fs.readFileSync(file);let end=bytes.length-22;
 for(;end>=Math.max(0,bytes.length-65557);end--)if(bytes.readUInt32LE(end)===0x06054b50)break;
 assert(end>=0,'ZIP end missing');const count=bytes.readUInt16LE(end+10);let at=bytes.readUInt32LE(end+16);const records=new Map();
 for(let i=0;i<count;i++){
  assert.equal(bytes.readUInt32LE(at),0x02014b50);const n=bytes.readUInt16LE(at+28),extra=bytes.readUInt16LE(at+30),comment=bytes.readUInt16LE(at+32),name=bytes.toString('utf8',at+46,at+46+n);
  assert(!records.has(name),'Duplicate HAP entry');
  records.set(name,{local:bytes.readUInt32LE(at+42),size:bytes.readUInt32LE(at+20),method:bytes.readUInt16LE(at+10)});at+=46+n+extra+comment;
 }
  function decode(name){
   const r=records.get(name);assert(r,'HAP entry missing: '+name);const start=r.local+30+bytes.readUInt16LE(r.local+26)+bytes.readUInt16LE(r.local+28),compressed=bytes.subarray(start,start+r.size);
   assert(r.method===0||r.method===8);return r.method===0?compressed:zlib.inflateRawSync(compressed);
  }
  const publicNames=new Set([...policy.nativeBase.flatMap(name=>['libs/arm64-v8a/'+name,'libs/x86_64/'+name]),
   ...policy.arm64Runtime.map(name=>'libs/arm64-v8a/'+name),...policy.rawfiles.map(name=>'resources/rawfile/'+name),
   ...policy.frameworkFiles,'ets/sourceMaps.map']);
  return {names:[...records.keys()],read(name){
   assert(/^(libs\/[^/]+\/[^/]+\.so|ets\/(modules\.abc|sourceMaps\.map)|resources\/rawfile\/term\/[^/]+\.(html|css|js)|resources\/rawfile\/pages\/license\.html|resources\/rawfile\/licenses\/mksh-(COPYRIGHT|terms)\.txt)$/.test(name),'Public code/license entries only');
   return decode(name);
  },publicHash(name){
   assert(publicNames.has(name),'Public application entries only; do not read signing entries');
   return hash(decode(name));
  }};
}
function compareSigned(unsigned,signed){
 const a=entries(unsigned),b=entries(signed);inspectInventory(a.names);
 const added=b.names.filter(name=>!a.names.includes(name));
 assert(added.length<=1&&added.every(name=>name==='.pages.info'),'Unexpected entry added by signer');
 assert.deepEqual(b.names.filter(name=>name!=='.pages.info').sort(),[...a.names].sort(),'Original package entries changed during signing');
 for(const name of a.names)assert.equal(b.publicHash(name),a.publicHash(name),'Public application bytes changed during signing: '+name);
 return {publicEntriesUnchanged:true,publicEntriesCompared:a.names.length,addedSignerEntries:added,signerEntryContentRead:false};
}
function code(bytes){
  assert.equal(bytes.toString('hex',0,4),'7f454c46');const segments=[],off=Number(bytes.readBigUInt64LE(32));
  for(let i=0;i<bytes.readUInt16LE(56);i++){const h=off+i*56;if(bytes.readUInt32LE(h)!==1||!(bytes.readUInt32LE(h+4)&1))continue;
   const offset=Number(bytes.readBigUInt64LE(h+8)),size=Number(bytes.readBigUInt64LE(h+32)),data=Buffer.from(bytes.subarray(offset,offset+size));
   // Some upstream RX LOADs start at file offset zero and include the ELF
   // header. SDK strip changes only the section-table offset/count/name index;
   // these are not runtime instructions. Keep every other header/code byte.
   if(offset===0&&size>=64){data.fill(0,40,48);data.fill(0,60,64);}
   segments.push({offset,size,sha256:hash(data)});
 }return segments;
}
function inspect(file,payloads){
  const hap=entries(file),names=hap.names.filter(name=>name.startsWith('libs/'));
  const inventory=inspectInventory(hap.names);
  const excluded=names.filter(name=>/libhish_(probe|bench|mksh_trial)|libhish_linux_(busybox|dash|.*check|musl_dynamic|musl_static|glibc_dynamic|glibc_static|glibc_static_compat|fs_identity|glibc_libc|glibc_padded|glibc_separate|identity|hishbench.*)\.so$/.test(name));
 assert.deepEqual(excluded,[],'Diagnostic/intermediate libraries leaked into daily HAP');
  const runtime=[];for(const name of policy.arm64Runtime){
  const packaged=hap.read('libs/arm64-v8a/'+name),source=fs.readFileSync(require('node:path').join(payloads,name));
  assert.deepEqual(code(packaged),code(source),'Wrong runtime code packaged: '+name);runtime.push({name,code:code(packaged)});
 }
  for(const abi of ['arm64-v8a','x86_64']){
   const main=hap.read('libs/'+abi+'/libhish_main.so');
   for(const marker of ['DOWNLOAD_ORIGINAL_BYTES_EXTRACTED','PROBE baseline BEGIN','diagnosticsEnabled',
    'probe requires root, kind','libchild_process.so','libhilog_ndk.z.so'])
    assert(!main.includes(Buffer.from(marker)),'Native diagnostic branch/dependency leaked: '+marker);
  }
   const abc=hap.read('ets/modules.abc');
   for(const marker of ['AuditArkChild','ProcessRouteAudit','TerminalProbe','runNativeProbe','nativeProbe','独立启动的原生探针','检测原生能力'])
    assert(!abc.includes(Buffer.from(marker)),'Diagnostic ArkTS code leaked: '+marker);
   const sourceMaps=hap.names.includes('ets/sourceMaps.map')?inspectSourceMaps(hap.read('ets/sourceMaps.map')):{sourceMapPresent:false};
  for(const abi of ['arm64-v8a','x86_64']){
   const shell=hap.read('libs/'+abi+'/libhish_shell.so');
   assert(shell.includes(Buffer.from('HiSHDefaultShellMain')),'Default mksh adapter missing');
    for(const name of ['HiSHMrshMain','MKSH-TRIAL> ','[mksh-entry]','__cyg_profile_func_enter',
    'HISH_RUN_FIXED_RANGE','HISH_RUN_ADDRESS_CONFLICT','HiSHRunPtraceFile','MapExecutableTemplate','RunPatchedImage',
    'HiSHPrepareCompilerEnvironment','HiSHPrepareNetworkEnvironment','HiSHCompilerData',
    'HISH_TOOLCHAIN_ROOT','C_INCLUDE_PATH','GIT_EXEC_PATH','GIT_TEMPLATE_DIR',
    'libhish_linux_git.so','libhish_linux_curl.so','libhish_linux_tcc.so','BEGIN CERTIFICATE'])
    assert(!shell.includes(Buffer.from(name)),'Retired/trial shell leaked: '+name);
  }
 const htmlName=hap.names.find(name=>name.endsWith('/rawfile/term/term.html'));assert(htmlName,'Terminal HTML missing');
  assert(!hap.read(htmlName).includes(Buffer.from('xterm-addon-webgl.js')),'Unused WebGL is still loaded');
  for(const name of ['mksh-COPYRIGHT.txt','mksh-terms.txt'])assert(hap.names.includes('resources/rawfile/licenses/'+name),'mksh license missing');
  return {dailyPackageVerified:true,bytes:fs.statSync(file).size,sha256:hash(fs.readFileSync(file)),nativeLibraries:names,runtime,...inventory,...sourceMaps,
    runtimeCodeIgnoredHeaderFields:['e_shoff','e_shnum','e_shstrndx'],
    diagnosticsExcluded:true,diagnosticArktsExcluded:true,ordinaryApplicationsExcluded:true,compilerDataExcluded:true,applicationEnvironmentInstallersExcluded:true,originalDownloadFixtureExcluded:true,unusedWebglNotLoaded:true,unusedWebglExcluded:true,developmentDocsExcluded:true,defaultShell:'mksh',otherShellComponentsExcluded:true};
}
module.exports={entries,code,inspectInventory,inspectSourceMaps,inspect,compareSigned};
if(require.main===module)console.log(JSON.stringify(inspect(process.argv[2],process.argv[3]),null,2));
