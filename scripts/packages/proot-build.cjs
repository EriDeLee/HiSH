// Reproducible platform port. Upstream sources and build-only headers are
// hash-pinned; adaptation is explicit, never labelled as an upstream APK.
const fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process'),crypto=require('node:crypto'),assert=require('node:assert/strict');
const lock=require('./proot-lock.json'),repo=path.resolve(__dirname,'../..'),cache=path.join(repo,'.test/packages/proot'),hash=b=>crypto.createHash('sha256').update(b).digest('hex');
async function buildPRoot(sdk,archive){
 fs.mkdirSync(cache,{recursive:true});const downloaded=[];
 for(const item of lock.sources){const file=path.join(cache,item.file);if(!fs.existsSync(file)){const r=await fetch(item.url);assert(r.ok,item.url);const b=Buffer.from(await r.arrayBuffer());assert.equal(hash(b),item.sha256,item.file);fs.writeFileSync(file,b);}const bytes=fs.readFileSync(file);assert.equal(hash(bytes),item.sha256,item.file);downloaded.push([item,bytes]);}
 const musl=path.join(repo,'.test/linux-tools/musl'),lib=path.join(musl,'usr/lib'),clang=path.join(sdk,'llvm/bin/clang.exe');
 const inputs=[__filename,...['hish_platform.c','hish_platform.h','probe.c'].map(n=>path.join(__dirname,'proot',n)),clang,...['Scrt1.o','crti.o','crtn.o'].map(n=>path.join(lib,n)),path.join(musl,'lib/libc.musl-aarch64.so.1')].map(n=>[n,hash(fs.readFileSync(n))]);
 const fingerprint=hash(JSON.stringify({lock,inputs})),build=path.join(cache,fingerprint),manifest=path.join(build,'manifest.json');
 if(fs.existsSync(manifest)){const m=JSON.parse(fs.readFileSync(manifest));assert.equal(m.fingerprint,fingerprint);for(const[n,file]of [['program','proot-hish'],['probe','proot-probe']]){assert.equal(path.resolve(m[n].file),path.join(build,file));assert.equal(hash(fs.readFileSync(m[n].file)),m[n].sha256,'Cached PRoot '+n);}return m;}
 fs.mkdirSync(build,{recursive:true});
 for(const[item,bytes]of downloaded)for(const[n,b]of archive(bytes)){if(!n.startsWith(item.prefix))continue;const relative=n.slice(item.prefix.length);assert(relative&&!relative.split('/').includes('..')&&!relative.includes('\\'));const dest=path.join(build,item.directory,relative);fs.mkdirSync(path.dirname(dest),{recursive:true});fs.writeFileSync(dest,b);}
 const src=path.join(build,'source/src');
 function patch(name,from,to){const file=path.join(src,name),text=fs.readFileSync(file,'utf8');assert.equal(text.split(from).length,2,name+' source anchor');fs.writeFileSync(file,text.replace(from,to));}
 for(const n of ['hish_platform.c','hish_platform.h'])fs.copyFileSync(path.join(__dirname,'proot',n),path.join(src,n));
 patch('cli/cli.c','#include "cli/cli.h"','#include "cli/cli.h"\n#include "hish_platform.h"');
 patch('cli/cli.c','    status = initialize_bindings(tracee);','    if(hish_platform_bindings(tracee)<0)return -1;\n    status = initialize_bindings(tracee);');
 patch('cli/cli.c','    talloc_enable_leak_report();','    setenv("PROOT_NO_SECCOMP","1",1);\n    if(!getenv("PROOT_TMP_DIR")&&getenv("TMPDIR"))setenv("PROOT_TMP_DIR",getenv("TMPDIR"),1);\n    talloc_enable_leak_report();');
 patch('path/path.c','#include "path/path.h"','#include "path/path.h"\n#include "hish_platform.h"');
 patch('path/path.c','    /* Use "/" as the base if it is an absolute guest path. */','    if(hish_platform_path(user_path,result))return 0;\n    /* Use "/" as the base if it is an absolute guest path. */');
 patch('tracee/event.c','#include "tracee/event.h"','#include "tracee/event.h"\n#include <sys/syscall.h>\nextern char **environ;');
 patch('tracee/event.c','execvp(tracee->exe, argv[0] != NULL ? argv : default_argv);','syscall(SYS_execve,tracee->exe,argv[0] != NULL ? argv : default_argv,environ);');
 patch('execve/enter.c','#include "arch.h"','#include "arch.h"\n#include "hish_platform.h"');
 patch('execve/enter.c','\tstatus = extract_load_info(tracee, tracee->load_info->interp);',`
        const char *native=getenv("HISH_NATIVE_LIBDIR");
        const char *loader=strstr(tracee->load_info->interp->host_path,"ld-musl-aarch64")?"libhish_linux_musl_loader.so":strstr(tracee->load_info->interp->host_path,"ld-linux-aarch64")?"libhish_linux_glibc_loader.so":NULL;
        if(!native||!loader)return -ENOEXEC;
        tracee->load_info->interp->host_path=talloc_asprintf(tracee->load_info->interp,"%s/%s",native,loader);
        tracee->load_info->interp->user_path=talloc_strdup(tracee->load_info->interp,tracee->load_info->interp->host_path);
        status = extract_load_info(tracee, tracee->load_info->interp);`);
 patch('execve/enter.c','    compute_load_addresses(tracee);','    compute_load_addresses(tracee);\n    status=hish_prepare_exec(tracee);if(status<0)return status;');
 patch('execve/enter.c','    loader_path = get_loader_path(tracee);','    loader_path = "/system/bin/toybox";');
 patch('execve/exit.c','#include "execve/execve.h"','#include "execve/execve.h"\n#include "hish_platform.h"');
 patch('execve/exit.c','    const word_t stack_pointer = peek_reg(tracee, CURRENT, STACK_POINTER);','    const word_t kernel_stack = peek_reg(tracee, CURRENT, STACK_POINTER);\n    const word_t stack_pointer=hish_script_end(tracee,kernel_stack);\n    if(!stack_pointer)return -EFAULT;');
 patch('execve/exit.c','peek_word(tracee, stack_pointer + sizeof_word(tracee))','peek_word(tracee, kernel_stack + sizeof_word(tracee))');
 patch('execve/exit.c','statement->start.stack_pointer = stack_pointer;','statement->start.stack_pointer = kernel_stack;');
 patch('execve/exit.c','    poke_reg(tracee, STACK_POINTER, stack_pointer - buffer_size);','    status=hish_publish_script(tracee,kernel_stack,stack_pointer-buffer_size,buffer,buffer_size);\n    save_current_regs(tracee, ORIGINAL);tracee->_regs_were_changed=true;return status;\n    poke_reg(tracee, STACK_POINTER, stack_pointer - buffer_size);');
 patch('extension/portmap/portmap.c','#include <linux/net.h>','#define _UAPI_LINUX_SOCKET_H\n#include <linux/net.h>');
 fs.writeFileSync(path.join(src,'build.h'),'#define HAVE_PROCESS_VM 1\n#define VERSION '+JSON.stringify(lock.version)+'\n');
 const make=fs.readFileSync(path.join(src,'GNUmakefile'),'utf8'),objects=make.split('OBJECTS +=')[1].split('define define_from_arch.h')[0].match(/[a-zA-Z0-9_/-]+\.o/g).filter(n=>!n.startsWith('loader/')).map(n=>n.slice(0,-2)+'.c');objects.push('hish_platform.c');
 const common=['--target=aarch64-linux-musl','--sysroot='+musl,'-nostdlib','-Os','-fPIE','-ffunction-sections','-fdata-sections','-fno-emulated-tls','-fno-stack-protector','-mno-outline-atomics','-D_GNU_SOURCE','-D_FILE_OFFSET_BITS=64','-I'+src,'-I'+path.join(build,'sysroot/usr/include'),'-I'+path.join(build,'uthash/src'),'-I'+path.join(musl,'usr/include'),'-idirafter',path.join(sdk,'sysroot/usr/include'),'-idirafter',path.join(sdk,'sysroot/usr/include/aarch64-linux-ohos')];
 function run(args){const r=cp.spawnSync(clang,args,{encoding:'utf8',windowsHide:true,maxBuffer:4e6});assert.equal(r.status,0,r.stdout+r.stderr);}
 for(let i=0;i<objects.length;i++)run([...common,'-c',path.join(src,objects[i]),'-o',path.join(build,i+'.o')]);
 const link=['-fuse-ld=lld','-pie','-Wl,--gc-sections','-Wl,-z,max-page-size=4096','-Wl,--dynamic-linker=/lib/ld-musl-aarch64.so.1'],crt=[path.join(lib,'Scrt1.o'),path.join(lib,'crti.o')],end=[path.join(musl,'lib/libc.musl-aarch64.so.1'),path.join(sdk,'llvm/lib/clang/15.0.4/lib/aarch64-linux-ohos/libclang_rt.builtins.a'),path.join(lib,'crtn.o')];
 const program=path.join(build,'proot-hish'),probe=path.join(build,'proot-probe');
 run([...common,...link,...crt,...objects.map((_,i)=>path.join(build,i+'.o')),path.join(build,'sysroot/usr/lib/libtalloc.so.2.4.3'),...end,'-o',program]);
 run([...common,...link,...crt,path.join(__dirname,'proot/probe.c'),...end,'-o',probe]);
 const result={version:lock.version,commit:lock.commit,sources:lock.sources,fingerprint,program:{file:program,sha256:hash(fs.readFileSync(program))},probe:{file:probe,sha256:hash(fs.readFileSync(probe))},patches:['hish_platform.c','hish_platform.h'],sourceFiles:objects.map(n=>[n,hash(fs.readFileSync(path.join(src,n)))])};
 fs.writeFileSync(manifest,JSON.stringify(result,null,2));return result;
}
module.exports={buildPRoot};
