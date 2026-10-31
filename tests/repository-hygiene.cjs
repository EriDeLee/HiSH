const test=require('node:test'),assert=require('node:assert/strict'),fs=require('node:fs'),path=require('node:path'),cp=require('node:child_process');
const repo=path.resolve(__dirname,'..');
function ignored(name){
 const r=cp.spawnSync('git',['check-ignore','--no-index','-q','--',name],{cwd:repo,encoding:'utf8',windowsHide:true});
 if(r.error)throw r.error;assert(r.status===0||r.status===1,r.stderr);return r.status===0;
}
test('root contains no project documentation or AI forwarding documents',()=>{
 const names=fs.readdirSync(repo,{withFileTypes:true}).filter(item=>item.isFile()).map(item=>item.name);
 assert.deepEqual(names.filter(name=>/\.(md|rst|txt)$/i.test(name)||/^(README|CLAUDE|AGENTS)(\.|$)/i.test(name)),[]);
 assert(fs.existsSync(path.join(repo,'LICENSE')));
});
test('all maintained project Markdown is under docs, upstream documentation is exempt',()=>{
 for(const name of ['README.md','README_EN.md','CLAUDE.md','scripts/linux/README.md','third_party/README.md',
  'bench/README.md','bench/results-compare-2026-10-23.md','bench/results-native-2026-10-23.md',
  'feature/hish_main/src/main/resources/rawfile/term/README.md'])assert(!fs.existsSync(path.join(repo,name)),name);
  for(const name of ['topics.md','product.md','distribution.md','process-lifecycle.md','elf-runtime.md','shell.md',
   'build.md','testing.md','safety.md','repository.md','terminal.md','third-party.md','verification.md'])
  assert(fs.existsSync(path.join(repo,'docs',name)),name);
});
test('documentation relative file links resolve inside the repository',()=>{
 function visit(dir){for(const entry of fs.readdirSync(dir,{withFileTypes:true})){
  const file=path.join(dir,entry.name);if(entry.isDirectory()){visit(file);continue;}if(!entry.name.endsWith('.md'))continue;
   assert(!/^(readme(?:[_-].*)?|claude|agents)\.md$/i.test(entry.name),'Generic document was moved instead of split: '+file);
   const text=fs.readFileSync(file,'utf8');assert(text.split('\n').length<=320,'Mixed-topic handoff was copied intact: '+file);
   for(const match of text.matchAll(/\]\(([^)\s]+)(?:\s+[^)]*)?\)/g)){
   const href=match[1];if(/^(?:[a-z][a-z0-9+.-]*:|#)/i.test(href))continue;
   const target=path.resolve(dir,decodeURIComponent(href.split('#')[0]));
   assert(target===repo||target.startsWith(repo+path.sep),'Link escapes repository: '+file+' '+href);
   assert(fs.existsSync(target),'Broken documentation link: '+path.relative(repo,file)+' -> '+href);
  }
 }}visit(path.join(repo,'docs'));
});
test('gitignore excludes generated outputs, local state and signing material',()=>{
 for(const name of ['node_modules/pkg/file.js','feature/hish_main/oh_modules/pkg/file.ets','product/phone/build/output',
  'feature/hish_main/.cxx/cache','feature/hish_main/.hvigor/cache','.test/linux-tools/runtime-current.cmake',
  'local.properties','build-profile.json5','build-profile.json5.backup','.hallmark/preflight.json','.claude/settings.json',
  'bench/hishbench-musl-dyn','bench/hishbench-musl-static','output.hap','output.so.6','object.o',
  '.env.local','sign-private.txt','signing/profile.p7b','signing/private.key'])assert(ignored(name),'Not ignored: '+name);
});
test('gitignore keeps source, backups, docs, locks and required generated headers visible',()=>{
  for(const name of ['build-profile.template.json5','oh-package-lock.json5','docs/safety.md','docs/history/permissions.md',
  'bench/hishbench.c','tests/package-inventory.cjs','tests/fixtures/diagnostics/TerminalProbe.ets','scripts/daily-package-policy.json',
  'third_party/mksh/Build.sh','third_party/mksh/hish-source-manifest.json','third_party/mrsh/main.c',
  'third_party/toybox/scripts/prereq/generated/newtoys.h','third_party/xterm-optional/xterm-addon-webgl.js',
  '.clang-format','.clang-tidy','.env.example','fixture.bin'])assert(!ignored(name),'Source hidden: '+name);
});
