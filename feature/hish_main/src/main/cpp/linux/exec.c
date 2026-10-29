#define _GNU_SOURCE
#include "native_tools.h"
#include "hish_elf_fault.h"
#include "exec_bridge_shared.h"
#include "process_identity.h"
#include "shell_script.h"
#include "elf_code_mapping.h"
#include "elf_execution_access.h"
#include "linux_library_path.h"
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/mman.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __GLIBC__
// glibc's public pthread_atfork wrapper is supplied by libc_nonshared.a and
// expects the per-DSO token normally emitted by crtbeginS.o.
__attribute__((visibility("hidden"))) void* __dso_handle = &__dso_handle;
#endif

// This code runs with the caller's Linux TLS/libc. Never call the inherited
// OHOS C++ runtime after a Linux loader has installed its own thread pointer.
// No syscall interception or instruction translation is used.
extern char** environ;
enum { MAX_ARGS = 1024, MAX_PHDR = 128, STACK_SIZE = 4 * 1024 * 1024 };
enum Runtime { Automatic, Static, Musl, Glibc, Ohos };
typedef struct {
    int fd;
    void* reservation;
    size_t size;
    uintptr_t bias, entry, phdr;
    Elf64_Ehdr eh;
    Elf64_Phdr ph[MAX_PHDR];
    char interpreter[PATH_MAX];
} Image;

static int error(int value) { errno = value; return -1; }
static void trace_stage(const char* stage)
{ if(getenv("HISH_SPAWN_DIAGNOSTICS"))syscall(SYS_write,2,stage,strlen(stage)); }
// exec is also entered after fork from a multithreaded caller. Data buffers
// must not acquire an allocator lock left owned by a vanished sibling thread.
static void* exec_data(size_t size)
{ void* memory=mmap(NULL,size,PROT_READ|PROT_WRITE,MAP_PRIVATE|MAP_ANONYMOUS,-1,0);return memory==MAP_FAILED?NULL:memory; }
static void free_exec_data(void* memory,size_t size) { if(memory)munmap(memory,size); }
static int range(uint64_t offset, uint64_t size, uint64_t limit)
{
    return offset <= limit && size <= limit - offset;
}
static int read_exact(int fd, void* data, size_t size, off_t offset)
{
    size_t at = 0;
    while (at < size) {
        ssize_t n = syscall(SYS_pread64,fd,(char*)data+at,size-at,offset+at);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return n < 0 ? -1 : error(ENOEXEC);
        at += n;
    }
    return 0;
}
static void release(Image* image)
{
    if (image->fd >= 0) close(image->fd);
    if (image->reservation) munmap(image->reservation, image->size);
}
static int load(const char* path, Image* image,int preflight)
{
    image->fd = open(path, O_RDONLY | O_CLOEXEC);
    if (image->fd < 0) return -1;
    struct stat info;
    if (fstat(image->fd, &info)) return -1;
    if (!S_ISREG(info.st_mode) || info.st_size < (off_t)sizeof(Elf64_Ehdr) || info.st_size > 64 * 1024 * 1024) return error(ENOEXEC);
    if (read_exact(image->fd, &image->eh, sizeof(image->eh), 0)) return -1;
    const Elf64_Ehdr* eh = &image->eh;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) || eh->e_ident[EI_CLASS] != ELFCLASS64 || eh->e_ident[EI_DATA] != ELFDATA2LSB ||
        eh->e_ident[EI_VERSION] != EV_CURRENT || eh->e_version != EV_CURRENT || eh->e_machine != EM_AARCH64 ||
        (eh->e_type != ET_DYN && eh->e_type != ET_EXEC) || eh->e_phentsize != sizeof(Elf64_Phdr) ||
        !eh->e_phnum || eh->e_phnum > MAX_PHDR || !range(eh->e_phoff, eh->e_phnum * sizeof(Elf64_Phdr), info.st_size)) return error(ENOEXEC);
    if (read_exact(image->fd, image->ph, eh->e_phnum * sizeof(Elf64_Phdr), eh->e_phoff)) return -1;
    const size_t page = getauxval(AT_PAGESZ);
    if (page != 4096) return error(ENOTSUP);
    uint64_t low = UINT64_MAX, high = 0, alignment = page;
    int executable_entry = 0;
    for (size_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr* ph = image->ph + i;
        if (ph->p_type == PT_INTERP) {
            if (*image->interpreter || !ph->p_filesz || ph->p_filesz > sizeof(image->interpreter) || !range(ph->p_offset, ph->p_filesz, info.st_size)) return error(ENOEXEC);
            if (read_exact(image->fd, image->interpreter, ph->p_filesz, ph->p_offset)) return -1;
            if (image->interpreter[ph->p_filesz - 1]) return error(ENOEXEC);
        }
        if (ph->p_type == PT_GNU_STACK && (ph->p_flags & PF_X)) return error(EACCES);
        if (ph->p_type != PT_LOAD || !ph->p_memsz) continue;
        if (ph->p_filesz > ph->p_memsz || !range(ph->p_offset, ph->p_filesz, info.st_size) || ph->p_memsz > UINT64_MAX - page || ph->p_vaddr > UINT64_MAX - ph->p_memsz - page ||
            ph->p_align > (1u << 24) || (ph->p_align && ((ph->p_align & (ph->p_align - 1)) || ph->p_offset % ph->p_align != ph->p_vaddr % ph->p_align)) || ph->p_offset % page != ph->p_vaddr % page) return error(ENOEXEC);
        if ((ph->p_flags & PF_X) && ((ph->p_flags & PF_W) || ph->p_filesz != ph->p_memsz)) return error(EACCES);
        uint64_t begin = ph->p_vaddr & ~(uint64_t)(page - 1), end = (ph->p_vaddr + ph->p_memsz + page - 1) & ~(uint64_t)(page - 1);
        if (begin < low) low = begin;
        if (end > high) high = end;
        if (ph->p_align > alignment) alignment = ph->p_align;
        if ((ph->p_flags & PF_X) && eh->e_entry >= ph->p_vaddr && eh->e_entry - ph->p_vaddr < ph->p_filesz) executable_entry = 1;
        for (size_t j = 0; j < i; j++) {
            const Elf64_Phdr* other = image->ph + j;
            if (other->p_type != PT_LOAD || !other->p_memsz) continue;
            uint64_t a = other->p_vaddr & ~(uint64_t)(page - 1), b = (other->p_vaddr + other->p_memsz + page - 1) & ~(uint64_t)(page - 1);
            if (begin < b && end > a) return error(ENOEXEC);
        }
    }
    if (!executable_entry || high <= low || high - low > 256 * 1024 * 1024) return error(ENOEXEC);
    image->size = high - low;
    if (eh->e_type == ET_EXEC&&!preflight) {
        void* memory = mmap((void*)low, image->size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
        if (memory == MAP_FAILED) return -1;
        if ((uintptr_t)memory != low) { munmap(memory, image->size); return error(ENOMEM); }
        image->reservation = memory;
    } else {
        void* memory = mmap(NULL, image->size + alignment, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (memory == MAP_FAILED) return -1;
        uintptr_t start = (uintptr_t)memory, aligned = start + ((low - start) & (alignment - 1));
        if (aligned != start) munmap(memory, aligned - start);
        munmap((void*)(aligned + image->size), alignment - (aligned - start));
        image->reservation = (void*)aligned;
    }
    image->bias = (uintptr_t)image->reservation - low;
    for (size_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr* ph = image->ph + i;
        if (ph->p_type != PT_LOAD || !ph->p_memsz) continue;
        uint64_t begin = ph->p_vaddr & ~(uint64_t)(page - 1), end = (ph->p_vaddr + ph->p_memsz + page - 1) & ~(uint64_t)(page - 1);
        int protection = (ph->p_flags & PF_R ? PROT_READ : 0) | (ph->p_flags & PF_W ? PROT_WRITE : 0) | (ph->p_flags & PF_X ? PROT_EXEC : 0);
        void* target = (void*)(image->bias + begin);
        if (ph->p_flags & PF_X) {
            const uint64_t offset = ph->p_offset & ~(uint64_t)(page - 1);
            const char* stage = NULL;
            if (HishMapELFCode(target, end - begin, protection, image->fd, offset,
                ph->p_offset - offset + ph->p_filesz, NULL, &stage) == MAP_FAILED) {
                const int saved = errno;
                if (getenv("HISH_ELF_DIAGNOSTICS")) dprintf(2,"HiSH ELF: %s errno=%d\n",stage,errno);
                return error(saved);
            }
        } else {
            if (mmap(target, end - begin, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED ||
                read_exact(image->fd, (void*)(image->bias + ph->p_vaddr), ph->p_filesz, ph->p_offset) || mprotect(target, end - begin, protection)) return -1;
        }
        if (eh->e_phoff >= ph->p_offset && range(eh->e_phoff - ph->p_offset, eh->e_phnum * sizeof(Elf64_Phdr), ph->p_filesz)) image->phdr = image->bias + ph->p_vaddr + eh->e_phoff - ph->p_offset;
    }
    if (!image->phdr) return error(ENOEXEC);
    image->entry = image->bias + eh->e_entry;
    return 0;
}
__attribute__((naked, noreturn)) static void enter(void* entry, void* stack)
{
    __asm__ volatile("mov x2,x0\nmov sp,x1\nmov x0,xzr\nbr x2");
}
static uintptr_t put(uintptr_t* top, const char* text)
{
    size_t size = strlen(text) + 1;
    *top -= size; memcpy((void*)*top, text, size); return *top;
}
static int single_threaded(void)
{
    DIR* directory = opendir("/proc/self/task");
    if (!directory) return 0;
    size_t count = 0;
    struct dirent* item;
    while ((item = readdir(directory))) if (item->d_name[0] >= '0' && item->d_name[0] <= '9') count++;
    closedir(directory); return count == 1;
}
static int kernel_bridge(const char* file,char* const argv[],char* const envp[],enum Runtime runtime,const char* libraries)
{
    trace_stage("EXEC_BRIDGE_BEGIN\n");
    size_t argc=0,envc=0,file_size=strnlen(file,PATH_MAX)+1,total=sizeof(HishExecBridgeHeader)+file_size;
    if(file_size>PATH_MAX)return error(ENAMETOOLONG);
    for(;argv&&argv[argc];argc++){if(argc>=MAX_ARGS-1)return error(E2BIG);size_t n=strnlen(argv[argc],1024*1024)+1;if(n>1024*1024||total+n>HISH_EXEC_BRIDGE_LIMIT)return error(E2BIG);total+=n;}
    if(!argc)return error(EINVAL);
    for(;envp&&envp[envc];envc++){if(envc>=4096)return error(E2BIG);size_t n=strnlen(envp[envc],1024*1024)+1;if(n>1024*1024||total+n>HISH_EXEC_BRIDGE_LIMIT)return error(E2BIG);total+=n;}
    char preload[PATH_MAX+64],search[PATH_MAX+64];
    if(snprintf(preload,sizeof(preload),"LD_PRELOAD=%s/libhish_exec_bridge.so",libraries)>=(int)sizeof(preload)||snprintf(search,sizeof(search),"LD_LIBRARY_PATH=%s:/system/lib64/ndk",libraries)>=(int)sizeof(search))return error(ENAMETOOLONG);
    int target=open(file,O_RDONLY|O_CLOEXEC);if(target<0)return -1;
    int pinned=fcntl(target,F_DUPFD,3),saved=errno;close(target);if(pinned<0)return error(saved);
    int request=memfd_create("hish-exec-request",MFD_ALLOW_SEALING);if(request<0){saved=errno;close(pinned);return error(saved);}
    if(request<3){int higher=fcntl(request,F_DUPFD,3);saved=errno;close(request);request=higher;if(request<0){close(pinned);return error(saved);}}
    if(fcntl(request,F_SETFD,0)||fcntl(pinned,F_SETFD,0)){saved=errno;close(request);close(pinned);return error(saved);}
    HishExecBridgeHeader header={HISH_EXEC_BRIDGE_MAGIC,HISH_EXEC_BRIDGE_VERSION,runtime,argc,envc,pinned,total};
    trace_stage("EXEC_BRIDGE_ALLOCATE\n");
    char* bytes=exec_data(total);if(!bytes){close(request);close(pinned);return -1;}
    memcpy(bytes,&header,sizeof(header));memcpy(bytes+sizeof(header),file,file_size);size_t at=sizeof(header)+file_size;
    for(size_t i=0;i<argc;i++){size_t n=strlen(argv[i])+1;memcpy(bytes+at,argv[i],n);at+=n;}
    for(size_t i=0;i<envc;i++){size_t n=strlen(envp[i])+1;memcpy(bytes+at,envp[i],n);at+=n;}
    at=0;while(at<total){ssize_t n=syscall(SYS_write,request,bytes+at,total-at);if(n<0&&errno==EINTR)continue;if(n<=0)break;at+=n;}saved=errno;free_exec_data(bytes,total);
    if(at!=total||fcntl(request,F_ADD_SEALS,F_SEAL_WRITE|F_SEAL_GROW|F_SEAL_SHRINK|F_SEAL_SEAL)){if(at==total)saved=errno;close(request);close(pinned);return error(saved);}
    char channel[64];snprintf(channel,sizeof(channel),"HISH_EXEC_BRIDGE_FD=%d",request);
    char** environment=exec_data(4100*sizeof(*environment));if(!environment){close(request);close(pinned);return -1;}size_t count=0;
    for(size_t i=0;i<envc;i++)if(strncmp(envp[i],"LD_",3)&&strncmp(envp[i],"GLIBC_TUNABLES=",14)&&strncmp(envp[i],"HISH_EXEC_BRIDGE_FD=",20))environment[count++]=envp[i];
    environment[count++]=preload;environment[count++]=search;environment[count++]=channel;environment[count]=NULL;
    // If another firmware ignores the preload, fail visibly instead of
    // accidentally reporting success from an unrelated `toybox true`.
    char* args[]={"toybox","hish-exec-bridge-not-entered",NULL};
    trace_stage("EXEC_BRIDGE_KERNEL\n");
    syscall(SYS_execve,"/system/bin/toybox",args,environment);saved=errno;free_exec_data(environment,4100*sizeof(*environment));close(request);close(pinned);return error(saved);
}
static int run_image(const char* file, char* const argv[], char* const envp[], enum Runtime hint)
{
    const char* bridge=getenv("HISH_KERNEL_EXEC_BRIDGE");const int use_bridge=!bridge||!strcmp(bridge,"1");
    // A user-space jump cannot terminate the caller's other threads as exec does.
    if (!use_bridge&&!single_threaded()) return error(ENOTSUP);
    trace_stage("EXEC_IMAGE_ALLOCATE\n");
    Image* images=exec_data(2*sizeof(Image));if(!images)return -1;
    Image* main=images;Image* loader=images+1;main->fd=loader->fd=-1;
    char** environment=NULL;uintptr_t* words=NULL;char identity[PATH_MAX+32],canonical[PATH_MAX];
    void* stack = MAP_FAILED; int result = -1, saved;
    const char* libraries = getenv("HISH_NATIVE_LIBDIR");
    if (!libraries || libraries[0] != '/') {free_exec_data(images,2*sizeof(Image));return error(ENOENT);}
    if (load(file, main,use_bridge)) goto done;
    trace_stage("EXEC_IMAGE_LOADED\n");
    enum Runtime runtime = Static;
    char interpreter[PATH_MAX], preload[PATH_MAX * 3], library_path[HISH_LIBRARY_PATH_LIMIT + 16], library_dir[PATH_MAX + 32];
    if (*main->interpreter) {
        runtime = hint == Automatic ? (strstr(main->interpreter, "ld-musl-aarch64") ? Musl : strstr(main->interpreter, "ld-linux-aarch64") ? Glibc : Automatic) : hint;
        if (runtime == Automatic || runtime == Static) { errno = ENOEXEC; goto done; }
        if (runtime == Ohos) snprintf(interpreter, sizeof(interpreter), "/lib/ld-musl-aarch64.so.1");
        else snprintf(interpreter, sizeof(interpreter), "%s/%s", libraries, runtime == Musl ? "libhish_linux_musl_loader.so" : "libhish_linux_glibc_loader.so");
        if (load(interpreter, loader,use_bridge)) goto done;
    }
    if(use_bridge){
        trace_stage("EXEC_IMAGE_PREFLIGHT_OK\n");
        // Validate/map first so known format, path and RX failures still return
        // to the caller. Only the kernel exec below replaces the old process.
        release(loader);release(main);free_exec_data(images,2*sizeof(Image));
        return kernel_bridge(file,argv,envp,hint,libraries);
    }
    size_t argc = 0, envc = 0, total = 0;
    while (argv && argv[argc]) { total += strnlen(argv[argc], 1024 * 1024) + 1; if (++argc >= MAX_ARGS || total > 1024 * 1024) { errno = E2BIG; goto done; } }
    if (!argc) { errno = EINVAL; goto done; }
    environment=exec_data((4096+6)*sizeof(*environment));words=exec_data((MAX_ARGS+4096+96)*sizeof(*words));
    if(!environment||!words){errno=ENOMEM;goto done;}
    for (size_t i = 0; envp && envp[i]; i++) {
        total += strnlen(envp[i], 1024 * 1024) + 1;
        if (i >= 4096 || total > 2 * 1024 * 1024) { errno = E2BIG; goto done; }
        if (!strncmp(envp[i], "LD_LIBRARY_PATH=", 16) || !strncmp(envp[i], "LD_PRELOAD=", 11) || !strncmp(envp[i], "LD_AUDIT=", 9) ||
            !strncmp(envp[i], "GLIBC_TUNABLES=", 14) || !strncmp(envp[i], "HISH_NATIVE_LIBDIR=", 19) || !strncmp(envp[i],"HISH_EXECUTABLE=",16)) continue;
        environment[envc++] = envp[i];
    }
    memcpy(library_path,"LD_LIBRARY_PATH=",16);
    if(runtime==Musl){if(HishMuslLibraryPath(library_path+16,sizeof(library_path)-16,libraries,envp))goto done;}
    else snprintf(library_path+16,sizeof(library_path)-16,"%s%s",libraries,runtime==Ohos?":/system/lib64/ndk":"");
    snprintf(library_dir, sizeof(library_dir), "HISH_NATIVE_LIBDIR=%s", libraries);
    environment[envc++] = library_path; environment[envc++] = library_dir;
    if (runtime == Musl || runtime == Glibc) {
        if (runtime == Musl) snprintf(preload, sizeof(preload), "LD_PRELOAD=%s/libhish_linux_identity_padded.so:%s/libhish_linux_musl_loader.so", libraries, libraries);
        else snprintf(preload, sizeof(preload), "LD_PRELOAD=%s/libhish_linux_glibc_tty.so:%s/libhish_linux_glibc_compat.so", libraries, libraries);
        environment[envc++] = preload;
    }
    environment[envc++] = "GLIBC_TUNABLES=glibc.pthread.rseq=0";
    snprintf(identity,sizeof(identity),"HISH_EXECUTABLE=%s",HishImageName(file,canonical,sizeof(canonical)));environment[envc++]=identity;
    stack = mmap(NULL, STACK_SIZE + 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED || mprotect(stack, 4096, PROT_NONE)) goto done;
    uintptr_t top = (uintptr_t)stack + STACK_SIZE + 4096; size_t n = 0;
    words[n++] = argc;
    for (size_t i = 0; i < argc; i++) words[n++] = put(&top, argv[i]);
    words[n++] = 0;
    for (size_t i = 0; i < envc; i++) words[n++] = put(&top, environment[i]);
    words[n++] = 0;
    uintptr_t execname = put(&top, file); top -= 16;
    if (getrandom((void*)top, 16, 0) != 16) goto done;
    uintptr_t aux[] = {AT_PAGESZ,4096,AT_PHDR,main->phdr,AT_PHENT,sizeof(Elf64_Phdr),AT_PHNUM,main->eh.e_phnum,AT_ENTRY,main->entry,AT_BASE,loader->bias,
        AT_UID,getuid(),AT_EUID,geteuid(),AT_GID,getgid(),AT_EGID,getegid(),AT_SECURE,0,AT_EXECFN,execname,AT_RANDOM,top,
        AT_HWCAP,getauxval(AT_HWCAP),AT_HWCAP2,getauxval(AT_HWCAP2),AT_SYSINFO_EHDR,getauxval(AT_SYSINFO_EHDR),AT_CLKTCK,getauxval(AT_CLKTCK),AT_NULL,0};
    memcpy(words + n, aux, sizeof(aux)); n += sizeof(aux)/sizeof(*aux);
    top = (top - n * sizeof(uintptr_t)) & ~(uintptr_t)15; memcpy((void*)top, words, n * sizeof(uintptr_t));
    // Dispositions and CLOEXEC follow exec semantics; old mappings remain until
    // this command process exits. This is deliberately not a complete kernel exec.
    for (int sig = 1; sig < NSIG; sig++) {
        struct sigaction old, action = {0};
        if (sig == SIGKILL || sig == SIGSTOP) continue;
        if (!sigaction(sig, NULL, &old) && old.sa_handler != SIG_IGN) { action.sa_handler = SIG_DFL; sigemptyset(&action.sa_mask); sigaction(sig, &action, NULL); }
    }
    DIR* descriptors = opendir("/proc/self/fd");
    if (descriptors) {
        int own = dirfd(descriptors); struct dirent* item;
        while ((item = readdir(descriptors))) {
            char* end; long fd = strtol(item->d_name, &end, 10);
            if (!*end && fd >= 0 && fd != own && fd <= INT_MAX && (fcntl(fd, F_GETFD) & FD_CLOEXEC)) close(fd);
        }
        closedir(descriptors);
    }
    HishInstallELFDiagnostics();
    enter((void*)(runtime != Static ? loader->entry : main->entry), (void*)top);
done:
    saved = errno; if (stack != MAP_FAILED) munmap(stack, STACK_SIZE + 4096);
    release(loader); release(main);free_exec_data(images,2*sizeof(Image));free_exec_data(environment,(4096+6)*sizeof(*environment));free_exec_data(words,(MAX_ARGS+4096+96)*sizeof(*words));errno = saved; return result;
}
static int system_path(const char* path)
{
    return !strncmp(path,"/system/",8) || !strncmp(path,"/bin/",5) || !strncmp(path,"/usr/bin/",9);
}
static int routed(const char* path, char* const argv[], char* const envp[])
{
    const char* libraries = getenv("HISH_NATIVE_LIBDIR"), *prefix = getenv("PREFIX");
    if (!libraries) return error(ENOENT);
    const char* name = strrchr(path, '/'); name = name ? name + 1 : path;
    int virtual_path = !strchr(path, '/') || system_path(path);
    if (prefix && !strncmp(path, prefix, strlen(prefix)) && !strncmp(path + strlen(prefix), "/bin/", 5)) virtual_path = 1;
    char packaged[PATH_MAX];
    if (virtual_path) {
        if (HishDisabledTool(name)) return error(ENOTSUP);
#ifdef HISH_ENABLE_DIAGNOSTICS
        if (!strcmp(name,"hish-run")) {
            size_t at=1;int diagnostic=0,notify=0;
            while(argv[at]){if(!strcmp(argv[at],"--diagnostic"))diagnostic=1;else if(!strcmp(argv[at],"--notify"))notify=1;else break;at++;}
            if(argv[at]&&!strcmp(argv[at],"--"))at++;
            if(!argv[at])return error(EINVAL);
            if(access(argv[at],X_OK))return -1;
            return kernel_bridge(argv[at],argv+at,envp,(enum Runtime)(HISH_EXEC_BRIDGE_PTRACE+diagnostic+2*notify),libraries);
        }
#endif
#define ROUTE(tool,file,runtime) if (!strcmp(name,tool)) { snprintf(packaged,sizeof(packaged),"%s/%s",libraries,file); return run_image(packaged,argv,envp,runtime); }
        HISH_NATIVE_TOOLS(ROUTE)
#undef ROUTE
        // Explicit aliases only. Unknown commands keep their ordinary error.
        const char* aliases[] = {"sh","mksh","cat","grep","sed","ls","head","tail","find","env","sort","cut","tr","od","wc","sleep","true","false","echo","printf","pwd","mkdir","rm","cp","mv","touch","vi","uname","chmod","cmp","dd","hish-tools",NULL};
        for (size_t i = 0; aliases[i]; i++) if (!strcmp(name, aliases[i])) {
            size_t argc = 0; while (argv[argc] && argc < MAX_ARGS - 2) argc++;
            if (argv[argc]) return error(E2BIG);
            char* args[MAX_ARGS];args[0]=(char*)name;
            for(size_t j=1;j<=argc;j++)args[j]=argv[j];
            snprintf(packaged,sizeof(packaged),"%s/libhish_shell.so",libraries);
            return kernel_bridge(packaged,args,envp,(enum Runtime)HISH_EXEC_BRIDGE_APPLET,libraries);
        }
    }
    // The OHOS and Linux musl interpreters share a basename, but not an ABI.
    // Never infer that an unknown system program is a Linux musl binary.
    if (system_path(path)) return access(path,F_OK) ? -1 : error(EACCES);
    unsigned char header[512] = {0}; int fd = open(path,O_RDONLY|O_CLOEXEC);
    ssize_t count = fd < 0 ? -1 : read(fd,header,sizeof(header)-1); if (fd >= 0) close(fd);
    if (count < 0) return -1;
#ifndef HISH_ENABLE_DIAGNOSTICS
    if (prefix && !strncmp(path,prefix,strlen(prefix)) && !strncmp(path+strlen(prefix),"/bin/",5) &&
        !strchr(path+strlen(prefix)+5,'/') && count>0 && HishRetiredApplicationWrapper(name,(char*)header,(size_t)count))
        return error(ENOENT);
#endif
    if (count >= SELFMAG && !memcmp(header,ELFMAG,SELFMAG)) {
        if (HishELFExecutionAccess(path,libraries)) return -1;
        return run_image(path,argv,envp,Automatic);
    }
    if (count > 0 && !memchr(header,0,count) && !access(path,X_OK)) {
        // Only known shell interpreters; never silently interpret Python as sh.
        const char* shell;char* option;if(HishShellScript((char*)header,count,&shell,&option)<0)return -1;
        size_t argc = 0; while (argv[argc] && argc < MAX_ARGS - 3) argc++;
        if (argv[argc]) return error(E2BIG);
        char* args[MAX_ARGS];size_t at=0;args[at++]=(char*)shell;if(option)args[at++]=option;args[at++]=(char*)path;
        for (size_t j = 1; j <= argc; j++) args[at++] = argv[j];
        return routed("/bin/sh",args,envp);
    }
    return error(ENOEXEC);
}
static int kernel_environment(char* const envp[],char** clean)
{
    size_t count=0;
    for(size_t i=0;envp&&envp[i];i++){
        if(i>=4096)return error(E2BIG);
        if(!strncmp(envp[i],"LD_PRELOAD=",11)||!strncmp(envp[i],"LD_LIBRARY_PATH=",16)||
            !strncmp(envp[i],"LD_AUDIT=",9)||!strncmp(envp[i],"GLIBC_TUNABLES=",14))continue;
        clean[count++]=envp[i];
    }
    clean[count]=NULL;return 0;
}
static int application_shell(const char* path)
{
    const char* name=strrchr(path,'/');name=name?name+1:path;
    if(strcmp(name,"sh")&&strcmp(name,"mksh"))return 0;
    if(!strchr(path,'/')||system_path(path))return 1;
    const char* prefix=getenv("PREFIX");
    return prefix&&!strncmp(path,prefix,strlen(prefix))&&!strncmp(path+strlen(prefix),"/bin/",5);
}
int execve(const char* path, char* const argv[], char* const envp[])
{
    path=HishExecutablePath(path);
    // Even if another firmware permits /bin/sh, it must not silently replace
    // the selected application shell with the system shell.
    if(application_shell(path))return routed(path,argv,envp);
    // A relative path, symlink or fd may also resolve to an allowed OHOS
    // executable. Do not pass Linux loader state to the kernel bootstrap.
    char** clean=exec_data(4097*sizeof(*clean));if(!clean)return -1;
    if(kernel_environment(envp,clean)){int saved=errno;free_exec_data(clean,4097*sizeof(*clean));return error(saved);}
    syscall(SYS_execve,path,argv,clean);
    int saved=errno;free_exec_data(clean,4097*sizeof(*clean));errno=saved;
    if (errno != EACCES && errno != ENOEXEC && errno != ENOENT) return -1;
    return routed(path,argv,envp);
}
int execv(const char* path, char* const argv[]) { return execve(path,argv,environ); }
int execveat(int directory,const char* path,char* const argv[],char* const envp[],int flags)
{
    if(flags&~(AT_EMPTY_PATH|AT_SYMLINK_NOFOLLOW))return error(EINVAL);
    if(!*path&&!(flags&AT_EMPTY_PATH))return error(ENOENT);
    char** clean=exec_data(4097*sizeof(*clean));if(!clean)return -1;
    if(kernel_environment(envp,clean)){int saved=errno;free_exec_data(clean,4097*sizeof(*clean));return error(saved);}
    syscall(SYS_execveat,directory,path,argv,clean,flags);
    int saved=errno;free_exec_data(clean,4097*sizeof(*clean));errno=saved;
    if(errno!=EACCES&&errno!=ENOEXEC&&errno!=ENOENT)return -1;
    struct stat info;
    if(flags&AT_SYMLINK_NOFOLLOW){
        if(fstatat(directory,path,&info,AT_SYMLINK_NOFOLLOW|(flags&AT_EMPTY_PATH)))return -1;
        if(S_ISLNK(info.st_mode))return error(ELOOP);
    }
    if(path[0]=='/'||directory==AT_FDCWD)return routed(path,argv,envp);
    char pinned[PATH_MAX];
    int count=snprintf(pinned,sizeof(pinned),"/proc/self/fd/%d%s%s",directory,*path?"/":"",path);
    if(count<0||(size_t)count>=sizeof(pinned))return error(ENAMETOOLONG);
    // Keep the descriptor open until mapping is complete. The normal image
    // transition then applies FD_CLOEXEC, including descriptor-based exec.
    return routed(pinned,argv,envp);
}
int fexecve(int fd,char* const argv[],char* const envp[])
{
    return execveat(fd,"",argv,envp,AT_EMPTY_PATH);
}
int execvpe(const char* file, char* const argv[], char* const envp[])
{
    if (strchr(file,'/')) return execve(file,argv,envp);
    const char* search = getenv("PATH"); if (!search) search = "/bin:/usr/bin";
    int denied = 0;
    for (const char* at = search;;) {
        const char* end = strchr(at,':'); size_t length = end ? (size_t)(end-at) : strlen(at);
        if (length + strlen(file) + 2 < PATH_MAX) {
            char path[PATH_MAX]; if (length) snprintf(path,sizeof(path),"%.*s/%s",(int)length,at,file); else snprintf(path,sizeof(path),"./%s",file);
            execve(path,argv,envp);
            if (errno == EACCES) denied = 1; else if (errno != ENOENT && errno != ENOTDIR) return -1;
        }
        if (!end) break; at = end + 1;
    }
    return error(denied?EACCES:ENOENT);
}
int execvp(const char* file, char* const argv[]) { return execvpe(file,argv,environ); }
static int exec_list(const char* path, const char* first, va_list list, int search, int explicit_environment)
{
    char* args[MAX_ARGS]; size_t count = 0; const char* value = first;
    while (value) {
        if (count == MAX_ARGS - 1) return error(E2BIG);
        args[count++] = (char*)value; value = va_arg(list,const char*);
    }
    args[count] = NULL;
    char* const* environment = explicit_environment ? va_arg(list,char* const*) : environ;
    return search ? execvpe(path,args,(char* const*)environment) : execve(path,args,(char* const*)environment);
}
int execl(const char* path, const char* first, ...)
{
    va_list list; va_start(list,first); int result = exec_list(path,first,list,0,0); va_end(list); return result;
}
int execlp(const char* path, const char* first, ...)
{
    va_list list; va_start(list,first); int result = exec_list(path,first,list,1,0); va_end(list); return result;
}
int execle(const char* path, const char* first, ...)
{
    va_list list; va_start(list,first); int result = exec_list(path,first,list,0,1); va_end(list); return result;
}
// Own the public file-action API rather than depending on musl/glibc private
// object layouts. Snapshot before fork; atfork keeps the registry usable in a
// child even when another parent thread is building an unrelated action list.
enum ActionKind { ActionOpen, ActionClose, ActionDup, ActionChdir, ActionFchdir, ActionCloseFrom, ActionForeground };
typedef struct FileAction {
    struct FileAction* next;
    enum ActionKind kind;
    int fd, target, flags;
    mode_t mode;
    char* path;
} FileAction;
typedef struct ActionList {
    struct ActionList* next;
    const posix_spawn_file_actions_t* key;
    FileAction* first;
    FileAction* last;
} ActionList;
static pthread_mutex_t actions_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_once_t actions_once = PTHREAD_ONCE_INIT;
static int actions_setup_error;
static ActionList* action_lists;
typedef struct PipeStream { struct PipeStream* next; FILE* stream; int fd; pid_t pid; } PipeStream;
static pthread_mutex_t streams_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t system_lock = PTHREAD_MUTEX_INITIALIZER;
static PipeStream* pipe_streams;
static _Thread_local int opening_pipe;
static unsigned system_users;
static struct sigaction system_int,system_quit;
static void actions_prepare(void) { pthread_mutex_lock(&actions_lock);pthread_mutex_lock(&system_lock);pthread_mutex_lock(&streams_lock); }
static void actions_release(void) { pthread_mutex_unlock(&streams_lock);pthread_mutex_unlock(&system_lock);pthread_mutex_unlock(&actions_lock); }
static void actions_child(void)
{
    int diagnostic=getenv("HISH_SPAWN_DIAGNOSTICS")!=NULL;
    if(diagnostic)syscall(SYS_write,2,"SPAWN_ATFORK_CHILD\n",19);
    // Only popen's child closes earlier popen streams; ordinary fork keeps them.
    if(opening_pipe)for(PipeStream* at=pipe_streams;at;at=at->next)close(at->fd);
    if(system_users){sigaction(SIGINT,&system_int,NULL);sigaction(SIGQUIT,&system_quit,NULL);system_users=0;}
    actions_release();
    if(diagnostic)syscall(SYS_write,2,"SPAWN_ATFORK_DONE\n",18);
}
static void actions_setup(void) { actions_setup_error = pthread_atfork(actions_prepare,actions_release,actions_child); }
static ActionList* find_actions(const posix_spawn_file_actions_t* key)
{
    for (ActionList* list=action_lists;list;list=list->next) if(list->key==key)return list;
    return NULL;
}
static void free_actions(FileAction* item)
{
    while(item){FileAction* next=item->next;free(item->path);free(item);item=next;}
}
int posix_spawn_file_actions_init(posix_spawn_file_actions_t* key)
{
    int result=pthread_once(&actions_once,actions_setup);if(result)return result;
    if(actions_setup_error)return actions_setup_error;
    ActionList* list=calloc(1,sizeof(*list));if(!list)return ENOMEM;list->key=key;
    pthread_mutex_lock(&actions_lock);
    if(find_actions(key)){pthread_mutex_unlock(&actions_lock);free(list);return EINVAL;}
    list->next=action_lists;action_lists=list;pthread_mutex_unlock(&actions_lock);return 0;
}
int posix_spawn_file_actions_destroy(posix_spawn_file_actions_t* key)
{
    pthread_mutex_lock(&actions_lock);ActionList** at=&action_lists;
    while(*at&&(*at)->key!=key)at=&(*at)->next;
    ActionList* list=*at;if(list)*at=list->next;pthread_mutex_unlock(&actions_lock);
    if(!list)return EINVAL;free_actions(list->first);free(list);return 0;
}
static int action_fd_valid(int fd)
{
    struct rlimit limit;
    return fd>=0 && (!getrlimit(RLIMIT_NOFILE,&limit) && (rlim_t)fd<limit.rlim_cur);
}
static int append_action(posix_spawn_file_actions_t* key,enum ActionKind kind,int fd,int target,const char* path,int flags,mode_t mode)
{
    if((kind!=ActionChdir&&kind!=ActionCloseFrom&&!action_fd_valid(fd))||(kind==ActionDup&&!action_fd_valid(target)))return EBADF;
    FileAction* item=calloc(1,sizeof(*item));if(!item)return ENOMEM;
    item->kind=kind;item->fd=fd;item->target=target;item->flags=flags;item->mode=mode;
    if(path&&!(item->path=strdup(path))){free(item);return ENOMEM;}
    pthread_mutex_lock(&actions_lock);ActionList* list=find_actions(key);
    if(list){if(list->last)list->last->next=item;else list->first=item;list->last=item;}
    pthread_mutex_unlock(&actions_lock);if(!list){free_actions(item);return EINVAL;}return 0;
}
int posix_spawn_file_actions_addopen(posix_spawn_file_actions_t* key,int fd,const char* path,int flags,mode_t mode)
{ return append_action(key,ActionOpen,fd,0,path,flags,mode); }
int posix_spawn_file_actions_addclose(posix_spawn_file_actions_t* key,int fd)
{ return append_action(key,ActionClose,fd,0,NULL,0,0); }
int posix_spawn_file_actions_adddup2(posix_spawn_file_actions_t* key,int fd,int target)
{ return append_action(key,ActionDup,fd,target,NULL,0,0); }
int posix_spawn_file_actions_addchdir_np(posix_spawn_file_actions_t* key,const char* path)
{ return append_action(key,ActionChdir,0,0,path,0,0); }
int posix_spawn_file_actions_addfchdir_np(posix_spawn_file_actions_t* key,int fd)
{ return append_action(key,ActionFchdir,fd,0,NULL,0,0); }
int posix_spawn_file_actions_addclosefrom_np(posix_spawn_file_actions_t* key,int fd)
{ if(fd<0)return EBADF;return append_action(key,ActionCloseFrom,fd,0,NULL,0,0); }
int posix_spawn_file_actions_addtcsetpgrp_np(posix_spawn_file_actions_t* key,int fd)
{ return append_action(key,ActionForeground,fd,0,NULL,0,0); }
static int snapshot_actions(const posix_spawn_file_actions_t* key,FileAction** output)
{
    *output=NULL;if(!key)return 0;
    pthread_mutex_lock(&actions_lock);ActionList* list=find_actions(key);FileAction** tail=output;int result=list?0:EINVAL;
    if(list)for(const FileAction* at=list->first;at;at=at->next){
        FileAction* item=malloc(sizeof(*item));if(!item){result=ENOMEM;break;}
        *item=*at;item->next=NULL;item->path=at->path?strdup(at->path):NULL;
        if(at->path&&!item->path){free(item);result=ENOMEM;break;}*tail=item;tail=&item->next;
    }
    pthread_mutex_unlock(&actions_lock);if(result){free_actions(*output);*output=NULL;}return result;
}
static int action_uses_fd(const FileAction* list,int fd)
{
    for(;list;list=list->next)if((list->kind!=ActionChdir&&list->kind!=ActionCloseFrom&&list->fd==fd)||(list->kind==ActionDup&&list->target==fd))return 1;
    return 0;
}
static int close_from(int low,int channel)
{
    // Keep exactly the private error descriptor until exec closes it. Later
    // actions may reopen descriptors above low, so CLOEXEC is not equivalent.
    // close_range is filtered with fatal SIGSYS on the tested phone, rather
    // than a recoverable ENOSYS. Enumerate real descriptors instead.
    int directory=open("/proc/self/fd",O_RDONLY|O_DIRECTORY|O_CLOEXEC);
    if(directory>=0){
        struct LinuxDirent { uint64_t ino; int64_t off; unsigned short length; unsigned char type; char name[]; };
        char bytes[4096];long count;
        while((count=syscall(SYS_getdents64,directory,bytes,sizeof(bytes)))>0){
            for(long at=0;at<count;){struct LinuxDirent* entry=(void*)(bytes+at);if(!entry->length){close(directory);return EIO;}
                char* end;long fd=strtol(entry->name,&end,10);
                if(!*end&&fd>=low&&fd<=INT_MAX&&fd!=directory&&fd!=channel)close((int)fd);
                at+=entry->length;
            }
        }
        int saved=errno;close(directory);return count<0?saved:0;
    }
    struct rlimit limit;if(getrlimit(RLIMIT_NOFILE,&limit))return errno;
    for(rlim_t fd=low;fd<limit.rlim_cur&&fd<=INT_MAX;fd++)if((int)fd!=channel)close((int)fd);
    return 0;
}
static int perform_actions(const FileAction* item,int channel)
{
    for(;item;item=item->next){
        if(item->kind==ActionClose){if(close(item->fd)&&errno!=EBADF)return errno;}
        else if(item->kind==ActionDup){
            if(item->fd==item->target){int flags=fcntl(item->fd,F_GETFD);if(flags<0||fcntl(item->fd,F_SETFD,flags&~FD_CLOEXEC))return errno;}
            else if(dup2(item->fd,item->target)<0)return errno;
        }else if(item->kind==ActionOpen){
            close(item->fd);int fd=open(item->path,item->flags,item->mode);if(fd<0)return errno;
            if(fd!=item->fd){int result=dup2(fd,item->fd),saved=errno;close(fd);if(result<0)return saved;}
        }else if(item->kind==ActionChdir){if(chdir(item->path))return errno;}
        else if(item->kind==ActionFchdir){if(fchdir(item->fd))return errno;}
        else if(item->kind==ActionCloseFrom){int result=close_from(item->fd,channel);if(result)return result;}
        else if(item->kind==ActionForeground){if(tcsetpgrp(item->fd,getpgrp()))return errno;}
    }
    return 0;
}
static int spawn_command(pid_t* pid, const char* path, const posix_spawn_file_actions_t* actions,
    const posix_spawnattr_t* attributes, char* const argv[], char* const envp[], int search)
{
    short flags = 0; sigset_t mask, defaults; pid_t group = 0;struct sched_param scheduler={0};int policy=0;
    if (attributes) {
        int result = posix_spawnattr_getflags(attributes,&flags); if (result) return result;
        if (flags & ~(POSIX_SPAWN_RESETIDS | POSIX_SPAWN_SETPGROUP | POSIX_SPAWN_SETSIGDEF | POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_USEVFORK | POSIX_SPAWN_SETSID | POSIX_SPAWN_SETSCHEDPARAM | POSIX_SPAWN_SETSCHEDULER)) return ENOTSUP;
        if ((flags & POSIX_SPAWN_SETSIGMASK) && (result=posix_spawnattr_getsigmask(attributes,&mask))) return result;
        if ((flags & POSIX_SPAWN_SETSIGDEF) && (result=posix_spawnattr_getsigdefault(attributes,&defaults))) return result;
        if ((flags & POSIX_SPAWN_SETPGROUP) && (result=posix_spawnattr_getpgroup(attributes,&group))) return result;
        if ((flags & (POSIX_SPAWN_SETSCHEDPARAM|POSIX_SPAWN_SETSCHEDULER)) && (result=posix_spawnattr_getschedparam(attributes,&scheduler))) return result;
        if ((flags & POSIX_SPAWN_SETSCHEDULER) && (result=posix_spawnattr_getschedpolicy(attributes,&policy))) return result;
    }
    FileAction* list=NULL;int result=snapshot_actions(actions,&list);if(result)return result;
    int channel[2];if(pipe2(channel,O_CLOEXEC)){int saved=errno;free_actions(list);return saved;}
    for(int i=0;i<2;i++)if(channel[i]<3){
        int fd=fcntl(channel[i],F_DUPFD_CLOEXEC,3),saved=errno;
        if(fd<0){close(channel[0]);close(channel[1]);free_actions(list);return saved;}
        close(channel[i]);channel[i]=fd;
    }
    // File actions must never overwrite/close the private exec-error channel.
    while(action_uses_fd(list,channel[1])){
        int fd=fcntl(channel[1],F_DUPFD_CLOEXEC,channel[1]+1),saved=errno;
        if(fd<0){close(channel[0]);close(channel[1]);free_actions(list);return saved;}
        close(channel[1]);channel[1]=fd;
    }
    const int diagnostic=getenv("HISH_SPAWN_DIAGNOSTICS")!=NULL;
    pid_t child = fork();
    if (child < 0) { int saved=errno;close(channel[0]);close(channel[1]);free_actions(list);return saved; }
    if (!child) {
        if(diagnostic)syscall(SYS_write,2,"SPAWN_CHILD_ENTER\n",18);
        close(channel[0]); int failed = 0;
        if ((flags & POSIX_SPAWN_RESETIDS) && (getuid()!=geteuid() || getgid()!=getegid())) failed=EPERM;
        if (!failed && (flags & POSIX_SPAWN_SETSID) && setsid()<0) failed=errno;
        if (!failed && (flags & POSIX_SPAWN_SETPGROUP) && setpgid(0,group)) failed=errno;
        if(!failed&&(flags&(POSIX_SPAWN_SETSCHEDULER|POSIX_SPAWN_SETSCHEDPARAM))){
            struct sched_param current;int inherited=sched_getscheduler(0);
            int unchanged=inherited>=0&&!sched_getparam(0,&current)&&current.sched_priority==scheduler.sched_priority&&(!(flags&POSIX_SPAWN_SETSCHEDULER)||inherited==policy);
            if(!unchanged){
                if(flags&POSIX_SPAWN_SETSCHEDULER){if(sched_setscheduler(0,policy,&scheduler))failed=errno;}
                else if(sched_setparam(0,&scheduler))failed=errno;
            }
        }
        if (!failed && (flags & POSIX_SPAWN_SETSIGMASK) && sigprocmask(SIG_SETMASK,&mask,NULL)) failed=errno;
        if (!failed && (flags & POSIX_SPAWN_SETSIGDEF)) for (int sig=1;sig<NSIG;sig++) if(sig!=SIGKILL&&sig!=SIGSTOP&&sigismember(&defaults,sig)==1) {
            struct sigaction action={0};action.sa_handler=SIG_DFL;sigemptyset(&action.sa_mask);
            if(sigaction(sig,&action,NULL)){failed=errno;break;}
        }
        if(!failed)failed=perform_actions(list,channel[1]);
        if(diagnostic)syscall(SYS_write,2,"SPAWN_CHILD_EXEC\n",17);
        if (!failed) { if(search)execvpe(path,argv,envp);else execve(path,argv,envp);failed=errno; }
        size_t at=0;while(at<sizeof(failed)){ssize_t n=syscall(SYS_write,channel[1],(char*)&failed+at,sizeof(failed)-at);if(n<0&&errno==EINTR)continue;if(n<=0)break;at+=n;}
        _exit(127);
    }
    free_actions(list);close(channel[1]); int failed=0;size_t at=0;
    while(at<sizeof(failed)) {
        ssize_t n=read(channel[0],(char*)&failed+at,sizeof(failed)-at);
        if(n<0&&errno==EINTR)continue;
        if(n<0){failed=errno;at=sizeof(failed);break;}
        if(!n)break;at+=n;
    }
    close(channel[0]);
    if(at) {
        if(at!=sizeof(failed))failed=EIO;
        kill(child,SIGKILL);
        while(waitpid(child,NULL,0)<0&&errno==EINTR){}
        return failed;
    }
    if(pid)*pid=child;return 0;
}
int posix_spawn(pid_t* pid, const char* path, const posix_spawn_file_actions_t* actions,
    const posix_spawnattr_t* attributes, char* const argv[], char* const envp[])
{
    return spawn_command(pid,path,actions,attributes,argv,envp,0);
}
int posix_spawnp(pid_t* pid, const char* path, const posix_spawn_file_actions_t* actions,
    const posix_spawnattr_t* attributes, char* const argv[], char* const envp[])
{
    return spawn_command(pid,path,actions,attributes,argv,envp,1);
}
typedef struct { pid_t child;sigset_t mask; } SystemWait;
static void system_release(void)
{
    pthread_mutex_lock(&system_lock);
    if(!--system_users){sigaction(SIGINT,&system_int,NULL);sigaction(SIGQUIT,&system_quit,NULL);}
    pthread_mutex_unlock(&system_lock);
}
static void system_cancel(void* argument)
{
    SystemWait* context=argument;
    if(context->child>0){kill(context->child,SIGKILL);while(waitpid(context->child,NULL,0)<0&&errno==EINTR){}}
    system_release();pthread_sigmask(SIG_SETMASK,&context->mask,NULL);
}
int system(const char* command)
{
    if (!command) {
        const char* libraries=getenv("HISH_NATIVE_LIBDIR");char file[PATH_MAX];
        if(!libraries)return 0;snprintf(file,sizeof(file),"%s/libhish_shell.so",libraries);return access(file,R_OK)==0;
    }
    int initialized=pthread_once(&actions_once,actions_setup);if(initialized||actions_setup_error)return error(initialized?initialized:actions_setup_error);
    int cancel;pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&cancel);
    struct sigaction ignored={0};ignored.sa_handler=SIG_IGN;sigemptyset(&ignored.sa_mask);
    pthread_mutex_lock(&system_lock);int saved=0;
    if(!system_users){
        if(sigaction(SIGINT,&ignored,&system_int))saved=errno;
        else if(sigaction(SIGQUIT,&ignored,&system_quit)){saved=errno;sigaction(SIGINT,&system_int,NULL);}
    }
    if(!saved)system_users++;
    sigset_t defaults;sigemptyset(&defaults);
    if(system_int.sa_handler!=SIG_IGN)sigaddset(&defaults,SIGINT);
    if(system_quit.sa_handler!=SIG_IGN)sigaddset(&defaults,SIGQUIT);
    pthread_mutex_unlock(&system_lock);
    if(saved){pthread_setcancelstate(cancel,NULL);return error(saved);}
    sigset_t blocked;sigemptyset(&blocked);sigaddset(&blocked,SIGCHLD);SystemWait context={.child=-1};
    saved=pthread_sigmask(SIG_BLOCK,&blocked,&context.mask);
    if(saved){system_release();pthread_setcancelstate(cancel,NULL);return error(saved);}
    posix_spawnattr_t attributes;posix_spawnattr_init(&attributes);
    posix_spawnattr_setsigmask(&attributes,&context.mask);posix_spawnattr_setsigdefault(&attributes,&defaults);
    posix_spawnattr_setflags(&attributes,POSIX_SPAWN_SETSIGMASK|POSIX_SPAWN_SETSIGDEF);
    char* args[]={"sh","-c",(char*)command,NULL};int status=-1;
    saved=posix_spawn(&context.child,"/bin/sh",NULL,&attributes,args,environ);posix_spawnattr_destroy(&attributes);
    pthread_cleanup_push(system_cancel,&context);
    pthread_setcancelstate(cancel,NULL);
    if(!saved){pid_t waited;do{waited=waitpid(context.child,&status,0);}while(waited<0&&errno==EINTR);if(waited<0){status=-1;saved=errno;}}
    else if(saved==ENOENT||saved==EACCES||saved==ENOEXEC)status=127<<8;
    pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,NULL);
    pthread_cleanup_pop(0);
    system_release();pthread_sigmask(SIG_SETMASK,&context.mask,NULL);pthread_setcancelstate(cancel,NULL);
    if(saved)errno=saved;return status;
}
FILE* popen(const char* command,const char* mode)
{
    if(!mode||(*mode!='r'&&*mode!='w')||(mode[1]&&(mode[1]!='e'||mode[2]))){errno=EINVAL;return NULL;}
    int initialized=pthread_once(&actions_once,actions_setup);if(initialized||actions_setup_error){errno=initialized?initialized:actions_setup_error;return NULL;}
    int cancel;pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&cancel);
    PipeStream* entry=calloc(1,sizeof(*entry));int descriptors[2],saved=0;
    if(!entry||pipe2(descriptors,O_CLOEXEC)){saved=entry?errno:ENOMEM;free(entry);pthread_setcancelstate(cancel,NULL);errno=saved;return NULL;}
    int reading=*mode=='r',parent=descriptors[reading?0:1],child=descriptors[reading?1:0];
    // fdopen before fork gives an allocation failure a clean rollback path.
    FILE* stream=fdopen(parent,reading?"r":"w");
    if(!stream){saved=errno;close(parent);close(child);free(entry);pthread_setcancelstate(cancel,NULL);errno=saved;return NULL;}
    posix_spawn_file_actions_t actions;saved=posix_spawn_file_actions_init(&actions);
    if(!saved){
        if(parent!=(reading?1:0))saved=posix_spawn_file_actions_addclose(&actions,parent);
        if(!saved)saved=posix_spawn_file_actions_adddup2(&actions,child,reading?1:0);
        if(!saved&&child!=(reading?1:0))saved=posix_spawn_file_actions_addclose(&actions,child);
        char* args[]={"sh","-c",(char*)command,NULL};opening_pipe=1;
        if(!saved)saved=posix_spawn(&entry->pid,"/bin/sh",&actions,NULL,args,environ);
        opening_pipe=0;posix_spawn_file_actions_destroy(&actions);
    }
    close(child);
    if(saved){fclose(stream);free(entry);pthread_setcancelstate(cancel,NULL);errno=saved;return NULL;}
    entry->stream=stream;entry->fd=parent;
    pthread_mutex_lock(&streams_lock);entry->next=pipe_streams;pipe_streams=entry;
    if(!mode[1])fcntl(parent,F_SETFD,0);
    pthread_mutex_unlock(&streams_lock);
    pthread_setcancelstate(cancel,NULL);return stream;
}
int pclose(FILE* stream)
{
    int cancel;pthread_setcancelstate(PTHREAD_CANCEL_DISABLE,&cancel);
    pthread_mutex_lock(&streams_lock);PipeStream** at=&pipe_streams;
    while(*at&&(*at)->stream!=stream)at=&(*at)->next;
    PipeStream* entry=*at;if(entry)*at=entry->next;
    // Close under the registry lock so a simultaneous fork cannot close a
    // recycled descriptor in its atfork handler.
    if(entry)fclose(stream);
    pthread_mutex_unlock(&streams_lock);
    if(!entry){pthread_setcancelstate(cancel,NULL);return error(ECHILD);}
    int status=-1;pid_t waited;do{waited=waitpid(entry->pid,&status,0);}while(waited<0&&errno==EINTR);
    int saved=errno;free(entry);pthread_setcancelstate(cancel,NULL);if(waited<0)return error(saved);return status;
}
// A real fork avoids mutating a suspended vfork parent's stack/address space
// while constructing a Linux ELF image. libc's private spawn remains separate.
pid_t vfork(void) { return fork(); }
