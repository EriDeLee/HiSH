/* Basic, strictly POSIX interactive line interface */
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <unistd.h>
#include <mrsh/shell.h>
#include <mrsh/parser.h>
#include "frontend.h"
#include "native_elf.h"
static volatile sig_atomic_t interrupted;
extern ssize_t HiSHTtyRead(int fd, void *bytes, size_t size);

static ssize_t terminal_line(char **line, size_t *capacity) {
	if (!getenv("HISH_TTY_STATE_FD")) return getline(line, capacity, stdin);
	size_t used = 0;
	for (;;) {
		if (used + 2 > *capacity) {
			size_t next = *capacity ? *capacity * 2 : 128;
			char *buffer = realloc(*line, next);
			if (!buffer) return -1;
			*line = buffer; *capacity = next;
		}
		char byte;
		ssize_t result = HiSHTtyRead(STDIN_FILENO, &byte, 1);
		if (result < 0) return -1;
		if (!result) { (*line)[used] = 0; return used ? (ssize_t)used : -1; }
		(*line)[used++] = byte;
		if (byte == '\n') { (*line)[used] = 0; return used; }
	}
}

void interactive_init(struct mrsh_state *state) {
	// Pipe transport has no terminal driver to dispatch SIGINT. The owner
	// signals the private process group; preserve the shell while commands die.
	struct sigaction action = {0};
	extern void HiSHPipeInterrupt(int signal);
	action.sa_handler = HiSHPipeInterrupt;
	sigaction(SIGINT, &action, NULL);
}

void HiSHPipeInterrupt(int signal) { (void)signal; interrupted = 1; }
bool HiSHTakeInterrupt(void) { bool value = interrupted != 0; interrupted = 0; return value; }

size_t interactive_next(struct mrsh_state *state,
		char **line, const char *prompt) {
	HiSHResetVirtualTerminal();
	fprintf(stderr, "%s", prompt);
	size_t len = 0;
	char *_line = NULL;
	errno = 0;
	interrupted = 0;
	ssize_t n_read = terminal_line(&_line, &len);
	if (n_read < 0) {
		free(_line);
		if (errno == EINTR) {
			clearerr(stdin);
			*line = strdup("\n");
			return 1;
		}
		if (errno != 0) {
			perror("getline");
		}
		return 0;
	}
	*line = _line;
	return n_read;
}
