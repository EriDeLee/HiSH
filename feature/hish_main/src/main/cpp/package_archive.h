#pragma once
#include <zlib.h>
#include <algorithm>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <stdexcept>
#include <string>
#include <vector>

namespace HishPackage {
namespace fs = std::filesystem;
inline void Require(bool ok, const std::string& message) { if (!ok) throw std::runtime_error(message); }
inline std::vector<unsigned char> Read(const fs::path& path) {
    std::ifstream f(path, std::ios::binary); Require(bool(f), "read " + path.string());
    f.seekg(0, std::ios::end); auto size=f.tellg(); Require(size>=0 && size<=64*1024*1024, "file size limit");
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
inline void Write(const fs::path& root, const std::string& name, const unsigned char* bytes, size_t size, unsigned mode) {
    auto dest=root/SafeName(name); fs::create_directories(dest.parent_path());
    // All roots are fresh, owned generations; no archive symlinks are created.
    Require(!fs::exists(dest),"duplicate archive file: "+name);
    std::ofstream out(dest,std::ios::binary); Require(bool(out),"create "+name);
    out.write(reinterpret_cast<const char*>(bytes),size); out.close(); Require(bool(out),"write "+name);
    fs::permissions(dest,fs::perms(mode&0777),fs::perm_options::replace);
}
inline std::string Field(const unsigned char* p, size_t n) { return std::string(reinterpret_cast<const char*>(p),strnlen(reinterpret_cast<const char*>(p),n)); }
inline size_t Octal(const unsigned char* p,size_t n) {
    size_t value=0; for(size_t i=0;i<n;i++){if(p[i]==' '||!p[i])continue;Require(p[i]>='0'&&p[i]<='7'&&value<=(128*1024*1024)/8,"invalid tar number");value=value*8+p[i]-'0';}return value;
}
struct Link { std::string name,target; unsigned mode; };
struct Extractor {
    fs::path root; std::vector<Link> links;
    explicit Extractor(fs::path directory):root(std::move(directory)){}
    std::string Extract(const std::vector<unsigned char>& compressed,bool requireMetadata=true) {
        const auto tar=Inflate(compressed.data(),compressed.size());Require(tar.size()%512==0,"truncated tar block");std::string metadata,paxPath,paxLink;
        for(size_t at=0;at+512<=tar.size();) {
            const auto* h=tar.data()+at; at+=512;
            if(std::all_of(h,h+512,[](unsigned char c){return !c;}))continue;
            size_t sum=0;for(size_t i=0;i<512;i++)sum+=i>=148&&i<156?32:h[i];Require(sum==Octal(h+148,8),"tar header checksum");
            const size_t size=Octal(h+124,12);Require(size<=tar.size()-at,"truncated tar body");
            std::string name=Field(h,100),prefix=Field(h+345,155),target=Field(h+157,100);if(!prefix.empty())name=prefix+"/"+name;
            const char type=h[156];const auto* data=tar.data()+at;
            if(type=='x'||type=='g') {
                // Only standard path/linkpath overrides affect extraction.
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
                if(name==".PKGINFO")metadata.assign(reinterpret_cast<const char*>(data),size);
                else if(name.front()=='.') { /* Signed control/scripts: never executed. */ }
                else if(type=='5')fs::create_directories(root/name);
                else if(type=='0'||type==0)Write(root,name,data,size,mode);
                else if(type=='1'||type=='2') {
                    Require(!target.empty(),"empty package link");fs::path resolved=target.front()=='/'?fs::path(target.substr(1)):(type=='1'?fs::path(target):fs::path(name).parent_path()/target);
                    const auto normal=resolved.lexically_normal().generic_string();SafeName(normal);links.push_back({name,normal,mode});
                } else throw std::runtime_error("unsupported archive member: "+name);
            }
            at+=(size+511)/512*512;Require(at<=tar.size(),"truncated tar padding");
        }
        Require(!requireMetadata||!metadata.empty(),"missing APK metadata");return metadata;
    }
    void MaterializeLinks() {
        // Alpine commonly lists a SONAME link before its real file. Resolve
        // after every dependency has been extracted; never copy a host target.
        for(size_t pass=0;!links.empty()&&pass<64;pass++) {
            size_t before=links.size();
            for(auto i=links.begin();i!=links.end();) {
                auto target=root/i->target;
                if(fs::is_regular_file(target)) { auto bytes=Read(target);Write(root,i->name,bytes.data(),bytes.size(),static_cast<unsigned>(fs::status(target).permissions()));i=links.erase(i); }
                else ++i;
            }
            Require(links.empty()||links.size()<before,"unresolved or non-file package link");
        }
        Require(links.empty(),"package link depth limit");
    }
};
}
