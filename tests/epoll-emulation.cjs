// This phone's kernel ignores EPOLLONESHOT (docs/epoll.md): the always-writable stdout that Bun registers with it is reported on
// every wait, so Bun's event loop never sleeps. The Linux preload library emulates the flag and turns epoll_pwait2, which the
// app's seccomp filter kills, into epoll_pwait. These tests run the real bookkeeping on the host against a model of Linux's epoll
// and a model of the phone's, compile the glue for both C libraries with strict warnings, and pin the rules the design rests on.
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process');
const repo=path.resolve(__dirname,'..'),cpp=path.join(repo,'feature/hish_main/src/main/cpp'),root=path.join(repo,'.test/epoll-emulation');fs.mkdirSync(root,{recursive:true});
const run=(exe,args,options)=>cp.spawnSync(exe,args,{encoding:'utf8',windowsHide:true,maxBuffer:8e6,...options});
const read=file=>fs.readFileSync(file,'utf8');
const sdk=process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME||path.resolve(repo,'../command-line-tools');
const code=text=>text.split(/\r?\n/).filter(line=>!/^\s*\/\//.test(line)).join('\n');
test('the one-shot bookkeeping makes the phone\'s epoll look like Linux\'s (43 directed cases and a 120000-step random walk)',()=>{
  const locate=run('where.exe',['g++.exe']);assert.equal(locate.status,0,'Host tests require the existing g++ toolchain');
  const compiler=locate.stdout.trim().split(/\r?\n/)[0],bin=path.dirname(compiler);
  const version=run(compiler,['-dumpversion']).stdout.trim(),standard=path.resolve(bin,'../include/c++',version);
  const executable=path.join(root,'epoll-core.exe');
  const compiled=run(compiler,['-std=c++17','-Wall','-Wextra','-Werror','-O2','-isystem',standard,'-isystem',path.join(standard,'x86_64-w64-mingw32'),'-I'+cpp,path.join(__dirname,'fixtures/epoll_core.cpp'),'-static-libgcc','-static-libstdc++','-o',executable]);
  assert.equal(compiled.status,0,compiled.stdout+compiled.stderr);
  const result=run(executable,[]);
  assert.equal(result.status,0,result.stdout+result.stderr);
  assert.match(result.stdout,/EPOLL_CORE_PASS/);
  assert.match(result.stdout,/random walk: \d+ steps, 0 mismatches/);
  assert.equal(result.stdout.split('\n').filter(line=>line.startsWith('ok ')).length,44,'every case must run');
});
test('the epoll fixes compile with strict warnings for musl and glibc',()=>{
  const musl=path.join(repo,'.test/linux-tools/musl'),glibc=path.join(repo,'.test/linux-tools/glibc');
  assert(fs.existsSync(musl)&&fs.existsSync(glibc),'Build the Linux runtime first (node scripts/linux/build.cjs)');
  const native=path.join(sdk,'sdk/default/openharmony/native'),clang=path.join(native,'llvm/bin/clang.exe');
  const compile=(label,args)=>{const r=run(clang,[...args,'-fsyntax-only','-Wall','-Wextra','-Werror','-O2']);if(r.error)throw r.error;assert.equal(r.status,0,label+'\n'+r.stdout+r.stderr);};
  const gnu=['-I'+cpp,'-I'+path.join(glibc,'usr/include'),'-I'+path.join(glibc,'usr/include/aarch64-linux-gnu'),'-idirafter',path.join(native,'sysroot/usr/include'),'-idirafter',path.join(native,'sysroot/usr/include/aarch64-linux-ohos')];
  for(const file of [path.join(cpp,'linux/epoll.c'),path.join(cpp,'linux/links.c')]){
    compile('musl '+path.basename(file),['--target=aarch64-linux-musl','--sysroot='+musl,'-fPIC','-I'+cpp,'-I'+path.join(musl,'usr/include'),file]);
    compile('glibc '+path.basename(file),['--target=aarch64-linux-gnu','--sysroot='+glibc,'-fPIC',...gnu,file]);
  }
  const check=path.join(__dirname,'fixtures/linux_epoll.c');
  compile('musl linux_epoll.c',['--target=aarch64-linux-musl','--sysroot='+musl,'-I'+path.join(musl,'usr/include'),check]);
  compile('glibc linux_epoll.c',['--target=aarch64-linux-gnu','--sysroot='+glibc,...gnu,check]);
});
test('the on-device epoll check program links for both C libraries',()=>{
  const dir=path.join(root,'check-'+process.pid+'-'+Date.now());
  const r=run(process.execPath,[path.join(repo,'scripts/linux/link-check-build.cjs'),dir,path.join(sdk,'sdk/default/openharmony/native')]);
  assert.equal(r.status,0,r.stdout+r.stderr);
  for(const [name,interpreter] of [['linux_epoll_musl','/lib/ld-musl-aarch64.so.1'],['linux_epoll_glibc','/lib/ld-linux-aarch64.so.1']]){
    const bytes=fs.readFileSync(path.join(dir,name));
    assert.equal(bytes.readUInt32BE(0),0x7f454c46,name+' is an ELF file');assert.equal(bytes.readUInt16LE(18),183,name+' is AArch64');
    assert(bytes.includes(Buffer.from(interpreter+'\0')),name+' asks for '+interpreter);
  }
});
test('the design rules hold: the core makes no system calls, epoll.c cannot call itself, links.c asks it first, both libraries build it',()=>{
  const core=read(path.join(cpp,'hish_epoll_core.h')),glue=code(read(path.join(cpp,'linux/epoll.c'))),links=read(path.join(cpp,'linux/links.c')),build=read(path.join(repo,'scripts/linux/build.cjs'));
  // The bookkeeping is shared with the host test, so it may not reach the operating system.
  assert.deepEqual([...core.matchAll(/^#include <([^>]+)>/gm)].map(m=>m[1]),['stddef.h','stdint.h','string.h']);
  assert(!/\b(syscall|malloc|calloc|free|getenv|dlsym|pthread_[a-z_]+)\s*\(/.test(code(core)),'hish_epoll_core.h must be bookkeeping only');
  // epoll.c replaces syscall()-level callers' epoll calls, so it must reach the kernel with its own svc, never through syscall().
  assert(!/\bsyscall\s*\(/.test(glue),'epoll.c must not call syscall()');
  assert(/dlsym\(RTLD_NEXT, "epoll_pwait"\)/.test(glue),'epoll_pwait keeps the C library\'s own behaviour');
  const defined=[...glue.matchAll(/^(?:__attribute__\(\(visibility\("default"\)\)\) )?int ((?:hish_)?epoll_[a-z0-9_]+)\(/gm)].map(m=>m[1]).sort();
  assert.deepEqual(defined,['epoll_create','epoll_create1','epoll_ctl','epoll_pwait','epoll_pwait2','epoll_wait','hish_epoll_syscall'],'the library defines exactly these functions');
  // links.c owns syscall(); it must offer every call to epoll.c first, even when the link emulation is switched off.
  assert(/extern int hish_epoll_syscall\([^)]*\) __attribute__\(\(weak\)\);/.test(links),'the hook is weak, so links.c stays linkable without epoll.c');
  const body=links.slice(links.indexOf('long syscall(long number, ...)'));
  assert(body.indexOf('hish_epoll_syscall(number')>0&&body.indexOf('hish_epoll_syscall(number')<body.indexOf('if (!enabled)'),'syscall() asks epoll.c before the link switch');
  // Both Linux preload libraries carry it, and the runtime build tracks the files.
  const command=end=>{const stop=build.indexOf(end);assert(stop>0,end+' is built');return build.slice(build.lastIndexOf("run(['--target=",stop),stop);};
  assert(command('libhish_linux_identity_padded.so').includes('linux/epoll.c'),'build.cjs must compile epoll.c into the musl library');
  assert(command('libhish_linux_glibc_tty.so').includes('linux/epoll.c'),'build.cjs must compile epoll.c into the glibc library');
  for(const name of ['linux/epoll.c','hish_epoll_core.h'])assert(build.includes("'"+name+"'"),'the runtime manifest must list '+name+' as an input');
  // It is for Linux programs only: the HAP's own libraries do not take it.
  assert(!/epoll/.test(read(path.join(cpp,'CMakeLists.txt'))),'CMake must not build the epoll fixes into the HAP libraries');
});
