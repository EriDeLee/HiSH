#include "package_archive.h"
#include "native_elf.h"
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <fcntl.h>
#include <set>
#include <sstream>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>

using namespace HishPackage;
namespace {
#if defined(__aarch64__)
#include "package_upstream.h"
extern "C" const unsigned char HiSHPackageData[],HiSHPackageDataEnd[];
bool PackageName(const std::string& name) {
    return !name.empty()&&name.size()<128&&isalnum(static_cast<unsigned char>(name.front()))&&name.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+_.-")==std::string::npos;
}
std::string Text(const fs::path& p) {auto b=Read(p);return std::string(b.begin(),b.end());}
void TextFile(const fs::path& p,const std::string& s) {Write(p.parent_path(),p.filename().string(),reinterpret_cast<const unsigned char*>(s.data()),s.size(),0600);}
std::string Value(const std::string& metadata,const std::string& key) {
    std::istringstream stream(metadata);std::string line;while(std::getline(stream,line))if(line.rfind(key+" = ",0)==0)return line.substr(key.size()+3);return "";
}
fs::path Temporary(const fs::path& parent,const char* stem) {
    std::string p=(parent/(std::string(stem)+"XXXXXX")).string();Require(mkdtemp(p.data())!=nullptr,"create private generation: "+std::string(strerror(errno)));return p;
}
struct Lock {
    int fd;
    explicit Lock(const fs::path& p):fd(open(p.c_str(),O_CREAT|O_RDWR|O_CLOEXEC,0600)) {if(fd<0)throw std::runtime_error("open package lock");if(flock(fd,LOCK_EX|LOCK_NB)){close(fd);throw std::runtime_error("another package operation is running");}}
    ~Lock(){close(fd);}
};
fs::path Bootstrap(const fs::path& base) {
    const fs::path dest=base/("bootstrap-" HISH_PACKAGE_BOOTSTRAP_SHA256);
    if(fs::exists(dest/"ready"))return dest;
    const auto stage=Temporary(base,".bootstrap-");
    try {
        auto bytes=Inflate(HiSHPackageData,HiSHPackageDataEnd-HiSHPackageData);Require(bytes.size()>=8&&!memcmp(bytes.data(),"HISHPKG1",8),"bootstrap format");
        size_t at=8;while(at<bytes.size()) {
            Require(bytes.size()-at>=12,"truncated bootstrap");uint32_t n=0,size=0,mode=0;
            memcpy(&n,bytes.data()+at,4);memcpy(&size,bytes.data()+at+4,4);memcpy(&mode,bytes.data()+at+8,4);at+=12;
            Require(n&&n<4096&&n<=bytes.size()-at&&size<=bytes.size()-at-n,"bootstrap bounds");std::string name(reinterpret_cast<const char*>(bytes.data()+at),n);at+=n;
            Write(stage,name,bytes.data()+at,size,mode);at+=size;
        }
        fs::create_directories(stage/"lib/apk/db");TextFile(stage/"ready","1\n");
        fs::rename(stage,dest);return dest;
    } catch (...) {fs::remove_all(stage);throw;}
}
int Execute(const fs::path& executable,const std::vector<std::string>& arguments,const fs::path& libraries,const fs::path& certificate={}) {
    fflush(nullptr);pid_t pid=fork();Require(pid>=0,"fork package process");
    if(pid==0) {
        setenv("LD_LIBRARY_PATH",libraries.c_str(),1);
        if(!certificate.empty()) {setenv("SSL_CERT_FILE",certificate.c_str(),1);setenv("APK_CONFIG","/dev/null",1);unsetenv("SSL_NO_VERIFY_HOSTNAME");unsetenv("SSL_CERT_DIR");}
        std::vector<char*> argv;std::string command=executable.filename().string();argv.push_back(command.data());for(const auto& arg:arguments)argv.push_back(const_cast<char*>(arg.c_str()));argv.push_back(nullptr);
        HiSHExecFile(executable.c_str(),argv.data());dprintf(2,"pkg: execute %s: %s (%s)\n",executable.c_str(),strerror(errno),HiSHExecError());_exit(126);
    }
    int status;pid_t done;do{done=waitpid(pid,&status,0);}while(done<0&&errno==EINTR);Require(done==pid,"wait for package process");return WIFEXITED(status)?WEXITSTATUS(status):128+WTERMSIG(status);
}
int Apk(const fs::path& boot,const std::vector<std::string>& args) {
    std::vector<std::string> command{"--root",boot.string(),"--arch","aarch64","--no-cache","--timeout","30"};command.insert(command.end(),args.begin(),args.end());
    return Execute(boot/"sbin/apk",command,boot/"lib",boot/"etc/ssl/certs/ca-certificates.crt");
}
std::set<std::string> World(const fs::path& root) {
    std::set<std::string> world;if(!fs::exists(root/".hish-package/world"))return world;
    std::istringstream in(Text(root/".hish-package/world"));std::string name;while(std::getline(in,name)){Require(PackageName(name),"invalid installed world entry");world.insert(name);}return world;
}
void Check(const fs::path& root,const std::set<std::string>& world,const fs::path& boot) {
    const std::string libraries=(root/"usr/lib").string()+":"+(root/"lib").string();
    if(world.count("jq"))Require(Execute(root/"usr/bin/jq",{"-ne","({answer: (6*7), text: \"中文\"} | .answer == 42 and .text == \"中文\")"},libraries)==0,"jq functional check failed");
    if(world.count("zstd")) {
        const auto temp=Temporary(root,".check-");
        try {std::string payload;for(int i=0;i<8192;i++)payload+="HiSH zstd roundtrip 中文\n";TextFile(temp/"input",payload);
            Require(Execute(root/"usr/bin/zstd",{"-q","-f",(temp/"input").string(),"-o",(temp/"input.zst").string()},libraries)==0,"zstd compression check failed");
            Require(Execute(root/"usr/bin/zstd",{"-q","-d","-f",(temp/"input.zst").string(),"-o",(temp/"output").string()},libraries)==0,"zstd decompression check failed");
            Require(Text(temp/"output")==payload,"zstd roundtrip bytes differ");fs::remove_all(temp);
        }catch(...){fs::remove_all(temp);throw;}
    }
    if(world.count("bun"))Require(Execute(root/"usr/bin/bun",{"-e","import assert from 'node:assert/strict';assert.equal(6*7,42);const c=Bun.spawn([process.execPath,'-e','console.log(42)'],{stdout:'pipe'});assert.equal((await new Response(c.stdout).text()).trim(),'42');assert.equal(await c.exited,0);console.log('Bun script/subprocess check passed');"},libraries)==0,"bun functional check failed");
    if(world.count("proot")){
        const auto temp=Temporary(root,".check-proot-");
        try {const auto guest=temp/"root";fs::create_directories(guest/"bin");fs::create_directories(guest/"lib");fs::create_directories(guest/"proc");fs::create_directories(guest/"dev");
            auto probe=Read(boot/"sbin/proot-probe");Write(guest,"bin/probe",probe.data(),probe.size(),0700);TextFile(guest/"hish-marker","HISH_ROOTFS_42\n");
            char native[PATH_MAX];Require(HiSHNativeLibraryDirectory(native,sizeof native)==0,"native loader path unavailable");auto loader=Read(fs::path(native)/"libhish_linux_musl_loader.so");Write(guest,"lib/ld-musl-aarch64.so.1",loader.data(),loader.size(),0700);
            Require(Execute(root/"usr/bin/proot",{"-r",guest.string(),"-w","/","-b","/proc:/proc","-b","/dev:/dev","/bin/probe"},libraries)==0,"PRoot rootfs/fork/exec check failed");fs::remove_all(temp);
        }catch(...){fs::remove_all(temp);throw;}
    }
    for(const auto& name:world)if(name!="jq"&&name!="zstd"&&name!="bun"&&name!="proot")printf("%s: installed; no built-in functional probe.\n",name.c_str());
}
void Install(const fs::path& base,const fs::path& boot,const std::set<std::string>& world) {
    const auto stage=Temporary(base,".transaction-");const auto candidate=stage/"root",downloads=stage/"downloads";fs::create_directories(candidate);fs::create_directory(downloads);
    try {
        std::string receipt,desired;for(const auto& name:world)desired+=name+"\n";
        if(!world.empty()) {
            std::set<std::string> alpine=world;alpine.erase("bun");alpine.erase("proot");if(world.count("proot"))alpine.insert("talloc");
            if(world.count("bun")){std::istringstream requires(HishUpstream::Bun_alpineDependencies);std::string name;while(requires>>name){Require(PackageName(name),"invalid Bun dependency metadata");alpine.insert(name);}}
            if(!alpine.empty()){
                std::vector<std::string> args{"fetch","--recursive","--output",downloads.string()};args.insert(args.end(),alpine.begin(),alpine.end());
                Require(Apk(boot,args)==0,"download/verification failed; installed generation retained");
            }
            Extractor extractor(candidate);std::set<std::string> seen;
            for(const auto& item:fs::directory_iterator(downloads)) {
                Require(item.is_regular_file()&&item.path().extension()==".apk","unexpected downloaded file");
                Require(Apk(boot,{"verify",item.path().string()})==0,"APK signature rejected: "+item.path().filename().string());
                const auto metadata=extractor.Extract(Read(item.path())),name=Value(metadata,"pkgname"),version=Value(metadata,"pkgver"),arch=Value(metadata,"arch");
                Require(PackageName(name)&&seen.insert(name).second,"invalid or duplicate package name: "+name);
                Require(!version.empty()&&(arch=="aarch64"||arch=="noarch"),"wrong package architecture/metadata");receipt+=name+"\t"+version+"\n";
                TextFile(candidate/".hish-package/metadata"/(name+".txt"),metadata);
            }
            for(const auto& name:alpine)Require(seen.count(name),"missing requested package");
            extractor.MaterializeLinks();
            if(world.count("bun")){
                const auto file=downloads/"bun.tgz",unpack=stage/"bun";fs::create_directory(unpack);
                const auto cache=base/"cache"/(std::string("bun-")+HishUpstream::Bun_sha512+".tgz");fs::create_directories(cache.parent_path());
                if(fs::exists(cache)){
                    Require(Execute(boot/"sbin/fetch-helper",{"--hash",cache.string(),HishUpstream::Bun_sha512},boot/"lib")==0,"cached Bun checksum failed; installed generation retained");fs::copy_file(cache,file);
                }else {Require(Execute(boot/"sbin/fetch-helper",{HishUpstream::Bun_url,file.string(),HishUpstream::Bun_sha512},boot/"lib",boot/"etc/ssl/certs/ca-certificates.crt")==0,"Bun download/checksum failed; installed generation retained");fs::copy_file(file,cache);}
                Extractor bun(unpack);bun.Extract(Read(file),false);bun.MaterializeLinks();fs::create_directories(candidate/"usr/bin");Require(!fs::exists(candidate/"usr/bin/bun"),"Bun path conflict");fs::rename(unpack/HishUpstream::Bun_member,candidate/"usr/bin/bun");fs::permissions(candidate/"usr/bin/bun",fs::perms(0700));
                receipt+="bun\t"+std::string(HishUpstream::Bun_version)+"\tupstream-sha512:"+HishUpstream::Bun_sha512+"\n";
                const std::string bunx="#!/bin/sh\nexec bun x \"$@\"\n";Write(candidate,"usr/bin/bunx",reinterpret_cast<const unsigned char*>(bunx.data()),bunx.size(),0700);
            }
            if(world.count("proot")){
                auto binary=Read(boot/"sbin/proot-hish");Write(candidate,"usr/bin/proot",binary.data(),binary.size(),0700);
                TextFile(candidate/".hish-package/metadata/proot.json",Text(boot/"etc/hish/proot-provenance.json"));receipt+="proot\t"+std::string(HishUpstream::Proot_version)+"\tplatform-port-source:"+HishUpstream::Proot_commit+"\n";
            }
        }
        TextFile(candidate/".hish-package/world",desired);TextFile(candidate/".hish-package/installed.tsv",receipt);
        const auto active=base/"root",previous=base/"previous";
        if(fs::exists(previous))fs::remove_all(previous);
        const bool old=fs::exists(active);if(old)fs::rename(active,previous);
        try{fs::rename(candidate,active);}catch(...){if(old)fs::rename(previous,active);throw;}
        fs::remove_all(stage);puts("Package transaction complete. Installation does not imply runtime compatibility.\nRun pkg doctor for available functional probes; upstream install scripts are not run.");
    } catch (...) {fs::remove_all(stage);throw;}
}
#endif
}
extern "C" int HiSHPackageMain(char* const argv[]) {
    try {
#if defined(__aarch64__)
        const std::string action=argv[1]?argv[1]:"help";
        if(action=="help"||action=="--help") {puts("HiSH pkg - Alpine v3.23 main+community/aarch64; official Bun distribution\nUsage: pkg search [NAME] | update | install NAME... | remove NAME...\n       pkg list | info NAME | upgrade | doctor\nRepository package names and dependencies are not restricted by a catalog.\nInstall scripts/services are not run; links become private copies.\nInstallation and runtime compatibility are reported separately.");return 0;}
        const char* prefix=getenv("PREFIX");Require(prefix&&*prefix== '/',"PREFIX unavailable");
        const auto base=fs::path(prefix).parent_path()/"packages",root=base/"root";
        fs::create_directories(base);Lock lock(base/"lock");
        // Recover a kill between the two activation renames before any command.
        if(!fs::exists(root)&&fs::exists(base/"previous"))fs::rename(base/"previous",root);
        if(action=="list") {Require(!argv[2],"pkg list takes no arguments");if(fs::exists(root/".hish-package/installed.tsv"))printf("%s",Text(root/".hish-package/installed.tsv").c_str());return 0;}
        if(action=="search") {Require(!argv[2]||!argv[3],"pkg search takes at most one argument");if(!argv[2]||std::string("bun").find(argv[2])!=std::string::npos)puts("bun - official pinned ARM64 musl distribution");if(!argv[2]||std::string("proot").find(argv[2])!=std::string::npos)puts("proot - source-built HiSH platform port");std::vector<std::string> args{"search","--"};if(argv[2])args.push_back(argv[2]);return Apk(Bootstrap(base),args);}
        if(action=="doctor") {Require(!argv[2],"pkg doctor takes no arguments");auto world=World(root);if(world.empty()){puts("No applications installed.");return 0;}Check(root,world,Bootstrap(base));puts("Available functional probes passed; packages without probes remain unverified.");return 0;}
        Require(action=="install"||action=="remove"||action=="upgrade"||action=="update"||action=="info","unknown pkg command");
        std::set<std::string> selected;
        for(size_t i=2;argv[i];i++){Require(i<64&&PackageName(argv[i]),"invalid package name: "+std::string(argv[i]));selected.insert(argv[i]);}
        if(action=="install"||action=="remove"||action=="info")Require(!selected.empty(),"a package name is required");
        if(action=="update"||action=="upgrade")Require(selected.empty(),"command takes no package arguments");
        const auto boot=Bootstrap(base);
        if(action=="update")return Apk(boot,{"update"});
        if(action=="info") {if(selected.erase("bun"))printf("bun %s\n%s\nSHA512 %s\n",HishUpstream::Bun_version,HishUpstream::Bun_url,HishUpstream::Bun_sha512);if(selected.erase("proot"))printf("proot %s\nHiSH platform source port; commit %s; dependency talloc\n",HishUpstream::Proot_version,HishUpstream::Proot_commit);if(selected.empty())return 0;std::vector<std::string> args{"policy"};args.insert(args.end(),selected.begin(),selected.end());return Apk(boot,args);}
        auto world=World(root);
        if(action=="install")world.insert(selected.begin(),selected.end());
        else if(action=="remove")for(const auto& name:selected)Require(world.erase(name)>0,"package is not installed: "+name);
        Install(base,boot,world);return 0;
#else
        (void)argv;fputs("pkg: repository packages require an ARM64 device\n",stderr);return 1;
#endif
    } catch(const std::exception& e){fprintf(stderr,"pkg: %s\n",e.what());return 1;}
}
