#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace HishPackage {
struct Sha256 {
    uint32_t h[8]={0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    uint64_t length=0; unsigned char buffer[64]; size_t used=0;
    static uint32_t Rotate(uint32_t v,unsigned n){return (v>>n)|(v<<(32-n));}
    void Block(const unsigned char* p) {
        static const uint32_t k[64]={0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
            0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
            0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
            0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
        uint32_t w[64];
        for(int i=0;i<16;i++)w[i]=uint32_t(p[i*4])<<24|uint32_t(p[i*4+1])<<16|uint32_t(p[i*4+2])<<8|p[i*4+3];
        for(int i=16;i<64;i++){uint32_t s0=Rotate(w[i-15],7)^Rotate(w[i-15],18)^(w[i-15]>>3),s1=Rotate(w[i-2],17)^Rotate(w[i-2],19)^(w[i-2]>>10);w[i]=w[i-16]+s0+w[i-7]+s1;}
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for(int i=0;i<64;i++){
            uint32_t t1=hh+(Rotate(e,6)^Rotate(e,11)^Rotate(e,25))+((e&f)^(~e&g))+k[i]+w[i],t2=(Rotate(a,2)^Rotate(a,13)^Rotate(a,22))+((a&b)^(a&c)^(b&c));
            hh=g;g=f;f=e;e=d+t1;d=c;c=b;b=a;a=t1+t2;
        }
        h[0]+=a;h[1]+=b;h[2]+=c;h[3]+=d;h[4]+=e;h[5]+=f;h[6]+=g;h[7]+=hh;
    }
    void Update(const void* data,size_t size) {
        const auto* p=static_cast<const unsigned char*>(data); length+=size;
        while(size){size_t n=std::min<size_t>(size,64-used);memcpy(buffer+used,p,n);used+=n;p+=n;size-=n;if(used==64){Block(buffer);used=0;}}
    }
    std::string Hex() {
        const uint64_t bits=length*8; unsigned char pad=0x80; Update(&pad,1); pad=0; while(used!=56)Update(&pad,1);
        unsigned char tail[8]; for(int i=0;i<8;i++)tail[i]=static_cast<unsigned char>(bits>>(56-i*8)); Update(tail,8);
        static const char digits[]="0123456789abcdef"; std::string out; for(uint32_t v:h)for(int s=28;s>=0;s-=4)out+=digits[(v>>s)&15]; return out;
    }
};
inline std::string Sha256Hex(const void* data,size_t size){Sha256 s;s.Update(data,size);return s.Hex();}
inline std::string Base64ToHex(const std::string& text) {
    std::string raw; uint32_t acc=0; int bits=0;
    for(char c:text){
        int v; if(c>='A'&&c<='Z')v=c-'A'; else if(c>='a'&&c<='z')v=c-'a'+26; else if(c>='0'&&c<='9')v=c-'0'+52; else if(c=='+')v=62; else if(c=='/')v=63; else if(c=='=')break; else throw std::runtime_error("invalid base64");
        acc=(acc<<6)|uint32_t(v); bits+=6; if(bits>=8){bits-=8;raw+=static_cast<char>((acc>>bits)&255);}
    }
    static const char digits[]="0123456789abcdef"; std::string out; for(unsigned char c:raw){out+=digits[c>>4];out+=digits[c&15];} return out;
}
inline bool IsHex(const std::string& s,size_t length){if(s.size()!=length)return false;for(char c:s)if(!((c>='0'&&c<='9')||(c>='a'&&c<='f')))return false;return true;}
inline std::string HashFile(const std::string& path) {
    FILE* f=fopen(path.c_str(),"rb"); if(!f)throw std::runtime_error("read "+path);
    Sha256 s; unsigned char chunk[65536]; size_t n;
    while((n=fread(chunk,1,sizeof chunk,f))>0)s.Update(chunk,n);
    const bool bad=ferror(f); fclose(f); if(bad)throw std::runtime_error("read "+path); return s.Hex();
}
// One entry of a package's file ownership record. sha256 is a hex digest for
// regular files, "D" for an explicit directory and "L:TARGET" for a symlink.
struct OwnedFile { std::string path,sha256; unsigned mode=0600; };

struct Json {
    enum Type {TNull,TBool,TNumber,TString,TArray,TObject} type=TNull;
    bool boolean=false; double number=0; std::string text; std::vector<Json> items; std::vector<std::pair<std::string,Json>> members;
    const Json* Find(const std::string& key) const {if(type==TObject)for(const auto& m:members)if(m.first==key)return &m.second;return nullptr;}
    const Json& At(const std::string& key) const {const Json* v=Find(key);if(!v)throw std::runtime_error("missing JSON field: "+key);return *v;}
    const std::string& Str() const {if(type!=TString)throw std::runtime_error("JSON string expected");return text;}
    long long Int() const {if(type!=TNumber||number!=static_cast<long long>(number))throw std::runtime_error("JSON integer expected");return static_cast<long long>(number);}
    bool Bool() const {if(type!=TBool)throw std::runtime_error("JSON boolean expected");return boolean;}
    const std::vector<Json>& List() const {if(type!=TArray)throw std::runtime_error("JSON array expected");return items;}
    std::string StrOr(const std::string& key,const std::string& fallback) const {const Json* v=Find(key);return v?v->Str():fallback;}
};
struct JsonParser {
    const std::string& s; size_t at=0;
    explicit JsonParser(const std::string& text):s(text){}
    [[noreturn]] void Fail(const char* why) const {throw std::runtime_error(std::string("invalid JSON: ")+why);}
    void Space(){while(at<s.size()&&(s[at]==' '||s[at]=='\n'||s[at]=='\r'||s[at]=='\t'))at++;}
    void Utf8(std::string& out,uint32_t c){
        if(c<0x80)out+=char(c); else if(c<0x800){out+=char(0xc0|c>>6);out+=char(0x80|(c&63));}
        else if(c<0x10000){out+=char(0xe0|c>>12);out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
        else {out+=char(0xf0|c>>18);out+=char(0x80|((c>>12)&63));out+=char(0x80|((c>>6)&63));out+=char(0x80|(c&63));}
    }
    uint32_t Hex4(){if(s.size()-at<4)Fail("short escape");uint32_t v=0;for(int i=0;i<4;i++){char c=s[at++];v<<=4;if(c>='0'&&c<='9')v|=c-'0';else if(c>='a'&&c<='f')v|=c-'a'+10;else if(c>='A'&&c<='F')v|=c-'A'+10;else Fail("bad escape");}return v;}
    std::string String() {
        if(s[at]!='"')Fail("string expected");at++;std::string out;
        while(true){
            if(at>=s.size())Fail("unterminated string");char c=s[at++];
            if(c=='"')break; if(static_cast<unsigned char>(c)<0x20)Fail("control character");
            if(c!='\\'){out+=c;continue;}
            if(at>=s.size())Fail("bad escape");c=s[at++];
            switch(c){case '"':out+='"';break;case '\\':out+='\\';break;case '/':out+='/';break;case 'b':out+='\b';break;case 'f':out+='\f';break;case 'n':out+='\n';break;case 'r':out+='\r';break;case 't':out+='\t';break;
                case 'u':{uint32_t v=Hex4();if(v>=0xd800&&v<0xdc00){if(s.compare(at,2,"\\u"))Fail("lone surrogate");at+=2;uint32_t low=Hex4();if(low<0xdc00||low>0xdfff)Fail("bad surrogate");v=0x10000+((v-0xd800)<<10)+(low-0xdc00);}else if(v>=0xdc00&&v<=0xdfff)Fail("lone surrogate");Utf8(out,v);break;}
                default:Fail("bad escape");}
        }
        return out;
    }
    Json Value(int depth) {
        if(depth>32)Fail("too deep");Space();if(at>=s.size())Fail("unexpected end");Json v;char c=s[at];
        if(c=='{'){
            v.type=Json::TObject;at++;Space();
            if(s[at]=='}'){at++;return v;}
            while(true){Space();std::string key=String();Space();if(at>=s.size()||s[at++]!=':')Fail("colon expected");
                for(const auto& m:v.members)if(m.first==key)Fail("duplicate key");
                v.members.emplace_back(key,Value(depth+1));Space();if(at>=s.size())Fail("unexpected end");
                if(s[at]==','){at++;continue;} if(s[at]=='}'){at++;break;} Fail("object separator");}
        } else if(c=='['){
            v.type=Json::TArray;at++;Space();
            if(s[at]==']'){at++;return v;}
            while(true){v.items.push_back(Value(depth+1));Space();if(at>=s.size())Fail("unexpected end");
                if(s[at]==','){at++;continue;} if(s[at]==']'){at++;break;} Fail("array separator");}
        } else if(c=='"'){v.type=Json::TString;v.text=String();}
        else if(!s.compare(at,4,"true")){v.type=Json::TBool;v.boolean=true;at+=4;}
        else if(!s.compare(at,5,"false")){v.type=Json::TBool;at+=5;}
        else if(!s.compare(at,4,"null")){at+=4;}
        else {
            size_t start=at;if(s[at]=='-')at++;
            if(at>=s.size()||!(s[at]>='0'&&s[at]<='9'))Fail("value expected");
            while(at<s.size()&&((s[at]>='0'&&s[at]<='9')||s[at]=='.'||s[at]=='e'||s[at]=='E'||s[at]=='+'||s[at]=='-'))at++;
            v.type=Json::TNumber;v.number=strtod(s.substr(start,at-start).c_str(),nullptr);
        }
        return v;
    }
};
inline Json ParseJson(const std::string& text) {
    if(text.size()>8*1024*1024)throw std::runtime_error("JSON size limit");
    JsonParser p(text);Json v=p.Value(0);p.Space();if(p.at!=text.size())p.Fail("trailing data");return v;
}
inline std::string Quote(const std::string& s) {
    std::string out="\"";for(unsigned char c:s){if(c=='"'||c=='\\'){out+='\\';out+=char(c);}else if(c=='\n')out+="\\n";else if(c<0x20){char b[8];snprintf(b,sizeof b,"\\u%04x",c);out+=b;}else out+=char(c);}return out+"\"";
}
}
