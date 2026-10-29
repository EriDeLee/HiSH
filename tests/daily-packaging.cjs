const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process');
const {hash}=require('../scripts/linux/source-runtime.cjs');
const repo=path.resolve(__dirname,'..'),root=path.join(repo,'.test/daily-packaging-tests');fs.mkdirSync(root,{recursive:true});
const sdk=process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME||path.resolve(repo,'../command-line-tools');
const cmake=path.join(sdk,'sdk/default/openharmony/native/build-tools/cmake/bin/cmake.exe');
function fixture(){
 const dir=fs.mkdtempSync(path.join(root,'case-')),payloads=path.join(dir,'payloads');fs.mkdirSync(payloads);
 const input=path.join(dir,'source.c'),binary=path.join(payloads,'runtime.so');fs.writeFileSync(input,'source');fs.writeFileSync(binary,'runtime');
 const manifest={diagnostics:false,inputs:[{file:input,sha256:hash(fs.readFileSync(input))}],files:[{name:'runtime.so',sha256:hash(fs.readFileSync(binary))}]};
 function save(){fs.writeFileSync(path.join(payloads,'runtime-manifest.json'),JSON.stringify(manifest));}
 save();const file=path.join(dir,'check.cmake'),out=path.join(dir,'out');fs.mkdirSync(out);
 fs.writeFileSync(file,[`set(CMAKE_SYSTEM_PROCESSOR aarch64)`,`set(CMAKE_CURRENT_BINARY_DIR [=[${dir.replace(/\\/g,'/')}]=])`,
  `set(HISH_LINUX_PAYLOAD_DIR [=[${payloads.replace(/\\/g,'/')}]=])`,`set(CMAKE_LIBRARY_OUTPUT_DIRECTORY [=[${out.replace(/\\/g,'/')}]=])`,
  `include([=[${path.join(repo,'feature/hish_main/src/main/cpp/runtime_payloads.cmake').replace(/\\/g,'/')}]=])`].join('\n'));
 return {dir,out,input,binary,payloads,manifest,save,run:()=>cp.spawnSync(cmake,['-P',file],{encoding:'utf8',windowsHide:true})};
}
test('daily CMake guard accepts the exact input/payload bytes',()=>{const f=fixture();assert.equal(f.run().status,0);});
test('daily CMake guard refuses stale source rather than falling back',()=>{const f=fixture();fs.writeFileSync(f.input,'changed');const r=f.run();assert.notEqual(r.status,0);assert.match(r.stderr,/Stale or changed runtime input/);});
test('daily CMake guard refuses a diagnostic runtime',()=>{const f=fixture();f.manifest.diagnostics=true;f.save();assert.notEqual(f.run().status,0);});
test('daily CMake guard refuses missing runtime provenance',()=>{const f=fixture();fs.renameSync(path.join(f.payloads,'runtime-manifest.json'),path.join(f.dir,'saved-manifest.json'));assert.notEqual(f.run().status,0);});
test('old generated probe bytes are archived, never packaged or deleted',()=>{
 const f=fixture(),name='libhish_probe_payload.so',data=Buffer.from('old generated probe'),ordinary=path.join(f.out,'libhish_shell.so');
 fs.writeFileSync(path.join(f.out,name),data);fs.writeFileSync(ordinary,'ordinary');assert.equal(f.run().status,0);
 assert(!fs.existsSync(path.join(f.out,name)));assert.deepEqual(fs.readFileSync(path.join(f.dir,'excluded-diagnostics',hash(data),name)),data);
 assert.equal(fs.readFileSync(ordinary,'utf8'),'ordinary');
});
test('retired BusyBox/dash payloads are archived, retaining source and bytes',()=>{
 const f=fixture();for(const name of ['libhish_linux_busybox.so','libhish_linux_dash.so'])fs.writeFileSync(path.join(f.out,name),name);
 assert.equal(f.run().status,0);
 for(const name of ['libhish_linux_busybox.so','libhish_linux_dash.so']){
  assert(!fs.existsSync(path.join(f.out,name)));
  assert.equal(fs.readFileSync(path.join(f.dir,'excluded-diagnostics',hash(Buffer.from(name)),name),'utf8'),name);
 }
});
test('former ordinary applications and their private dependencies are archived, not packaged or deleted',()=>{
 const f=fixture(),names=['libhish_linux_tcc.so','libhish_linux_git.so','libhish_linux_curl.so',
  'libhish_linux_git_remote_http.so','libhish_linux_gnu_printf.so','libcrypto3.so','libcurl4.so','libzstd1.so'];
 for(const name of names)fs.writeFileSync(path.join(f.out,name),name);
 assert.equal(f.run().status,0);
 for(const name of names){assert(!fs.existsSync(path.join(f.out,name)));
  assert.equal(fs.readFileSync(path.join(f.dir,'excluded-diagnostics',hash(Buffer.from(name)),name),'utf8'),name);}
});
test('manifest and relative policy dependencies generate one canonical CMake input',()=>{
 const f=fixture(),policy=path.join(repo,'scripts/daily-package-policy.json').replace(/\\/g,'/');
 fs.appendFileSync(path.join(f.dir,'check.cmake'),[
  '',`set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS [=[${policy}]=] [=[${path.dirname(policy)}/../scripts/daily-package-policy.json]=])`,
  `include([=[${path.join(repo,'feature/hish_main/src/main/cpp/configure_inputs.cmake').replace(/\\/g,'/')}]=])`,
  'get_property(INPUTS DIRECTORY PROPERTY CMAKE_CONFIGURE_DEPENDS)',
  'set(COUNT 0)',`foreach(INPUT \${INPUTS})`,`if(INPUT STREQUAL [=[${policy}]=])`,'math(EXPR COUNT "${COUNT}+1")','endif()','endforeach()',
  'if(NOT COUNT EQUAL 1)','message(FATAL_ERROR "Policy input is missing or duplicated")','endif()',''].join('\n'));
 const r=f.run();assert.equal(r.status,0,r.stdout+r.stderr);
});
