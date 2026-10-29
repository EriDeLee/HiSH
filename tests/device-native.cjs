// Runs probes inside the installed app. HDC only deploys/drives tests.
const fs = require('node:fs');
const path = require('node:path');
const { spawnSync } = require('node:child_process');
const [target, outputDir, ...requested] = process.argv.slice(2);
if (!target || !outputDir) throw new Error('Usage: node tests/device-native.cjs TARGET OUTPUT_DIR [--audit | PROBE...]');
const home = process.env.HARMONYOS_COMMANDLINE_TOOLS_HOME;
const hdc = process.env.HDC_PATH || (home ? path.join(home, 'sdk/default/openharmony/toolchains/hdc.exe') : 'hdc');
const allowed = new Set(['combined-notify-check','combined-check','seccomp-check','ptrace-prepare','proot-check','ptrace-compiled','ptrace-advanced','ptrace-check','input-check','process-check','job-check','bridge-check','exec-preload','compiler-check','jit-check','workflow-check','network-check','session-check','tty-check','audit-lifecycle','hybrid-check','audit-glibc-maps','audit-linux-programs','framework-ark','framework-api','framework','baseline','prepare','pty','fork','vfork-exec','fork-exec','spawn-system','spawn-bin',
  'spawn-bundle','spawn-home','spawn-loader','spawn-toybox','exec-system','exec-bundle','exec-home','exec-loader',
  'fexec-bundle','fexec-home','execveat-home','module-bundle','module-home','module-elf','fork-module',
  'pipe-fork','pipe-plain','file-modes','toybox-list','code-pages','memfd-exec','memfd-module','shell-check']);
const auditFamilies = ['inventory','pty','self-code','loader-api','mapping-variants','storage-origins','system-loader','devices','interpreter','inline-syscalls','other-memory','archive','map-bundle','map-home','map-cache','map-memfd','map-anon',
  'exec-bundle','exec-home','exec-memfd','load-bundle','load-home','load-cache','load-memfd',
  'manual-bundle','manual-home','manual-memfd','manual-static-bundle','manual-static-pie','cleanup'];
allowed.add('exec-check');
for (const family of auditFamilies) allowed.add('audit-'+family);
const auditSuite = ['baseline','framework','framework-api','framework-ark', ...auditFamilies.filter(x=>x!=='cleanup').map(x=>'audit-'+x),
  'spawn-toybox','module-elf','shell-check','saved-spawn-toybox','saved-module-elf',
  'saved-audit-manual-bundle','saved-audit-manual-static-bundle','saved-audit-manual-static-pie','saved-audit-interpreter','audit-cleanup'];
const probes = requested.length ? requested.flatMap(x=>x==='--audit'?auditSuite:[x]) : ['baseline','prepare','pty','fork','fork-module','module-bundle',
  'module-home','spawn-system','spawn-toybox','spawn-home','fexec-home','module-elf','shell-check'];
for (const item of probes) if (!allowed.has(item.replace(/^saved-/, ''))) throw new Error('Unknown probe: '+item);
fs.mkdirSync(outputDir, {recursive:true});
function run(...args) {
  const r = spawnSync(hdc, ['-t',target,...args], {encoding:'utf8',windowsHide:true,timeout:20000});
  if(r.error || r.status !== 0) throw r.error || Error(r.stdout+r.stderr);
  return r.stdout;
}
function sleep(ms) { Atomics.wait(new Int32Array(new SharedArrayBuffer(4)),0,0,ms); }
let failed = false;
for (const kind of probes) {
  const seen = new Set(run('shell','hilog -x -T HiSHProbe').split(/\r?\n/));
  run('shell','aa force-stop com.lee.hish');
  const start = run('shell',`aa start -b com.lee.hish -a EntryAbility -m phone --ps nativeProbe ${kind}`);
  if (!start.includes('start ability successfully')) throw Error(start);
  let fresh = '';
  for(let n=0;n<(kind==='network-check'?260:kind==='exec-check'||kind.startsWith('combined-')?180:kind==='seccomp-check'?90:60);n++) {
    sleep(500);
    const batch = run('shell','hilog -x -T HiSHProbe').split(/\r?\n/).filter(line=>line&&!seen.has(line));
    if(batch.length) fresh += (fresh?'\n':'') + batch.join('\n');
    for(const line of batch) seen.add(line);
    if(fresh.includes(`PROBE ${kind} END`)) break;
    if(n>=5 && fresh.includes(`PROBE ${kind} BEGIN`) && !/^\d+( \d+)*$/.test(run('shell','pidof com.lee.hish').trim())) break;
  }
  fs.writeFileSync(path.join(outputDir,`${kind}.log`),fresh+'\n');
  const complete = fresh.includes(`PROBE ${kind} END`);
  if(!complete) failed = true;
  // A complete capability audit may report denied syscalls. Only exit=0 on all
  // three cases demonstrates availability; recorded denial is not feature success.
  if(kind==='seccomp-check') {
    if(!fresh.includes('SECCOMP_AUDIT_END')||fresh.includes('TIMEOUT')||(fresh.match(/SECCOMP CASE .* (exit=|signal=)/g)||[]).length!==3)failed=true;
    const available=(fresh.match(/SECCOMP CASE .* exit=0/g)||[]).length===3;
    console.log('seccomp user notification: '+(available?'round trip verified':'unavailable or failed; inspect saved-seccomp-check for exact stage'));
  }
  if(kind==='combined-check'&&(['COMBINED_PHONE_ELF_READY','COMBINED_NEW_ELF_NATIVE_OK','COMBINED_MRSH_STATUS=0','COMBINED_PREFIX_LIBC_OK','COMBINED_LINUX_EXEC_BRIDGE_OK','COMBINED_STATIC_PIE_OK','COMBINED_MUSL_TLS_THREADS_OK','COMBINED_GLIBC_TLS_THREADS_OK','COMBINED_MALFORMED_REJECTED_OK','COMBINED_CLEANUP_OK','COMBINED_END'].some(x=>!fresh.includes(x))||fresh.includes('COMBINED_ASSERT_')||fresh.includes('TIMEOUT')))failed=true;
  if(kind==='combined-notify-check'&&(['COMBINED_NOTIFY_STATUS=42','COMBINED_NOTIFY_OK','COMBINED_NOTIFY_END'].some(x=>!fresh.includes(x))||!(/HISH_RUN_NOTIFICATIONS=[1-9]\d*/.test(fresh))||fresh.includes('TIMEOUT')))failed=true;
  if(kind==='ptrace-check'&&(!fresh.includes('PTRACE_AUDIT_END')||fresh.includes('TIMEOUT')||(fresh.match(/PTRACE CASE .* (exit=|signal=)/g)||[]).length!==7))failed=true;
  if(kind==='ptrace-advanced'&&(!fresh.includes('PTRACE_ADVANCED_END')||fresh.includes('TIMEOUT')||(fresh.match(/PTRACE CASE .* (exit=|signal=)/g)||[]).length!==5))failed=true;
  if(kind==='ptrace-compiled'&&(!fresh.includes('PTRACE_COMPILED_END')||!fresh.includes('phone-compiled-native-function exit=0')||fresh.includes('TIMEOUT')))failed=true;
  if(kind==='proot-check'&&['PROOT_PHONE_OBJECT_READY','PROOT_PHONE_ELF_READY','PROOT_VERSION_STATUS=0','PROOT_EVAL_END'].some(x=>!fresh.includes(x)))failed=true;
  if(kind==='ptrace-prepare'&&['PTRACE_PHONE_OBJECT_READY','PTRACE_PREPARE_END'].some(x=>!fresh.includes(x)))failed=true;
  if(kind==='shell-check') {
    const markers=['PIPE_AND_SUBSTITUTION_OK','FUNCTION_OK','LOOP_AND_ARITHMETIC_OK','SHELL_MULTILINE_OK','SUBSHELL_OK',
      'NESTED_SHELL_OK','BACKGROUND_WAIT_OK','STORAGE_CLEANUP_OK','SHELL_NATIVE_END'];
    if(markers.some(marker=>!fresh.includes(marker)) || /ASSERT_\w+_FAIL|Bad file descriptor|TIMEOUT/.test(fresh)) failed = true;
  }
  if(kind==='hybrid-check') {
    if(!fresh.includes('HYBRID_FD_EXEC_OK'))failed=true;
    const markers=['HYBRID_TOOL_CATALOG_OK','HYBRID_GLIBC_OK','HYBRID_DISTRO_TOOLS_OK','HYBRID_LINUX_CHILD_EXEC_OK','HYBRID_EXEC_CONTRACT_OK','HYBRID_LINUX_SYSTEM_EXEC_OK','HYBRID_SPAWN_SYSTEM_OK','HYBRID_SPAWN_FILES_OK','HYBRID_MALFORMED_ELF_OK','HYBRID_ABSOLUTE_ELF_SCRIPT_OK','HYBRID_PIPE_SUBSTITUTION_OK','HYBRID_EXIT_STATUS_OK','HYBRID_REDIRECTION_CLEANUP_OK',
      'HYBRID_BACKGROUND_WAIT_OK','HYBRID_SYSTEM_EXEC_OK','HYBRID_EXEC_BUILTIN_OK','HYBRID_END'];
    if(markers.some(marker=>!fresh.includes(marker)) || /ASSERT_\w+_FAIL|Bad file descriptor|TIMEOUT/.test(fresh)) failed=true;
  }
  if(kind==='tty-check'&&(!fresh.includes('TTY command=linux-tty-check result=PASS')||!fresh.includes('TTY command=glibc-tty-check result=PASS')||fresh.includes('TTY_ASSERT')||fresh.includes('TIMEOUT')))failed=true;
  if(kind==='session-check'&&((fresh.match(/SESSION PASS/g)||[]).length!==4||fresh.includes('ASSERT')))failed=true;
  if(kind==='workflow-check'&&['WORKFLOW_LINUX_TOOL_EXEC_OK','WORKFLOW_GIT_COMMIT_OK','WORKFLOW_GIT_CLONE_OK','WORKFLOW_SCRIPT_OK','WORKFLOW_GIT_DIFF_OK','WORKFLOW_SIGNAL_STATUS_OK','WORKFLOW_HOME_ELF_EXECUTED_OK','WORKFLOW_NONEXEC_REJECTED_OK','WORKFLOW_CLEANUP_OK','WORKFLOW_END'].some(x=>!fresh.includes(x)))failed=true;
  if(kind==='network-check'&&['NETWORK_HTTPS_VERIFIED_OK','NETWORK_INVALID_CA_REJECTED_OK','NETWORK_GIT_REMOTE_OK','NETWORK_GIT_CLONE_OK','NETWORK_END'].some(x=>!fresh.includes(x)))failed=true;
  // This checks compilation and records the real execution-policy boundary;
  // a denied output is deliberately not reported as compile-and-run success.
  if(kind==='compiler-check'&&['COMPILER_OBJECT_OK','COMPILER_LINKED_ELF_OK','POLICY_KERNEL_EXEC_ERRNO=13','POLICY_RX_MMAP_ERRNO=13','COMPILER_EXECUTED_OK','COMPILER_END'].some(x=>!fresh.includes(x)))failed=true;
  if(kind==='exec-check'&&(['EXEC_DEFAULT_COMPILED_OK','EXEC_DIRECT_STATUS=23','EXEC_DIRECT_OK','EXEC_ASH_OK','EXEC_DASH_OK','EXEC_CROSS_LIBC_OK','EXEC_A_OUT_DEFAULT_OK','EXEC_COMPILED_EXEC_OK','EXEC_HOME_FD_OK','EXEC_MULTI_THREAD_FILE_OK','EXEC_RESPONSE_DEFAULT_OK','EXEC_EXPLICIT_ADDRESS_OK','EXEC_STATIC_OK','EXEC_NOEXEC_REJECTED_OK','EXEC_TRUNCATED_REJECTED_OK','EXEC_CLEANUP_OK','EXEC_END','HISH_ELF_ANONYMOUS_RX'].some(x=>!fresh.includes(x))||fresh.includes('TIMEOUT')))failed=true;
  if(kind==='jit-check'&&(['JIT_BARE_RUN_STATUS=23','JIT_TCC_HOST_LIBC_RUN_STATUS=23','JIT_EXIT_STATUS=23','JIT_HOST_LIBC_VERIFIED_OK','JIT_END'].some(x=>!fresh.includes(x))||(fresh.match(/JIT_MULTI_STATUS=23/g)||[]).length!==3||['first','second','third'].some(arg=>!fresh.includes(`JIT_MULTI_OK arg=${arg} value=42 heap=OK file=OK thread=OK errno=OK`))||fresh.includes('TIMEOUT')))failed=true;
  if(kind==='bridge-check'&&['BRIDGE_THREADS_MAPS_OK','BRIDGE_EXEC_CONTRACT_OK','BRIDGE_STDIO_CLOEXEC_OK','BRIDGE_SPAWN_FILES_OK','BRIDGE_FD_EXEC_OK','BRIDGE_END'].some(x=>!fresh.includes(x)))failed=true;
  if(kind==='job-check'&&(!fresh.includes('JOB result=PASS')||fresh.includes('TIMEOUT')))failed=true;
  if(kind==='input-check'&&['INPUT command=linux-musl-check result=PASS','INPUT command=linux-glibc-check result=PASS'].some(x=>!fresh.includes(x)))failed=true;
  if(kind==='process-check'&&(['PROCESS_MUSL_STATUS=0','PROCESS_GLIBC_STATUS=0','PROCESS_LOGICAL_SELF_EXEC_OK','PROCESS_SHEBANG_FLAGS_ENV_OK','PROCESS_END'].some(x=>!fresh.includes(x))||['PROCESS_CONCURRENT_SYSTEM_OK','PROCESS_SYSTEM_CANCEL_REAP_OK','PROCESS_POPEN_READ_WRITE_CLOSE_OK','PROCESS_CLOSEFROM_ERROR_CHANNEL_OK','PROCESS_SCHEDULER_RESULT_CHECKED'].some(x=>fresh.split(x).length!==3)))failed=true;
  if(kind==='process-check'&&((fresh.match(/PROCESS_SCHEDULER_APPLIED_OK/g)||[]).length!==2||fresh.includes('PROCESS_SCHEDULER_UNAVAILABLE')))failed=true;
  console.log(`${kind}: ${complete?'recorded':'INCOMPLETE (check fault logs)'} -> ${path.join(outputDir,kind+'.log')}`);
}
if(failed) process.exitCode=1;
