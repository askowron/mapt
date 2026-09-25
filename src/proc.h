#ifndef MAPT_PROC_H
#define MAPT_PROC_H

/* Line callback: "line" is NUL terminated and only valid for the duration
 * of the call, is_err != 0 means the line came from standard error. */
typedef void (*ProcLine)(void *ud, const char *line, int is_err);

/* Tick callback: called while the child is silent.  Returning non zero
 * requests cancellation (SIGTERM is sent to the child). */
typedef int (*ProcTick)(void *ud);

/* Run argv, feeding stdin_data (may be NULL) to the child and streaming
 * its output line by line.  Both streams are kept separate.
 *
 * Returns the exit status, 128+signal when killed, or -1 when the child
 * could not be started at all. */
int proc_run(char *const argv[], const char *stdin_data,
	     ProcLine on_line, ProcTick on_tick, void *ud);

#endif
