// Host driver for the real package core. The backend is a local fake
// repository: REPO/index.tsv (name, file, dependencies), REPO/*.apk and
// REPO/npm/* for registry metadata and artifacts.
#include "package_cli.h"
#include <iostream>
using namespace HishPackage;
namespace {
std::string Env(const char* name){const char* v=getenv(name);return v?v:"";}
void Online(){Require(Env("HISH_TEST_OFFLINE")!="1","network unavailable (test offline)");}
struct FakeBackend final : Backend {
    fs::path repo; std::vector<std::string> log;
    explicit FakeBackend(fs::path r):repo(std::move(r)){}
    std::map<std::string,std::pair<std::string,std::vector<std::string>>> Index() const {
        std::map<std::string,std::pair<std::string,std::vector<std::string>>> out; std::istringstream in(ReadText(repo/"index.tsv")); std::string line;
        while(std::getline(in,line)){if(line.empty())continue;auto c=SplitTabs(line);std::vector<std::string> deps;std::istringstream d(c.size()>2?c[2]:"");std::string x;while(d>>x)deps.push_back(x);out[c[0]]={c[1],deps};}
        return out;
    }
    void FetchAlpine(const std::set<std::string>& names,const fs::path& dir) override {
        Online(); const auto index=Index(); std::set<std::string> done; std::vector<std::string> todo(names.begin(),names.end());
        WriteText(repo/"last-fetch.txt",[&]{std::string s;for(const auto& n:names)s+=n+"\n";return s;}());
        // Simulate a resolver that reports success but produces no files.
        if(!Env("HISH_TEST_EMPTY_FETCH").empty()&&names.count(Env("HISH_TEST_EMPTY_FETCH")))return;
        while(!todo.empty()){const std::string n=todo.back();todo.pop_back();if(!done.insert(n).second)continue;
            auto it=index.find(n);Require(it!=index.end(),"download/verification failed: no such package "+n);
            fs::copy_file(repo/it->second.first,dir/it->second.first);for(const auto& d:it->second.second)todo.push_back(d);}
    }
    void VerifyApk(const fs::path& file) override {Require(!fs::exists(repo/("bad-"+file.filename().string())),"APK signature rejected: "+file.filename().string());}
    int UpdateIndexes() override {Online();return 0;}
    int SearchAlpine(const std::string& term) override {for(const auto& kv:Index())if(kv.first.find(term)!=std::string::npos)Say(kv.first);return 0;}
    int PolicyAlpine(const std::vector<std::string>& names) override {for(const auto& n:names)Say("policy "+n);return 0;}
    std::string FetchText(const std::string& url) override {
        Online(); const std::string tail=url.substr(url.rfind('/')+1); const fs::path f=repo/"npm"/(tail+".json");
        Require(fs::exists(f),"metadata download failed: "+url); return ReadText(f);
    }
    void FetchArtifact(const std::string& url,const fs::path& dest,const std::string& sha512) override {
        Online(); const std::string tail=url.substr(url.rfind('/')+1);
        std::string expected=ReadText(repo/"npm"/(tail+".sha512")); while(!expected.empty()&&expected.back()=='\n')expected.pop_back();
        Require(expected==sha512,"download or SHA-512 check failed: "+url); fs::copy_file(repo/"npm"/tail,dest); WriteText(repo/"artifact-downloads.txt",ReadText(repo/"artifact-downloads.txt")+tail+"\n");
    }
    void CheckArtifact(const fs::path&,const std::string&) override {}
    int RunCheck(const std::string& script,const fs::path&,const fs::path&) override {return script.find("exit 1")!=std::string::npos;}
    bool Referenced(const fs::path& generation) override {const std::string held=Env("HISH_TEST_REFERENCED");return !held.empty()&&generation.filename().string()==held;}
    std::string Probe(const std::string&) override {return "not available on host";}
};
}
int main(int argc,char** argv) {
    try {
        Require(argc>=5,"usage: package-core BASE PAYLOADS MANIFEST REPO COMMAND...");
        if(std::string(argv[1])=="vercmp"){Require(argc==5,"vercmp HAVE OP WANT");std::cout<<(Satisfies(argv[2],argv[3],argv[4])?"yes":"no")<<"\n";return 0;}
        FakeBackend io(argv[4]); Context c(argv[1],argv[2],ReadText(argv[3]),io); c.forceLinks=Env("HISH_TEST_LINKS");
        const std::string crash=Env("HISH_TEST_CRASH"); if(!crash.empty())c.store.hook=[crash](const char* point){if(crash==point){fflush(nullptr);_exit(3);}};
        std::vector<std::string> args; for(int i=5;i<argc;i++)args.push_back(argv[i]);
        if(!args.empty()&&args[0]=="migrate"){std::string error;const bool ok=Migrate(c,error);if(!ok)std::cerr<<error<<"\n";return ok?0:1;}
        if(!args.empty()&&args[0]=="pending"){std::cout<<(MigrationPending(c)?"pending":"accepted")<<"\n";return 0;}
        return Command(c,args);
    } catch(const std::exception& e){std::cerr<<"pkg: "<<e.what()<<"\n";return 1;}
}
