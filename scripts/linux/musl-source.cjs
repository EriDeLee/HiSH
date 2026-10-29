// Native-Windows source build: no configure/Makefile/package scripts are run.
// Invoke explicitly. This does NOT replace payloads, build a HAP or touch a phone.
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto'),zlib=require('node:zlib'),cp=require('node:child_process'),assert=require('node:assert/strict');
const repo=path.resolve(__dirname,'../..'),fixture=path.join(repo,'.test/linux-tools');
const cpp=path.join(repo,'feature/hish_main/src/main/cpp');
const commit='8aef0c37b0ad23dc4137f0e4755b97a59dc698b8';
const patches=[
 ['handle-aux-at_base.patch','a76f79b801497ad994746cf82bb6eaf86f9e1ae646e6819fbae8532a7f4eee53a96ac1d4e789ec8f66aea2a68027b0597f7a579b3369e01258da8accfce41370'],
 ['isatty-glibc-align.patch','4fe4309d9e5eb12f3aaf9dac300c7bf1a9e104d9b0e7754353314f4b6507de68d8d5bf0ac45164d7e2ab4d71728b5ef8b302f8ee7a6b548f54e022218031e2cf'],
 ['ldso-fix-non-functional-fix-to-early-dynamic-PAGE_SI.patch','1b85079c706b017106ecbf4afbc65b4a2171297a673035645e4c3ddb7e2aae0549b04932df83778323b7329ae8d32160a89e0c1b2534c278708c61b1f40a1ed8'],
 ['CVE-2025-26519.patch','7a6a9836d2de91afc1115868e68f347bd2365fa48188e65938cfa654ae9bafdbb3a56bf12d3185a96800a85198378c8dbf9c25d977ca0e318220529fa4458123'],
 ['stdio-skip-empty-iovec-when-buffering-is-disabled.patch','a70068d2a16b64188f04fccd180585b025aa8bccd4e823d8fdea0be1a80ee8b74cb485ecd22e72b5ce3f141526aca563b6c69e1389c0340fad15c9b9bee6885c'],
 ['backport-posix_getdents.patch','703982a7624a377037c28cfd572cf22b64ee57e362a60975c91caa9c85fd1edde1f82933e91fb44425976178c59f50f5ebb651216f96c05c0f755e205e578eb0'],
 ['0001-add-stub-for-pthread_mutexattr_setprioceiling.patch','83ec0f774dbb5d4f4d4b917d149ea0ef609d028d18ae7624e90fedda6e3174c91ade93751bfd376adfa2848155110fc4dd02b6790363b98e5395c4787e8731e8'],
 ['iconv-gb18030-fix.patch','f7849abeab0e4eab992a80464afa07b9c8aae5cee76040523b9dbc0931435f28ea8f5792b8a4a0cd6d608ac2f37e3225e89afcb3171a0a6df44450ed57cee83b'],
 ['CVE-2026-40200.patch','a64ab7688d1a85e560b5687783df482d2467a79a74400da5a1601382847d6b4e6a79b7529a8dc80c46ddda8dc2a812f2261557fc8ee98dc3bdf7322761bd6d9c'],
];
const archive={name:'musl-1.2.5.tar.gz',url:'https://musl.libc.org/releases/musl-1.2.5.tar.gz',sha512:'7bb7f7833923cd69c7a1a9b8a5f1784bfd5289663eb6061dcd43d583e45987df8a68a1be05d75cc1c88a3f5b610653d1a70f4a9cff4d8f7fd41ae73ee058c17c'};
const sha=(bytes,algorithm='sha256')=>crypto.createHash(algorithm).update(bytes).digest('hex');
const text=file=>fs.readFileSync(file,'utf8').replace(/\r\n/g,'\n');
const write=(file,bytes)=>{fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,bytes);};
function replaceOnce(source,before,after){assert(source.includes(before),'Missing source anchor: '+before.slice(0,80));assert.equal(source.indexOf(before),source.lastIndexOf(before),'Ambiguous source anchor');return source.replace(before,after);}
function safeName(name){assert(name&&!name.startsWith('/')&&!name.includes('\\')&&!name.includes('\0')&&!name.split('/').some(p=>p==='..'||p.includes(':')),'Unsafe relative path');return name;}
function extract(bytes,root){
 const tar=zlib.gunzipSync(bytes,{maxOutputLength:64*1024*1024});
 for(let at=0;at+512<=tar.length;){const header=tar.subarray(at,at+512);at+=512;if(header.every(b=>b===0))continue;
  const field=(a,n)=>header.subarray(a,a+n).toString('utf8').split('\0')[0];
  let sum=0;for(let i=0;i<512;i++)sum+=i>=148&&i<156?32:header[i];assert.equal(sum,parseInt(field(148,8).trim(),8),'tar header checksum');
  const prefix=field(345,155),name=(prefix?prefix+'/':'')+field(0,100),size=parseInt(field(124,12).trim()||'0',8),type=field(156,1);
  assert(Number.isSafeInteger(size)&&size>=0&&at+size<=tar.length,'tar bounds');
  if(type==='g'){
   const metadata=tar.subarray(at,at+size).toString('utf8');
   for(let p=0;p<metadata.length;){const space=metadata.indexOf(' ',p),length=Number(metadata.slice(p,space));assert(space>p&&Number.isSafeInteger(length)&&length>0&&p+length<=metadata.length,'PAX bounds');const entry=metadata.slice(space+1,p+length);assert(/^(comment|mtime|atime|ctime)=/.test(entry)&&entry.endsWith('\n'),'No PAX path/size overrides');p+=length;}
   at+=Math.ceil(size/512)*512;continue;
  }
  assert(name.startsWith('musl-1.2.5/'),'Unexpected archive root: '+name);
  const relative=name.slice('musl-1.2.5/'.length);
  if(relative&&type!=='5'){safeName(relative);assert(type==='0'||type==='','No archive links/special files');const file=path.join(root,relative);assert(!fs.existsSync(file),'Duplicate archive member');write(file,tar.subarray(at,at+size));}
  at+=Math.ceil(size/512)*512;
 }
}
// Exact unified hunks, no fuzzy context removal, shell invocation or patch hooks.
function applyPatch(root,patch){
 const lines=patch.replace(/\r\n/g,'\n').split('\n');let current=null,contents=null,delta=0,files=0;
 const flush=()=>{if(current!==null){write(path.join(root,current),contents.join('\n'));current=null;}};
 for(let i=0;i<lines.length;i++){
  if(lines[i].startsWith('--- ')&&lines[i+1]?.startsWith('+++ ')){
   flush();const old=lines[i].slice(4).split(/\s/)[0],next=lines[++i].slice(4).split(/\s/)[0];
   assert(next.startsWith('b/'),'No deletion or arbitrary patch target');current=safeName(next.slice(2));
   if(old==='/dev/null'){assert(!fs.existsSync(path.join(root,current)),'New patch file exists');contents=[''];}
   else {assert.equal(old,'a/'+current,'No patch rename');contents=text(path.join(root,current)).split('\n');}
   delta=0;files++;continue;
  }
  const hunk=lines[i].match(/^@@ -(\d+)(?:,(\d+))? \+(\d+)(?:,(\d+))? @@/);if(!hunk)continue;assert(current!==null,'Hunk without file');
  const oldCount=hunk[2]===undefined?1:Number(hunk[2]),newCount=hunk[4]===undefined?1:Number(hunk[4]);
  const before=[],after=[];let oldUsed=0,newUsed=0;
  while(oldUsed<oldCount||newUsed<newCount){const line=lines[++i];assert(line!==undefined,'Truncated hunk');
   if(line[0]===' '){before.push(line.slice(1));after.push(line.slice(1));oldUsed++;newUsed++;}
   else if(line[0]==='-'){before.push(line.slice(1));oldUsed++;}
   else if(line[0]==='+'){after.push(line.slice(1));newUsed++;}
   else throw Error('Bad hunk line '+line);
  }
  assert.equal(oldUsed,oldCount);assert.equal(newUsed,newCount);
  let at=(Number(hunk[1])||1)-1+delta;
  const matches=pos=>before.every((line,n)=>contents[pos+n]===line);
  if(!matches(at)){const candidates=[];for(let p=0;p<=contents.length-before.length;p++)if(matches(p))candidates.push(p);assert.equal(candidates.length,1,'Patch context mismatch: '+current);at=candidates[0];}
  contents.splice(at,oldCount,...after);delta+=newCount-oldCount;
 }
 flush();assert(files,'No files patched');
}
function sourceChanges(root){
 const file=path.join(root,'ldso/dynlink.c');let source=text(file);
 source=replaceOnce(source,'#include "dynlink.h"','#include "dynlink.h"\n#include "linux/musl_dso_mapping.h"');
 source=replaceOnce(source,'\tphsize = eh->e_phentsize * eh->e_phnum;','\tif (HishMuslDSOHeader(fd, eh)) goto error;\n\tphsize = eh->e_phentsize * eh->e_phnum;');
 const begin=source.indexOf('\taddr_max += PAGE_SIZE-1;'),end=source.indexOf('done_mapping:',begin);assert(begin>=0&&end>begin);
 source=source.slice(0,begin)+[
  '\tif (HishMuslMapDSO(fd, eh, ph0, PAGE_SIZE, &map, &map_len, &base)) goto error;',
  '\tdso->map = map;', '\tdso->map_len = map_len;', '\tdso->phdr = 0;', '\tdso->phnum = 0;',
  '\tfor (ph=ph0, i=eh->e_phnum; i; i--, ph++) {',
  '\t\tif (ph->p_type != PT_LOAD || !ph->p_memsz) continue;',
  '\t\tif (!dso->phdr && eh->e_phoff >= ph->p_offset &&',
  '\t\t    eh->e_phoff-ph->p_offset <= ph->p_filesz &&',
  '\t\t    phsize <= ph->p_filesz-(eh->e_phoff-ph->p_offset)) {',
  '\t\t\tdso->phdr = (void *)(base + ph->p_vaddr + (eh->e_phoff-ph->p_offset));',
  '\t\t\tdso->phnum = eh->e_phnum;', '\t\t\tdso->phentsize = eh->e_phentsize;', '\t\t}', '\t}',
  '\tif (!dso->phdr) { errno = ENOEXEC; goto error; }','',
 ].join('\n')+source.slice(end);
 source=replaceOnce(source,'if (!sys_path) sys_path = "/lib:/usr/local/lib:/usr/lib";','if (!sys_path) sys_path = ""; /* Never search OHOS system libraries. */');
 write(file,source);
 for(const [name,args] of [['sched_getparam','pid, param'],['sched_getscheduler','pid'],['sched_setparam','pid, param'],['sched_setscheduler','pid, sched, param']]){
  const file=path.join(root,'src/sched',name+'.c');write(file,replaceOnce(text(file),'return __syscall_ret(-ENOSYS);','return syscall(SYS_'+name+', '+args+');'));
 }
 const attrs=path.join(root,'src/process/posix_spawnattr_sched.c');assert.equal((text(attrs).match(/return ENOSYS;/g)||[]).length,4,'spawnattr source drift');
 write(attrs,text(path.join(cpp,'linux/musl_spawnattr.c')));
}
function generatedHeaders(root,build){
 const inputs=['arch/aarch64/bits/alltypes.h.in','include/alltypes.h.in'];
 let all=inputs.map(file=>text(path.join(root,file))).join('');
 all=all.split('\n').map(line=>{let m=line.match(/^TYPEDEF (.*) ([^ ]*);$/);if(m)return '#if defined(__NEED_'+m[2]+') && !defined(__DEFINED_'+m[2]+')\ntypedef '+m[1]+' '+m[2]+';\n#define __DEFINED_'+m[2]+'\n#endif\n';
  m=line.match(/^(STRUCT|UNION) * ([^ ]*) (.*);$/);if(m){const kind=m[1].toLowerCase(),name=kind+'_'+m[2];return '#if defined(__NEED_'+name+') && !defined(__DEFINED_'+name+')\n'+kind+' '+m[2]+' '+m[3]+';\n#define __DEFINED_'+name+'\n#endif\n';}return line;
 }).join('\n');
 write(path.join(build,'include/bits/alltypes.h'),all);
 const syscall=text(path.join(root,'arch/aarch64/bits/syscall.h.in'));
 write(path.join(build,'include/bits/syscall.h'),syscall+syscall.split('\n').filter(line=>line.includes('__NR_')).map(line=>line.replace('__NR_','SYS_')).join('\n')+'\n');
 write(path.join(build,'internal/version.h'),'#define VERSION "1.2.5-hish-source1"\n');
}
function sourceList(root){
 const dirs=fs.readdirSync(path.join(root,'src'),{withFileTypes:true}).filter(e=>e.isDirectory()).map(e=>'src/'+e.name).concat(['src/malloc/mallocng','ldso']);
 const files=new Map();for(const dir of dirs){
  for(const entry of fs.readdirSync(path.join(root,dir),{withFileTypes:true}))if(entry.isFile()&&entry.name.endsWith('.c'))files.set(dir+'/'+entry.name.slice(0,-2),dir+'/'+entry.name);
  const arch=path.join(root,dir,'aarch64');if(fs.existsSync(arch))for(const entry of fs.readdirSync(arch,{withFileTypes:true}))if(entry.isFile()&&/\.[csS]$/.test(entry.name))files.set(dir+'/'+entry.name.slice(0,-2),dir+'/aarch64/'+entry.name);
 }
 return [...files.values()].sort();
}
function command(executable,args,cwd,log){
 fs.appendFileSync(log,JSON.stringify({executable,args})+'\n');
 const result=cp.spawnSync(executable,args,{cwd,windowsHide:true,encoding:'utf8',maxBuffer:8*1024*1024});
 if(result.stdout||result.stderr)fs.appendFileSync(log,result.stdout+result.stderr);
 if(result.error)throw result.error;assert.equal(result.status,0,path.basename(executable)+' failed; see '+log);
 return result.stdout;
}
async function download(cache,item){
 const file=path.join(cache,item.name);if(!fs.existsSync(file)){
  const response=await fetch(item.url);assert(response.ok,item.name+' HTTP '+response.status);
  const bytes=Buffer.from(await response.arrayBuffer());assert.equal(sha(bytes,'sha512'),item.sha512,item.name+' checksum');write(file,bytes);
 }
 const bytes=fs.readFileSync(file);assert.equal(sha(bytes,'sha512'),item.sha512,item.name+' cached checksum');return bytes;
}
async function build(sdk){
 assert(sdk,'Pass the native SDK directory');sdk=path.resolve(sdk);
 const clang=path.join(sdk,'llvm/bin/clang.exe'),readelf=path.join(sdk,'llvm/bin/llvm-readelf.exe');
 assert(fs.existsSync(clang)&&fs.existsSync(readelf),'Missing existing LLVM tools');
 const inputs=['linux/musl_dso_mapping.h','elf_code_mapping.h','linux/musl_spawnattr.c'].map(name=>[name,sha(fs.readFileSync(path.join(cpp,name)))]);
 const fingerprint=sha(JSON.stringify({commit,patches,archive,inputs,script:sha(fs.readFileSync(__filename))}));
 const cache=path.join(fixture,'musl-source-cache'),directory=path.join(fixture,'musl-source-build',fingerprint.slice(0,20));
 assert(!fs.existsSync(directory),'Existing build/evidence directory; do not overwrite: '+directory);
 fs.mkdirSync(directory,{recursive:true});fs.mkdirSync(cache,{recursive:true});
 const root=path.join(directory,'source'),out=path.join(directory,'build'),log=path.join(directory,'commands.log');
 const manifest={fingerprint,commit,archive,patches,inputs,startedAt:new Date().toISOString(),status:'preparing',sourceOnly:true,phoneTouched:false};
 try{
  extract(await download(cache,archive),root);
  for(const [name,sha512] of patches){const bytes=await download(cache,{name,sha512,url:'https://raw.githubusercontent.com/alpinelinux/aports/'+commit+'/main/musl/'+name});applyPatch(root,bytes.toString('utf8'));}
  sourceChanges(root);generatedHeaders(root,out);
  const files=sourceList(root);manifest.translationUnits=files.length;
  const flags=['--target=aarch64-linux-musl','-std=c99','-ffreestanding','-nostdinc','-D_XOPEN_SOURCE=700','-O2','-fPIC','-fno-stack-protector','-fno-strict-aliasing','-fno-unwind-tables','-fno-asynchronous-unwind-tables','-ffunction-sections','-fdata-sections','-mno-outline-atomics',
   '-I'+path.join(root,'arch/aarch64'),'-I'+path.join(root,'arch/generic'),'-I'+path.join(out,'internal'),'-I'+path.join(root,'src/include'),'-I'+path.join(root,'src/internal'),'-I'+path.join(out,'include'),'-I'+path.join(root,'include'),'-I'+cpp];
  const objects=[];for(const [index,file] of files.entries()){
   const object=path.join(out,'obj',file.replace(/\.[csS]$/,'.o'));fs.mkdirSync(path.dirname(object),{recursive:true});
   command(clang,[...flags,'-c',path.join(root,file),'-o',object],directory,log);objects.push(object);
   if(index%100===0)console.log('musl source '+index+'/'+files.length);
  }
  const output=path.join(out,'libhish_linux_musl_loader.so');
  const builtins=path.join(sdk,'llvm/lib/clang/15.0.4/lib/aarch64-linux-ohos/libclang_rt.builtins.a');assert(fs.existsSync(builtins),'Missing existing compiler runtime');
  const link=[...flags,'-fuse-ld=lld','-nostdlib','-shared','-Wl,-e,_dlstart','-Wl,-soname,libc.musl-aarch64.so.1','-Wl,-z,max-page-size=4096','-Wl,--hash-style=both','-Wl,--gc-sections','-Wl,--no-undefined','-Wl,--exclude-libs=ALL','-Wl,--dynamic-list='+path.join(root,'dynamic.list'),...objects,builtins,'-o',output];
  const response=path.join(out,'link.rsp');write(response,link.map(arg=>JSON.stringify(arg.replace(/\\/g,'/'))).join('\n')+'\n');
  command(clang,['@'+response],directory,log);
  const elf=command(readelf,['-h','-l','-d',output],directory,log);write(path.join(directory,'elf.txt'),elf);
  assert(/AArch64/.test(elf)&&!elf.includes('NEEDED'),'Standalone AArch64 musl required');
  fs.copyFileSync(path.join(root,'COPYRIGHT'),path.join(directory,'COPYRIGHT'));
  manifest.status='built';manifest.output=output;manifest.sha256=sha(fs.readFileSync(output));
  console.log(JSON.stringify({status:manifest.status,output,sha256:manifest.sha256,manifest:path.join(directory,'manifest.json'),payloadReplaced:false,phoneTouched:false},null,2));
 }catch(error){manifest.status='failed';manifest.error=error.message;throw error;}
 finally{manifest.endedAt=new Date().toISOString();write(path.join(directory,'manifest.json'),JSON.stringify(manifest,null,2)+'\n');}
}
module.exports={applyPatch,replaceOnce,generatedHeaders,sourceList,extract};
if(require.main===module)build(process.argv[2]).catch(error=>{console.error(error.stack);process.exitCode=1;});
