#ifndef HISH_NATIVE_DOWNLOAD_PROBE_H
#define HISH_NATIVE_DOWNLOAD_PROBE_H
// Test-build-only data and driving. No ELF-loader/runtime changes.
extern "C" const unsigned char HiSHDownloadProbeData[], HiSHDownloadProbeDataEnd[];

template<class Log> static void HiSHDownloadProbe(const std::string& root, Log log)
{
    const unsigned char* at=HiSHDownloadProbeData;
    const unsigned char* const end=HiSHDownloadProbeDataEnd;
    const auto take=[&](size_t count){
        if(count>static_cast<size_t>(end-at))throw std::runtime_error("download data bounds");
        const unsigned char* result=at;at+=count;return result;
    };
    const auto number=[&](){const unsigned char* p=take(4);return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);};
    std::string pattern=root+"/home/.hish-download-abi-XXXXXX";
    if(!mkdtemp(pattern.data()))throw std::runtime_error("download test mkdir errno="+std::to_string(errno));
    const std::string directory=pattern;
    log("DOWNLOAD_ROOT="+directory);
    const uint32_t count=number();if(count>4096)throw std::runtime_error("download record count");
    for(uint32_t i=0;i<count;i++){
        const uint32_t nameSize=number(),size=number(),mode=number();
        if(!nameSize||nameSize>1024||size>64*1024*1024||(mode!=0600&&mode!=0700))throw std::runtime_error("download record format");
        const char* nameBytes=reinterpret_cast<const char*>(take(nameSize));std::string name(nameBytes,nameSize);
        if(name.front()=='/'||name.find("..")!=std::string::npos||name.find('\0')!=std::string::npos||name.find('\\')!=std::string::npos)throw std::runtime_error("download record path");
        const unsigned char* bytes=take(size);const std::string file=directory+"/"+name;
        for(size_t slash=directory.size()+1;(slash=file.find('/',slash))!=std::string::npos;slash++){
            const auto parent=file.substr(0,slash);if(mkdir(parent.c_str(),0700)&&errno!=EEXIST)throw std::runtime_error("download parent mkdir");
        }
        const int fd=open(file.c_str(),O_CREAT|O_EXCL|O_WRONLY|O_CLOEXEC,mode);
        if(fd<0)throw std::runtime_error("download target create");
        size_t done=0;
        while(done<size){const ssize_t n=write(fd,bytes+done,size-done);if(n<0&&errno==EINTR)continue;if(n<=0){close(fd);throw std::runtime_error("download target write");}done+=n;}
        const int synced=fsync(fd);close(fd);if(synced)throw std::runtime_error("download target sync");
    }
    if(at!=end)throw std::runtime_error("download trailing data");
    log("DOWNLOAD_ORIGINAL_BYTES_EXTRACTED count="+std::to_string(count));
    const auto run=[&](const std::string& label,const std::string& body,bool interactive){
        const auto session=hish::Start(root,100,30,"shell");
        std::string input="cd '"+directory+"'\nexport HISH_ELF_DIAGNOSTICS=1 HISH_MUSL_DSO_DIAGNOSTICS=1\n"+body;
        if(!interactive)input+="\nexit\n";
        std::string transcript;size_t sent=0;bool typed=false,eof=false,cancelled=false;
        log("DOWNLOAD_SESSION_BEGIN "+label);
        for(int tick=0;tick<12500;tick++){
            if(sent<input.size())sent+=hish::Write(session.id,reinterpret_cast<const uint8_t*>(input.data()+sent),input.size()-sent);
            auto output=hish::Read(session.id);
            if(!output.bytes.empty()){std::string text(output.bytes.begin(),output.bytes.end());transcript+=text;log(text);}
            if(interactive&&!typed&&output.rawInput&&transcript.find("DOWNLOAD_VIM_INTERACTIVE_BEGIN")!=std::string::npos&&transcript.find('\033')!=std::string::npos){
                // Only feed the editor after observing its own terminal/raw state.
                const std::string keys="iVIM_INTERACTIVE_OK\033:wq\r";
                input+=keys;typed=true;log("DOWNLOAD_VIM_KEYS_SENT");
            }
            if(!output.running){eof=true;break;}
            if(tick==11999){
                log("DOWNLOAD_SESSION_TIMEOUT "+label+"; requesting Ctrl-C, no force-kill");
                const uint8_t interrupt=3;hish::Write(session.id,&interrupt,1);
                input+="\nexit\n";cancelled=true;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        // Never trigger Close's forced-cleanup fallback on a still-running test.
        if(eof)hish::Close(session.id);
        else {log("DOWNLOAD_SESSION_UNCLOSED "+label);throw std::runtime_error("test did not exit after Ctrl-C; manual graceful cleanup required");}
        log("DOWNLOAD_SESSION_END "+label+" typed="+std::to_string(typed)+" cancelled="+std::to_string(cancelled));
        return transcript;
    };
    const auto ordinary=run("ordinary-dot-slash",R"SH(busybox sha256sum -c checksums.sha256 > checksum-check.log 2>&1
s=$?
printf 'DOWNLOAD_CHECKSUM_STATUS=%s\n' "$s"
if test "$s" != 0; then exit 88; fi
libs=$(hish-tools --path linux-musl-check)
libs="${libs%/*}"
cp "$libs/libhish_linux_fs_identity.so" fs-identity && chmod 700 fs-identity && ./fs-identity
printf 'DOWNLOAD_STATUS filesystem-identity %s\n' "$?"
./bin/jq-static -ce '.numbers | map(. * 2) | add' input.json > jq-static.out 2> jq-static.err
printf 'DOWNLOAD_STATUS jq-static %s\n' "$?"
cat jq-static.out jq-static.err
./bin/xxhsum -H0 empty > xxhash.out 2> xxhash.err
printf 'DOWNLOAD_STATUS xxhash %s\n' "$?"
cat xxhash.out xxhash.err
./bin/xxhsum -c xxhash.out > xxhash-check.out 2>&1
printf 'DOWNLOAD_STATUS xxhash-check %s\n' "$?"
cat xxhash-check.out
printf 'changed\n' > empty
./bin/xxhsum -c xxhash.out > xxhash-reject.out 2>&1
printf 'DOWNLOAD_STATUS xxhash-corrupt %s\n' "$?"
cat xxhash-reject.out
./bin/mawk '{s += $1} END {print s}' numbers.txt > mawk.out 2> mawk.err
printf 'DOWNLOAD_STATUS mawk %s\n' "$?"
cat mawk.out mawk.err
./bin/jq-dynamic -ce '.numbers | map(. * 2) | add' input.json > jq-no-libs.out 2> jq-no-libs.err
printf 'DOWNLOAD_STATUS jq-no-libs %s\n' "$?"
cat jq-no-libs.out jq-no-libs.err
LD_LIBRARY_PATH="$PWD/lib/musl" ./bin/jq-dynamic -ce '.numbers | map(. * 2) | add' input.json > jq-with-libs.out 2> jq-with-libs.err
printf 'DOWNLOAD_STATUS jq-with-libs %s\n' "$?"
cat jq-with-libs.out jq-with-libs.err
LD_LIBRARY_PATH="$PWD/lib/musl" ./bin/zstd -vv -T2 -f data.bin -o compressed.zst > zstd.out 2> zstd.err
s=$?
printf 'DOWNLOAD_STATUS zstd-compress %s\n' "$s"
cat zstd.out zstd.err
if test "$s" = 0; then
  LD_LIBRARY_PATH="$PWD/lib/musl" ./bin/zstd -q -d -f compressed.zst -o roundtrip.bin
  printf 'DOWNLOAD_STATUS zstd-decompress %s\n' "$?"
  busybox cmp -s data.bin roundtrip.bin
  printf 'DOWNLOAD_STATUS zstd-roundtrip %s\n' "$?"
else
  printf 'DOWNLOAD_SKIP zstd-decompress zstd-roundtrip\n'
fi
LD_LIBRARY_PATH="$PWD/lib/musl" ./bin/pzstd -p 2 -f data.bin -o pzstd.zst > pzstd.out 2> pzstd.err
printf 'DOWNLOAD_STATUS pzstd-compress %s\n' "$?"
cat pzstd.out pzstd.err
TERMINFO="$PWD/share/terminfo" LD_LIBRARY_PATH="$PWD/lib/musl" ./bin/vim -Nu NONE -i NONE -n -es -c 'call writefile(["VIM_BATCH_OK"],"vim-batch.txt")' -c 'qa!' > vim-batch.out 2> vim-batch.err
printf 'DOWNLOAD_STATUS vim-batch %s\n' "$?"
cat vim-batch.out vim-batch.err
if test -f vim-batch.txt; then cat vim-batch.txt; fi
printf 'DOWNLOAD_ORDINARY_END\n'
)SH",false);
    if(ordinary.find("DOWNLOAD_CHECKSUM_STATUS=0")==std::string::npos)throw std::runtime_error("download file checksums did not pass; no target execution allowed");
    run("linux-shell-fixed-address-control",R"SH(busybox sh -c './bin/jq-static -ce ".numbers | map(. * 2) | add" input.json' > jq-ash.out 2> jq-ash.err
printf 'DOWNLOAD_STATUS jq-static-ash %s\n' "$?"
cat jq-ash.out jq-ash.err
printf 'DOWNLOAD_ASH_CONTROL_END\n'
)SH",false);
    // One compound command: no interim shell prompt can be mistaken for Vim
    // having exited before the editor actually enters raw mode.
    run("vim-interactive",R"SH(printf 'DOWNLOAD_VIM_INTERACTIVE_BEGIN\n'; TERMINFO="$PWD/share/terminfo" LD_LIBRARY_PATH="$PWD/lib/musl" ./bin/vim -Nu NONE -i NONE -n vim-interactive.txt; s=$?; printf 'DOWNLOAD_STATUS vim-interactive %s\n' "$s"; if test -f vim-interactive.txt; then cat vim-interactive.txt; fi; printf '\nDOWNLOAD_VIM_INTERACTIVE_END\n'; exit
)SH",true);
    log("DOWNLOAD_ACCEPTANCE_RECORDED; inspect statuses/output, not a compatibility PASS");
    log("DOWNLOAD_EVIDENCE_RETAINED="+directory);
}
#endif
