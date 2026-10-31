// The fixed-address loader may reclaim the dormant static ArkTS heap that a
// terminal session inherits from the app. This compiles the real header on the
// host and checks that nothing else in the same address range is ever released.
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process');
const repo=path.resolve(__dirname,'..'),root=path.join(repo,'.test/elf-dormant-heap');fs.mkdirSync(root,{recursive:true});
const run=(exe,args)=>cp.spawnSync(exe,args,{encoding:'utf8',windowsHide:true,maxBuffer:8e6});
test('only a verified dormant ArkTS reservation is released for a fixed-address image',()=>{
 const locate=run('where.exe',['g++.exe']);assert.equal(locate.status,0,'Host loader tests require the existing g++ toolchain');
 const compiler=locate.stdout.trim().split(/\r?\n/)[0],bin=path.dirname(compiler);
 const version=run(compiler,['-dumpversion']).stdout.trim(),standard=path.resolve(bin,'../include/c++',version);
 const executable=path.join(root,'elf-dormant-heap.exe');
 const compiled=run(compiler,['-std=c++17','-Wall','-Wextra','-Werror','-isystem',standard,'-isystem',path.join(standard,'x86_64-w64-mingw32'),'-I'+path.join(repo,'feature/hish_main/src/main/cpp'),path.join(__dirname,'fixtures/elf_dormant_heap.cpp'),'-static-libgcc','-static-libstdc++','-o',executable]);
 assert.equal(compiled.status,0,compiled.stdout+compiled.stderr);
 const result=run(executable,[]);
 assert.equal(result.status,0,result.stdout+result.stderr);
 assert.match(result.stdout,/DORMANT_HEAP_PASS/);
 assert.equal(result.stdout.split('\n').filter(l=>l.startsWith('ok ')).length,12,'every case must run');
});
