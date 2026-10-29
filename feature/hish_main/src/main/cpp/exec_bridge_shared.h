#pragma once
#include <stdint.h>
#define HISH_EXEC_BRIDGE_MAGIC 0x48455842u
#define HISH_EXEC_BRIDGE_VERSION 2u
#define HISH_EXEC_BRIDGE_LIMIT (2u * 1024u * 1024u)
// Additional explicit command modes; existing runtime values 0..4 are unchanged.
#define HISH_EXEC_BRIDGE_PTRACE 5u
#define HISH_EXEC_BRIDGE_PTRACE_DIAGNOSTIC 6u
#define HISH_EXEC_BRIDGE_PTRACE_NOTIFY 7u
#define HISH_EXEC_BRIDGE_PTRACE_NOTIFY_DIAGNOSTIC 8u
#define HISH_EXEC_BRIDGE_APPLET 9u
typedef struct {
    uint32_t magic, version, runtime, argc, envc, target_fd, bytes;
} HishExecBridgeHeader;
