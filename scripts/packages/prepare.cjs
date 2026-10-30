// Build-time only: pin the unmodified upstream APK client, its dependencies,
// public repository keys and CA bundle. No ordinary applications enter the HAP.
const fs=require('node:fs'),path=require('node:path'),zlib=require('node:zlib'),crypto=require('node:crypto'),assert=require('node:assert/strict'),cp=require('node:child_process');
const lock=require('./bootstrap-lock.json'),repo=path.resolve(__dirname,'../..'),output=path.join(repo,'.test/packages');
const hash=b=>crypto.createHash('sha256').update(b).digest('hex');
function archive(bytes){
 const result=new Map(),b=zlib.gunzipSync(bytes,{maxOutputLength:64*1024*1024});
 for(let at=0;at+512<=b.length;){const h=b.subarray(at,at+512);at+=512;if(h.every(x=>!x))continue;
  const s=(a,n)=>h.toString('utf8',a,a+n).split('\0')[0],name=(s(345,155)?s(345,155)+'/':'')+s(0,100),size=parseInt(s(124,12).trim()||'0',8);
  assert(Number.isSafeInteger(size)&&size>=0&&at+size<=b.length,'Invalid tar size');
  let sum=0;for(let i=0;i<512;i++)sum+=i>=148&&i<156?32:h[i];assert.equal(sum,parseInt(s(148,8),8),'Tar header checksum');
  if(['0',''].includes(s(156,1))){assert(!result.has(name),'Duplicate tar entry');result.set(name,b.subarray(at,at+size));}
  at+=Math.ceil(size/512)*512;
 }return result;
}
async function prepare(sdk){
 fs.mkdirSync(output,{recursive:true});const records=[];
 for(const pkg of lock.packages){const saved=path.join(output,pkg.file);let bytes;
  if(fs.existsSync(saved))bytes=fs.readFileSync(saved);else{const r=await fetch(lock.repository+'/'+lock.architecture+'/'+pkg.file);assert(r.ok,pkg.file+' HTTP '+r.status);bytes=Buffer.from(await r.arrayBuffer());assert.equal(hash(bytes),pkg.sha256,pkg.file);fs.writeFileSync(saved,bytes);}
  const entries=verifiedArchive(bytes,pkg);
  for(const [from,to] of Object.entries(pkg.files)){assert(entries.has(from),'Missing regular member: '+from);records.push({name:to,bytes:entries.get(from),mode:to==='sbin/apk'?0o700:0o600});}
 }
 records.push({name:'etc/apk/repositories',bytes:Buffer.from(lock.repository+'\n'+lock.repository.replace('/main','/community')+'\n'),mode:0o600});
 // APK's update applet opens its own (empty) world, independent of HiSH's
 // installed-generation metadata. No software is registered in this DB.
 records.push({name:'etc/apk/world',bytes:Buffer.alloc(0),mode:0o600});
 const native=sdk||process.env.OHOS_NATIVE_SDK||path.resolve(repo,'../command-line-tools/sdk/default/openharmony/native'),musl=path.join(repo,'.test/linux-tools/musl'),lib=path.join(musl,'usr/lib'),link=path.join(output,'link');fs.mkdirSync(link,{recursive:true});
 for(const r of records.filter(r=>r.name.startsWith('lib/')))fs.writeFileSync(path.join(link,path.basename(r.name)),r.bytes);
 const helper=path.join(output,'fetch-helper'),clang=path.join(native,'llvm/bin/clang.exe');
 const built=cp.spawnSync(clang,['--target=aarch64-linux-musl','--sysroot='+musl,'-fuse-ld=lld','-nostdlib','-Os','-fPIE','-pie','-fno-emulated-tls','-fno-stack-protector','-mno-outline-atomics','-Wl,-z,max-page-size=4096','-Wl,--export-dynamic','-Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1',path.join(lib,'Scrt1.o'),path.join(lib,'crti.o'),path.join(__dirname,'fetch.c'),'-I'+path.join(musl,'usr/include'),'-L'+link,'-Wl,--no-as-needed','-l:libapk.so.3.0.0','-l:libssl.so.3','-l:libcrypto.so.3',path.join(musl,'lib/libc.musl-aarch64.so.1'),path.join(native,'llvm/lib/clang/15.0.4/lib/aarch64-linux-ohos/libclang_rt.builtins.a'),path.join(lib,'crtn.o'),'-o',helper],{encoding:'utf8',windowsHide:true});assert.equal(built.status,0,built.stdout+built.stderr);
 records.push({name:'sbin/fetch-helper',bytes:fs.readFileSync(helper),mode:0o700});
 const proot=await require('./proot-build.cjs').buildPRoot(native,archive);
 records.push({name:'sbin/proot-hish',bytes:fs.readFileSync(proot.program.file),mode:0o700},{name:'sbin/proot-probe',bytes:fs.readFileSync(proot.probe.file),mode:0o700});
 records.push({name:'etc/hish/proot-provenance.json',bytes:Buffer.from(JSON.stringify({version:proot.version,commit:proot.commit,sources:proot.sources,sha256:proot.program.sha256,fingerprint:proot.fingerprint})),mode:0o600});
 const upstream=require('./upstream.json');records.push({name:'etc/hish/upstream.json',bytes:Buffer.from(JSON.stringify(upstream)),mode:0o600});
 fs.writeFileSync(path.join(output,'package_upstream.h'),'#pragma once\nnamespace HishUpstream {\n'+Object.entries({Bun:upstream.bun,Proot:{version:proot.version,commit:proot.commit}}).flatMap(([name,values])=>Object.entries(values).map(([k,v])=>'inline constexpr const char* '+name+'_'+k+' = '+JSON.stringify(v)+';')).join('\n')+'\n}\n');
 const chunks=[Buffer.from('HISHPKG1')];for(const r of records){assert(/^[a-zA-Z0-9@._/+-]+$/.test(r.name)&&!r.name.split('/').includes('..'));const n=Buffer.from(r.name),h=Buffer.alloc(12);h.writeUInt32LE(n.length);h.writeUInt32LE(r.bytes.length,4);h.writeUInt32LE(r.mode,8);chunks.push(h,n,r.bytes);}
 const blob=zlib.gzipSync(Buffer.concat(chunks),{level:9}),digest=hash(blob),dest=path.join(output,digest+'.gz');
 if(!fs.existsSync(dest))fs.writeFileSync(dest,blob);
 fs.writeFileSync(path.join(output,'bootstrap.cmake'),'set(HISH_PACKAGE_BOOTSTRAP [=['+dest.replace(/\\/g,'/')+']=])\nset(HISH_PACKAGE_BOOTSTRAP_SHA256 '+digest+')\n');
 fs.writeFileSync(path.join(output,'provenance.json'),JSON.stringify({repository:lock.repository,architecture:lock.architecture,sha256:digest,bytes:blob.length,packages:lock.packages,files:records.map(r=>({name:r.name,bytes:r.bytes.length,sha256:hash(r.bytes)}))},null,2)+'\n');
 return {dest,sha256:digest,bytes:blob.length};
}
if(require.main===module)prepare(process.argv[2]).then(x=>console.log(JSON.stringify(x))).catch(e=>{console.error(e);process.exitCode=1;});
function verifiedArchive(bytes,pkg){assert.equal(hash(bytes),pkg.sha256,pkg.file+' checksum');return archive(bytes);}
module.exports={archive,prepare,verifiedArchive};
