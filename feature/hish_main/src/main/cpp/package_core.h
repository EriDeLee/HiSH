#pragma once
#include "package_plan.h"
#include "package_store.h"
#include <functional>

// Package transactions. All device access (network, APK client, processes,
// /proc) goes through Backend, so the same logic runs in host tests.
namespace HishPackage {
inline void Say(const std::string& s){fputs(s.c_str(),stdout);fputc('\n',stdout);fflush(stdout);}
struct Backend {
    virtual ~Backend()=default;
    virtual void FetchAlpine(const std::set<std::string>& names,const fs::path& dir)=0;
    virtual void VerifyApk(const fs::path& file)=0;
    virtual int UpdateIndexes()=0;
    virtual int SearchAlpine(const std::string& term)=0;
    virtual int PolicyAlpine(const std::vector<std::string>& names)=0;
    virtual std::string FetchText(const std::string& url)=0;
    virtual void FetchArtifact(const std::string& url,const fs::path& dest,const std::string& sha512)=0;
    virtual void CheckArtifact(const fs::path& file,const std::string& sha512)=0;
    virtual int RunCheck(const std::string& script,const fs::path& root,const fs::path& temp)=0;
    // True unless it is certain that no process uses the generation.
    virtual bool Referenced(const fs::path& generation)=0;
    virtual std::string Probe(const std::string&){return "not available";}
};
struct Context {
    fs::path base,payloads; std::string manifestText,manifestSha; Manifest manifest; Backend& io; Store store;
    std::string forceLinks; // "", "copy" or "real" (tests)
    Context(fs::path b,fs::path p,const std::string& text,Backend& backend)
        :base(std::move(b)),payloads(std::move(p)),manifestText(text),manifestSha(Sha256Hex(text.data(),text.size())),manifest(ParseManifest(text)),io(backend),store(base){}
    fs::path StateDir() const {return base/"state";}
    fs::path Cache() const {return base/"cache";}
};
struct State {
    std::string id; fs::path root; bool exists=false; std::set<std::string> world;
    std::map<std::string,Record> records; std::map<std::string,std::vector<OwnedFile>> files;
    std::map<std::string,std::string> pkginfo,localJson; std::string links;
};
inline State LoadState(const Store& store) {
    State s; s.id=store.Active(); if(s.id.empty())return s;
    Require(store.Valid(s.id),"active package generation "+s.id+" is incomplete");
    s.exists=true; s.root=store.Dir(s.id); const fs::path meta=store.Meta(s.id);
    s.world=ParseWorld(ReadText(meta/"world")); s.links=ReadText(meta/"links");
    for(const auto& r:ParseRecords(ReadText(meta/"installed.tsv"))) {
        s.records[r.name]=r;
        if(fs::exists(meta/"files"/(r.name+".tsv")))s.files[r.name]=ParseFiles(ReadText(meta/"files"/(r.name+".tsv")));
        if(fs::exists(meta/"metadata"/(r.name+".PKGINFO")))s.pkginfo[r.name]=ReadText(meta/"metadata"/(r.name+".PKGINFO"));
        if(fs::exists(meta/"local"/(r.name+".json")))s.localJson[r.name]=ReadText(meta/"local"/(r.name+".json"));
    }
    return s;
}
// Current manifest plus local entries that an earlier app version installed
// and the current one no longer provides. They stay reserved and are kept.
inline Manifest Effective(const Context& c,const State& s) {
    Manifest m=c.manifest;
    for(const auto& kv:s.localJson)if(!FindLocal(m,kv.first)&&IsLocalSource(s.records.at(kv.first).source)){
        LocalPackage p=ParseLocal(ParseJson(kv.second)); p.retired=true; m.local.push_back(p);
    }
    return m;
}
struct LinkCaps { bool symlink=false,hardlink=false; };
inline LinkCaps ProbeLinks(const fs::path& dir) {
    LinkCaps caps; std::error_code ec; const fs::path a=dir/".probe-file",s=dir/".probe-symlink",h=dir/".probe-hardlink";
    WriteText(a,"x");
    fs::create_symlink(".probe-file",s,ec); caps.symlink=!ec&&fs::read_symlink(s,ec)==".probe-file"&&!ec; ec.clear();
    fs::create_hard_link(a,h,ec); caps.hardlink=!ec&&fs::equivalent(a,h,ec)&&!ec;
    fs::remove(s,ec);fs::remove(h,ec);fs::remove(a,ec); return caps;
}
inline void Place(const fs::path& from,const fs::path& to,unsigned mode,bool hard) {
    fs::create_directories(to.parent_path());
    Require(!fs::exists(to)&&!fs::is_symlink(to),"file conflict: "+to.filename().string());
    if(hard){std::error_code ec;fs::create_hard_link(from,to,ec);if(!ec)return;}
    fs::copy_file(from,to); WriteMode(to,mode);
}
struct Transaction {
    Context& c; const State& s; Manifest m; fs::path stage; LinkCaps caps;
    std::vector<Record> records; std::map<std::string,std::vector<OwnedFile>> owned; std::map<std::string,std::string> pkginfo,localJson;
    std::map<std::string,std::map<std::string,std::string>> scripts; std::vector<std::string> messages; std::set<std::string> fresh;
    Transaction(Context& context,const State& state):c(context),s(state),m(Effective(context,state)){}
    bool Have(const std::string& n) const {for(const auto& r:records)if(r.name==n)return true;return false;}
    // Carry an installed package unchanged. Modified files keep the user's bytes
    // and the original recorded digest, so the change stays visible.
    void Carry(const std::string& name) {
        const Record& r=s.records.at(name); auto it=s.files.find(name);
        if(it==s.files.end()){
            messages.push_back(name+": record kept, but its files are not tracked and are not in the new generation; reinstall it");
            records.push_back(r); return;
        }
        for(const auto& f:it->second) {
            const fs::path dest=stage/f.path;
            if(f.sha256=="D"){fs::create_directories(dest);continue;}
            if(f.sha256.rfind("L:",0)==0){fs::create_directories(dest.parent_path());Require(!fs::exists(dest)&&!fs::is_symlink(dest),"file conflict: "+f.path);fs::create_symlink(f.sha256.substr(2),dest);continue;}
            const fs::path src=s.root/f.path;
            Require(fs::is_regular_file(src),"installed file of "+name+" is missing: "+f.path+"; remove and reinstall the package");
            if(HashFile(src.string())!=f.sha256)messages.push_back(name+": modified file kept: "+f.path);
            Place(src,dest,f.mode,caps.hardlink);
        }
        records.push_back(r); owned[name]=it->second;
        if(s.pkginfo.count(name))pkginfo[name]=s.pkginfo.at(name);
        if(s.localJson.count(name))localJson[name]=s.localJson.at(name);
        const fs::path old=c.store.Meta(s.id)/"scripts"/name;
        if(fs::exists(old))for(const auto& e:fs::directory_iterator(old))scripts[name][e.path().filename().string()]=ReadText(e.path());
    }
    void InstallLocal(const LocalPackage& p,Extractor& ex) {
        Require(!p.retired,"app-managed package "+p.name+" is no longer provided by this app version");
        Record r; r.name=p.name; r.version=p.version; r.source="local:manifest"; localJson[p.name]=LocalJson(p);
        if(p.kind=="apk") {
            const fs::path file=c.payloads/p.payload; Require(fs::is_regular_file(file)&&HashFile(file.string())==p.sha256,"app package payload is damaged: "+p.name+"; reinstall the app");
            const std::string meta=ex.Extract(Read(file),true,p.name);
            Require(MetaValue(meta,"pkgname")==p.name&&MetaValue(meta,"pkgver")==p.version,"app package metadata mismatch: "+p.name);
            pkginfo[p.name]=meta;
        } else {
            for(const auto& f:p.files){
                const fs::path file=c.payloads/f.payload; Require(fs::is_regular_file(file)&&HashFile(file.string())==f.sha256,"app package payload is damaged: "+p.name+"; reinstall the app");
                Place(file,stage/f.path,f.mode,false); owned[p.name].push_back({f.path,f.sha256,f.mode});
            }
        }
        r.digest=LocalDigest(p);
        records.push_back(r); fresh.insert(p.name);
    }
    void Local(const std::string& name,Extractor& ex,bool migration,const std::set<std::string>& explicitInstall) {
        if(Have(name))return;
        auto it=s.records.find(name); const LocalPackage* p=FindLocal(m,name);
        if(it==s.records.end()){InstallLocal(*p,ex);return;}
        if(!IsLocalSource(it->second.source)) {
            Require(!explicitInstall.count(name),name+" is installed from '"+it->second.source+"', not from the app; run pkg remove "+name+" first");
            messages.push_back(name+": kept unverified copy ("+it->second.source+"); remove it to use the app-managed package");
            Carry(name); return;
        }
        if(migration&&!p->retired&&(it->second.version!=p->version||it->second.digest!=LocalDigest(*p))) {
            messages.push_back(name+": "+it->second.version+" -> "+p->version+" (app update)"); InstallLocal(*p,ex); return;
        }
        Carry(name);
    }
    // Offline dependency closure over installed APK metadata. Returns nullopt
    // when any requirement is not already satisfied by an installed package.
    std::optional<std::pair<std::set<std::string>,std::set<std::string>>> Closure(const std::set<std::string>& wanted) const {
        std::map<std::string,std::string> provider;
        for(const auto& kv:s.records){
            if(kv.second.source!="alpine"||!s.files.count(kv.first)||!s.pkginfo.count(kv.first))continue;
            provider[kv.first]=kv.first;
            for(const auto& pr:MetaValues(s.pkginfo.at(kv.first),"provides"))provider[ParseDependency(pr).name]=kv.first;
        }
        std::set<std::string> alpine,local; std::vector<std::string> todo(wanted.begin(),wanted.end());
        while(!todo.empty()){
            const std::string want=todo.back(); todo.pop_back(); std::string version;
            if(const LocalPackage* p=LocalProviding(m,want,version)){local.insert(p->name);continue;}
            auto it=provider.find(want); if(it==provider.end())return std::nullopt;
            if(!alpine.insert(it->second).second)continue;
            for(const auto& token:MetaValues(s.pkginfo.at(it->second),"depend")){const auto d=ParseDependency(token);if(!d.conflict)todo.push_back(d.name);}
        }
        return std::make_pair(alpine,local);
    }
    void Alpine(const std::set<std::string>& names,bool upgrade,bool offline,Extractor& ex,bool migration,const std::set<std::string>& explicitInstall) {
        if(names.empty())return;
        if(!upgrade)if(auto closure=Closure(names)){
            for(const auto& n:closure->first)Carry(n);
            for(const auto& n:closure->second)Local(n,ex,migration,explicitInstall);
            return;
        }
        Require(!offline,"repository packages that are not installed are required; this step does not use the network");
        const fs::path downloads=stage/".downloads"; fs::create_directories(downloads);
        c.io.FetchAlpine(names,downloads);
        std::vector<fs::path> files; for(const auto& e:fs::directory_iterator(downloads))files.push_back(e.path());
        std::sort(files.begin(),files.end());
        std::set<std::string> dropped; std::vector<std::string> metas;
        for(const auto& file:files) {
            const std::string fname=file.filename().string(),name=PackageFromFile(fname);
            Require(fs::is_regular_file(file)&&ValidName(name),"unexpected downloaded file: "+fname);
            if(Reserved(m,name)){dropped.insert(name);continue;}
            c.io.VerifyApk(file);
            std::string meta;
            try{meta=ex.Extract(Read(file),true,name);}catch(const std::exception& e){throw std::runtime_error(fname+": "+e.what());}
            const std::string pname=MetaValue(meta,"pkgname"),version=MetaValue(meta,"pkgver"),arch=MetaValue(meta,"arch");
            Require(pname==name&&!version.empty()&&(arch=="aarch64"||arch=="noarch"),"wrong package metadata or architecture: "+fname);
            Require(!Have(pname),"duplicate package: "+pname);
            Record r; r.name=pname; r.version=version; r.source="alpine"; r.digest="sha256:"+HashFile(file.string());
            records.push_back(r); pkginfo[pname]=meta; fresh.insert(pname); metas.push_back(meta);
        }
        for(const auto& n:dropped){messages.push_back(n+": repository copy not used; app-managed package is used instead");Local(n,ex,migration,explicitInstall);}
        // A resolver that downloads nothing must not drop packages that are
        // already installed, and a requested name that is neither downloaded nor
        // installed fails the whole transaction instead of publishing a smaller
        // generation.
        for(const auto& n:names){
            if(Have(n))continue;
            if(s.records.count(n)){Carry(n);continue;}
            std::string provider;
            for(const auto& kv:s.pkginfo)for(const auto& token:MetaValues(kv.second,"provides"))
                if(ParseDependency(token).name==n&&s.records.count(kv.first))provider=kv.first;
            if(!provider.empty()){Carry(provider);continue;}
            throw std::runtime_error("package not found in the repository: "+n+"; transaction not activated");
        }
        // Every repository requirement on an app-managed name must hold for the local version.
        for(const auto& meta:metas)for(const auto& token:MetaValues(meta,"depend")){
            const auto d=ParseDependency(token); std::string version; const LocalPackage* p=LocalProviding(m,d.name,version);
            if(!p||!Have(p->name))continue;
            if(d.conflict)throw std::runtime_error("dependency conflict: "+MetaValue(meta,"pkgname")+" conflicts with app-managed "+p->name);
            if(!d.op.empty()&&!Satisfies(version,d.op,d.version))
                throw std::runtime_error("dependency conflict: "+MetaValue(meta,"pkgname")+" requires "+token+", app-managed "+p->name+" provides "+(version.empty()?"no version":version)+"; transaction not activated");
        }
        fs::remove_all(downloads);
    }
    std::string RemoteMetadata(const RemoteSource& r,const std::string& version,bool refresh) {
        const fs::path cached=c.StateDir()/"remote"/(r.name+".json");
        if(version.empty()&&!refresh&&fs::exists(cached))return ReadText(cached);
        std::string encoded; for(char ch:r.package)encoded+=ch=='/'?std::string("%2f"):std::string(1,ch);
        const std::string text=c.io.FetchText(r.registry+"/"+encoded+"/"+(version.empty()?"latest":version));
        if(version.empty())WriteText(cached,text);
        return text;
    }
    struct Artifact { std::string version,url,sha512; };
    Artifact Validate(const RemoteSource& r,const std::string& text) const {
        const Json j=ParseJson(text); Artifact a;
        Require(j.At("name").Str()==r.package,"registry metadata names a different package");
        a.version=j.At("version").Str(); Require(!a.version.empty()&&a.version.find_first_of("/ \t\n")==std::string::npos,"invalid registry version");
        const Json& dist=j.At("dist"); a.url=dist.At("tarball").Str();
        Require(a.url.rfind(r.registry+"/",0)==0&&a.url.find("..")==std::string::npos,"registry tarball URL is outside the configured registry");
        const std::string integrity=dist.At("integrity").Str(); Require(integrity.rfind("sha512-",0)==0,"registry metadata has no SHA-512 integrity");
        a.sha512=Base64ToHex(integrity.substr(7)); Require(IsHex(a.sha512,128),"invalid registry integrity");
        auto has=[&](const char* key,const char* value){const Json* v=j.Find(key);if(!v)return true;for(const auto& i:v->List())if(i.Str()==value)return true;return false;};
        Require(has("os","linux")&&has("cpu","arm64")&&has("libc","musl"),"registry package is not for linux/arm64/musl");
        return a;
    }
    void Remote(const std::string& name,bool upgrade) {
        const RemoteSource& r=*FindRemote(m,name); auto it=s.records.find(name);
        const std::string source="npm:"+r.registry+"/"+r.package;
        const bool installed=it!=s.records.end()&&it->second.source==source;
        Artifact a;
        if(installed&&!upgrade){Carry(name);return;}
        a=Validate(r,RemoteMetadata(r,"",upgrade));
        if(installed&&it->second.digest=="sha512:"+a.sha512){Carry(name);return;}
        const fs::path cache=c.Cache()/(name+"-"+a.sha512+".tgz"); fs::create_directories(cache.parent_path());
        if(fs::exists(cache))c.io.CheckArtifact(cache,a.sha512);
        else {const fs::path part=cache.string()+".part";std::error_code ec;fs::remove(part,ec);c.io.FetchArtifact(a.url,part,a.sha512);fs::rename(part,cache);}
        const fs::path unpack=stage/(".unpack-"+name); fs::create_directories(unpack);
        Extractor tx(unpack); tx.Extract(Read(cache),false); tx.MaterializeLinks();
        const fs::path member=unpack/r.member; Require(fs::is_regular_file(member),"registry archive lacks "+r.member);
        const fs::path dest=stage/r.install; fs::create_directories(dest.parent_path()); Require(!fs::exists(dest),"file conflict: "+r.install);
        fs::rename(member,dest); WriteMode(dest,0700); owned[name].push_back({r.install,HashFile(dest.string()),0700});
        for(const auto& e:r.extra){Write(stage,e.path,reinterpret_cast<const unsigned char*>(e.content.data()),e.content.size(),e.mode);owned[name].push_back({e.path,Sha256Hex(e.content.data(),e.content.size()),e.mode});}
        fs::remove_all(unpack);
        Record rec; rec.name=name; rec.version=a.version; rec.source=source; rec.digest="sha512:"+a.sha512; records.push_back(rec); fresh.insert(name);
    }
    // A user-modified file of a freshly installed package keeps the user's
    // bytes; the package's new bytes are placed next to it as PATH.hish-new.
    void PreserveModified() {
        for(const auto& name:fresh){
            auto it=s.files.find(name); if(it==s.files.end())continue;
            for(const auto& f:it->second){
                if(f.sha256=="D"||f.sha256.rfind("L:",0)==0)continue;
                const fs::path old=s.root/f.path,now=stage/f.path;
                if(!fs::is_regular_file(old)||!fs::is_regular_file(now)||HashFile(old.string())==f.sha256)continue;
                fs::rename(now,now.string()+".hish-new"); fs::copy_file(old,now); WriteMode(now,f.mode);
                messages.push_back(name+": modified file kept: "+f.path+" (new version saved as "+f.path+".hish-new)");
            }
        }
    }
};
struct Plan { std::set<std::string> world,explicitInstall; bool upgrade=false,migration=false,offline=false; };
inline std::string Describe(const Record& r){return r.name+" "+r.version;}
inline std::string Commit(Context& c,const Plan& plan) {
    State s=LoadState(c.store); Transaction t(c,s);
    // Requested names that only an earlier app version could supply are kept.
    std::set<std::string> resolvable,retained;
    for(const auto& n:plan.world){
        auto it=s.records.find(n);
        if(!FindLocal(t.m,n)&&!FindRemote(t.m,n)&&it!=s.records.end()&&it->second.source!="alpine")retained.insert(n); else resolvable.insert(n);
    }
    const Resolution r=Resolve(t.m,resolvable);
    t.stage=c.store.Stage();
    try {
        t.caps=ProbeLinks(t.stage);
        if(c.forceLinks=="copy")t.caps.symlink=false; if(c.forceLinks=="real")t.caps.symlink=true;
        Extractor ex(t.stage,t.caps.symlink?LinkMode::Real:LinkMode::Copy);
        for(const auto& n:r.local)t.Local(n,ex,plan.migration,plan.explicitInstall);
        for(const auto& n:retained){t.messages.push_back(n+": kept; its source is not configured in this app version");t.Carry(n);}
        for(const auto& n:r.remote){Require(!plan.offline||(s.records.count(n)&&!plan.upgrade),n+" is not installed; this step does not use the network");t.Remote(n,plan.upgrade);}
        t.Alpine(r.alpine,plan.upgrade,plan.offline,ex,plan.migration,plan.explicitInstall);
        try{ex.MaterializeLinks();}catch(const std::exception& e){throw std::runtime_error(std::string(e.what())+"; transaction not activated");}
        for(auto& kv:ex.owned)if(t.fresh.count(kv.first))for(const auto& f:kv.second)t.owned[kv.first].push_back(f);
        for(auto& rec:t.records)if(t.fresh.count(rec.name)){
            rec.copiedLinks=ex.copiedLinks[rec.name];
            const auto& sc=ex.scripts[rec.name]; std::string skipped;
            for(const auto& kv:sc){t.scripts[rec.name][kv.first]=kv.second;if(kv.first!=".pre-deinstall"&&kv.first!=".post-deinstall"){if(!skipped.empty())skipped+=",";skipped+=kv.first;}}
            if(!skipped.empty()){rec.state="unconfigured";rec.notes="scripts-not-run:"+skipped;}
        }
        t.PreserveModified();
        std::sort(t.records.begin(),t.records.end(),[](const Record& a,const Record& b){return a.name<b.name;});
        const fs::path meta=t.stage/".hish-package"; std::string world; for(const auto& n:plan.world)world+=n+"\n";
        const std::string table=FormatRecords(t.records);
        for(const auto& rec:t.records){
            WriteText(meta/"files"/(rec.name+".tsv"),FormatFiles(t.owned[rec.name]));
            if(t.pkginfo.count(rec.name))WriteText(meta/"metadata"/(rec.name+".PKGINFO"),t.pkginfo[rec.name]);
            if(t.localJson.count(rec.name))WriteText(meta/"local"/(rec.name+".json"),t.localJson[rec.name]);
            for(const auto& kv:t.scripts[rec.name])WriteText(meta/"scripts"/rec.name/kv.first,kv.second);
        }
        WriteText(meta/"links",t.caps.symlink?"real\n":"copy\n"); WriteText(meta/"manifest",c.manifestSha+"\n");
        WriteText(meta/"world",world); WriteText(meta/"installed.tsv",table); WriteText(meta/"complete",Store::Marker(table,world));
        const std::string id=c.store.NewId(); c.store.Commit(t.stage,id);
        // Summary against the previous generation.
        std::map<std::string,Record> now; for(const auto& rec:t.records)now[rec.name]=rec;
        for(const auto& kv:now){auto old=s.records.find(kv.first);
            if(old==s.records.end())Say("installed "+Describe(kv.second)+" ("+kv.second.source+")");
            else if(old->second.version!=kv.second.version||old->second.digest!=kv.second.digest)Say("changed "+kv.first+" "+old->second.version+" -> "+kv.second.version);
            if(kv.second.state=="unconfigured"&&(old==s.records.end()||old->second.state!="unconfigured"))Say(kv.first+": install scripts were not run ("+kv.second.notes.substr(16)+"); marked unconfigured, see pkg doctor");}
        for(const auto& kv:s.records)if(!now.count(kv.first))Say("removed "+Describe(kv.second));
        if(plan.upgrade)for(const auto& kv:now)if(IsLocalSource(kv.second.source))Say("kept app-managed "+Describe(kv.second)+"; it changes only with an app update");
        for(const auto& msg:t.messages)Say(msg);
        c.store.Collect([&](const std::string& g){return c.io.Referenced(c.store.Dir(g));});
        return id;
    } catch(...) { std::error_code ec; fs::remove_all(t.stage,ec); throw; }
}

// ---- previous layout import (native/packages/root) ----
inline bool ImportLegacy(Context& c) {
    if(!c.store.Active().empty())return false;
    fs::path old=c.base/"root"; if(!fs::exists(old/".hish-package"/"installed.tsv"))old=c.base/"previous";
    if(!fs::exists(old/".hish-package"/"installed.tsv"))return false;
    const std::string world=ReadText(old/".hish-package"/"world"),table=ReadText(old/".hish-package"/"installed.tsv");
    WriteText(c.StateDir()/"legacy-snapshot"/"world",world); WriteText(c.StateDir()/"legacy-snapshot"/"installed.tsv",table);
    const fs::path stage=c.store.Stage();
    try {
        for(auto it=fs::recursive_directory_iterator(old);it!=fs::recursive_directory_iterator();++it){
            const fs::path rel=fs::relative(it->path(),old);
            if(rel.begin()->string()==".hish-package"){it.disable_recursion_pending();continue;}
            if(it->is_symlink()){it.disable_recursion_pending();continue;}
            if(it->is_directory())fs::create_directories(stage/rel);
            // Copies, not hard links: the old directory stays independent.
            else if(it->is_regular_file())Place(it->path(),stage/rel,static_cast<unsigned>(it->status().permissions())&0777,false);
        }
        const fs::path meta=stage/".hish-package"; std::vector<Record> out;
        for(Record r:ParseRecords(table)){
            const std::string hint=LegacyHint(r),legacyColumn=r.source; std::vector<OwnedFile> owned; r.legacy=false; r.notes="imported";
            if(const LocalPackage* p=FindLocal(c.manifest,r.name)) {
                // Compare with what the app payload would install.
                std::vector<OwnedFile> expected;
                if(p->kind=="files")for(const auto& f:p->files)expected.push_back({f.path,f.sha256,f.mode});
                else {const fs::path probe=stage/".probe";fs::create_directories(probe);Extractor ex(probe);ex.Extract(Read(c.payloads/p->payload),true,p->name);ex.MaterializeLinks();expected=ex.owned[p->name];fs::remove_all(probe);}
                bool same=r.version==p->version;
                for(auto& f:expected){if(f.sha256=="D")continue;const fs::path at=stage/f.path;
                    if(!fs::is_regular_file(at)){same=false;continue;}const std::string actual=HashFile(at.string());if(actual!=f.sha256)same=false;owned.push_back({f.path,actual,f.mode});}
                if(same){r.source="local:manifest";r.digest=LocalDigest(*p);r.state="configured";WriteText(meta/"local"/(r.name+".json"),LocalJson(*p));}
                else if(hint=="port"){
                    // Installed by the previous app version from its own payload:
                    // app-managed by origin. The digest differs on purpose, so the
                    // app migrator replaces it with this version's payload.
                    r.source="local:manifest";r.digest="legacy-app-payload";r.state="configured";WriteText(meta/"local"/(r.name+".json"),LocalJson(*p));
                }
                else {r.source="legacy-unknown";r.state="legacy-unknown";r.digest="-";}
            } else if(const RemoteSource* src=FindRemote(c.manifest,r.name);src&&hint=="npm") {
                r.source="npm:"+src->registry+"/"+src->package; r.digest="sha512:"+legacyColumn.substr(16);
                for(const std::string& path:[&]{std::vector<std::string> v{src->install};for(const auto& e:src->extra)v.push_back(e.path);return v;}())
                    if(fs::is_regular_file(stage/path))owned.push_back({path,HashFile((stage/path).string()),static_cast<unsigned>(fs::status(stage/path).permissions())&0777});
                r.state="configured";
            } else if(hint=="alpine") {
                r.source="alpine"; r.state="configured";
                if(fs::exists(old/".hish-package"/"metadata"/(r.name+".txt")))WriteText(meta/"metadata"/(r.name+".PKGINFO"),ReadText(old/".hish-package"/"metadata"/(r.name+".txt")));
            } else {r.source="legacy-unknown";r.state="legacy-unknown";}
            if(!owned.empty())WriteText(meta/"files"/(r.name+".tsv"),FormatFiles(owned));
            out.push_back(r);
        }
        const std::string newTable=FormatRecords(out);
        WriteText(meta/"links","copy\n"); WriteText(meta/"world",world); WriteText(meta/"installed.tsv",newTable); WriteText(meta/"complete",Store::Marker(newTable,world));
        c.store.Commit(stage,c.store.NewId());
        Say("pkg: imported the previous package layout; the old directory is kept unchanged");
        return true;
    } catch(...) {std::error_code ec;fs::remove_all(stage,ec);throw;}
}

// ---- app-side migration to a new HAP manifest ----
inline fs::path AcceptedFile(const Context& c){return c.StateDir()/"accepted-manifest";}
// Pending when an installed app-managed package differs from what this app
// version provides under the same name. Retired names are simply kept.
inline bool MigrationPending(const Context& c) {
    const State s=LoadState(c.store);
    for(const auto& kv:s.records){
        if(!IsLocalSource(kv.second.source))continue;
        const LocalPackage* p=FindLocal(c.manifest,kv.first);
        if(p&&(kv.second.version!=p->version||kv.second.digest!=LocalDigest(*p)))return true;
    }
    return false;
}
// Runs only from the app's session start, never from a pkg command.
inline bool Migrate(Context& c,std::string& error) {
    try {
        ImportLegacy(c);
        for(const auto& note:c.store.Recover())Say("pkg: "+note);
        if(!MigrationPending(c)){WriteText(AcceptedFile(c),c.manifestSha+"\n");return true;}
        const State s=LoadState(c.store); Plan p; p.world=s.world; p.migration=true; p.offline=true;
        Commit(c,p); WriteText(AcceptedFile(c),c.manifestSha+"\n"); std::error_code ec; fs::remove(c.StateDir()/"migration-error",ec);
        return true;
    } catch(const std::exception& e) {
        error=e.what(); WriteText(c.StateDir()/"migration-error",error+"\n"); return false;
    }
}
}
