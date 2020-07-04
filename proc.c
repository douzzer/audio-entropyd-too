/*
 * $Id: proc.c,v 2.1 2003/02/06 22:07:00 folkert Exp $
 * $Log: proc.c,v $
 * Revision 2.1  2003/02/06 22:07:00  folkert
 * added logging
 *
 * Revision 2.0  2003/01/27 17:47:02  folkert
 * *** empty log message ***
 *
 */

#include <sys/types.h>
#include <errno.h>
#include <stdio.h>
#include <unistd.h>
#include "error.h"

int become_daemon(void)
{
	if (daemon(0, 0) == -1)
		error_exit("become_daemon::daemon: failed");

	return 0;
}

int write_pidfile(char *fname)
{
	FILE *fh = fopen(fname, "w");
        if (!fh)
		error_exit("write_pidfile::fopen: failed creating file %s", fname);

        fprintf(fh, "%i", getpid());

        fclose(fh);

	return 0;
}
