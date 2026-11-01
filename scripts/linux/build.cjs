const fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process');
const fixture=path.resolve(__dirname,'../../.test/linux-tools');
const sdk=process.argv[2]||process.env.OHOS_NATIVE_SDK||(process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME?path.join(process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME,'sdk/default/openharmony/native'):null);
if(!sdk)throw Error('Pass the native SDK directory or set OHOS_NATIVE_SDK');
const repo=path.resolve(__dirname,'../..');
const diagnostics=process.argv.includes('--diagnostics');
if(process.argv.slice(3).some(arg=>arg!=='--diagnostics'))throw Error('Unknown build option');
const {ensure,hash}=require('./source-runtime.cjs'),sourceRuntime=ensure(sdk);
const generation=path.join(fixture,'runtime-builds',new Date().toISOString().replace(/[:.]/g,'-')+'-'+process.pid);
const output=path.join(generation,'payloads');fs.mkdirSync(output,{recursive:true});
const clang=path.join(sdk,'llvm/bin/clang.exe'),source=path.join(repo,'tests/fixtures/linux_runtime.c');
function run(args){const r=cp.spawnSync(clang,args,{encoding:'utf8',windowsHide:true});if(r.status!==0)throw Error(r.stdout+r.stderr);if(r.stderr)console.log(r.stderr.trim());}
const musl=path.join(fixture,'musl'),glibc=path.join(fixture,'glibc');
const common=['-fuse-ld=lld','-nostdlib','-O2','-fPIE','-fno-emulated-tls','-fno-stack-protector','-mno-outline-atomics','-Wl,-z,max-page-size=4096'];
if(diagnostics)for(const mode of ['static','dynamic']) {
 const lib=path.join(musl,'usr/lib');const file=path.join(output,'libhish_linux_musl_'+mode+'.so');
 run(['--target=aarch64-linux-musl','--sysroot='+musl,...common,...(mode==='static'?['-static-pie']:['-pie','-Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1']),
  path.join(lib,mode==='static'?'rcrt1.o':'Scrt1.o'),path.join(lib,'crti.o'),source,'-I'+path.join(musl,'usr/include'),
  mode==='static'?path.join(lib,'libc.a'):path.join(musl,'lib/libc.musl-aarch64.so.1'),
  path.join(sdk,'llvm/lib/clang/15.0.4/lib/aarch64-linux-ohos/libclang_rt.builtins.a'),path.join(lib,'crtn.o'),'-o',file]);console.log('built '+path.basename(file));
}
// Portable benchmark: the same source compiles inside a container with plain cc.
// static = copy into any Linux container; dynamic = HAP tool with dlopen for GPU.
const bench=path.join(repo,'bench/hishbench.c'),muslLib=path.join(musl,'usr/lib');
if(diagnostics)for(const mode of ['static','dynamic']){
 run(['--target=aarch64-linux-musl','--sysroot='+musl,...common,...(mode==='static'?['-static-pie']:['-pie','-Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1']),
  path.join(muslLib,mode==='static'?'rcrt1.o':'Scrt1.o'),path.join(muslLib,'crti.o'),bench,'-I'+path.join(musl,'usr/include'),
  mode==='static'?path.join(muslLib,'libc.a'):path.join(musl,'lib/libc.musl-aarch64.so.1'),
  path.join(sdk,'llvm/lib/clang/15.0.4/lib/aarch64-linux-ohos/libclang_rt.builtins.a'),path.join(muslLib,'crtn.o'),
  '-o',path.join(output,mode==='static'?'libhish_linux_hishbench_static.so':'libhish_linux_hishbench.so')]);
 console.log('built libhish_linux_hishbench'+(mode==='static'?'_static':'')+'.so');
}
const lib=path.join(glibc,'usr/lib/aarch64-linux-gnu');
if(diagnostics)run(['--target=aarch64-linux-gnu','--sysroot='+glibc,...common,'-pie','-Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1',
 path.join(lib,'Scrt1.o'),path.join(lib,'crti.o'),source,'-I'+path.join(glibc,'usr/include'),'-I'+path.join(glibc,'usr/include/aarch64-linux-gnu'),
 '-idirafter',path.join(sdk,'sysroot/usr/include'),'-idirafter',path.join(sdk,'sysroot/usr/include/aarch64-linux-ohos'),
 path.join(lib,'libc.so.6'),path.join(lib,'libc_nonshared.a'),path.join(lib,'crtn.o'),'-o',path.join(output,'libhish_linux_glibc_dynamic.so')]);
if(diagnostics)console.log('built libhish_linux_glibc_dynamic.so');
if(diagnostics)run(['--target=aarch64-linux-gnu','--sysroot='+glibc,...common,'-static-pie',
 path.join(lib,'rcrt1.o'),path.join(lib,'crti.o'),source,'-I'+path.join(glibc,'usr/include'),'-I'+path.join(glibc,'usr/include/aarch64-linux-gnu'),
 '-idirafter',path.join(sdk,'sysroot/usr/include'),'-idirafter',path.join(sdk,'sysroot/usr/include/aarch64-linux-ohos'),'-Wl,--start-group',
 path.join(lib,'libc.a'),path.join(lib,'libc_nonshared.a'),path.join(sdk,'llvm/lib/clang/15.0.4/lib/aarch64-linux-ohos/libclang_rt.builtins.a'),
 path.join(sdk,'llvm/lib/aarch64-linux-ohos/libunwind.a'),'-Wl,--end-group',path.join(lib,'crtn.o'),'-o',path.join(output,'libhish_linux_glibc_static.so')]);
if(diagnostics)console.log('built libhish_linux_glibc_static.so');
for(const [from,to] of [[sourceRuntime.output,'libhish_linux_musl_loader.so'],
 [path.join(lib,'ld-linux-aarch64.so.1'),'libhish_linux_glibc_loader.so']])fs.copyFileSync(from,path.join(output,to));
const original=fs.readFileSync(path.join(lib,'libc.so.6'));let end=0;
for(let i=0;i<original.readUInt16LE(56);i++){const p=Number(original.readBigUInt64LE(32))+i*56;if(original.readUInt32LE(p)===1)end=Math.max(end,Number(original.readBigUInt64LE(p+16)+original.readBigUInt64LE(p+40)));}
const padding=path.join(generation,'file-extent-zero.bin');fs.writeFileSync(padding,Buffer.alloc((end+4095)&~4095));
const objcopy=path.join(sdk,'llvm/bin/llvm-objcopy.exe');
function padShared(input,dest){const r=cp.spawnSync(objcopy,['--add-section=.hish.file_extent='+padding,'--set-section-flags=.hish.file_extent=readonly',input,dest],{encoding:'utf8',windowsHide:true});if(r.status!==0)throw Error(r.stdout+r.stderr);}
padShared(path.join(lib,'libc.so.6'),path.join(generation,'libhish_linux_glibc_padded.so'));
// Preserve all code/data virtual addresses. Split the original combined RX LOAD
// into a read-only header LOAD and an RX tail, using an optional PT_NOTE slot.
// This tests the same separate-header layout produced by modern ELF linkers.
function separateHeader(input){
const separate=Buffer.from(input);
const phoff=Number(separate.readBigUInt64LE(32)),phnum=separate.readUInt16LE(56),headers=[];
for(let i=0;i<phnum;i++)headers.push(Buffer.from(separate.subarray(phoff+i*56,phoff+(i+1)*56)));
const first=headers.findIndex(h=>h.readUInt32LE(0)===1),note=headers.findIndex(h=>h.readUInt32LE(0)===4);
if(first<0||note<0||headers[first].readBigUInt64LE(8)!==0n||headers[first].readBigUInt64LE(16)!==0n||headers[first].readBigUInt64LE(32)<8192n)throw Error('unsupported glibc ELF split');
const head=Buffer.from(headers[first]),tail=Buffer.from(head);head.writeUInt32LE(4,4);head.writeBigUInt64LE(4096n,32);head.writeBigUInt64LE(4096n,40);
tail.writeBigUInt64LE(4096n,8);tail.writeBigUInt64LE(4096n,16);tail.writeBigUInt64LE(4096n,24);tail.writeBigUInt64LE(tail.readBigUInt64LE(32)-4096n,32);tail.writeBigUInt64LE(tail.readBigUInt64LE(40)-4096n,40);
const replacement=[];for(let i=0;i<headers.length;i++){if(i===first)replacement.push(head,tail);else if(i!==note)replacement.push(headers[i]);}
replacement.forEach((h,i)=>h.copy(separate,phoff+i*56));return separate;
}
fs.writeFileSync(path.join(generation,'libhish_linux_glibc_separate.so'),separateHeader(fs.readFileSync(path.join(generation,'libhish_linux_glibc_padded.so'))));
function cloneFallback(input,dest){
 const b=Buffer.from(fs.readFileSync(input));let count=0;
 const off=Number(b.readBigUInt64LE(32)),num=b.readUInt16LE(56);
 for(let p=0;p<num;p++){const h=off+p*56;if(b.readUInt32LE(h)!==1||!(b.readUInt32LE(h+4)&1))continue;const start=Number(b.readBigUInt64LE(h+8)),size=Number(b.readBigUInt64LE(h+32));
  for(let i=start;i+8<=start+size;i+=4){const op=b.readUInt32LE(i);if((op===0xd2803668||op===0x52803668)&&b.readUInt32LE(i+4)===0xd4000001){b.writeUInt32LE(0x928004a0,i+4);count++;}}
 }
 if(count!==1)throw Error('clone3 fallback pattern drift: '+count);fs.writeFileSync(dest,b);
}
if(diagnostics)cloneFallback(path.join(output,'libhish_linux_glibc_static.so'),path.join(output,'libhish_linux_glibc_static_compat.so'));
cloneFallback(path.join(generation,'libhish_linux_glibc_separate.so'),path.join(output,'libhish_linux_glibc_compat.so'));
// glibc's cancellation unwinder is dlopened by name. HAP native entries must
// end in .so, so adapt its single data string without moving any instruction.
const compatFile=path.join(output,'libhish_linux_glibc_compat.so'),compat=fs.readFileSync(compatFile),unwindName=Buffer.from('libgcc_s.so.1\0');
const unwindAt=compat.indexOf(unwindName);if(unwindAt<0||compat.indexOf(unwindName,unwindAt+1)>=0)throw Error('glibc unwinder name drift');
compat.fill(0,unwindAt,unwindAt+unwindName.length);compat.write('libgcc_s1.so',unwindAt);fs.writeFileSync(compatFile,compat);
const unwind=fs.readFileSync(path.join(lib,'libgcc_s.so.1'));let unwindExtent=unwind.length;
for(let i=0;i<unwind.readUInt16LE(56);i++){const p=Number(unwind.readBigUInt64LE(32))+i*56;if(unwind.readUInt32LE(p)===1)unwindExtent=Math.max(unwindExtent,Number(unwind.readBigUInt64LE(p+16)+unwind.readBigUInt64LE(p+40)));}
const unwindPadded=Buffer.alloc(Math.ceil(unwindExtent/4096)*4096);unwind.copy(unwindPadded);
fs.writeFileSync(path.join(output,'libgcc_s1.so'),separateHeader(unwindPadded));
const diagnosticFlags=diagnostics?['-DHISH_ENABLE_DIAGNOSTICS=1']:[];
run(['--target=aarch64-linux-musl','--sysroot='+musl,...diagnosticFlags,'-fuse-ld=lld','-nostdlib','-O2','-fPIC','-shared','-Wl,-z,max-page-size=4096',
 path.join(repo,'feature/hish_main/src/main/cpp/linux/identity.c'),path.join(repo,'feature/hish_main/src/main/cpp/linux/terminal.c'),path.join(repo,'feature/hish_main/src/main/cpp/linux/exec.c'),
 path.join(repo,'feature/hish_main/src/main/cpp/linux/links.c'),
  '-I'+path.join(repo,'feature/hish_main/src/main/cpp'),'-I'+path.join(musl,'usr/include'),path.join(musl,'lib/libc.musl-aarch64.so.1'),
   path.join(sdk,'llvm/lib/clang/15.0.4/lib/aarch64-linux-ohos/libclang_rt.builtins.a'),'-o',path.join(output,'libhish_linux_identity_padded.so')]);
run(['--target=aarch64-linux-gnu','--sysroot='+glibc,...diagnosticFlags,'-fuse-ld=lld','-nostdlib','-O2','-fPIC','-shared','-Wl,-z,max-page-size=4096',
 path.join(repo,'feature/hish_main/src/main/cpp/linux/terminal.c'),path.join(repo,'feature/hish_main/src/main/cpp/linux/exec.c'),path.join(repo,'feature/hish_main/src/main/cpp/linux/links.c'),'-I'+path.join(repo,'feature/hish_main/src/main/cpp'),
 '-I'+path.join(glibc,'usr/include'),'-I'+path.join(glibc,'usr/include/aarch64-linux-gnu'),'-idirafter',path.join(sdk,'sysroot/usr/include'),'-idirafter',path.join(sdk,'sysroot/usr/include/aarch64-linux-ohos'),
  path.join(lib,'libc.so.6'),path.join(lib,'libc_nonshared.a'),path.join(sdk,'llvm/lib/clang/15.0.4/lib/aarch64-linux-ohos/libclang_rt.builtins.a'),'-o',path.join(output,'libhish_linux_glibc_tty.so')]);
if(diagnostics)for(const family of ['musl','glibc']){
 const root=family==='musl'?musl:glibc,libraries=family==='musl'?path.join(musl,'usr/lib'):lib;
 run(['--target=aarch64-linux-'+(family==='musl'?'musl':'gnu'),'--sysroot='+root,...common,'-pie','-Wl,--dynamic-linker='+(family==='musl'?'/lib/ld-musl-aarch64.so.1':'/lib/ld-linux-aarch64.so.1'),
  path.join(libraries,'Scrt1.o'),path.join(libraries,'crti.o'),path.join(repo,'tests/fixtures/linux_tty.c'),'-I'+path.join(root,'usr/include'),
  ...(family==='glibc'?['-I'+path.join(root,'usr/include/aarch64-linux-gnu'),'-idirafter',path.join(sdk,'sysroot/usr/include'),'-idirafter',path.join(sdk,'sysroot/usr/include/aarch64-linux-ohos')]:[]),
  family==='musl'?path.join(musl,'lib/libc.musl-aarch64.so.1'):path.join(lib,'libc.so.6'),path.join(libraries,'crtn.o'),'-o',path.join(output,family==='musl'?'libhish_linux_tty_check.so':'libhish_linux_glibc_tty_check.so')]);
}
console.log('payload directory '+output);
// Keep the former application recipes as explicit diagnostic fixtures only.
// A daily build owns the platform runtime, never ordinary application versions.
if(diagnostics){
for(const [name,file] of [['printf','libhish_linux_gnu_printf.so'],['uname','libhish_linux_gnu_uname.so']])fs.copyFileSync(path.join(glibc,'usr/bin',name),path.join(output,file));

// HAP native-library entries must end in .so. Keep DT_NEEDED/SONAME consistent
// with those filenames, without changing instructions, addresses or string sizes.
function dynamicStrings(bytes){
 const off=Number(bytes.readBigUInt64LE(32)),count=bytes.readUInt16LE(56),loads=[];let dynamic;
 for(let i=0;i<count;i++){const h=off+i*56,type=bytes.readUInt32LE(h);
  if(type===1)loads.push({offset:Number(bytes.readBigUInt64LE(h+8)),address:Number(bytes.readBigUInt64LE(h+16)),size:Number(bytes.readBigUInt64LE(h+32))});
  if(type===2)dynamic={offset:Number(bytes.readBigUInt64LE(h+8)),size:Number(bytes.readBigUInt64LE(h+32))};
 }
 if(!dynamic)throw Error('missing ELF dynamic table');let strings;const entries=[];
 for(let p=dynamic.offset;p<dynamic.offset+dynamic.size;p+=16){const tag=Number(bytes.readBigUInt64LE(p)),value=Number(bytes.readBigUInt64LE(p+8));if(!tag)break;if(tag===5)strings=value;if(tag===1||tag===14)entries.push({tag,value});}
 const segment=loads.find(l=>strings>=l.address&&strings<l.address+l.size);if(!segment)throw Error('missing ELF string table');
 const base=segment.offset+strings-segment.address;
 return entries.map(e=>{const at=base+e.value,end=bytes.indexOf(0,at);if(end<0)throw Error('unterminated dynamic string');return{...e,at,name:bytes.toString('utf8',at,end)};});
}
const rename=name=>name.replace(/\.so\.(\d+)$/, '$1.so');
const networkPrograms=[['usr/bin/curl','libhish_linux_curl.so'],['usr/bin/git','libhish_linux_git.so'],
 ['usr/libexec/git-core/git-remote-http','libhish_linux_git_remote_http.so'],['usr/libexec/git-core/git-http-fetch','libhish_linux_git_http_fetch.so'],
 ['usr/libexec/git-core/git-http-push','libhish_linux_git_http_push.so']];
const pending=networkPrograms.map(([file,dest])=>({file:path.join(musl,file),dest})),seen=new Map();
while(pending.length){const {file,dest}=pending.shift();if(seen.has(dest)){if(seen.get(dest)!==file)throw Error('dependency filename collision '+dest);continue;}seen.set(dest,file);const bytes=fs.readFileSync(file);
 for(const entry of dynamicStrings(bytes)){
  if(entry.name==='libc.musl-aarch64.so.1')continue;
  if(!/^lib[\w+.-]+\.so(?:\.\d+)?$/.test(entry.name))throw Error('unsupported native dependency '+entry.name);
  const replacement=rename(entry.name);if(replacement.length>entry.name.length)throw Error('dependency name grows');
  bytes.fill(0,entry.at,entry.at+entry.name.length);bytes.write(replacement,entry.at,'utf8');
  if(entry.tag===1)pending.push({file:path.join(musl,'usr/lib',entry.name),dest:replacement});
 }
  fs.writeFileSync(path.join(output,dest),bytes);console.log('packaged '+dest);
}
// musl's mapping/scheduler/spawn changes now come from the verified source
// build. No musl binary offsets, overlapping headers or zero padding patches.
fs.copyFileSync(path.join(musl,'etc/ssl/cert.pem'),path.join(output,'ca-bundle.pem'));
// Data-only compiler sysroot. Executable code still comes from signed HAP
// libraries; extracting headers, CRT objects and archives grants no RX rights.
const compilerFiles=new Map();
function compilerTree(from,to){for(const entry of fs.readdirSync(from,{withFileTypes:true})){const source=path.join(from,entry.name),dest=to+'/'+entry.name;if(entry.isDirectory())compilerTree(source,dest);else if(entry.isFile())compilerFiles.set(dest,fs.readFileSync(source));}}
compilerTree(path.join(musl,'usr/include'),'include');
compilerTree(path.join(musl,'usr/lib/tcc'),'lib');
for(const file of ['crt1.o','Scrt1.o','crti.o','crtn.o','libc.a'])compilerFiles.set('lib/'+file,fs.readFileSync(path.join(musl,'usr/lib',file)));
compilerFiles.set('lib/libc.so',fs.readFileSync(path.join(musl,'lib/libc.musl-aarch64.so.1')));
const records=[];for(const [name,data] of [...compilerFiles].sort(([a],[b])=>a.localeCompare(b))){const text=Buffer.from(name),header=Buffer.alloc(8);header.writeUInt32LE(text.length);header.writeUInt32LE(data.length,4);records.push(header,text,data);}
const compilerData=Buffer.concat(records),compilerSize=Buffer.alloc(4);compilerSize.writeUInt32LE(compilerData.length);
fs.writeFileSync(path.join(output,'compiler-data.bin'),Buffer.concat([compilerSize,require('node:zlib').deflateSync(compilerData)]));
console.log('packaged compiler sysroot: '+compilerFiles.size+' files');
const tccSource=path.join(fixture,'tcc-source/tinycc-5fad6dc4a3cdb7c9094129a4c76e5d2599e94414');
fs.writeFileSync(path.join(tccSource,'config.h'),[
 '#define TCC_VERSION "0.9.28rc-hish"','#define CONFIG_TCC_MUSL 1','#define CONFIG_TCC_BACKTRACE 0','#define CONFIG_TCC_BCHECK 0',
 '#define CONFIG_TCC_CRTPREFIX "{B}"','#define CONFIG_TCC_LIBPATHS "{B}"',
 '#define CONFIG_TCC_SYSINCLUDEPATHS "{B}/include:{B}/../include"',
 '#define CONFIG_TCC_ELFINTERP "/lib/ld-musl-aarch64.so.1"',''].join('\n'));
run(['--target=aarch64-linux-musl','--sysroot='+musl,...common,'-pie','-Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1',
 path.join(musl,'usr/lib/Scrt1.o'),path.join(musl,'usr/lib/crti.o'),path.join(repo,'feature/hish_main/src/main/cpp/linux/tcc_main.c'),
 '-I'+tccSource,'-I'+path.join(musl,'usr/include'),path.join(musl,'lib/libc.musl-aarch64.so.1'),
 path.join(sdk,'llvm/lib/clang/15.0.4/lib/aarch64-linux-ohos/libclang_rt.builtins.a'),path.join(musl,'usr/lib/crtn.o'),'-o',path.join(output,'libhish_linux_tcc.so')]);
console.log('built native relocatable TinyCC from pinned source');
}
const cpp=path.join(repo,'feature/hish_main/src/main/cpp');
const inputs=[...sourceRuntime.inputs.map(([name])=>path.join(cpp,name)),
 ...['linux/identity.c','linux/terminal.c','linux/exec.c','linux/links.c','hish_link_core.h','hish_links_impl.h','hish_links_libc.h','native_tools.h','linux_library_path.h','elf_execution_access.h','process_identity.h','shell_script.h','exec_bridge_shared.h','hish_tty_shared.h',...(diagnostics?['linux/tcc_main.c']:[])].map(name=>path.join(cpp,name)),__filename,path.join(__dirname,'source-runtime.cjs'),path.join(__dirname,'../daily-package-policy.json')];
if(!diagnostics){
 const expected=require('../daily-package-policy.json').arm64Runtime;
 const actual=fs.readdirSync(output).filter(name=>name.endsWith('.so')).sort();
 require('node:assert/strict').deepEqual(actual,[...expected].sort(),'Base runtime must not include application packages');
}
const manifest={format:1,diagnostics,sourceManifest:sourceRuntime.file,sourceSha256:sourceRuntime.sha256,
 inputs:inputs.map(file=>({file,sha256:hash(fs.readFileSync(file))})),
 files:fs.readdirSync(output).sort().map(name=>({name,sha256:hash(fs.readFileSync(path.join(output,name)))}))};
fs.writeFileSync(path.join(output,'runtime-manifest.json'),JSON.stringify(manifest,null,2)+'\n');
fs.copyFileSync(path.join(path.dirname(sourceRuntime.file),'COPYRIGHT'),path.join(generation,'musl-COPYRIGHT'));
// Publish only after every compilation and manifest has succeeded. Old baseline
// payloads and every earlier generation are retained untouched.
const pointer=path.join(fixture,diagnostics?'diagnostics-current.cmake':'runtime-current.cmake');
const temporary=pointer+'.'+process.pid+'.tmp';
fs.writeFileSync(temporary,'set(HISH_LINUX_PAYLOAD_DIR [=['+output.replace(/\\/g,'/')+']=])\n');
fs.renameSync(temporary,pointer);
console.log(JSON.stringify({payloads:output,manifest:path.join(output,'runtime-manifest.json'),diagnostics,sourceSha256:sourceRuntime.sha256},null,2));
