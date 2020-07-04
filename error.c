#define MAX_BACKTRACE_LENGTH    256
#include <sys/types.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <syslog.h>
#include <regex.h>
#if defined(__GLIBC__)
#include <execinfo.h>
#endif

void print_trace(void)
{
#if defined(__GLIBC__)
	void *array[MAX_BACKTRACE_LENGTH];
	size_t size;

	size = backtrace(array, MAX_BACKTRACE_LENGTH);
	printf("Obtained %zd stack frames:\n", size);
	fflush(NULL);
	backtrace_symbols_fd(array, size, 1);
#endif
}

void error_exit(char *format, ...)
{
	char buffer[4096];
	va_list ap;

	va_start(ap, format);
	vsnprintf(buffer, sizeof(buffer), format, ap);
	va_end(ap);
	fprintf(stderr, "%s\n", buffer);
	syslog(LOG_INFO, "%s", buffer);
	printf("\n\n\nDebug information:\n");
	if (errno) fprintf(stderr, "errno: %d=%s (if applicable)\n", errno, strerror(errno));

#if defined(__GLIBC__)
	print_trace();
#endif

	fflush(NULL);

	(void)kill(0, SIGTERM); /* terminate every process in the process group of the current process */

	exit(EXIT_FAILURE);
}

