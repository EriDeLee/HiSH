#pragma once
#include "package_util.h"
#include <cctype>
#include <map>
#include <optional>
#include <set>
#include <sstream>

// Pure planning logic: manifest parsing, local-package locking, state records
// and APK dependency/version semantics. No I/O and no package names.
namespace HishPackage {
inline bool ValidName(const std::string& n) {
    return !n.empty()&&n.size()<128&&isalnum(static_cast<unsigned char>(n.front()))&&n.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789+_.-")==std::string::npos;
}
inline bool SafeRelative(const std::string& p) {
    if(p.empty()||p.size()>=4096||p.front()=='/'||p.find('\\')!=std::string::npos||p.find(':')!=std::string::npos||p.find('\0')!=std::string::npos)return false;
    size_t at=0;while(at<=p.size()){size_t end=p.find('/',at);if(end==std::string::npos)end=p.size();const std::string part=p.substr(at,end-at);if(part.empty()||part=="."||part=="..")return false;at=end+1;}
    return true;
}

// ---- APK versions (apk-tools version.c token order) ----
struct ApkVersion { std::vector<unsigned long long> numbers; int letter=0; std::vector<std::pair<int,unsigned long long>> suffixes; unsigned long long revision=0; };
inline bool ParseApkVersion(const std::string& v,ApkVersion& out) {
    out=ApkVersion{}; size_t i=0;
    auto number=[&](unsigned long long& n){
        if(i>=v.size()||!isdigit(static_cast<unsigned char>(v[i])))return false; n=0;
        while(i<v.size()&&isdigit(static_cast<unsigned char>(v[i]))){if(n>100000000000000000ULL)return false;n=n*10+static_cast<unsigned>(v[i]-'0');i++;}
        return true;
    };
    unsigned long long n; if(!number(n))return false; out.numbers.push_back(n);
    while(i<v.size()&&v[i]=='.'){i++;if(!number(n))return false;out.numbers.push_back(n);}
    if(i<v.size()&&islower(static_cast<unsigned char>(v[i]))&&(i+1==v.size()||v[i+1]=='_'||v[i+1]=='-'||v[i+1]=='~')){out.letter=v[i];i++;}
    static const char* pre[]={"alpha","beta","pre","rc"}; static const char* post[]={"cvs","svn","git","hg","p"};
    while(i<v.size()&&v[i]=='_'){
        i++; size_t start=i; while(i<v.size()&&islower(static_cast<unsigned char>(v[i])))i++;
        const std::string word=v.substr(start,i-start); int rank=0;
        for(int k=0;k<4;k++)if(word==pre[k])rank=k-4;
        for(int k=0;k<5;k++)if(word==post[k])rank=k+1;
        if(!rank)return false;
        unsigned long long sn=0; if(i<v.size()&&isdigit(static_cast<unsigned char>(v[i]))&&!number(sn))return false;
        out.suffixes.emplace_back(rank,sn);
    }
    if(i<v.size()&&v[i]=='~'){i++;size_t start=i;while(i<v.size()&&isxdigit(static_cast<unsigned char>(v[i])))i++;if(i==start)return false;}
    if(i<v.size()&&v[i]=='-'){if(v.compare(i,2,"-r"))return false;i+=2;if(!number(out.revision))return false;}
    return i==v.size();
}
inline int CompareApk(const ApkVersion& a,const ApkVersion& b) {
    for(size_t k=0;k<std::min(a.numbers.size(),b.numbers.size());k++)if(a.numbers[k]!=b.numbers[k])return a.numbers[k]<b.numbers[k]?-1:1;
    if(a.numbers.size()!=b.numbers.size())return a.numbers.size()<b.numbers.size()?-1:1;
    if(a.letter!=b.letter)return a.letter<b.letter?-1:1;
    for(size_t k=0;k<std::max(a.suffixes.size(),b.suffixes.size());k++){
        const auto x=k<a.suffixes.size()?a.suffixes[k]:std::make_pair(0,0ULL),y=k<b.suffixes.size()?b.suffixes[k]:std::make_pair(0,0ULL);
        if(x!=y)return x<y?-1:1;
    }
    if(a.revision!=b.revision)return a.revision<b.revision?-1:1;
    return 0;
}
// Unparseable versions or unknown operators never satisfy a constraint.
inline bool Satisfies(const std::string& have,const std::string& op,const std::string& want) {
    if(op.empty())return true;
    if(op=="~"){ApkVersion h,w;if(!ParseApkVersion(have,h))return false;if(!ParseApkVersion(want,w)&&!ParseApkVersion(want+"-r0",w))return false;
        return have.compare(0,want.size(),want)==0&&(have.size()==want.size()||!isdigit(static_cast<unsigned char>(have[want.size()])));}
    ApkVersion h,w; if(!ParseApkVersion(have,h)||!ParseApkVersion(want,w))return false;
    const int c=CompareApk(h,w);
    if(op=="=")return c==0; if(op==">=")return c>=0; if(op=="<=")return c<=0; if(op==">")return c>0; if(op=="<")return c<0;
    return false;
}
struct Dependency { std::string name,op,version; bool conflict=false; };
inline Dependency ParseDependency(std::string token) {
    Dependency d; if(!token.empty()&&token[0]=='!'){d.conflict=true;token.erase(0,1);}
    const size_t at=token.find_first_of("<>=~");
    if(at==std::string::npos){d.name=token;return d;}
    d.name=token.substr(0,at); size_t end=at; while(end<token.size()&&strchr("<>=~",token[end]))end++;
    d.op=token.substr(at,end-at); d.version=token.substr(end); return d;
}
inline std::vector<std::string> MetaValues(const std::string& metadata,const std::string& key) {
    std::vector<std::string> out; std::istringstream in(metadata); std::string line;
    while(std::getline(in,line))if(line.rfind(key+" = ",0)==0)out.push_back(line.substr(key.size()+3));
    return out;
}
inline std::string MetaValue(const std::string& metadata,const std::string& key){auto v=MetaValues(metadata,key);return v.empty()?"":v.front();}

// ---- manifest ----
struct FileEntry { std::string path,payload,sha256; unsigned mode=0600; };
struct LocalPackage {
    std::string name,version,kind,description,payload,sha256,origin;
    std::vector<std::string> depends,provides; std::vector<FileEntry> files; bool retired=false;
};
struct ExtraFile { std::string path,content; unsigned mode=0600; };
struct RemoteSource {
    std::string name,kind,description,registry,package,member,install;
    std::vector<std::string> alpineDependencies; std::vector<ExtraFile> extra;
};
struct Manifest { int format=0; std::vector<LocalPackage> local; std::vector<RemoteSource> remote; std::map<std::string,std::string> checks; };
inline std::vector<std::string> Strings(const Json* v) {
    std::vector<std::string> out; if(!v)return out; for(const auto& i:v->List())out.push_back(i.Str()); return out;
}
inline LocalPackage ParseLocal(const Json& item) {
    LocalPackage p; p.name=item.At("name").Str(); p.version=item.At("version").Str(); p.kind=item.At("kind").Str();
    p.description=item.StrOr("description",""); p.origin=item.StrOr("origin","");
    p.depends=Strings(item.Find("depends")); p.provides=Strings(item.Find("provides"));
    if(!ValidName(p.name)||p.version.empty())throw std::runtime_error("invalid local package: "+p.name);
    for(const auto& d:p.depends)if(!ValidName(ParseDependency(d).name))throw std::runtime_error("invalid dependency in "+p.name);
    if(p.kind=="apk") {
        p.payload=item.At("payload").Str(); p.sha256=item.At("sha256").Str();
        if(!SafeRelative(p.payload)||!IsHex(p.sha256,64))throw std::runtime_error("invalid local payload: "+p.name);
    } else if(p.kind=="files") {
        for(const auto& f:item.At("files").List()) {
            FileEntry e; e.path=f.At("path").Str(); e.payload=f.At("payload").Str(); e.sha256=f.At("sha256").Str(); e.mode=static_cast<unsigned>(f.At("mode").Int())&0777;
            if(!SafeRelative(e.path)||!SafeRelative(e.payload)||!IsHex(e.sha256,64))throw std::runtime_error("invalid local file: "+p.name);
            p.files.push_back(e);
        }
        if(p.files.empty())throw std::runtime_error("local package has no files: "+p.name);
    } else throw std::runtime_error("unknown local package kind: "+p.kind);
    return p;
}
inline std::string LocalJson(const LocalPackage& p) {
    auto list=[](const std::vector<std::string>& v){std::string s="[";for(size_t i=0;i<v.size();i++){if(i)s+=",";s+=Quote(v[i]);}return s+"]";};
    std::string s="{\"name\":"+Quote(p.name)+",\"version\":"+Quote(p.version)+",\"kind\":"+Quote(p.kind)+",\"description\":"+Quote(p.description)+",\"origin\":"+Quote(p.origin)+
        ",\"depends\":"+list(p.depends)+",\"provides\":"+list(p.provides);
    if(p.kind=="apk")s+=",\"payload\":"+Quote(p.payload)+",\"sha256\":"+Quote(p.sha256);
    else {s+=",\"files\":[";for(size_t i=0;i<p.files.size();i++){const auto& f=p.files[i];if(i)s+=",";s+="{\"path\":"+Quote(f.path)+",\"payload\":"+Quote(f.payload)+",\"sha256\":"+Quote(f.sha256)+",\"mode\":"+std::to_string(f.mode)+"}";}s+="]";}
    return s+"}";
}
// Identity of an app-managed package's content as recorded at install time.
inline std::string LocalDigest(const LocalPackage& p) {
    if(p.kind=="apk")return "sha256:"+p.sha256;
    const std::string j=LocalJson(p); return "sha256:"+Sha256Hex(j.data(),j.size());
}
inline Manifest ParseManifest(const std::string& text) {
    const Json root=ParseJson(text); Manifest m; m.format=static_cast<int>(root.At("format").Int());
    // Unknown formats are refused instead of being half understood.
    if(m.format!=1)throw std::runtime_error("unsupported package manifest format "+std::to_string(m.format));
    std::set<std::string> names;
    for(const auto& item:root.At("local").List()) {
        LocalPackage p=ParseLocal(item);
        if(!names.insert(p.name).second)throw std::runtime_error("duplicate package in manifest: "+p.name);
        m.local.push_back(p);
    }
    for(const auto& item:root.At("remote").List()) {
        RemoteSource r; r.name=item.At("name").Str(); r.kind=item.At("kind").Str(); r.description=item.StrOr("description","");
        if(!ValidName(r.name)||!names.insert(r.name).second)throw std::runtime_error("invalid or duplicate remote source: "+r.name);
        if(r.kind!="npm")throw std::runtime_error("unknown remote source kind: "+r.kind);
        r.registry=item.At("registry").Str(); r.package=item.At("package").Str(); r.member=item.At("member").Str(); r.install=item.At("install").Str();
        if(r.registry.rfind("https://",0)||r.registry.back()=='/'||!SafeRelative(r.member)||!SafeRelative(r.install)||r.package.empty()||r.package.find_first_of(" \t\n?#")!=std::string::npos)
            throw std::runtime_error("invalid remote source: "+r.name);
        r.alpineDependencies=Strings(item.Find("alpineDependencies"));
        for(const auto& d:r.alpineDependencies)if(!ValidName(d))throw std::runtime_error("invalid dependency in "+r.name);
        if(const Json* extra=item.Find("extra"))for(const auto& f:extra->List()) {
            ExtraFile e; e.path=f.At("path").Str(); e.content=f.At("content").Str(); e.mode=static_cast<unsigned>(f.At("mode").Int())&0777;
            if(!SafeRelative(e.path))throw std::runtime_error("invalid extra file in "+r.name);
            r.extra.push_back(e);
        }
        m.remote.push_back(r);
    }
    if(const Json* c=root.Find("checks"))for(const auto& kv:c->members){if(!ValidName(kv.first))throw std::runtime_error("invalid check name");m.checks[kv.first]=kv.second.Str();}
    return m;
}
inline const LocalPackage* FindLocal(const Manifest& m,const std::string& n){for(const auto& p:m.local)if(p.name==n)return &p;return nullptr;}
inline const RemoteSource* FindRemote(const Manifest& m,const std::string& n){for(const auto& p:m.remote)if(p.name==n)return &p;return nullptr;}
// Names a remote repository may never supply: every local main package name,
// installed or not, including retained entries of a previous app version.
inline bool Reserved(const Manifest& m,const std::string& n){return FindLocal(m,n)!=nullptr;}
inline const LocalPackage* LocalProviding(const Manifest& m,const std::string& name,std::string& version) {
    for(const auto& p:m.local){
        if(p.name==name){version=p.version;return &p;}
        for(const auto& pr:p.provides){const auto d=ParseDependency(pr);if(d.name==name){version=d.op=="="?d.version:"";return &p;}}
    }
    return nullptr;
}
// Alpine file name: NAME-VERSION-rN.apk; versions never contain a hyphen.
inline std::string PackageFromFile(const std::string& file) {
    if(file.size()<=4||file.compare(file.size()-4,4,".apk"))return "";
    std::string s=file.substr(0,file.size()-4); size_t a=s.rfind('-'); if(a==std::string::npos||a==0)return "";
    s.resize(a); a=s.rfind('-'); if(a==std::string::npos||a==0)return ""; s.resize(a); return s;
}
struct Resolution { std::vector<std::string> local,remote; std::set<std::string> alpine; };
inline void AddLocal(const Manifest& m,const std::string& name,Resolution& r,std::set<std::string>& active) {
    for(const auto& done:r.local)if(done==name)return;
    if(!active.insert(name).second)throw std::runtime_error("local package dependency cycle at "+name);
    const LocalPackage* p=FindLocal(m,name);
    for(const auto& token:p->depends){
        const auto d=ParseDependency(token); std::string version;
        if(const LocalPackage* q=LocalProviding(m,d.name,version)){
            if(!d.op.empty()&&!Satisfies(version,d.op,d.version))throw std::runtime_error("dependency conflict: local "+name+" requires "+token+", app-managed "+q->name+" is "+q->version);
            AddLocal(m,q->name,r,active);
        } else r.alpine.insert(d.name);
    }
    active.erase(name); r.local.push_back(name);
}
inline Resolution Resolve(const Manifest& m,const std::set<std::string>& world) {
    Resolution r; std::set<std::string> active;
    for(const auto& n:world) {
        if(FindLocal(m,n))AddLocal(m,n,r,active);
        else if(const RemoteSource* s=FindRemote(m,n)) { r.remote.push_back(n); for(const auto& d:s->alpineDependencies)if(FindLocal(m,d))AddLocal(m,d,r,active); else r.alpine.insert(d); }
        else r.alpine.insert(n);
    }
    return r;
}

// ---- installed state ----
// source: "local:manifest", "alpine", "npm:REGISTRY/PACKAGE" or "legacy-unknown".
// state: configured | unconfigured (install scripts not run) | legacy-unknown.
struct Record { std::string name,version,source,state="configured",digest="-",notes="-"; unsigned copiedLinks=0; bool legacy=false; };
inline bool IsLocalSource(const std::string& s){return s.rfind("local:",0)==0;}
inline std::vector<std::string> SplitTabs(const std::string& line) {
    std::vector<std::string> c; size_t at=0;
    while(true){size_t t=line.find('\t',at);if(t==std::string::npos){c.push_back(line.substr(at));break;}c.push_back(line.substr(at,t-at));at=t+1;}
    return c;
}
inline std::vector<Record> ParseRecords(const std::string& text) {
    std::vector<Record> out; std::istringstream in(text); std::string line;
    while(std::getline(in,line)) {
        if(line.empty())continue; const auto c=SplitTabs(line);
        if(c.size()<2||!ValidName(c[0])||c[1].empty())throw std::runtime_error("invalid installed record");
        Record r; r.name=c[0]; r.version=c[1];
        if(c.size()>=7){r.source=c[2];r.state=c[3];r.copiedLinks=static_cast<unsigned>(strtoul(c[4].c_str(),nullptr,10));r.digest=c[5];r.notes=c[6];}
        else if(c.size()==2||c.size()==3){r.legacy=true;r.source=c.size()==3?c[2]:"";}
        else throw std::runtime_error("invalid installed record columns");
        out.push_back(r);
    }
    return out;
}
inline std::string FormatRecords(const std::vector<Record>& rs) {
    std::string out; for(const auto& r:rs)out+=r.name+"\t"+r.version+"\t"+r.source+"\t"+r.state+"\t"+std::to_string(r.copiedLinks)+"\t"+r.digest+"\t"+r.notes+"\n"; return out;
}
inline std::string FormatFiles(const std::vector<OwnedFile>& fs){std::string out;for(const auto& f:fs)out+=f.path+"\t"+(f.sha256.empty()?"-":f.sha256)+"\t"+std::to_string(f.mode)+"\n";return out;}
inline std::vector<OwnedFile> ParseFiles(const std::string& text) {
    std::vector<OwnedFile> out; std::istringstream in(text); std::string line;
    while(std::getline(in,line)) {
        if(line.empty())continue; const size_t a=line.find('\t'),b=a==std::string::npos?a:line.find('\t',a+1);
        if(b==std::string::npos)throw std::runtime_error("invalid file record");
        OwnedFile f; f.path=line.substr(0,a); f.sha256=line.substr(a+1,b-a-1); if(f.sha256=="-")f.sha256.clear(); f.mode=static_cast<unsigned>(strtoul(line.c_str()+b+1,nullptr,10));
        if(!SafeRelative(f.path))throw std::runtime_error("unsafe path in file record"); out.push_back(f);
    }
    return out;
}
inline std::set<std::string> ParseWorld(const std::string& text) {
    std::set<std::string> out; std::istringstream in(text); std::string n;
    while(std::getline(in,n)){if(n.empty())continue;if(!ValidName(n))throw std::runtime_error("invalid installed world entry");out.insert(n);}
    return out;
}
// Format-1 rows of the previous implementation: the third column named the
// origin of non-repository packages.
inline std::string LegacyHint(const Record& r) {
    if(r.source.rfind("upstream-sha512:",0)==0)return "npm";
    if(r.source.rfind("platform-port-source:",0)==0)return "port";
    return r.source.empty()?"alpine":"unknown";
}
}
