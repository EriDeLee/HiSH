const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process'),zlib=require('node:zlib');
const repo=path.resolve(__dirname,'..'),root=path.join(repo,'.test/package-tests');fs.mkdirSync(root,{recursive:true});
const run=(exe,args)=>cp.spawnSync(exe,args,{encoding:'utf8',windowsHide:true,maxBuffer:4e6});
const locate=run('where.exe',['g++.exe']);assert.equal(locate.status,0,'Host archive tests require the existing g++ toolchain');const compiler=locate.stdout.trim().split(/\r?\n/)[0],bin=path.dirname(compiler);
const sdk=process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME||path.resolve(repo,'../command-line-tools');
const headers=path.join(root,'include');fs.mkdirSync(headers,{recursive:true});for(const n of ['zlib.h','zconf.h'])fs.copyFileSync(path.join(sdk,'sdk/default/openharmony/native/sysroot/usr/include',n),path.join(headers,n));
const executable=path.join(root,'archive-test.exe');
const version=run(compiler,['-dumpversion']).stdout.trim(),standard=path.resolve(bin,'../include/c++',version);
const compiled=run(compiler,['-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-isystem',standard,'-isystem',path.join(standard,'x86_64-w64-mingw32'),'-I'+headers,'-I'+path.join(repo,'feature/hish_main/src/main/cpp'),path.join(__dirname,'fixtures/package_archive.cpp'),'-L'+bin,'-l:zlib1.dll','-static-libgcc','-static-libstdc++','-o',executable]);
assert.equal(compiled.status,0,compiled.stdout+compiled.stderr);fs.copyFileSync(path.join(bin,'zlib1.dll'),path.join(root,'zlib1.dll'));
function tar(entries){const chunks=[];for(const e of entries){const h=Buffer.alloc(512),bytes=Buffer.from(e.bytes||'');h.write(e.name,0,100);h.write('0000755\0',100);h.write(bytes.length.toString(8).padStart(11,'0')+'\0',124);h.fill(32,148,156);h[156]=(e.type||'0').charCodeAt(0);if(e.link)h.write(e.link,157,100);h.write('ustar\0',257);h.write([...h].reduce((a,b)=>a+b,0).toString(8).padStart(6,'0')+'\0 ',148);chunks.push(h,bytes,Buffer.alloc((512-bytes.length%512)%512));}chunks.push(Buffer.alloc(1024));return zlib.gzipSync(Buffer.concat(chunks));}
const meta={name:'.PKGINFO',bytes:'pkgname = test\npkgver = 1\narch = aarch64\n'};
function extract(bytes){const dir=fs.mkdtempSync(path.join(root,'case-')),archive=path.join(dir,'input.apk'),dest=path.join(dir,'root');fs.writeFileSync(archive,bytes);return {...run(executable,[archive,dest]),dest};}
test('actual native extractor resolves forward links as exact private file copies',()=>{const r=extract(tar([meta,{name:'usr/lib/liba.so',type:'2',link:'liba.so.1'},{name:'usr/lib/liba.so.1',bytes:'unmodified bytes'}]));assert.equal(r.status,0,r.stderr);assert.equal(fs.readFileSync(path.join(r.dest,'usr/lib/liba.so'),'utf8'),'unmodified bytes');assert(!fs.lstatSync(path.join(r.dest,'usr/lib/liba.so')).isSymbolicLink());});
test('actual native extractor rejects traversal, escaping and cyclic links, duplicates and devices',()=>{
 for(const entries of [[{name:'../escape',bytes:'x'}],[{name:'/absolute',bytes:'x'}],[{name:'C:/escape',bytes:'x'}],[{name:'x',type:'2',link:'../escape'}],[{name:'x',type:'2',link:'y'},{name:'y',type:'2',link:'x'}],[{name:'x',bytes:'a'},{name:'x',bytes:'b'}],[{name:'dev/thing',type:'3'}],[{name:'x',type:'2',link:''}]])assert.notEqual(extract(tar([meta,...entries])).status,0,JSON.stringify(entries));
});
test('native extractor reads concatenated APK gzip members and never executes control scripts',()=>{const r=extract(Buffer.concat([tar([meta,{name:'.post-install',bytes:'exit 99\n'}]),tar([{name:'usr/bin/app',bytes:'original'}])]));assert.equal(r.status,0,r.stderr);assert(!fs.existsSync(path.join(r.dest,'.post-install')));});
test('native extractor rejects corrupt gzip, tar checksums and truncated payloads',()=>{const b=tar([meta,{name:'usr/bin/app',bytes:'x'}]);const corrupt=Buffer.from(b);corrupt[corrupt.length-8]^=1;assert.notEqual(extract(corrupt).status,0);assert.notEqual(extract(b.subarray(0,b.length-5)).status,0);const raw=zlib.gunzipSync(b);raw[0]^=1;assert.notEqual(extract(zlib.gzipSync(raw)).status,0);});
test('bootstrap preparation pins every upstream input and changes deterministically',async()=>{const {prepare}=require('../scripts/packages/prepare.cjs');const a=await prepare(),b=await prepare();assert.deepEqual(a,b);const manifest=JSON.parse(fs.readFileSync(path.join(repo,'.test/packages/provenance.json')));assert(manifest.files.some(f=>f.name==='sbin/apk'));assert(manifest.files.some(f=>f.name.endsWith('.rsa.pub')));assert(!manifest.files.some(f=>/\/bin\/(jq|zstd|git|curl)$/.test(f.name)));});
test('every app-managed payload in the manifest is in the bootstrap under local/ with its digest',()=>{
 const provenance=JSON.parse(fs.readFileSync(path.join(repo,'.test/packages/provenance.json'))),files=new Map(provenance.files.map(f=>[f.name,f.sha256]));
 const blob=zlib.gunzipSync(fs.readFileSync(path.join(repo,'.test/packages',provenance.sha256+'.gz')));let manifest;
 for(let at=8;at<blob.length;){const n=blob.readUInt32LE(at),size=blob.readUInt32LE(at+4),name=blob.toString('utf8',at+12,at+12+n);if(name==='etc/hish/packages.json')manifest=JSON.parse(blob.toString('utf8',at+12+n,at+12+n+size));at+=12+n+size;}
 assert(manifest&&manifest.format===1);
 const payloads=manifest.local.flatMap(p=>p.kind==='apk'?[{payload:p.payload,sha256:p.sha256}]:p.files);assert(payloads.length>0);
 for(const p of payloads)assert.equal(files.get('local/'+p.payload),p.sha256,p.payload);
 assert.deepEqual(manifest.local.map(p=>p.name).sort(),['proot','talloc']);assert(!manifest.local.some(p=>/^(bun|jq|zstd|git|curl)$/.test(p.name)));
});
test('the real bootstrap input validator rejects changed upstream package bytes',()=>{const {verifiedArchive}=require('../scripts/packages/prepare.cjs'),pkg=require('../scripts/packages/bootstrap-lock.json').packages[0],bytes=fs.readFileSync(path.join(repo,'.test/packages',pkg.file));assert(verifiedArchive(bytes,pkg).has('sbin/apk'));const changed=Buffer.from(bytes);changed[100]^=1;assert.throws(()=>verifiedArchive(changed,pkg),/checksum/);});
test('upstream tgz extraction is explicit; APK mode still requires signed package metadata',()=>{
 const dir=fs.mkdtempSync(path.join(root,'tgz-')),file=path.join(dir,'input.tgz'),dest=path.join(dir,'unpacked');
 fs.writeFileSync(file,tar([{name:'package/bin/bun',bytes:'actual upstream input'}]));
 assert.notEqual(run(executable,[file,path.join(dir,'apk')]).status,0);
 assert.equal(run(executable,[file,dest,'tgz']).status,0);assert.equal(fs.readFileSync(path.join(dest,'package/bin/bun'),'utf8'),'actual upstream input');
});
test('the Bun source supplies node through Bun node mode without taking the real Node.js path',()=>{
 const bun=require('../scripts/packages/sources.json').remote.find(r=>r.name==='bun'),node=bun.extra.find(e=>e.path.endsWith('/node'));
 // Install scripts call node by name. Bun enters node mode when its program name is node.
 assert.deepEqual(node,{path:'bin/node',content:'#!/bin/sh\nexec -a node bun "$@"\n',mode:448});
 // A real Node.js package owns usr/bin/node, which precedes bin/ in the session PATH.
 assert(!bun.extra.some(e=>e.path==='usr/bin/node'));
});
