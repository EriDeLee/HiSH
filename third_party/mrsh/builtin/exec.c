#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <unistd.h>
#include "builtin.h"
#include "mrsh_getopt.h"
#include "shell/path.h"
#include "shell/shell.h"
extern int HiSHHasApplet(const char *name);
extern void HiSHRunApplet(char **argv);
#include "native_elf.h"

static const char exec_usage[] = "usage: exec [command [argument...]]\n";

// Shell variables are kept separately from libc's environ. Unlike an ordinary
// external command, exec does not pass through run_process's child setup.
static void export_exec_variable(const char *key, void *value, void *context) {
	struct mrsh_variable *variable = value;
	int *error = context;
	if ((variable->attribs & MRSH_VAR_ATTRIB_EXPORT) &&
			setenv(key, variable->value, 1) != 0 && !*error) {
		*error = errno;
	}
}

int builtin_exec(struct mrsh_state *state, int argc, char *argv[]) {
	_mrsh_optind = 0;
	if (_mrsh_getopt(argc, argv, ":") != -1) {
		fprintf(stderr, "exec: unknown option -- %c\n", _mrsh_optopt);
		fprintf(stderr, exec_usage);
		return 1;
	}
	if (_mrsh_optind == argc) {
		return 0;
	}
	int environment_error = 0;
	mrsh_hashtable_for_each(&state_get_priv(state)->variables,
		export_exec_variable, &environment_error);
	if (environment_error) {
		errno = environment_error;
		perror("exec: export environment");
		return 126;
	}
	if (HiSHHasApplet(argv[_mrsh_optind])) HiSHRunApplet(&argv[_mrsh_optind]);

	char *path = expand_path(state, argv[_mrsh_optind], false, false);
	if (path == NULL) {
		fprintf(stderr, "exec: %s: command not found\n", argv[_mrsh_optind]);
		return 127;
	}
	HiSHExecFile(path, &argv[_mrsh_optind]);
	const int exec_error = errno;
	perror("exec");
	return exec_error == ENOENT || exec_error == ENOTDIR ? 127 : 126;
}
