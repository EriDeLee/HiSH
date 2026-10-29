# Run inside the ordinary HiSH shell, without diagnostic APIs or test libraries.
# Regression for builtin exec skipping mrsh's exported-variable synchronization.
export HISH_EXEC_EXPORT_REGRESSION=exported-value
exec busybox sh -c 'test "$HISH_EXEC_EXPORT_REGRESSION" = exported-value && printf "SHELL_EXEC_EXPORT_OK\n"'
