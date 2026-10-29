const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process');
const repo=path.resolve(__dirname,'..'),sdk=process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME||path.resolve(repo,'../command-line-tools');
test('base has no app registry and refuses only exact obsolete forwarding scripts',()=>{
 const r=cp.spawnSync(path.join(sdk,'sdk/default/openharmony/native/llvm/bin/clang.exe'),[
  '--target=aarch64-linux-musl','--sysroot='+path.join(repo,'.test/linux-tools/musl'),
  '-I'+path.join(repo,'.test/linux-tools/musl/usr/include'),'-I'+path.join(repo,'feature/hish_main/src/main/cpp'),
  '-O3','-mllvm','-inline-threshold=10000','-mllvm','-unroll-threshold=10000',
  '-S','-emit-llvm',path.join(__dirname,'fixtures/base_tool_contract.c'),'-o','-'],
  {encoding:'utf8',windowsHide:true,maxBuffer:4*1024*1024});
 if(r.error)throw r.error;assert.equal(r.status,0,r.stderr);
 const body=r.stdout.match(/define[^\n]*@base_tool_contract\([^]*?\n}/)?.[0];assert(body,r.stdout);
 assert.match(body,/ret i32 0/);assert.equal((body.match(/\bret\b/g)||[]).length,1,body);
 assert(!/\bcall\b|\bbr\b/.test(body),'Contract did not fully evaluate at compile time: '+body);
});
test('base policy retains only the six generic runtime libraries',()=>{
 const policy=require('../scripts/daily-package-policy.json');
 assert.deepEqual([...policy.arm64Runtime].sort(),['libgcc_s1.so','libhish_linux_glibc_compat.so',
  'libhish_linux_glibc_loader.so','libhish_linux_glibc_tty.so','libhish_linux_identity_padded.so','libhish_linux_musl_loader.so'].sort());
});
