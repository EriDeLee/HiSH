// Builds the on-device check program for the emulated symbolic links (tests/fixtures/linux_links.c), once
// for musl and once for glibc. Copy the results into the app's home on the phone, make them executable and
// run them as described in docs/testing.md. Nothing here is packaged into the HAP.
// Usage: node scripts/linux/link-check-build.cjs NEW_OUTPUT_DIR [NATIVE_SDK] [--reference MUSL_SOURCE_DIR]
// --reference also builds linux_links_musl_orig: the same program with glob, nftw, ftw and scandir replaced by the
// unmodified musl sources under MUSL_SOURCE_DIR/src, so that their output can be compared with HiSH's versions.
const fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process');
const repo=path.resolve(__dirname,'../..'),fixture=path.join(repo,'.test/linux-tools'),program=path.join(repo,'tests/fixtures/linux_links.c');
const args=process.argv.slice(2),at=args.indexOf('--reference'),reference=at<0?null:path.resolve(args.splice(at,2)[1]||'');
const [output,sdkArgument]=args;
if(!output||(at>=0&&!reference))throw Error('Usage: link-check-build.cjs NEW_OUTPUT_DIR [NATIVE_SDK] [--reference MUSL_SOURCE_DIR]');
const sdk=sdkArgument||process.env.OHOS_NATIVE_SDK||(process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME?path.join(process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME,'sdk/default/openharmony/native'):null);
if(!sdk)throw Error('Pass the native SDK directory or set OHOS_NATIVE_SDK');
const clang=path.join(sdk,'llvm/bin/clang.exe'),builtins=path.join(sdk,'llvm/lib/clang/15.0.4/lib/aarch64-linux-ohos/libclang_rt.builtins.a');
const musl=path.join(fixture,'musl'),glibc=path.join(fixture,'glibc'),gnu=path.join(glibc,'usr/lib/aarch64-linux-gnu'),muslLib=path.join(musl,'usr/lib');
for(const needed of [musl,glibc,program,...(reference?[path.join(reference,'src/regex/glob.c')]:[])])if(!fs.existsSync(needed))throw Error('Missing '+needed+' (build the Linux runtime first: node scripts/linux/build.cjs)');
fs.mkdirSync(path.resolve(output));
const out=name=>path.join(path.resolve(output),name);
function run(args){const r=cp.spawnSync(clang,args,{encoding:'utf8',windowsHide:true});if(r.status!==0)throw Error(r.stdout+r.stderr);}
const common=['-fuse-ld=lld','-nostdlib','-O2','-fPIE','-fno-emulated-tls','-fno-stack-protector','-mno-outline-atomics','-Wl,-z,max-page-size=4096','-pie'];
const muslLink=(extra,sources,file)=>run(['--target=aarch64-linux-musl','--sysroot='+musl,...common,...extra,'-Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1',
  path.join(muslLib,'Scrt1.o'),path.join(muslLib,'crti.o'),...sources,'-I'+path.join(musl,'usr/include'),path.join(musl,'lib/libc.musl-aarch64.so.1'),builtins,path.join(muslLib,'crtn.o'),'-o',file]);
muslLink([],[program],out('linux_links_musl'));
run(['--target=aarch64-linux-gnu','--sysroot='+glibc,...common,'-Wl,--dynamic-linker=/lib/ld-linux-aarch64.so.1',path.join(gnu,'Scrt1.o'),path.join(gnu,'crti.o'),program,
  '-I'+path.join(glibc,'usr/include'),'-I'+path.join(glibc,'usr/include/aarch64-linux-gnu'),'-idirafter',path.join(sdk,'sysroot/usr/include'),'-idirafter',path.join(sdk,'sysroot/usr/include/aarch64-linux-ohos'),
  path.join(gnu,'libc.so.6'),path.join(gnu,'crtn.o'),'-o',out('linux_links_glibc')]);
if(reference)muslLink(['-w','-D_GNU_SOURCE=1','-Dglob=orig_glob','-Dglobfree=orig_globfree','-Dnftw=orig_nftw','-Dftw=orig_ftw','-Dscandir=orig_scandir','-D__strchrnul=strchrnul'],
  [program,...['regex/glob.c','misc/nftw.c','legacy/ftw.c','dirent/scandir.c'].map(name=>path.join(reference,'src',name))],out('linux_links_musl_orig'));
console.log(fs.readdirSync(path.resolve(output)).sort().map(name=>path.join(path.resolve(output),name)).join('\n'));
