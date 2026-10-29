// Reuse only the exact source-build recipe and input bytes, never an old loader.
const fs=require('node:fs'),path=require('node:path'),crypto=require('node:crypto'),cp=require('node:child_process'),assert=require('node:assert/strict');
const repo=path.resolve(__dirname,'../..'),fixture=path.join(repo,'.test/linux-tools'),cpp=path.join(repo,'feature/hish_main/src/main/cpp');
const hash=bytes=>crypto.createHash('sha256').update(bytes).digest('hex');
function matches(manifest,scriptHash,readInput){
 if(manifest.status!=='built'||!Array.isArray(manifest.inputs))return false;
 if(!manifest.inputs.every(([name,digest])=>hash(readInput(name))===digest))return false;
 return hash(JSON.stringify({commit:manifest.commit,patches:manifest.patches,archive:manifest.archive,inputs:manifest.inputs,script:scriptHash}))===manifest.fingerprint;
}
function ensure(sdk){
 const script=path.join(__dirname,'musl-source.cjs'),scriptHash=hash(fs.readFileSync(script)),root=path.join(fixture,'musl-source-build');
 function find(){
  if(!fs.existsSync(root))return null;
  for(const entry of fs.readdirSync(root,{withFileTypes:true})){
   if(!entry.isDirectory())continue;const file=path.join(root,entry.name,'manifest.json');if(!fs.existsSync(file))continue;
   const manifest=JSON.parse(fs.readFileSync(file,'utf8'));
   if(!matches(manifest,scriptHash,name=>fs.readFileSync(path.join(cpp,name))))continue;
   assert.equal(path.resolve(manifest.output),path.join(root,entry.name,'build/libhish_linux_musl_loader.so'),'Unexpected source output');
   assert.equal(hash(fs.readFileSync(manifest.output)),manifest.sha256,'Cached source loader changed');
   return {file,...manifest};
  }
  return null;
 }
 let source=find();if(source)return source;
 const built=cp.spawnSync(process.execPath,[script,sdk],{stdio:'inherit',windowsHide:true});
 if(built.error)throw built.error;assert.equal(built.status,0,'Source runtime build failed');
 source=find();assert(source,'No verified source runtime after build');return source;
}
module.exports={hash,matches,ensure};
