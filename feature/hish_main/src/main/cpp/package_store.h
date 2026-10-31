#pragma once
#include "package_archive.h"
#include <functional>

#if defined(__unix__) || defined(__APPLE__)
#include <fcntl.h>
#include <unistd.h>
#endif

// Immutable generations plus one atomically replaced pointer file. Symlinks are
// never used for activation because the host may deny them.
namespace HishPackage {
inline void SyncPath(const fs::path& p) {
#if defined(__unix__) || defined(__APPLE__)
    int fd=open(p.c_str(),O_RDONLY|O_CLOEXEC); if(fd>=0){fsync(fd);close(fd);}
#else
    (void)p;
#endif
}
inline std::string ReadText(const fs::path& p) {
    std::ifstream in(p,std::ios::binary); if(!in)return ""; std::string s((std::istreambuf_iterator<char>(in)),std::istreambuf_iterator<char>()); return s;
}
inline void WriteText(const fs::path& p,const std::string& s,unsigned mode=0600) {
    fs::create_directories(p.parent_path()); std::ofstream out(p,std::ios::binary|std::ios::trunc); Require(bool(out),"create "+p.string());
    out.write(s.data(),s.size()); out.close(); Require(bool(out),"write "+p.string()); WriteMode(p,mode);
}
struct Store {
    fs::path base;
    std::function<void(const char*)> hook;
    explicit Store(fs::path b):base(std::move(b)){}
    fs::path Generations() const {return base/"generations";}
    fs::path ActiveFile() const {return base/"active";}
    fs::path Dir(const std::string& id) const {return Generations()/id;}
    fs::path Meta(const std::string& id) const {return Dir(id)/".hish-package";}
    void Point(const char* name) const {if(hook)hook(name);}
    static bool ValidId(const std::string& id){
        if(id.empty()||id.size()>40||id.front()=='.')return false;
        for(char c:id)if(!(isalnum(static_cast<unsigned char>(c))||c=='-'||c=='_'))return false; return true;
    }
    std::string Active() const {
        std::string id=ReadText(ActiveFile()); while(!id.empty()&&(id.back()=='\n'||id.back()=='\r'))id.pop_back();
        return ValidId(id)&&fs::is_directory(Dir(id))?id:"";
    }
    // A generation is complete when its marker equals the digest of its records.
    // The legacy layout (no marker) is accepted when its record table exists.
    bool Valid(const std::string& id) const {
        if(!ValidId(id)||!fs::is_directory(Dir(id)))return false;
        const fs::path marker=Meta(id)/"complete";
        if(!fs::exists(marker))return id.rfind("legacy-",0)==0&&fs::exists(Meta(id)/"installed.tsv");
        const std::string records=ReadText(Meta(id)/"installed.tsv"),world=ReadText(Meta(id)/"world");
        return ReadText(marker)==Sha256Hex(records.data(),records.size())+"\n"+Sha256Hex(world.data(),world.size())+"\n";
    }
    static std::string Marker(const std::string& records,const std::string& world){return Sha256Hex(records.data(),records.size())+"\n"+Sha256Hex(world.data(),world.size())+"\n";}
    std::vector<std::string> Ids() const {
        std::vector<std::string> out; if(!fs::exists(Generations()))return out;
        for(const auto& e:fs::directory_iterator(Generations())){const std::string n=e.path().filename().string();if(e.is_directory()&&ValidId(n))out.push_back(n);}
        std::sort(out.begin(),out.end()); return out;
    }
    std::string NewId() const {
        unsigned top=0; for(const auto& id:Ids()) if(id.size()==7&&id[0]=='g'){unsigned v=static_cast<unsigned>(strtoul(id.c_str()+1,nullptr,10));if(v>top)top=v;}
        char b[16]; snprintf(b,sizeof b,"g%06u",top+1); return b;
    }
    fs::path Stage() const {
        fs::create_directories(Generations());
        for(unsigned n=0;n<10000;n++){fs::path p=Generations()/(".tmp-"+std::to_string(n));std::error_code ec;if(fs::create_directory(p,ec))return p;}
        throw std::runtime_error("cannot create a private transaction directory");
    }
    // Commit point is the rename of the pointer file; a published but unreferenced
    // generation after a crash is retained and never activated implicitly.
    void Commit(const fs::path& staged,const std::string& id) {
        Require(ValidId(id)&&!fs::exists(Dir(id)),"generation id in use");
        Point("before-publish"); fs::rename(staged,Dir(id)); SyncPath(Generations());
        Point("after-publish");
        const fs::path next=base/"active.tmp"; WriteText(next,id+"\n"); SyncPath(next);
        Point("before-switch"); fs::rename(next,ActiveFile()); SyncPath(base);
        Point("after-switch");
    }
    // Returns a description of what was repaired; never contacts the network.
    std::vector<std::string> Recover() const {
        std::vector<std::string> notes;
        std::error_code ec;
        if(fs::exists(Generations()))for(const auto& e:fs::directory_iterator(Generations())){
            const std::string n=e.path().filename().string();
            if(n.rfind(".tmp-",0)==0){fs::remove_all(e.path(),ec);notes.push_back("discarded unfinished transaction "+n);}
        }
        if(fs::exists(base/"active.tmp")){fs::remove(base/"active.tmp",ec);notes.push_back("discarded unfinished activation record");}
        if(Active().empty()&&fs::exists(ActiveFile())){
            // Pointer names a missing/invalid generation: fall back to the newest complete one.
            std::string best; for(const auto& id:Ids())if(Valid(id))best=id;
            if(!best.empty()){WriteText(base/"active.tmp",best+"\n");fs::rename(base/"active.tmp",ActiveFile());notes.push_back("active pointer repaired to "+best);}
        }
        return notes;
    }
    // Keep the active generation, the newest complete previous one, and every
    // generation a running process may still use. Unfinished ones are kept too.
    std::vector<std::string> Collect(const std::function<bool(const std::string&)>& referenced,unsigned keep=2) const {
        std::vector<std::string> removed; const std::string active=Active(); std::set<std::string> keepSet;
        std::vector<std::string> ids=Ids(); if(!active.empty())keepSet.insert(active);
        unsigned seen=0; for(auto i=ids.rbegin();i!=ids.rend()&&seen<keep;++i)if(Valid(*i)){keepSet.insert(*i);seen++;}
        std::error_code ec;
        for(const auto& id:ids)if(!keepSet.count(id)&&Valid(id)&&!referenced(id)){fs::remove_all(Dir(id),ec);if(!ec)removed.push_back(id);ec.clear();}
        return removed;
    }
};
}
