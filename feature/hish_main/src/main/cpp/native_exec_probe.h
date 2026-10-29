#ifndef HISH_NATIVE_EXEC_PROBE_H
#define HISH_NATIVE_EXEC_PROBE_H
// Real phone-compiled ELF outputs: no hish-run or manual default link address.
static const char* HiSHExecProbeScript = R"SH(printf 'EXEC_BEGIN\n'
export HISH_ELF_DIAGNOSTICS=1
d=$(busybox mktemp -d "$HOME/.hish-exec.XXXXXX") || exit 201
cd "$d" || exit 202
cat > hello.c <<'SOURCE'
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static int data=17, bss[64], constructed;
static int *pointer=&data;
__attribute__((constructor)) static void initialize(void) { constructed=31; }
int main(int argc,char **argv) {
  if(argc!=2 || *pointer!=17 || bss[63] || constructed!=31 || !getenv("PREFIX")) return 81;
  printf("EXEC_HELLO arg=%s\n",argv[1]); return 23;
}
SOURCE
tcc hello.c -o hello || exit 203
printf 'EXEC_DEFAULT_COMPILED_OK\n'
./hello direct > result
status=$?
printf 'EXEC_DIRECT_STATUS=%s\n' "$status"
test "$status" = 23 && test "$(cat result)" = 'EXEC_HELLO arg=direct' || exit 204
printf 'EXEC_DIRECT_OK\n'
busybox sh -c './hello ash' > result
test "$?" = 23 && test "$(cat result)" = 'EXEC_HELLO arg=ash' || exit 205
printf 'EXEC_ASH_OK\n'
dash -c './hello dash' > result
test "$?" = 23 && test "$(cat result)" = 'EXEC_HELLO arg=dash' || exit 206
printf 'EXEC_DASH_OK\n'
linux-musl-check --exec ./hello musl > result
test "$?" = 23 && test "$(cat result)" = 'EXEC_HELLO arg=musl' || exit 207
linux-glibc-check --exec ./hello glibc > result
test "$?" = 23 && test "$(cat result)" = 'EXEC_HELLO arg=glibc' || exit 208
printf 'EXEC_CROSS_LIBC_OK\n'
tcc hello.c || exit 226
./a.out default > result
test "$?" = 23 && test "$(cat result)" = 'EXEC_HELLO arg=default' || exit 227
printf 'EXEC_A_OUT_DEFAULT_OK\n'
cat > chain.c <<'SOURCE'
#include <unistd.h>
int main(void) { execl("./hello","./hello","nested",(char*)0); return 89; }
SOURCE
tcc chain.c -o chain || exit 228
./chain > result
test "$?" = 23 && test "$(cat result)" = 'EXEC_HELLO arg=nested' || exit 229
printf 'EXEC_COMPILED_EXEC_OK\n'
for method in fexecve empty relative; do
  cp "$(hish-tools --path linux-musl-check)" fd-target && chmod 700 fd-target || exit 209
  test "$(linux-glibc-check --fd-exec "$method" ./fd-target)" = LINUX_FD_EXEC_OK || exit 210
done
printf 'EXEC_HOME_FD_OK\n'
cat > helper.c <<'SOURCE'
int helper(int n) { return n+2; }
SOURCE
cat > multi.c <<'SOURCE'
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
extern int helper(int);
static void *worker(void *p) { *(int*)p=helper(40); errno=7; return errno==7?p:NULL; }
int main(int argc,char **argv) {
  if(argc!=2) return 82;
  char *buffer=calloc(1,32); if(!buffer) return 83;
  int value=0; void *result=NULL; pthread_t thread; errno=9;
  if(pthread_create(&thread,NULL,worker,&value) || pthread_join(thread,&result) || result!=&value || value!=42 || errno!=9) return 84;
  snprintf(buffer,32,"EXEC_MULTI_%s_%d",argv[1],value);
  FILE *f=fopen("data.txt","w"); if(!f || fputs(buffer,f)<0 || fclose(f)) return 85;
  char back[32]={0}; f=fopen("data.txt","r");
  if(!f || !fgets(back,sizeof back,f) || fclose(f) || strcmp(back,buffer) || remove("data.txt")) return 86;
  puts(buffer); free(buffer); return 37;
}
SOURCE
tcc -c helper.c -o helper.o && tcc multi.c helper.o -o multi || exit 211
for arg in first second third; do
  ./multi "$arg" > result
  test "$?" = 37 && test "$(cat result)" = "EXEC_MULTI_${arg}_42" || exit 212
done
printf 'EXEC_MULTI_THREAD_FILE_OK\n'
printf 'hello.c -o response\n' > options
tcc @options || exit 213
./response response > result
test "$?" = 23 && test "$(cat result)" = 'EXEC_HELLO arg=response' || exit 214
printf 'EXEC_RESPONSE_DEFAULT_OK\n'
tcc -Wl,-Ttext=61000000 hello.c -o custom || exit 215
./custom custom > result
test "$?" = 23 && test "$(cat result)" = 'EXEC_HELLO arg=custom' || exit 216
printf 'EXEC_EXPLICIT_ADDRESS_OK\n'
cat > bare.c <<'SOURCE'
/* mov x0,#37; mov x8,#93; svc #0. TinyCC accepts the data directives,
   but its ARM assembler frontend does not accept AArch64 register mnemonics. */
void _start(void) { __asm__ volatile(".long 0xd28004a0\n.long 0xd2800ba8\n.long 0xd4000001"); __builtin_unreachable(); }
SOURCE
tcc -nostdlib bare.c -o bare || exit 217
./bare
test "$?" = 37 || exit 218
printf 'EXEC_STATIC_OK\n'
cp hello noexec && chmod 600 noexec || exit 219
./noexec > result 2>/dev/null
test "$?" = 126 && test ! -s result || exit 220
busybox sh -c './noexec' > result 2>/dev/null
test "$?" = 126 && test ! -s result || exit 221
dash -c './noexec' > result 2>/dev/null
test "$?" = 126 && test ! -s result || exit 222
printf 'EXEC_NOEXEC_REJECTED_OK\n'
cp hello truncated && busybox truncate -s 128 truncated || exit 223
./truncated > result 2>/dev/null
test "$?" = 126 && test ! -s result || exit 224
printf 'EXEC_TRUNCATED_REJECTED_OK\n'
rm -f hello.c hello a.out chain.c chain result fd-target helper.c helper.o multi.c multi options response custom bare.c bare noexec truncated
cd "$HOME" && rmdir "$d" || exit 225
printf 'EXEC_CLEANUP_OK\nEXEC_END\n'
exit 0
)SH";
#endif
