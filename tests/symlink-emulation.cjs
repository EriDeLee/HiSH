// The device refuses symlink(2) everywhere an app can write, so links are emulated:
// a small sticky-bit file holds the target, and HiSH's libc layers show it as a link.
// These tests run the real resolver and marker code on the host, compile both libc
// layers (Linux programs and HiSH's own library) with strict warnings, and pin the
// rules that keep those layers from calling themselves.
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process');
const repo=path.resolve(__dirname,'..'),cpp=path.join(repo,'feature/hish_main/src/main/cpp'),root=path.join(repo,'.test/symlink-emulation');fs.mkdirSync(root,{recursive:true});
const run=(exe,args,options)=>cp.spawnSync(exe,args,{encoding:'utf8',windowsHide:true,maxBuffer:8e6,...options});
const read=file=>fs.readFileSync(file,'utf8');
const sdk=process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME||path.resolve(repo,'../command-line-tools');
const code=text=>text.split(/\r?\n/).filter(line=>!/^\s*\/\//.test(line)).join('\n');
test('marker format and link resolution behave like POSIX symlinks (37 cases)',()=>{
  const locate=run('where.exe',['g++.exe']);assert.equal(locate.status,0,'Host tests require the existing g++ toolchain');
  const compiler=locate.stdout.trim().split(/\r?\n/)[0],bin=path.dirname(compiler);
  const version=run(compiler,['-dumpversion']).stdout.trim(),standard=path.resolve(bin,'../include/c++',version);
  const executable=path.join(root,'link-core.exe');
  const compiled=run(compiler,['-std=c++17','-Wall','-Wextra','-Werror','-isystem',standard,'-isystem',path.join(standard,'x86_64-w64-mingw32'),'-I'+cpp,path.join(__dirname,'fixtures/link_core.cpp'),'-static-libgcc','-static-libstdc++','-o',executable]);
  assert.equal(compiled.status,0,compiled.stdout+compiled.stderr);
  const result=run(executable,[]);
  assert.equal(result.status,0,result.stdout+result.stderr);
  assert.match(result.stdout,/LINK_CORE_PASS/);
  assert.equal(result.stdout.split('\n').filter(line=>line.startsWith('ok ')).length,37,'every case must run');
});
test('every build of the layer compiles with strict warnings',()=>{
  const musl=path.join(repo,'.test/linux-tools/musl'),glibc=path.join(repo,'.test/linux-tools/glibc');
  assert(fs.existsSync(musl)&&fs.existsSync(glibc),'Build the Linux runtime first (node scripts/linux/build.cjs)');
  const native=path.join(sdk,'sdk/default/openharmony/native'),clang=path.join(native,'llvm/bin/clang.exe');
  const compile=(label,args)=>{const r=run(clang,[...args,'-fsyntax-only','-Wall','-Wextra','-Werror']);if(r.error)throw r.error;assert.equal(r.status,0,label+'\n'+r.stdout+r.stderr);};
  const linux=(file,extra=[])=>compile('musl '+file,['--target=aarch64-linux-musl','--sysroot='+musl,...extra,'-O2','-fPIC','-I'+cpp,'-I'+path.join(musl,'usr/include'),path.join(cpp,file)]);
  linux('linux/links.c');
  linux('linux/exec.c');
  linux('linux/terminal.c');
  // glibc programs get the same layer, with the versioned stat entry points and the fortified open variants.
  const gnu=['-I'+cpp,'-I'+path.join(glibc,'usr/include'),'-I'+path.join(glibc,'usr/include/aarch64-linux-gnu'),'-idirafter',path.join(native,'sysroot/usr/include'),'-idirafter',path.join(native,'sysroot/usr/include/aarch64-linux-ohos')];
  compile('glibc links.c',['--target=aarch64-linux-gnu','--sysroot='+glibc,'-O2','-fPIC',...gnu,path.join(cpp,'linux/links.c')]);
  const ohos=(target,extra)=>compile('links_ohos.c '+target+' '+extra.join(' '),['--target='+target,'--sysroot='+path.join(native,'sysroot'),'-D_GNU_SOURCE',...extra,'-I'+cpp,path.join(cpp,'links_ohos.c')]);
  // Hidden inside libhish_shell.so, and exported from libhish_exec_bridge.so for the system applets.
  ohos('aarch64-linux-ohos',['-Dexit=HiSHChildExit','-Dpipe=HiSHPipe']);
  ohos('aarch64-linux-ohos',['-DHISH_LINKS_EXPORT=1']);
  // The HAP also builds the x86_64 emulator ABI, where the file must compile to (nearly) nothing.
  ohos('x86_64-linux-ohos',[]);
  ohos('x86_64-linux-ohos',['-DHISH_LINKS_EXPORT=1']);
  // The on-device check program must keep compiling for both C libraries.
  compile('musl linux_links.c',['--target=aarch64-linux-musl','--sysroot='+musl,'-O2','-I'+path.join(musl,'usr/include'),path.join(__dirname,'fixtures/linux_links.c')]);
  compile('glibc linux_links.c',['--target=aarch64-linux-gnu','--sysroot='+glibc,'-O2',...gnu,path.join(__dirname,'fixtures/linux_links.c')]);
});
test('the on-device check program links for both C libraries',()=>{
  const dir=path.join(root,'linkcheck-'+process.pid+'-'+Date.now());
  const r=run(process.execPath,[path.join(repo,'scripts/linux/link-check-build.cjs'),dir,path.join(sdk,'sdk/default/openharmony/native')]);
  assert.equal(r.status,0,r.stdout+r.stderr);
  for(const [name,interpreter] of [['linux_links_musl','/lib/ld-musl-aarch64.so.1'],['linux_links_glibc','/lib/ld-linux-aarch64.so.1']]){
    const bytes=fs.readFileSync(path.join(dir,name));
    assert.equal(bytes.readUInt32BE(0),0x7f454c46,name+' is an ELF file');assert.equal(bytes.readUInt16LE(18),183,name+' is AArch64');
    assert(bytes.includes(Buffer.from(interpreter+'\0')),name+' asks for '+interpreter);
  }
});
test('no layer calls the syscall() it replaces, and the shared logic has one home',()=>{
  const impl=code(read(path.join(cpp,'hish_links_impl.h'))),links=code(read(path.join(cpp,'linux/links.c'))),own=code(read(path.join(cpp,'links_ohos.c'))),libc=code(read(path.join(cpp,'hish_links_libc.h')));
  // Replacing syscall() is how raw callers are covered, so the code must reach the kernel with its own svc instead.
  assert.equal([...impl.matchAll(/\bsyscall\s*\(/g)].length,0,'shared logic must not call syscall');
  assert.equal([...libc.matchAll(/\bsyscall\s*\(/g)].length,0,'the libc functions must not call syscall');
  assert.equal([...links.matchAll(/\bsyscall\s*\(/g)].length,1);assert(/long syscall\(long number, \.\.\.\)/.test(links));
  assert.equal([...own.matchAll(/\bsyscall\s*\(/g)].length,1);assert(/HIDDEN long syscall\(long number, \.\.\.\)/.test(own));
  for(const name of ['openat','newfstatat','statx','faccessat2','readlinkat','symlinkat','unlinkat','mkdirat','renameat2','fchmodat','fchownat','utimensat','execve'])
    assert(new RegExp('case HL_'+name.toUpperCase()+':').test(links),'syscall() in links.c does not route '+name);
  // open, openat, readlink and readlinkat belong to terminal.c on the Linux side; a second definition would not link.
  const defined=text=>[...text.matchAll(/^[A-Za-z_][^\n(=;]*\b([a-z_0-9]+)\(/gm)].map(m=>m[1]);
  for(const name of ['open','openat','readlink','readlinkat'])assert(!defined(links.replace(/^static .*$/mg,'')).includes(name),name+' is defined in terminal.c');
  // Nothing outside the layer may need dlsym to find realpath again (glibc keeps dlsym elsewhere on old versions).
  assert(!/dlsym\(RTLD_NEXT,\s*"realpath"\)/.test(links+impl),'realpath is built on /proc/self/fd, not on dlsym');
  // Every function links_ohos.c defines goes through HIDDEN: hidden in libhish_shell.so (it must capture calls inside that
  // library and export nothing), exported only in the HISH_LINKS_EXPORT build for the system applets.
  const bodies=own.split(/\n(?=HIDDEN )/).slice(1);assert(bodies.length>=30,'expected the whole libc surface');
  assert.equal(own.split('\n').filter(line=>/^[A-Za-z_].*\)\s*(\{|$)/.test(line)&&!/^(HIDDEN|static|#|__attribute__\(\(visibility\("default"\)\)\) void HishLinksEnableForApplet)/.test(line)).length,0,'every definition in links_ohos.c must be HIDDEN');
  assert(/#ifdef HISH_LINKS_EXPORT\s*\n#define HISH_LINKS_START_DISABLED 1\s*\n#define HIDDEN __attribute__\(\(visibility\("default"\)\)\)\s*\n#else\s*\n#define HIDDEN __attribute__\(\(visibility\("hidden"\)\)\)\s*\n#endif/.test(own),'only the export build may make the layer visible');
  assert(/#ifdef HISH_LINKS_START_DISABLED\s*\nstatic int enabled = 0, tracing;/.test(impl)&&/#ifndef HISH_LINKS_START_DISABLED\s*\n__attribute__\(\(constructor\)\) static void links_init/.test(impl),'the exported copy starts off and has no constructor that could turn it on');
  const cmake=read(path.join(cpp,'CMakeLists.txt'));
  assert(/add_library\(hish_shell SHARED [^\n]*\blinks_ohos\.c\b/.test(cmake),'links_ohos.c must be part of the shell library');
  assert(/add_library\(hish_links_export OBJECT links_ohos\.c\)/.test(cmake)&&/target_compile_definitions\(hish_links_export PRIVATE [^\n]*HISH_LINKS_EXPORT=1\)/.test(cmake)&&/add_library\(hish_exec_bridge SHARED exec_bridge\.cpp \$<TARGET_OBJECTS:hish_links_export>\)/.test(cmake),'the exported copy must be compiled into the bridge library');
  const build=read(path.join(repo,'scripts/linux/build.cjs'));
  // The compile command of each library is the run([...]) call that ends with the library's name.
  const command=end=>{const stop=build.indexOf(end);assert(stop>0,end+' is built');return build.slice(build.lastIndexOf("run(['--target=",stop),stop);};
  assert(command('libhish_linux_identity_padded.so').includes('linux/links.c'),'build.cjs must compile links.c into the identity library');
  assert(command('libhish_linux_glibc_tty.so').includes('linux/links.c'),'build.cjs must compile links.c into the glibc library');
  for(const name of ['linux/links.c','hish_link_core.h','hish_links_impl.h','hish_links_libc.h'])assert(build.includes("'"+name+"'"),'the runtime manifest must list '+name+' as an input');
  const exec=read(path.join(cpp,'linux/exec.c'));
  assert(/extern const char\* HishLinkExecutablePath\([^)]*\) __attribute__\(\(weak\)\);/.test(exec)&&/if\(HishLinkExecutablePath\)path=HishLinkExecutablePath\(path,followed,sizeof\(followed\)\);/.test(exec),'exec.c must follow a link before the loader reads the file, and stay linkable without links.c');
});
