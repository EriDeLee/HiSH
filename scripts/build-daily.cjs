// Finite ordinary build. No signing reads, package install, service or daemon.
const fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process'),assert=require('node:assert/strict');
const repo=path.resolve(__dirname,'..'),sdk=process.argv[2],output=process.argv[3];
assert(sdk&&output,'Usage: node scripts/build-daily.cjs COMMAND_LINE_TOOLS NEW_EVIDENCE_DIR');
assert(!fs.existsSync(output),'Do not overwrite build evidence');fs.mkdirSync(output,{recursive:true});
const env={...process.env};for(const key of ['HISH_ENABLE_DIAGNOSTICS','HISH_DOWNLOAD_ABI_DATA','HISH_LINUX_PAYLOAD_DIR','HISH_MKSH_TRIAL_ROOT','HISH_MKSH_TRACE'])delete env[key];
const native=path.join(sdk,'sdk/default/openharmony/native');
function run(executable,args,name){
 const fd=fs.openSync(path.join(output,name),'wx');try{const r=cp.spawnSync(executable,args,{cwd:repo,env,windowsHide:true,stdio:['ignore',fd,fd]});if(r.error)throw r.error;assert.equal(r.status,0,name+' failed');}finally{fs.closeSync(fd);}
}
run(process.execPath,[path.join(__dirname,'linux/build.cjs'),native],'runtime-build.log');
for(const abi of ['arm64-v8a','x86_64']){
 const directory=path.join(repo,'feature/hish_main/.cxx/default/default/debug',abi);
 if(fs.existsSync(path.join(directory,'CMakeCache.txt')))run(path.join(native,'build-tools/cmake/bin/cmake.exe'),['-S',path.join(repo,'feature/hish_main/src/main/cpp'),'-B',directory],'cmake-'+abi+'.log');
}
run('cmd.exe',['/d','/c',path.join(sdk,'bin/hvigorw.bat'),'--mode','module','-p','module=phone@default','assembleHap','--no-daemon'],'build.log');
const pointer=fs.readFileSync(path.join(repo,'.test/linux-tools/runtime-current.cmake'),'utf8'),payloads=pointer.match(/\[=\[([^\r\n]+)\]=\]/)?.[1];assert(payloads);
const unsigned=path.join(repo,'product/phone/build/default/outputs/default/phone-default-unsigned.hap');
const inspection=require('./inspect-daily-hap.cjs').inspect(unsigned,payloads);
fs.copyFileSync(unsigned,path.join(output,'phone-daily-unsigned.hap'));
fs.writeFileSync(path.join(output,'package-verification.json'),JSON.stringify({...inspection,payloads},null,2)+'\n',{flag:'wx'});
console.log(JSON.stringify({built:true,output,bytes:inspection.bytes,payloads},null,2));
