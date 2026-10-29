const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path');
const {applyPatch,replaceOnce,generatedHeaders,sourceList,extract}=require('../scripts/linux/musl-source.cjs');
const parent=path.resolve(__dirname,'../.test/linux-tools/musl-source-tests');fs.mkdirSync(parent,{recursive:true});
const fixture=()=>fs.mkdtempSync(path.join(parent,'case-'));
function put(root,name,value){const file=path.join(root,name);fs.mkdirSync(path.dirname(file),{recursive:true});fs.writeFileSync(file,value);}
test('exact source anchor must be present and unique',()=>{
 assert.equal(replaceOnce('a b c','b','B'),'a B c');assert.throws(()=>replaceOnce('a','b','c'));assert.throws(()=>replaceOnce('b b','b','c'));
});
test('official musl archive accepts non-path PAX metadata and retains source contents',()=>{
 const archive=path.join(parent,'../musl-source-cache/musl-1.2.5.tar.gz');
 if(!fs.existsSync(archive))return;
 const root=fixture();extract(fs.readFileSync(archive),root);
 assert(fs.readFileSync(path.join(root,'ldso/dynlink.c'),'utf8').includes('static void *map_library('));
});
test('patch maintains surrounding lines and final newline',()=>{
 const root=fixture();put(root,'src/a.c','one\ntwo\nthree\n');
 applyPatch(root,'--- a/src/a.c\n+++ b/src/a.c\n@@ -1,3 +1,4 @@\n one\n-two\n+TWO\n+new\n three\n');
 assert.equal(fs.readFileSync(path.join(root,'src/a.c'),'utf8'),'one\nTWO\nnew\nthree\n');
});
test('patch supports a new file followed by another commit',()=>{
 const root=fixture();applyPatch(root,'--- /dev/null\n+++ b/new.c\n@@ -0,0 +1,1 @@\n+old\n-- \nversion\n--- a/new.c\n+++ b/new.c\n@@ -1 +1 @@\n-old\n+new\n');
 assert.equal(fs.readFileSync(path.join(root,'new.c'),'utf8'),'new\n');
});
test('wrong or ambiguous patch context is rejected',()=>{
 const root=fixture();put(root,'a','x\ny\nx\ny\n');
 assert.throws(()=>applyPatch(root,'--- a/a\n+++ b/a\n@@ -9,1 +9,1 @@\n-y\n+Y\n'));
 assert.equal(fs.readFileSync(path.join(root,'a'),'utf8'),'x\ny\nx\ny\n');
});
test('patch cannot escape the source tree or rename a file',()=>{
 const root=fixture();assert.throws(()=>applyPatch(root,'--- /dev/null\n+++ b/../escape\n@@ -0,0 +1 @@\n+x\n'));
 assert.throws(()=>applyPatch(root,'--- a/source\n+++ b/target\n@@ -1 +1 @@\n-a\n+b\n'));
});
test('generate musl type and syscall headers without running sed/sh',()=>{
 const root=fixture(),out=path.join(root,'build');
 put(root,'arch/aarch64/bits/alltypes.h.in','TYPEDEF unsigned long size_t;\n');
 put(root,'include/alltypes.h.in','STRUCT thing { int value; };\nUNION number { int value; };\n');
 put(root,'arch/aarch64/bits/syscall.h.in','#define __NR_read 63\n');generatedHeaders(root,out);
 const types=fs.readFileSync(path.join(out,'include/bits/alltypes.h'),'utf8');
 assert(types.includes('typedef unsigned long size_t;'));assert(types.includes('__NEED_struct_thing'));assert(types.includes('union number { int value; };'));
 assert.equal(fs.readFileSync(path.join(out,'include/bits/syscall.h'),'utf8'),'#define __NR_read 63\n#define SYS_read 63\n');
});
test('AArch64 implementations replace generic functions, not unrelated architectures',()=>{
 const root=fixture();for(const name of ['src/string/copy.c','src/string/aarch64/copy.S','src/string/x86_64/copy.s','src/malloc/a.c','src/malloc/mallocng/b.c','src/malloc/oldmalloc/c.c','ldso/dynlink.c'])put(root,name,'');
 assert.deepEqual(sourceList(root),['ldso/dynlink.c','src/malloc/a.c','src/malloc/mallocng/b.c','src/string/aarch64/copy.S']);
});
