#pragma once
#include "hish_link_core.h"
#include "package_util.h"
#include <zlib.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

namespace HishPackage {
namespace fs = std::filesystem;
inline void Require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
inline std::vector<unsigned char> Read(const fs::path& path) {
    std::ifstream f(path, std::ios::binary); Require(bool(f), "read " + path.string());
    f.seekg(0, std::ios::end); auto size=f.tellg(); Require(size>=0 && size<=128*1024*1024, "file size limit");
    f.seekg(0); std::vector<unsigned char> bytes(static_cast<size_t>(size));
    f.read(reinterpret_cast<char*>(bytes.data()), bytes.size()); Require(bool(f), "short read"); return bytes;
}
inline std::vector<unsigned char> Inflate(const unsigned char* bytes, size_t size) {
    Require(size>0 && size<=64*1024*1024, "compressed size limit");
    std::vector<unsigned char> result; z_stream z{}; Require(inflateInit2(&z,31)==Z_OK,"gzip initialization");
    z.next_in=const_cast<Bytef*>(bytes); z.avail_in=size; unsigned char chunk[32768];
    try {
        while (true) {
            z.next_out=chunk; z.avail_out=sizeof chunk; int rc=inflate(&z,Z_NO_FLUSH);
            const size_t used=sizeof chunk-z.avail_out; Require(result.size()+used<=128*1024*1024,"uncompressed size limit");
            result.insert(result.end(),chunk,chunk+used);
            if (rc==Z_STREAM_END) {
                if (!z.avail_in) break;
                Bytef* next=z.next_in; unsigned left=z.avail_in;
                Require(inflateReset2(&z,31)==Z_OK,"gzip member reset"); z.next_in=next; z.avail_in=left;
            } else Require(rc==Z_OK && (used || z.avail_in),"invalid or truncated gzip");
        }
    } catch (...) { inflateEnd(&z); throw; }
    inflateEnd(&z); return result;
}
inline std::string SafeName(const std::string& name) {
    Require(!name.empty() && name.size()<4096 && name.front()!='/' && name.find('\\')==std::string::npos && name.find(':')==std::string::npos && name.find('\0')==std::string::npos,"unsafe archive path");
    fs::path p(name); for (const auto& part:p) Require(part!=".." && part!=".","unsafe archive component");
    return name;
}
inline void WriteMode(const fs::path& dest, unsigned mode) { fs::permissions(dest,fs::perms(mode&0777),fs::perm_options::replace); }
// Where the device refuses symlinks, a directory link cannot be stored as a copy. It becomes
// the emulated marker of hish_link_core.h, which HiSH's Linux layer shows as a link. The
// sticky bit is the flag, so this is the one place that sets special mode bits.
inline void WriteLinkMarker(const fs::path& dest, const std::string& target) {
    std::string bytes(HISH_LINK_MAGIC_LENGTH+target.size(),'\0');
    const size_t size=HishLinkFormat(target.data(),target.size(),&bytes[0]); if(!size)throw std::runtime_error("unusable link target: "+target);
    { std::ofstream out(dest,std::ios::binary|std::ios::trunc); out.write(bytes.data(),static_cast<std::streamsize>(size)); out.close(); if(!out)throw std::runtime_error("write link marker "+dest.string()); }
    fs::permissions(dest,fs::perms(HISH_LINK_MODE),fs::perm_options::replace);
}
inline void Write(const fs::path& root, const std::string& name, const unsigned char* bytes, size_t size, unsigned mode) {
    auto dest=root/SafeName(name); fs::create_directories(dest.parent_path());
    // Roots are fresh private generations; symlinks are only created after every file.
    Require(!fs::exists(dest) && !fs::is_symlink(dest),"duplicate archive file: "+name);
    std::ofstream out(dest,std::ios::binary); Require(bool(out),"create "+name);
    out.write(reinterpret_cast<const char*>(bytes),size); out.close(); Require(bool(out),"write "+name);
    WriteMode(dest,mode);
}
inline std::string Field(const unsigned char* p, size_t n) { return std::string(reinterpret_cast<const char*>(p),strnlen(reinterpret_cast<const char*>(p),n)); }
inline size_t Octal(const unsigned char* p,size_t n) {
    size_t value=0; for(size_t i=0;i<n;i++){if(p[i]==' '||!p[i])continue;Require(p[i]>='0'&&p[i]<='7'&&value<=(128*1024*1024)/8,"invalid tar number");value=value*8+p[i]-'0';}return value;
}
inline std::vector<std::string> Components(const std::string& path) {
    std::vector<std::string> out; size_t at=0;
    while(at<=path.size()){size_t end=path.find('/',at);if(end==std::string::npos)end=path.size();out.push_back(path.substr(at,end-at));at=end+1;}
    return out;
}
inline std::string Join(const std::vector<std::string>& parts) {
    std::string out; for(const auto& p:parts){if(!out.empty())out+='/';out+=p;} return out;
}
// Follow symlinks through a table of link path -> raw target without touching
// the filesystem. Fails if resolution leaves the root, loops or is too deep.
inline bool ResolveInRoot(const std::map<std::string,std::string>& symlinks,const std::string& path,std::string& out,int budget=40) {
    std::vector<std::string> done; std::deque<std::string> todo;
    for(auto& c:Components(path))todo.push_back(c);
    while(!todo.empty()){
        std::string comp=todo.front(); todo.pop_front();
        if(comp.empty()||comp==".")continue;
        if(comp==".."){if(done.empty())return false;done.pop_back();continue;}
        done.push_back(comp);
        auto it=symlinks.find(Join(done));
        if(it==symlinks.end())continue;
        if(--budget<0)return false;
        done.pop_back(); std::string target=it->second;
        if(!target.empty()&&target.front()=='/'){done.clear();target=target.substr(1);}
        auto parts=Components(target); for(auto p=parts.rbegin();p!=parts.rend();++p)todo.push_front(*p);
    }
    out=Join(done); return true;
}
enum class LinkMode { Copy, Real };
struct Link { std::string name,target,owner; unsigned mode; bool hard; };
struct Extractor {
    fs::path root; std::vector<Link> links; LinkMode linkMode=LinkMode::Copy;
    std::map<std::string,std::vector<OwnedFile>> owned;
    std::map<std::string,std::map<std::string,std::string>> scripts;
    std::map<std::string,unsigned> copiedLinks, realLinks, fileModes;
    std::string lastOwner;
    explicit Extractor(fs::path directory,LinkMode mode=LinkMode::Copy):root(std::move(directory)),linkMode(mode){}
    static std::string MetaValue(const std::string& metadata,const std::string& key) {
        size_t at=0;while(at<metadata.size()){size_t end=metadata.find('\n',at);if(end==std::string::npos)end=metadata.size();
            if(metadata.compare(at,key.size()+3,key+" = ")==0)return metadata.substr(at+key.size()+3,end-at-key.size()-3);at=end+1;}
        return "";
    }
    std::string Extract(const std::vector<unsigned char>& compressed,bool requireMetadata=true,const std::string& ownerHint="") {
        const auto tar=Inflate(compressed.data(),compressed.size());Require(tar.size()%512==0,"truncated tar block");std::string metadata,paxPath,paxLink,owner=ownerHint;
        for(size_t at=0;at+512<=tar.size();) {
            const auto* h=tar.data()+at; at+=512;
            if(std::all_of(h,h+512,[](unsigned char c){return !c;}))continue;
            size_t sum=0;for(size_t i=0;i<512;i++)sum+=i>=148&&i<156?32:h[i];Require(sum==Octal(h+148,8),"tar header checksum");
            const size_t size=Octal(h+124,12);Require(size<=tar.size()-at,"truncated tar body");
            std::string name=Field(h,100),prefix=Field(h+345,155),target=Field(h+157,100);if(!prefix.empty())name=prefix+"/"+name;
            const char type=h[156];const auto* data=tar.data()+at;
            if(type=='x'||type=='g') {
                for(size_t pos=0;pos<size;){size_t end=pos;while(end<size&&data[end]!=' ')end++;Require(end<size,"bad PAX record");
                    std::string number(reinterpret_cast<const char*>(data+pos),end-pos);Require(!number.empty()&&number.size()<10&&number.find_first_not_of("0123456789")==std::string::npos,"bad PAX length");
                    size_t length=std::stoul(number);Require(length>end-pos+1&&length<=size-pos&&data[pos+length-1]=='\n',"bad PAX length");
                    std::string item(reinterpret_cast<const char*>(data+end+1),pos+length-end-2);auto eq=item.find('=');
                    if(eq!=std::string::npos){if(item.substr(0,eq)=="path")paxPath=item.substr(eq+1);if(item.substr(0,eq)=="linkpath")paxLink=item.substr(eq+1);}
                    pos+=length;
                }
            } else {
                if(!paxPath.empty())name=paxPath;if(!paxLink.empty())target=paxLink;paxPath.clear();paxLink.clear();
                SafeName(name);const unsigned mode=Octal(h+100,8)&0777;
                if(name==".PKGINFO"){metadata.assign(reinterpret_cast<const char*>(data),size);const std::string n=MetaValue(metadata,"pkgname");if(!n.empty())owner=n;}
                else if(name.front()=='.') {
                    // Control scripts are saved verbatim for review and never executed.
                    if(name.rfind(".SIGN.",0)!=0&&name.find('/')==std::string::npos&&size<=256*1024)scripts[owner][name]=std::string(reinterpret_cast<const char*>(data),size);
                }
                else if(type=='5'){
                    std::string dir=name; while(dir.size()>1&&dir.back()=='/')dir.pop_back();
                    fs::create_directories(root/dir);owned[owner].push_back({dir,"D",mode});
                }
                else if(type=='0'||type==0){Write(root,name,data,size,mode);owned[owner].push_back({name,Sha256Hex(data,size),mode});fileModes[name]=mode;}
                else if(type=='1'||type=='2') {
                    Require(!target.empty()&&target.find('\0')==std::string::npos&&target.find('\\')==std::string::npos,"empty or unsafe package link");
                    // Hard-link targets are archive-root paths; symlink targets stay raw and are resolved against the final tree.
                    if(type=='1')SafeName(target);
                    links.push_back({name,target,owner,mode,type=='1'});
                } else throw std::runtime_error("unsupported archive member: "+name);
            }
            at+=(size+511)/512*512;Require(at<=tar.size(),"truncated tar padding");
        }
        Require(!requireMetadata||!metadata.empty(),"missing APK metadata");lastOwner=owner;return metadata;
    }
    // A link to a sizeable program can be an emulated link instead of a copy: HiSH's loaders
    // follow it when they start the program (git alone has 148 such links to a 3 MB binary).
    // A library link must stay a copy because ld.so cannot see the emulation when it loads one,
    // and a data file may be read by code that cannot see it either.
    static constexpr uintmax_t kProgramLinkMinimum=64*1024;
    static bool LibraryName(const std::string& path) {
        const std::string base=fs::path(path).filename().string();
        return base.find(".so.")!=std::string::npos||(base.size()>3&&base.compare(base.size()-3,3,".so")==0);
    }
    bool ProgramLink(const std::string& link,const std::string& target) const {
        const auto mode=fileModes.find(target); std::error_code ec;
        return mode!=fileModes.end()&&(mode->second&0111)&&!LibraryName(link)&&!LibraryName(target)&&fs::file_size(root/target,ec)>=kProgramLinkMinimum&&!ec;
    }
    // Resolve every link against the final tree. Copy mode produces exact file
    // copies, except for directory links and links to sizeable programs, which become
    // emulated links; real mode creates symlinks after all regular files exist.
    void MaterializeLinks() {
        std::map<std::string,std::string> symlinks;
        for(const auto& l:links)if(!l.hard)symlinks[l.name]=l.target;
        std::vector<Link> pending=links; links.clear();
        for(const auto& l:pending) {
            std::string at;
            if(l.hard) {
                Require(SafeName(l.target)==l.target&&ResolveInRoot(symlinks,l.target,at),"unresolved package hard link: "+l.name);
            } else {
                std::string parent=fs::path(l.name).parent_path().generic_string();
                std::string resolvedParent;Require(parent.empty()||ResolveInRoot(symlinks,parent,resolvedParent),"package link parent escapes root: "+l.name);
                std::string joined=l.target.front()=='/'?l.target:(parent.empty()?l.target:parent+"/"+l.target);
                Require(ResolveInRoot(symlinks,joined,at),"package link escapes root or loops: "+l.name);
            }
            const auto target=root/at;
            if(fs::is_directory(target)) {
                Require(!l.hard,"unsupported directory hard link: "+l.name);
            } else Require(fs::is_regular_file(target),"unresolved or non-file package link: "+l.name);
        }
        for(const auto& l:pending) {
            std::string at;std::map<std::string,std::string>& table=symlinks;
            if(l.hard)ResolveInRoot(table,l.target,at);
            else{std::string parent=fs::path(l.name).parent_path().generic_string();ResolveInRoot(table,l.target.front()=='/'?l.target:(parent.empty()?l.target:parent+"/"+l.target),at);}
            const auto target=root/at,dest=root/l.name;
            Require(!fs::exists(dest)&&!fs::is_symlink(dest),"duplicate archive file: "+l.name);
            fs::create_directories(dest.parent_path());
            if(!l.hard&&(linkMode==LinkMode::Real||fs::is_directory(target)||ProgramLink(l.name,at))) {
                std::string relative=fs::path(at).lexically_relative(fs::path(l.name).parent_path()).generic_string();
                Require(!relative.empty(),"link target");
                if(linkMode==LinkMode::Real)fs::create_symlink(relative,dest);else WriteLinkMarker(dest,relative);
                owned[l.owner].push_back({l.name,"L:"+relative,l.mode});realLinks[l.owner]++;
            } else {
                auto bytes=Read(target);Write(root,l.name,bytes.data(),bytes.size(),static_cast<unsigned>(fs::status(target).permissions())&0777);
                owned[l.owner].push_back({l.name,Sha256Hex(bytes.data(),bytes.size()),static_cast<unsigned>(fs::status(target).permissions())&0777});copiedLinks[l.owner]++;
            }
        }
    }
};
}
