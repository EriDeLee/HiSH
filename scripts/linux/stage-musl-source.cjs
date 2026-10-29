// Separate candidate payload tree. Never edits the baseline payloads or downloads.
const fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process'),crypto=require('node:crypto'),assert=require('node:assert/strict');
const repo=path.resolve(__dirname,'../..'),fixture=path.join(repo,'.test/linux-tools'),cpp=path.join(repo,'feature/hish_main/src/main/cpp');
const hash=b=>crypto.createHash('sha256').update(b).digest('hex');
function dynamicStrings(bytes){
 const off=Number(bytes.readBigUInt64LE(32)),count=bytes.readUInt16LE(56),loads=[];let dynamic;
 for(let i=0;i<count;i++){const h=off+i*56,type=bytes.readUInt32LE(h);if(type===1)loads.push({offset:Number(bytes.readBigUInt64LE(h+8)),address:Number(bytes.readBigUInt64LE(h+16)),size:Number(bytes.readBigUInt64LE(h+32))});if(type===2)dynamic={offset:Number(bytes.readBigUInt64LE(h+8)),size:Number(bytes.readBigUInt64LE(h+32))};}
 assert(dynamic,'ELF dynamic table');let strings;const entries=[];
 for(let p=dynamic.offset;p<dynamic.offset+dynamic.size;p+=16){const tag=Number(bytes.readBigUInt64LE(p)),value=Number(bytes.readBigUInt64LE(p+8));if(!tag)break;if(tag===5)strings=value;if(tag===1||tag===14)entries.push({tag,value});}
 const segment=loads.find(l=>strings>=l.address&&strings<l.address+l.size);assert(segment,'ELF string table');const base=segment.offset+strings-segment.address;
 return entries.map(e=>{const at=base+e.value,end=bytes.indexOf(0,at);assert(end>=0,'ELF dynamic string');return{...e,at,name:bytes.toString('utf8',at,end)};});
}
function stage(manifestFile,sdk,directory){
 assert(manifestFile&&sdk&&directory,'Usage: stage-musl-source.cjs MANIFEST NATIVE_SDK NEW_OUTPUT_DIR');
 manifestFile=path.resolve(manifestFile);sdk=path.resolve(sdk);directory=path.resolve(directory);
 assert(directory.startsWith(path.join(fixture,'musl-candidates')+path.sep),'Candidate must stay in .test/linux-tools/musl-candidates');
 assert(!fs.existsSync(directory),'Do not overwrite candidate evidence');
 const source=JSON.parse(fs.readFileSync(manifestFile,'utf8'));assert.equal(source.status,'built');assert.equal(hash(fs.readFileSync(source.output)),source.sha256);
 for(const [name,digest] of source.inputs)assert.equal(hash(fs.readFileSync(path.join(cpp,name))),digest,'Source build stale: '+name);
 fs.mkdirSync(directory,{recursive:true});const payloads=path.join(directory,'payloads');fs.mkdirSync(payloads);
 const result={sourceManifest:manifestFile,sourceSha256:source.sha256,startedAt:new Date().toISOString(),baselinePayloadsModified:false,phoneTouched:false,status:'staging'};
 try{
  for(const entry of fs.readdirSync(path.join(fixture,'payloads'),{withFileTypes:true}))if(entry.isFile())fs.copyFileSync(path.join(fixture,'payloads',entry.name),path.join(payloads,entry.name));
  fs.copyFileSync(source.output,path.join(payloads,'libhish_linux_musl_loader.so'));
  fs.copyFileSync(path.join(path.dirname(manifestFile),'COPYRIGHT'),path.join(directory,'musl-COPYRIGHT'));
  const clang=path.join(sdk,'llvm/bin/clang.exe'),musl=path.join(fixture,'musl'),glibc=path.join(fixture,'glibc'),lib=path.join(glibc,'usr/lib/aarch64-linux-gnu');
  const builtins=path.join(sdk,'llvm/lib/clang/15.0.4/lib/aarch64-linux-ohos/libclang_rt.builtins.a');
  const common=['-DHISH_ENABLE_DIAGNOSTICS=1','-fuse-ld=lld','-nostdlib','-O2','-fPIC','-shared','-Wl,-z,max-page-size=4096'];
  function run(args){const r=cp.spawnSync(clang,args,{cwd:directory,encoding:'utf8',windowsHide:true,maxBuffer:8*1024*1024});fs.appendFileSync(path.join(directory,'compile.log'),JSON.stringify({executable:clang,args})+'\n'+(r.stdout||'')+(r.stderr||''));if(r.error)throw r.error;assert.equal(r.status,0,'Candidate compilation failed; see compile.log');}
  const muslLib=path.join(musl,'usr/lib');
  run(['--target=aarch64-linux-musl','--sysroot='+musl,'-fuse-ld=lld','-nostdlib','-O2','-fPIE','-fno-stack-protector','-mno-outline-atomics','-pie','-Wl,-z,max-page-size=4096','-Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1',path.join(muslLib,'Scrt1.o'),path.join(muslLib,'crti.o'),path.join(repo,'tests/fixtures/linux_fs_identity.c'),'-I'+path.join(musl,'usr/include'),path.join(musl,'lib/libc.musl-aarch64.so.1'),builtins,path.join(muslLib,'crtn.o'),'-o',path.join(payloads,'libhish_linux_fs_identity.so')]);
  run(['--target=aarch64-linux-musl','--sysroot='+musl,...common,path.join(cpp,'linux/identity.c'),path.join(cpp,'linux/terminal.c'),path.join(cpp,'linux/exec.c'),'-I'+cpp,'-I'+path.join(musl,'usr/include'),path.join(musl,'lib/libc.musl-aarch64.so.1'),builtins,'-o',path.join(payloads,'libhish_linux_identity.so')]);
  // The filename is retained for existing launch code; the new source loader no
  // longer needs to pad or split this DSO's ELF headers to work around file RX.
  fs.copyFileSync(path.join(payloads,'libhish_linux_identity.so'),path.join(payloads,'libhish_linux_identity_padded.so'));
  run(['--target=aarch64-linux-gnu','--sysroot='+glibc,...common,path.join(cpp,'linux/terminal.c'),path.join(cpp,'linux/exec.c'),'-I'+cpp,'-I'+path.join(glibc,'usr/include'),'-I'+path.join(glibc,'usr/include/aarch64-linux-gnu'),'-idirafter',path.join(sdk,'sysroot/usr/include'),'-idirafter',path.join(sdk,'sysroot/usr/include/aarch64-linux-ohos'),path.join(lib,'libc.so.6'),path.join(lib,'libc_nonshared.a'),builtins,'-o',path.join(payloads,'libhish_linux_glibc_tty.so')]);
  // Only bundled network tools retain the existing HAP-required SONAME rename.
  // Use their original LOAD layout, not the old overlapping R/RX-header trick.
  // This does NOT touch any original-download acceptance file or data blob.
  const queue=[['usr/bin/curl','libhish_linux_curl.so'],['usr/bin/git','libhish_linux_git.so'],['usr/libexec/git-core/git-remote-http','libhish_linux_git_remote_http.so'],['usr/libexec/git-core/git-http-fetch','libhish_linux_git_http_fetch.so'],['usr/libexec/git-core/git-http-push','libhish_linux_git_http_push.so']].map(([name,dest])=>({name,dest}));
  const seen=new Map();while(queue.length){const {name,dest}=queue.shift();if(seen.has(dest)){assert.equal(seen.get(dest),name);continue;}seen.set(dest,name);
   const bytes=fs.readFileSync(path.join(musl,name));for(const entry of dynamicStrings(bytes)){
    if(entry.name==='libc.musl-aarch64.so.1')continue;assert(/^lib[\w+.-]+\.so(?:\.\d+)?$/.test(entry.name),'Unsupported dependency');
    const renamed=entry.name.replace(/\.so\.(\d+)$/,'$1.so');assert(renamed.length<=entry.name.length);
    bytes.fill(0,entry.at,entry.at+entry.name.length);bytes.write(renamed,entry.at,'utf8');if(entry.tag===1)queue.push({name:'usr/lib/'+entry.name,dest:renamed});
   }
   fs.writeFileSync(path.join(payloads,dest),bytes);
  }
  result.payloads=payloads;result.files=fs.readdirSync(payloads).sort().map(name=>({name,sha256:hash(fs.readFileSync(path.join(payloads,name)))}));result.status='staged';
 }catch(error){result.status='failed';result.error=error.message;throw error;}
 finally{result.endedAt=new Date().toISOString();fs.writeFileSync(path.join(directory,'manifest.json'),JSON.stringify(result,null,2)+'\n');}
 console.log(JSON.stringify({status:result.status,payloads,manifest:path.join(directory,'manifest.json'),baselinePayloadsModified:false,phoneTouched:false},null,2));return result;
}
module.exports={dynamicStrings,stage};
if(require.main===module)try{stage(...process.argv.slice(2));}catch(error){console.error(error.stack);process.exitCode=1;}
