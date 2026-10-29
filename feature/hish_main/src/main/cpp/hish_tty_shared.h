#pragma once
#include <stdint.h>
#include <errno.h>
#include <signal.h>
#include <unistd.h>
#define HISH_TTY_MAGIC 0x48545459u
#define HISH_TTY_VERSION 2u
// A data-only shared page. This does not create a kernel PTY or executable memory.
typedef struct HishTtyState {
    uint32_t magic, version, columns, rows;
    uint32_t iflag, oflag, cflag, lflag;
    uint8_t cc[32];
    uint32_t input_speed, output_speed;
    int32_t foreground, session;
    uint64_t device, inode;
    // EOF is an input boundary, never a permanent socket half-close. The
    // producer and public read/readv adapters serialize only nonblocking I/O.
    uint32_t input_lock, eof_head, eof_tail, reserved;
    uint64_t input_written, eof_at[16];
} HishTtyState;
static inline int HishTryInputLock(HishTtyState* state)
{
    uint32_t expected=0;
    if(__atomic_compare_exchange_n(&state->input_lock,&expected,(uint32_t)getpid(),0,__ATOMIC_ACQUIRE,__ATOMIC_RELAXED))return 1;
    // A foreground command can be killed while holding the short I/O lock.
    // Recover after it has been reaped instead of wedging the next prompt.
    if(expected>1&&kill((pid_t)expected,0)<0&&errno==ESRCH)__atomic_compare_exchange_n(&state->input_lock,&expected,0,0,__ATOMIC_RELEASE,__ATOMIC_RELAXED);
    return 0;
}
static inline void HishUnlockInput(HishTtyState* state)
{ __atomic_store_n(&state->input_lock,0,__ATOMIC_RELEASE); }
