const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process');
const repo=path.resolve(__dirname,'..'),cpp=path.join(repo,'feature/hish_main/src/main/cpp'),{hash}=require('../scripts/linux/source-runtime.cjs');
test('supervisor reaps adopted exits without touching live shell jobs, ten rounds',()=>{
 const sdk=process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME||path.resolve(repo,'../command-line-tools');
 const r=cp.spawnSync(path.join(sdk,'sdk/default/openharmony/native/llvm/bin/clang++.exe'),['-std=c++17','-fsyntax-only','-Wall','-Wextra','-Werror','-I'+cpp,path.join(__dirname,'fixtures/supervisor_reap.cpp')],{encoding:'utf8',windowsHide:true,maxBuffer:4*1024*1024});if(r.error)throw r.error;assert.equal(r.status,0,r.stdout+r.stderr);
});
test('vendored mksh matches every pinned upstream byte',()=>{
 const root=path.join(repo,'third_party/mksh'),manifest=JSON.parse(fs.readFileSync(path.join(root,'hish-source-manifest.json'),'utf8'));
 assert.equal(manifest.commit,'88b0a4a12f2c219c1421ce5a3b90b97b785a08ad');for(const item of manifest.files)assert.equal(hash(fs.readFileSync(path.join(root,item.name))),item.sha256,item.name);
});
test('mksh license terms accompany the application',()=>{
 for(const [from,to] of [['COPYRIGHT','mksh-COPYRIGHT.txt'],['TaC-mksh.txt','mksh-terms.txt']])assert.deepEqual(fs.readFileSync(path.join(repo,'third_party/mksh',from)),fs.readFileSync(path.join(repo,'feature/hish_main/src/main/resources/rawfile/licenses',to)));
});
test('native OHOS terminal object does not depend on unavailable RTLD_NEXT lookup',()=>{
 const sdk=process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME||path.resolve(repo,'../command-line-tools');
 const file=path.join(repo,'feature/hish_main/.cxx/default/default/debug/arm64-v8a/CMakeFiles/hish_ohos_tty.dir/linux/terminal.c.o');
 assert(fs.existsSync(file),'Build the ordinary native target first');
 const r=cp.spawnSync(path.join(sdk,'sdk/default/openharmony/native/llvm/bin/llvm-nm.exe'),['--undefined-only',file],{encoding:'utf8',windowsHide:true,maxBuffer:4*1024*1024});if(r.error)throw r.error;assert.equal(r.status,0,r.stderr);
 assert(!/\bdlsym\b|\bpthread_once\b/.test(r.stdout),r.stdout);assert(/\bsyscall\b/.test(r.stdout));
});
test('ordinary native library contains mksh but not mrsh or trial instrumentation',()=>{
 const sdk=process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME||path.resolve(repo,'../command-line-tools');
 const lib=path.join(repo,'feature/hish_main/build/default/intermediates/cmake/default/obj/arm64-v8a/libhish_shell.so');
 const r=cp.spawnSync(path.join(sdk,'sdk/default/openharmony/native/llvm/bin/llvm-nm.exe'),['--defined-only',lib],{encoding:'utf8',windowsHide:true,maxBuffer:16*1024*1024});if(r.error)throw r.error;assert.equal(r.status,0,r.stderr);
 assert(/\bHiSHMkshMain\b/.test(r.stdout));assert(!/\bHiSHMrshMain\b|\bHiSHStartMkshTrial\b|\b__cyg_profile_func_enter\b|\b(?:ash|bash|sh)_main\b/.test(r.stdout));
 for(const name of ['sleep_main','printf_main','test_main'])assert(new RegExp('\\b'+name+'\\b').test(r.stdout),'Enabled utility omitted from linked table: '+name);
});
