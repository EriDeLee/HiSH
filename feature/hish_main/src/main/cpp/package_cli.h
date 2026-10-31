#pragma once
#include "package_core.h"

namespace HishPackage {
inline const char* Help() {
    return "HiSH pkg - Alpine v3.23 main+community/aarch64, app-managed packages and configured remote sources\n"
           "Usage: pkg search [TERM] | info NAME... | list | update\n"
           "       pkg install NAME... | remove NAME... | upgrade\n"
           "       pkg doctor [--functional] [--capabilities]\n"
           "Repository package names and dependencies are not restricted by a catalog.\n"
           "App-managed packages come with the app and change only with an app update.\n"
           "Install scripts are not run; such packages are marked unconfigured.\n"
           "Installation and runtime compatibility are reported separately.";
}
inline std::string Origin(const Record& r){
    if(IsLocalSource(r.source))return "app-managed";
    if(r.source=="alpine")return "alpine";
    if(r.source.rfind("npm:",0)==0)return "npm";
    return r.source;
}
inline int List(const Context& c) {
    const State s=LoadState(c.store);
    for(const auto& kv:s.records){
        const Record& r=kv.second; std::string line=r.name+"\t"+r.version+"\t"+Origin(r)+"\t"+r.state;
        if(!s.world.count(r.name))line+="\tdependency";
        if(r.copiedLinks)line+="\tlinks-copied:"+std::to_string(r.copiedLinks);
        Say(line);
    }
    return 0;
}
inline int Search(Context& c,const std::string& term) {
    for(const auto& p:c.manifest.local)if(term.empty()||p.name.find(term)!=std::string::npos)Say(p.name+" "+p.version+" - app-managed: "+p.description);
    for(const auto& r:c.manifest.remote)if(term.empty()||r.name.find(term)!=std::string::npos)Say(r.name+" - "+r.kind+" "+r.package+": "+r.description);
    for(const auto& p:c.manifest.local)if(term.empty()||p.name.find(term)!=std::string::npos)Say("note: repository packages named "+p.name+" are listed below but never installed over the app-managed copy");
    return c.io.SearchAlpine(term);
}
inline int Info(Context& c,const std::vector<std::string>& names) {
    const State s=LoadState(c.store); std::vector<std::string> alpine;
    for(const auto& n:names){
        auto installed=s.records.find(n);
        if(const LocalPackage* p=FindLocal(c.manifest,n)){
            Say(p->name+" "+p->version+" (app-managed; changes only with an app update)");
            if(!p->description.empty())Say("  "+p->description);
            if(!p->origin.empty())Say("  origin: "+p->origin);
            if(!p->depends.empty()){std::string d;for(const auto& x:p->depends)d+=" "+x;Say("  depends:"+d);}
            Say("  payload: "+(p->kind=="apk"?p->payload+" sha256 "+p->sha256:std::to_string(p->files.size())+" files"));
        } else if(const RemoteSource* r=FindRemote(c.manifest,n)){
            Say(r->name+" ("+r->kind+" "+r->registry+"/"+r->package+")");
            if(!r->description.empty())Say("  "+r->description);
            const fs::path cached=c.StateDir()/"remote"/(r->name+".json");
            if(fs::exists(cached)){const Json j=ParseJson(ReadText(cached));Say("  latest known: "+j.At("version").Str()+"; integrity from registry metadata (not independently signed)");}
            else Say("  latest version unknown; run pkg update");
        } else alpine.push_back(n);
        if(installed!=s.records.end())Say("  installed: "+installed->second.version+" ("+Origin(installed->second)+", "+installed->second.state+")");
    }
    return alpine.empty()?0:c.io.PolicyAlpine(alpine);
}
inline void RequireNoMigration(const Context& c) {
    if(!MigrationPending(c))return;
    std::string why=ReadText(c.StateDir()/"migration-error"); while(!why.empty()&&why.back()=='\n')why.pop_back();
    throw std::runtime_error("the app was updated and its package migration is not complete"+(why.empty()?std::string():" ("+why+")")+
        "; package changes are blocked until it is. Open a new terminal session to retry");
}
inline int Update(Context& c) {
    int rc=c.io.UpdateIndexes();
    State s=LoadState(c.store); Transaction t(c,s);
    for(const auto& r:c.manifest.remote){
        try{const auto a=t.Validate(r,t.RemoteMetadata(r,"",true));Say(r.name+": latest "+a.version+" (registry metadata)");}
        catch(const std::exception& e){Say(r.name+": metadata refresh failed: "+e.what());rc=rc?rc:1;}
    }
    return rc;
}
inline int Doctor(Context& c,bool functional,bool capabilities) {
    const State s=LoadState(c.store); int rc=0;
    Say("generation: "+(s.exists?s.id+" "+s.root.string():std::string("none")));
    Say("app manifest: "+c.manifestSha.substr(0,16)+(MigrationPending(c)?" (migration pending)":" (accepted)"));
    if(fs::exists(c.StateDir()/"migration-error"))Say("last migration error: "+ReadText(c.StateDir()/"migration-error"));
    if(fs::exists(c.base/"root"))Say("previous layout directory kept: "+(c.base/"root").string());
    if(s.exists)Say(std::string("links: ")+(s.links=="real\n"?"real links":"copied as files (this device refused symlinks)"));
    for(const auto& kv:s.records){
        const Record& r=kv.second; std::string line=r.name+" "+r.version+": "+Origin(r)+", "+r.state;
        unsigned modified=0,missing=0; auto files=s.files.find(r.name);
        if(files!=s.files.end())for(const auto& f:files->second){if(f.sha256=="D"||f.sha256.rfind("L:",0)==0)continue;const fs::path p=s.root/f.path;if(!fs::is_regular_file(p))missing++;else if(HashFile(p.string())!=f.sha256)modified++;}
        else line+=", files not tracked";
        if(modified)line+=", "+std::to_string(modified)+" modified";
        if(missing)line+=", "+std::to_string(missing)+" missing";
        if(r.copiedLinks)line+=", "+std::to_string(r.copiedLinks)+" links stored as copies";
        if(r.state=="unconfigured")line+=" ("+r.notes.substr(16)+" not run; text in "+(c.store.Meta(s.id)/"scripts"/r.name).string()+")";
        Say(line);
    }
    if(capabilities){
        const fs::path temp=c.base/"state"/".probe"; fs::create_directories(temp); const LinkCaps caps=ProbeLinks(temp); fs::remove_all(temp);
        Say(std::string("capability symlink: ")+(caps.symlink?"supported":"refused"));
        Say(std::string("capability hardlink: ")+(caps.hardlink?"supported":"refused"));
        for(const char* name:{"ptrace-syscall","seccomp-user-notify"})Say(std::string("capability ")+name+": "+c.io.Probe(name));
    }
    if(functional){
        for(const auto& kv:s.records){
            auto check=c.manifest.checks.find(kv.first);
            if(check==c.manifest.checks.end()){if(s.world.count(kv.first))Say(kv.first+": installed; no built-in functional probe");continue;}
            const fs::path temp=c.base/"state"/(".check-"+kv.first); std::error_code ec; fs::remove_all(temp,ec); fs::create_directories(temp);
            const int result=c.io.RunCheck(check->second,s.root,temp); fs::remove_all(temp,ec);
            Say(kv.first+": functional probe "+(result?"FAILED (exit "+std::to_string(result)+")":std::string("passed")));
            if(result)rc=1;
        }
        Say("Packages without probes remain unverified.");
    } else Say("Run pkg doctor --functional to execute the available functional probes.");
    return rc;
}
inline int Command(Context& c,const std::vector<std::string>& args) {
    const std::string action=args.empty()?"help":args[0];
    std::vector<std::string> rest(args.begin()+(args.empty()?0:1),args.end());
    if(action=="help"||action=="--help"){Say(Help());return 0;}
    if(action=="--print-root"){const std::string id=c.store.Active();if(id.empty())return 1;Say(c.store.Dir(id).string());return 0;}
    for(const auto& note:c.store.Recover())Say("pkg: "+note);
    ImportLegacy(c);
    if(action=="list"){Require(rest.empty(),"pkg list takes no arguments");return List(c);}
    if(action=="search"){Require(rest.size()<=1,"pkg search takes at most one argument");return Search(c,rest.empty()?"":rest[0]);}
    if(action=="doctor"){
        bool functional=false,capabilities=false;
        for(const auto& a:rest){if(a=="--functional")functional=true;else if(a=="--capabilities")capabilities=true;else throw std::runtime_error("unknown doctor option: "+a);}
        return Doctor(c,functional,capabilities);
    }
    Require(action=="install"||action=="remove"||action=="upgrade"||action=="update"||action=="info","unknown pkg command: "+action);
    std::set<std::string> selected;
    for(const auto& a:rest){Require(selected.size()<64&&ValidName(a),"invalid package name: "+a);selected.insert(a);}
    if(action=="install"||action=="remove"||action=="info")Require(!selected.empty(),"a package name is required");
    if(action=="update"||action=="upgrade")Require(selected.empty(),"command takes no package arguments");
    if(action=="info")return Info(c,rest);
    if(action=="update")return Update(c);
    RequireNoMigration(c);
    const State s=LoadState(c.store); Plan p; p.world=s.world;
    if(action=="install"){p.world.insert(selected.begin(),selected.end());p.explicitInstall=selected;}
    else if(action=="remove"){
        for(const auto& n:selected){
            if(!p.world.erase(n)){
                Require(s.records.count(n)>0,"package is not installed: "+n);
                throw std::runtime_error(n+" is installed as a dependency, not as a requested package");
            }
        }
        // App-managed dependencies are re-added by resolution; report why.
        const Manifest m=Effective(c,s);
        for(const auto& n:selected)for(const auto& kv:s.records)if(p.world.count(kv.first))if(const LocalPackage* q=FindLocal(m,kv.first))for(const auto& d:q->depends)
            if(ParseDependency(d).name==n)Say(n+" stays installed: required by "+kv.first);
    }
    else p.upgrade=true;
    Commit(c,p);
    Say("Package transaction complete. Installation does not imply runtime compatibility.");
    return 0;
}
}
