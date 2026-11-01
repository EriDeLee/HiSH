// Real package core (feature/hish_main/src/main/cpp/package_*.h) driven on the
// host through a fake repository backend. Exercises local-package locking,
// dependency conflicts, migrations, legacy import, crash recovery and cleanup.
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process'),zlib=require('node:zlib'),crypto=require('node:crypto');
const repo=path.resolve(__dirname,'..'),root=path.join(repo,'.test/package-core');fs.mkdirSync(root,{recursive:true});
const run=(exe,args,env={})=>cp.spawnSync(exe,args,{encoding:'utf8',windowsHide:true,maxBuffer:8e6,env:{...process.env,...env}});
const locate=run('where.exe',['g++.exe']);assert.equal(locate.status,0,'Host package tests require the existing g++ toolchain');
const compiler=locate.stdout.trim().split(/\r?\n/)[0],bin=path.dirname(compiler);
const sdk=process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME||path.resolve(repo,'../command-line-tools');
const headers=path.join(root,'include');fs.mkdirSync(headers,{recursive:true});for(const n of ['zlib.h','zconf.h'])fs.copyFileSync(path.join(sdk,'sdk/default/openharmony/native/sysroot/usr/include',n),path.join(headers,n));
const version=run(compiler,['-dumpversion']).stdout.trim(),standard=path.resolve(bin,'../include/c++',version);
const executable=path.join(root,'package-core.exe');
const compiled=run(compiler,['-std=c++17','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-isystem',standard,'-isystem',path.join(standard,'x86_64-w64-mingw32'),'-I'+headers,'-I'+path.join(repo,'feature/hish_main/src/main/cpp'),path.join(__dirname,'fixtures/package_core.cpp'),'-L'+bin,'-l:zlib1.dll','-static-libgcc','-static-libstdc++','-o',executable]);
assert.equal(compiled.status,0,compiled.stdout+compiled.stderr);fs.copyFileSync(path.join(bin,'zlib1.dll'),path.join(root,'zlib1.dll'));
const sha256=b=>crypto.createHash('sha256').update(b).digest('hex'),sha512=b=>crypto.createHash('sha512').update(b);
function tar(entries){const chunks=[];for(const e of entries){const h=Buffer.alloc(512),bytes=Buffer.from(e.bytes||'');h.write(e.name,0,100);h.write((e.mode||'0000755')+'\0',100);h.write(bytes.length.toString(8).padStart(11,'0')+'\0',124);h.fill(32,148,156);h[156]=(e.type||'0').charCodeAt(0);if(e.link)h.write(e.link,157,100);h.write('ustar\0',257);h.write([...h].reduce((a,b)=>a+b,0).toString(8).padStart(6,'0')+'\0 ',148);chunks.push(h,bytes,Buffer.alloc((512-bytes.length%512)%512));}chunks.push(Buffer.alloc(1024));return zlib.gzipSync(Buffer.concat(chunks));}
function apk(name,ver,{depends=[],provides=[],files=[],scripts={}}={}){
 const info=['pkgname = '+name,'pkgver = '+ver,'arch = aarch64',...depends.map(d=>'depend = '+d),...provides.map(p=>'provides = '+p)].join('\n')+'\n';
 return Buffer.concat([tar([{name:'.PKGINFO',bytes:info},...Object.entries(scripts).map(([n,b])=>({name:n,bytes:b}))]),tar(files)]);
}
// One isolated world: fake HAP payload directory, manifest, repository and package base.
function world(label){
 const dir=fs.mkdtempSync(path.join(root,label+'-')),payloads=path.join(dir,'payloads'),rep=path.join(dir,'repo'),base=path.join(dir,'base');
 for(const d of [payloads,rep,path.join(rep,'npm'),base])fs.mkdirSync(d,{recursive:true});
 const lib=apk('libshared','1.0-r0',{provides:['so:libshared.so.1=1.0'],files:[{name:'usr/lib/libshared.so.1.0',bytes:'local shared library'},{name:'usr/lib/libshared.so.1',type:'2',link:'libshared.so.1.0'}]});
 fs.writeFileSync(path.join(payloads,'libshared.apk'),lib);
 const add=(name,ver,opts,deps=[])=>{const file=name+'-'+ver+'.apk';fs.writeFileSync(path.join(rep,file),apk(name,ver,opts));fs.appendFileSync(path.join(rep,'index.tsv'),name+'\t'+file+'\t'+deps.join(' ')+'\n');};
 fs.writeFileSync(path.join(rep,'index.tsv'),'');
 add('hello','2.0-r0',{depends:['so:libshared.so.1'],files:[{name:'usr/bin/hello',bytes:'hello program'}]},['libshared']);
 add('libshared','9.9-r0',{files:[{name:'usr/lib/libshared.so.1.0',bytes:'REMOTE replacement'}]});
 add('needs-new','1.0-r0',{depends:['libshared>=2.0'],files:[{name:'usr/bin/needs-new',bytes:'x'}]},['libshared']);
 add('evil','1.0-r0',{files:[{name:'usr/lib/libshared.so.1.0',bytes:'path takeover'}]});
 add('toolx','99-r0',{files:[{name:'usr/bin/toolx',bytes:'REMOTE toolx'}]});
 add('plain','1.0-r0',{files:[{name:'usr/bin/plain',bytes:'plain v1'},{name:'usr/bin/plain-link',type:'2',link:'plain'},{name:'etc/plain.conf',bytes:'default config\n'}],scripts:{'.post-install':'#!/bin/sh\nadduser -S plain\n'}});
 add('libc6compat','1.0-r0',{files:[{name:'usr/lib/libcompat.so',bytes:'compat'}]});
 add('dirlink','1.0-r0',{files:[{name:'usr/share/real/x',bytes:'x'},{name:'usr/share/alias',type:'2',link:'real'}]});
 add('bigprog','1.0-r0',{files:[{name:'usr/bin/prog',bytes:'p'.repeat(70000)},{name:'usr/bin/prog-alias',type:'2',link:'prog'},{name:'usr/lib/libthing.so.1.0',bytes:'l'.repeat(70000)},{name:'usr/lib/libthing.so.1',type:'2',link:'libthing.so.1.0'},{name:'usr/share/data.bin',bytes:'d'.repeat(70000),mode:'0000644'},{name:'usr/share/data-alias',type:'2',link:'data.bin'},{name:'usr/bin/small',bytes:'tiny'},{name:'usr/bin/small-alias',type:'2',link:'small'}]});
 const toolx=Buffer.from('local toolx v1');fs.writeFileSync(path.join(payloads,'toolx-v1'),toolx);
 const tgz=tar([{name:'package/bin/runtime',bytes:'runtime 2.0.0'},{name:'package/package.json',bytes:'{}'}]);
 fs.writeFileSync(path.join(rep,'npm/runtime-2.0.0.tgz'),tgz);fs.writeFileSync(path.join(rep,'npm/runtime-2.0.0.tgz.sha512'),sha512(tgz).digest('hex'));
 const meta=(v,url)=>JSON.stringify({name:'@x/runtime',version:v,os:['linux'],cpu:['arm64'],libc:['musl'],dist:{tarball:url,integrity:'sha512-'+sha512(tgz).digest('base64')}});
 fs.writeFileSync(path.join(rep,'npm/latest.json'),meta('2.0.0','https://registry.example/@x/runtime/-/runtime-2.0.0.tgz'));
 const manifest={format:1,
  local:[{name:'toolx',version:'1.0-hish',kind:'files',description:'locally adapted tool',origin:'test port',depends:['libshared'],files:[{path:'usr/bin/toolx',payload:'toolx-v1',sha256:sha256(toolx),mode:448}]},
   {name:'libshared',version:'1.0-r0',kind:'apk',description:'bundled library',origin:'test repository',provides:['so:libshared.so.1=1.0'],payload:'libshared.apk',sha256:sha256(lib)}],
  remote:[{name:'runtime',kind:'npm',description:'runtime',registry:'https://registry.example',package:'@x/runtime',member:'package/bin/runtime',install:'usr/bin/runtime',alpineDependencies:['libc6compat'],extra:[{path:'usr/bin/runtimex',content:'#!/bin/sh\nexec runtime x "$@"\n',mode:448}]}],
  checks:{toolx:'toolx --check',plain:'exit 1'}};
 const mfile=path.join(dir,'manifest.json');fs.writeFileSync(mfile,JSON.stringify(manifest));
 const pkg=(args,env={})=>run(executable,[base,payloads,mfile,rep,...args],env);
 const ok=(args,env)=>{const r=pkg(args,env);assert.equal(r.status,0,args.join(' ')+'\n'+r.stdout+r.stderr);return r.stdout;};
 const active=()=>{const id=fs.readFileSync(path.join(base,'active'),'utf8').trim();return path.join(base,'generations',id);};
 const file=rel=>fs.readFileSync(path.join(active(),rel),'utf8');
 const records=()=>Object.fromEntries(fs.readFileSync(path.join(active(),'.hish-package/installed.tsv'),'utf8').trim().split('\n').filter(Boolean).map(l=>{const c=l.split('\t');return [c[0],{version:c[1],source:c[2],state:c[3],notes:c[6]}];}));
 return {dir,payloads,rep,base,mfile,manifest,pkg,ok,active,file,records,add,lib,toolx};
}
test('APK version comparison follows apk-tools ordering and refuses unparseable input',()=>{
 const cmp=(a,op,b)=>run(executable,['vercmp',a,op,b]).stdout.trim();
 for(const [a,op,b,want] of [['1.0-r0','>=','1.0','yes'],['1.10','>','1.9','yes'],['1.0_rc1','<','1.0','yes'],['1.0_p1','>','1.0','yes'],['1.0a','>','1.0','yes'],['2.4.3-r0','<','2.4.3-r1','yes'],['1.2.3','~','1.2','yes'],['1.20','~','1.2','no'],['abc','=','abc','no'],['1.0','!!','1.0','no']])
  assert.equal(cmp(a,op,b),want,a+op+b);
});
test('an app-managed package installs offline with its bundled dependency',()=>{
 const w=world('offline');w.ok(['install','toolx'],{HISH_TEST_OFFLINE:'1'});
 assert.equal(w.file('usr/bin/toolx'),'local toolx v1');assert.equal(w.file('usr/lib/libshared.so.1.0'),'local shared library');
 assert.equal(w.file('usr/lib/libshared.so.1'),'local shared library');
 const r=w.records();assert.equal(r.toolx.source,'local:manifest');assert.equal(r.libshared.source,'local:manifest');
 assert(!fs.existsSync(path.join(w.rep,'last-fetch.txt')),'offline local install must not query the repository');
});
test('repository packages never replace an app-managed name, path or version',()=>{
 const w=world('lock');w.ok(['install','toolx']);
 const out=w.ok(['install','hello']);assert.match(out,/libshared: repository copy not used/);
 assert.equal(w.file('usr/lib/libshared.so.1.0'),'local shared library');assert.equal(w.file('usr/bin/hello'),'hello program');
 assert.equal(w.records().libshared.version,'1.0-r0');
 // Same name in the repository: the request goes to the app copy and never to the repository.
 w.ok(['install','toolx']);assert(!fs.readFileSync(path.join(w.rep,'last-fetch.txt'),'utf8').includes('toolx'));
 const before=fs.readFileSync(path.join(w.base,'active'),'utf8');
 let r=w.pkg(['install','needs-new']);assert.notEqual(r.status,0);assert.match(r.stderr,/dependency conflict: needs-new requires libshared>=2\.0, app-managed libshared provides 1\.0-r0/);
 r=w.pkg(['install','evil']);assert.notEqual(r.status,0);assert.match(r.stderr,/duplicate archive file: usr\/lib\/libshared\.so\.1\.0/);
 assert.equal(fs.readFileSync(path.join(w.base,'active'),'utf8'),before,'failed transactions must not change the active generation');
 assert.deepEqual(fs.readdirSync(path.join(w.base,'generations')).filter(n=>n.startsWith('.tmp')),[]);
});
test('upgrade, update and unrelated changes keep the installed app-managed bytes',()=>{
 const w=world('upgrade');w.ok(['install','toolx','hello']);
 const digest=()=>sha256(fs.readFileSync(path.join(w.active(),'usr/bin/toolx')))+sha256(fs.readFileSync(path.join(w.active(),'usr/lib/libshared.so.1.0')));
 const before=digest();const out=w.ok(['upgrade']);assert.match(out,/kept app-managed toolx 1\.0-hish/);
 w.ok(['update']);w.ok(['install','plain']);w.ok(['remove','plain']);assert.equal(digest(),before);
 // Changing the HAP manifest does not let pkg install the new payload.
 const v2=Buffer.from('local toolx v2');fs.writeFileSync(path.join(w.payloads,'toolx-v2'),v2);
 w.manifest.local[0].version='2.0-hish';w.manifest.local[0].files[0]={path:'usr/bin/toolx',payload:'toolx-v2',sha256:sha256(v2),mode:448};fs.writeFileSync(w.mfile,JSON.stringify(w.manifest));
 assert.equal(w.pkg(['pending']).stdout.trim(),'pending');
 for(const args of [['upgrade'],['install','plain'],['remove','toolx'],['install','toolx']]){const r=w.pkg(args);assert.notEqual(r.status,0,args.join(' '));assert.match(r.stderr,/migration is not complete/);}
 assert.equal(w.file('usr/bin/toolx'),'local toolx v1');
 // The app migrator applies the new manifest to installed packages only.
 w.manifest.local.push({name:'newtool',version:'1',kind:'files',files:[{path:'usr/bin/newtool',payload:'toolx-v2',sha256:sha256(v2),mode:448}]});fs.writeFileSync(w.mfile,JSON.stringify(w.manifest));
 w.ok(['migrate'],{HISH_TEST_OFFLINE:'1'});assert.equal(w.file('usr/bin/toolx'),'local toolx v2');assert.equal(w.records().toolx.version,'2.0-hish');
 assert(!fs.existsSync(path.join(w.active(),'usr/bin/newtool')),'migration must not install packages that were not installed');
 assert.equal(w.pkg(['pending']).stdout.trim(),'accepted');w.ok(['install','plain']);
});
test('a migration error left by an older app version is dropped once nothing is pending',()=>{
 const w=world('stale-error');w.ok(['install','toolx'],{HISH_TEST_OFFLINE:'1'});
 const file=path.join(w.base,'state','migration-error');fs.mkdirSync(path.dirname(file),{recursive:true});
 fs.writeFileSync(file,'read /old/bootstrap-0123/talloc-2.4.3-r0.apk\n');
 assert.equal(w.pkg(['pending']).stdout.trim(),'accepted');
 assert.doesNotMatch(w.ok(['doctor']),/last migration error/,'an obsolete record must not be reported');
 assert(fs.existsSync(file),'doctor only reports, it never edits state');
 w.ok(['migrate'],{HISH_TEST_OFFLINE:'1'});assert(!fs.existsSync(file),'the session-start migration removes the obsolete record');
 // While a migration really is pending, the record still says why, on one line.
 w.manifest.local[0].version='2.0-hish';fs.writeFileSync(w.mfile,JSON.stringify(w.manifest));
 assert.equal(w.pkg(['pending']).stdout.trim(),'pending');fs.writeFileSync(file,'could not read payload\n');
 const pending=w.ok(['doctor']);assert.match(pending,/\(migration pending\)\r?\nlast migration error: could not read payload\r?\nlinks:/);
});
test('removing a package keeps app-managed dependencies still needed, offline',()=>{
 const w=world('remove');w.ok(['install','toolx','hello']);
 const out=w.ok(['remove','toolx'],{HISH_TEST_OFFLINE:'1'});assert.match(out,/removed toolx/);
 const r=w.records();assert(!r.toolx);assert.equal(r.libshared.source,'local:manifest');assert(r.hello);
 const fail=w.pkg(['remove','libshared']);assert.notEqual(fail.status,0);assert.match(fail.stderr,/installed as a dependency/);
});
test('install scripts are recorded as not run and the package is marked unconfigured',()=>{
 const w=world('scripts');const out=w.ok(['install','plain'],{HISH_TEST_LINKS:'copy'});
 assert.match(out,/plain: install scripts were not run \(\.post-install\); marked unconfigured/);
 const r=w.records();assert.equal(r.plain.state,'unconfigured');assert.equal(r.plain.notes,'scripts-not-run:.post-install');
 assert.equal(fs.readFileSync(path.join(w.active(),'.hish-package/scripts/plain/.post-install'),'utf8'),'#!/bin/sh\nadduser -S plain\n');
 assert(!fs.lstatSync(path.join(w.active(),'usr/bin/plain-link')).isSymbolicLink());assert.equal(w.file('usr/bin/plain-link'),'plain v1');
 assert.match(w.ok(['list']),/plain\t1\.0-r0\talpine\tunconfigured\tlinks-copied:1/);
 const doctor=w.ok(['doctor']);assert.match(doctor,/plain 1\.0-r0: alpine, unconfigured, 1 links stored as copies \(\.post-install not run/);
});
test('a link to a sizeable program becomes an emulated link; library, data and small-program links stay copies',()=>{
 const w=world('proglink');
 w.ok(['install','bigprog'],{HISH_TEST_LINKS:'copy'});
 assert.equal(w.file('usr/bin/prog-alias'),'!<hish-symlink>prog','a link to a 70 KB program is a marker');
 assert.equal(w.file('usr/lib/libthing.so.1'),'l'.repeat(70000),'ld.so cannot follow a marker, so a library link stays a copy');
 assert.equal(w.file('usr/share/data-alias'),'d'.repeat(70000),'a link to a data file stays a copy');
 assert.equal(w.file('usr/bin/small-alias'),'tiny','a link to a small program stays a copy');
 assert.match(w.ok(['list'],{HISH_TEST_LINKS:'copy'}),/bigprog\t1\.0-r0\talpine\tconfigured\tlinks-copied:3/);
 w.ok(['install','plain'],{HISH_TEST_LINKS:'copy'});
 assert.equal(w.file('usr/bin/prog-alias'),'!<hish-symlink>prog','the marker survives into the next generation');
 assert.equal(w.file('usr/bin/prog'),'p'.repeat(70000));
});
test('a directory link becomes an emulated link marker where symlinks are refused, and survives later generations',()=>{
 const w=world('dirlink'),marker='!<hish-symlink>real';
 w.ok(['install','dirlink'],{HISH_TEST_LINKS:'copy'});
 assert.equal(w.file('usr/share/alias'),marker,'the link is a marker file holding the relative target');
 assert.equal(w.file('usr/share/real/x'),'x');assert(!fs.lstatSync(path.join(w.active(),'usr/share/alias')).isSymbolicLink());
 w.ok(['install','plain'],{HISH_TEST_LINKS:'copy'});
 assert.equal(w.file('usr/share/alias'),marker,'a package carried into the next generation keeps its marker');
 assert.match(w.ok(['doctor'],{HISH_TEST_LINKS:'copy'}),/dirlink 1\.0-r0: alpine, configured/);
 w.ok(['remove','dirlink'],{HISH_TEST_LINKS:'copy'});
 assert(!fs.existsSync(path.join(w.active(),'usr/share/alias')),'removing the package removes the marker');
});
test('a user-modified file is kept and the new bytes are saved beside it',()=>{
 const w=world('modified');w.ok(['install','plain']);
 const conf=path.join(w.active(),'etc/plain.conf');fs.chmodSync(conf,0o644);fs.writeFileSync(conf,'user config\n');
 // A changed repository version re-extracts the package.
 w.add('plain','1.1-r0',{files:[{name:'usr/bin/plain',bytes:'plain v2'},{name:'etc/plain.conf',bytes:'default config v2\n'}]});
 const out=w.ok(['upgrade']);assert.match(out,/plain: modified file kept: etc\/plain\.conf \(new version saved as etc\/plain\.conf\.hish-new\)/);
 assert.equal(w.file('etc/plain.conf'),'user config\n');assert.equal(w.file('etc/plain.conf.hish-new'),'default config v2\n');assert.equal(w.file('usr/bin/plain'),'plain v2');
 assert.match(w.ok(['doctor']),/plain 1\.1-r0: alpine, configured, 1 modified/);
});
test('remote registry packages are data driven, checked and kept unless upgraded',()=>{
 const w=world('remote');w.ok(['install','runtime']);
 assert.equal(w.file('usr/bin/runtime'),'runtime 2.0.0');assert.equal(w.file('usr/bin/runtimex'),'#!/bin/sh\nexec runtime x "$@"\n');
 const r=w.records();assert.equal(r.runtime.version,'2.0.0');assert.equal(r.runtime.source,'npm:https://registry.example/@x/runtime');assert(r.libc6compat);
 w.ok(['install','plain'],{});assert.equal(fs.readFileSync(path.join(w.rep,'artifact-downloads.txt'),'utf8'),'runtime-2.0.0.tgz\n','kept remote packages are not downloaded again');
 // Metadata pointing outside the configured registry or with a wrong digest is refused.
 const meta=JSON.parse(fs.readFileSync(path.join(w.rep,'npm/latest.json')));meta.version='2.1.0';meta.dist.tarball='https://elsewhere.example/runtime.tgz';fs.writeFileSync(path.join(w.rep,'npm/latest.json'),JSON.stringify(meta));
 let f=w.pkg(['upgrade']);assert.notEqual(f.status,0);assert.match(f.stderr,/outside the configured registry/);
 meta.dist.tarball='https://registry.example/@x/runtime/-/runtime-2.0.0.tgz';meta.dist.integrity='sha512-'+crypto.createHash('sha512').update('other').digest('base64');fs.writeFileSync(path.join(w.rep,'npm/latest.json'),JSON.stringify(meta));
 f=w.pkg(['upgrade']);assert.notEqual(f.status,0);assert.match(f.stderr,/SHA-512 check failed/);assert.equal(w.records().runtime.version,'2.0.0');
});
test('every crash point leaves exactly the old or the new complete generation',()=>{
 for(const [point,expectNew] of [['before-publish',false],['after-publish',false],['before-switch',false],['after-switch',true]]){
  const w=world('crash');w.ok(['install','toolx']);const old=fs.readFileSync(path.join(w.base,'active'),'utf8');
  const r=w.pkg(['install','plain'],{HISH_TEST_CRASH:point});assert.equal(r.status,3,point);
  w.ok(['list']);const now=fs.readFileSync(path.join(w.base,'active'),'utf8');
  assert.equal(now!==old,expectNew,point);assert.equal(Boolean(w.records().plain),expectNew,point);
  assert.deepEqual(fs.readdirSync(path.join(w.base,'generations')).filter(n=>n.startsWith('.tmp')),[],point);
  assert(!fs.existsSync(path.join(w.base,'active.tmp')),point);w.ok(['install','plain']);
 }
});
test('old generations are collected unless a running process still uses them',()=>{
 const w=world('gc');w.ok(['install','toolx']);const first=fs.readFileSync(path.join(w.base,'active'),'utf8').trim();
 for(const n of ['plain','hello','libc6compat'])w.ok(['install',n],{HISH_TEST_REFERENCED:first});
 const ids=fs.readdirSync(path.join(w.base,'generations')).sort();assert(ids.includes(first),'referenced generation must stay');assert.equal(ids.length,3);
 w.ok(['install','runtime']);assert(!fs.readdirSync(path.join(w.base,'generations')).includes(first));
});
test('the previous single-directory layout is imported without changing it',()=>{
 const w=world('legacy'),old=path.join(w.base,'root'),meta=path.join(old,'.hish-package');
 fs.mkdirSync(path.join(old,'usr/bin'),{recursive:true});fs.mkdirSync(path.join(meta,'metadata'),{recursive:true});
 fs.writeFileSync(path.join(old,'usr/bin/toolx'),w.toolx);fs.writeFileSync(path.join(old,'usr/bin/runtime'),'runtime 1.0');fs.writeFileSync(path.join(old,'usr/bin/hello'),'hello program');
 const libdir=path.join(old,'usr/lib');fs.mkdirSync(libdir,{recursive:true});fs.writeFileSync(path.join(libdir,'libshared.so.1.0'),'tampered');fs.writeFileSync(path.join(libdir,'libshared.so.1'),'tampered');
 fs.writeFileSync(path.join(meta,'world'),'toolx\nruntime\nhello\n');
 fs.writeFileSync(path.join(meta,'installed.tsv'),'hello\t2.0-r0\nlibshared\t1.0-r0\nruntime\t1.0\tupstream-sha512:'+'ab'.repeat(64)+'\ntoolx\t1.0-hish\tplatform-port-source:abc\n');
 fs.writeFileSync(path.join(meta,'metadata/hello.txt'),'pkgname = hello\n');
 const snapshot=fs.readFileSync(path.join(meta,'installed.tsv'));
 const out=w.ok(['list']);assert.match(out,/imported the previous package layout/);
 const r=w.records();assert.equal(r.toolx.source,'local:manifest');assert.equal(r.libshared.source,'legacy-unknown');assert.equal(r.runtime.source,'npm:https://registry.example/@x/runtime');assert.equal(r.hello.source,'alpine');
 assert.equal(w.file('usr/bin/runtime'),'runtime 1.0');assert.deepEqual(fs.readFileSync(path.join(meta,'installed.tsv')),snapshot);assert(fs.existsSync(path.join(old,'usr/bin/toolx')));
 const reinstall=w.pkg(['install','libshared']);assert.notEqual(reinstall.status,0);assert.match(reinstall.stderr,/run pkg remove libshared first/);
});
test('a repository fetch that silently returns nothing fails without dropping installed packages',()=>{
 const w=world('empty-fetch');w.ok(['install','hello']);const before=w.pkg(['--print-root']).stdout.trim();
 const failed=w.pkg(['install','evil'],{HISH_TEST_EMPTY_FETCH:'evil'});
 assert.notEqual(failed.status,0,failed.stdout);
 assert.match(failed.stderr,/package not found in the repository: evil; transaction not activated/);
 assert.equal(w.pkg(['--print-root']).stdout.trim(),before,'active generation changed after a failed install');
 assert.equal(w.records().hello.version,'2.0-r0');assert.equal(w.file('usr/bin/hello'),'hello program');
});
test('a package the previous app version installed itself is migrated to the current payload',()=>{
 const w=world('legacy-port'),old=path.join(w.base,'root'),meta=path.join(old,'.hish-package');
 fs.mkdirSync(path.join(old,'usr/bin'),{recursive:true});fs.mkdirSync(meta,{recursive:true});
 fs.writeFileSync(path.join(old,'usr/bin/toolx'),'toolx from the previous app version');
 fs.writeFileSync(path.join(meta,'world'),'toolx\n');fs.writeFileSync(path.join(meta,'installed.tsv'),'toolx\t0.9-hish\tplatform-port-source:abc\n');
 w.ok(['list'],{HISH_TEST_OFFLINE:'1'});assert.equal(w.records().toolx.source,'local:manifest');
 assert.equal(w.pkg(['pending']).stdout.trim(),'pending');
 const blocked=w.pkg(['install','plain']);assert.notEqual(blocked.status,0);assert.match(blocked.stderr,/migration is not complete/);
 w.ok(['migrate'],{HISH_TEST_OFFLINE:'1'});assert.equal(w.file('usr/bin/toolx'),'local toolx v1');assert.equal(w.records().toolx.version,'1.0-hish');
 assert.equal(w.records().libshared.source,'local:manifest');assert.equal(fs.readFileSync(path.join(old,'usr/bin/toolx'),'utf8'),'toolx from the previous app version');
});
