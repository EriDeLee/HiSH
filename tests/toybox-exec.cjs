// Programs started by Toybox must reach the Linux loader. The embedded Toybox (today
// xargs is its only applet that starts another program) does so in one place,
// xexec(), whose execvp the build renames to a HiSH function. env, timeout, nice,
// nohup, setsid, taskset, time and find come from the system toybox instead; like
// every other system applet they run with libhish_exec_bridge.so preloaded, which
// gives them the same execvp and the emulated-link layer. These tests pin that design,
// compile the changed C and C++ against the OHOS headers and run the real PATH
// lookup and environment helpers on the host.
const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process');
const repo=path.resolve(__dirname,'..'),cpp=path.join(repo,'feature/hish_main/src/main/cpp'),toybox=path.join(repo,'third_party/toybox'),root=path.join(repo,'.test/toybox-exec');fs.mkdirSync(root,{recursive:true});
const run=(exe,args,options)=>cp.spawnSync(exe,args,{encoding:'utf8',windowsHide:true,maxBuffer:8e6,...options});
const read=file=>fs.readFileSync(file,'utf8');
const sdk=process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME||path.resolve(repo,'../command-line-tools');
const native=path.join(sdk,'sdk/default/openharmony/native');
const ohos=['--target=aarch64-linux-ohos','--sysroot='+path.join(native,'sysroot'),'-fsyntax-only','-Wall','-Wextra','-Werror','-D_GNU_SOURCE','-I'+cpp];
// The same files CMake compiles: main.c, lib/*.c and toys/*/*.c.
function compiledSources(){
  const files=[path.join(toybox,'main.c')],c=dir=>fs.readdirSync(dir).filter(name=>name.endsWith('.c')).map(name=>path.join(dir,name));
  files.push(...c(path.join(toybox,'lib')));
  for(const group of fs.readdirSync(path.join(toybox,'toys'),{withFileTypes:true}))if(group.isDirectory())files.push(...c(path.join(toybox,'toys',group.name)));
  return files;
}
// Builds a fixture with the host g++ (as elf-dormant-heap does) and returns its path.
function hostFixture(name){
  const locate=run('where.exe',['g++.exe']);assert.equal(locate.status,0,'Host tests require the existing g++ toolchain');
  const compiler=locate.stdout.trim().split(/\r?\n/)[0],bin=path.dirname(compiler);
  const version=run(compiler,['-dumpversion']).stdout.trim(),standard=path.resolve(bin,'../include/c++',version);
  const executable=path.join(root,name+'.exe');
  const compiled=run(compiler,['-std=c++17','-Wall','-Wextra','-Werror','-isystem',standard,'-isystem',path.join(standard,'x86_64-w64-mingw32'),'-I'+cpp,path.join(__dirname,'fixtures',name+'.cpp'),'-static-libgcc','-static-libstdc++','-o',executable]);
  assert.equal(compiled.status,0,compiled.stdout+compiled.stderr);
  return executable;
}
test('Toybox starts other programs only in xexec, and the build redirects that execvp',()=>{
  const calls=[];
  for(const file of compiledSources())read(file).split(/\r?\n/).forEach(line=>{
    const found=!/^\s*(\/\/|\/\*|\*)/.test(line)&&line.match(/\b(exec(?:v|vp|ve|l|lp|le|vpe)|posix_spawnp?)\s*\(/);
    if(found)calls.push(path.relative(toybox,file).replace(/\\/g,'/')+' '+found[1]);
  });
  assert.deepEqual(calls.sort(),['lib/xwrap.c execv','lib/xwrap.c execv','lib/xwrap.c execvp'],'a new exec path would bypass the redirect');
  const cmake=read(path.join(cpp,'CMakeLists.txt'));
  assert(cmake.includes('set_source_files_properties("${TOYBOX}/lib/xwrap.c" PROPERTIES COMPILE_DEFINITIONS "execvp=HiSHToyboxExecvp")'));
  assert(/add_library\(hish_shell SHARED [^\n]*\btoybox_adapter\.c\b/.test(cmake));
});
test('every system applet runs with the bridge library preloaded',()=>{
  const shell=read(path.join(cpp,'shell_module.c')),bridge=read(path.join(cpp,'exec_bridge.cpp'));
  // The applets that start programs need the exec hooks and all of them need the link layer, so there is no name list.
  assert(!/starts_programs/.test(shell),'no applet may be left out of the preloaded launch');
  assert(shell.indexOf('HiSHExecSystemApplet(system)')>0&&shell.indexOf('HiSHExecSystemApplet(system)')<shell.indexOf('HiSHExecFile("/system/bin/toybox"'),'the preloaded launch must come first, the plain launch is only the fallback');
  assert(!/if\s*\([^)]*\)\s*HiSHExecSystemApplet/.test(shell),'the preloaded launch is unconditional');
  // The link layer in the bridge library starts off and is switched on only inside a system applet.
  const marker=bridge.indexOf('getenv(HISH_SYSTEM_APPLET_MARKER)');
  assert(marker>0&&/HishRestoreSystemAppletEnvironment\(environ\);HishLinksEnableForApplet\(\);return;/.test(bridge.slice(marker,marker+200)),'the marker branch restores the environment, then enables the links');
  const declared=bridge.indexOf('extern "C" void HishLinksEnableForApplet(void);');assert(declared>0&&declared<marker,'the link switch is declared before it is used');
  assert(bridge.split('HishLinksEnableForApplet();').length===2,'only the system applet branch may enable the links');
  assert(/int execvp\(const char\* file,char\* const argv\[\]\)\{return HiSHToyboxExecvp\(file,argv\);\}/.test(bridge),'bridge must export execvp');
  assert(/int execve\(const char\* path,char\* const argv\[\],char\* const envp\[\]\)\{return HiSHMkshExecve\(path,argv,envp\);\}/.test(bridge),'bridge must export execve, which env calls itself');
  assert(/pid_t vfork\(void\)\{return fork\(\);\}/.test(bridge),'bridge must turn vfork into fork');
  // The loader's own exec calls must never go through libc's name, which the preload now replaces.
  const loader=read(path.join(cpp,'native_elf.cpp')).split(/\r?\n/).filter(line=>!/^\s*\/\//.test(line)).join('\n');
  assert(!/\bexecve\s*\(/.test(loader),'native_elf.cpp must use KernelExecve, not libc execve');
  assert(/long KernelExecve\([^)]*\)\{return syscall\(SYS_execve,path,argv,envp\);\}/.test(loader));
  assert(bridge.indexOf('HISH_SYSTEM_APPLET_MARKER')>0&&bridge.indexOf('HISH_SYSTEM_APPLET_MARKER')<bridge.indexOf('HISH_PROOT_SCRIPT'),'the marker is handled before any other startup request');
});
test('the changed C and C++ compile against the OHOS headers with the build definitions',()=>{
  const clang=name=>path.join(native,'llvm/bin',name);
  const check=(compiler,file,extra)=>{const r=run(clang(compiler),[...ohos,...extra,path.join(cpp,file)]);if(r.error)throw r.error;assert.equal(r.status,0,file+'\n'+r.stdout+r.stderr);};
  check('clang.exe','toybox_adapter.c',['-Dexecvp=HiSHToyboxExecvp']);
  check('clang.exe','shell_module.c',['-Dexit=HiSHChildExit','-Dpipe=HiSHPipe','-include',path.join(cpp,'shell_toybox_options.h')]);
  check('clang++.exe','exec_bridge.cpp',['-std=c++17']);
});
test('command lookup follows execvp: PATH order, empty entries, directories and unusable files',()=>{
  const executable=hostFixture('toybox_exec_path');
  const scratch=fs.mkdtempSync(path.join(root,'case-'));
  for(const file of ['a/tool','b/tool','b/both','noexec/only','noexec/both','here']){fs.mkdirSync(path.dirname(path.join(scratch,file)),{recursive:true});fs.writeFileSync(path.join(scratch,file),'x');}
  fs.mkdirSync(path.join(scratch,'dirs/tool'),{recursive:true});
  const result=run(executable,[],{cwd:scratch});
  assert.equal(result.status,0,result.stdout+result.stderr);
  assert.match(result.stdout,/TOYBOX_EXEC_PASS/);
  assert.equal(result.stdout.split('\n').filter(line=>line.startsWith('ok ')).length,21,'every case must run');
});
test('a system applet gets the app preload for its own startup and the caller environment back',()=>{
  const result=run(hostFixture('system_applet_env'),[]);
  assert.equal(result.status,0,result.stdout+result.stderr);
  assert.match(result.stdout,/SYSTEM_APPLET_ENV_PASS/);
  assert.equal(result.stdout.split('\n').filter(line=>line.startsWith('ok ')).length,14,'every case must run');
});
