/*
 *  Copyright (C) 2002 - 2007 Tomasz Kojm <tkojm@clamav.net>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston,
 *  MA 02110-1301, USA.
 */

#ifdef	_MSC_VER
#include <winsock.h>
#endif

#if HAVE_CONFIG_H
#include "clamav-config.h"
#endif

#if defined HAVE_FD_PASSING && defined FDPASS_NEED_XOPEN
/* to expose BSD 4.4/Unix98 semantics instead of BSD 4.3 semantics */
#define _XOPEN_SOURCE 500
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef	HAVE_UNISTD_H
#include <unistd.h>
#endif
#include <sys/types.h>
#ifndef	C_WINDOWS
#include <dirent.h>

#include <sys/socket.h>
#ifdef HAVE_FD_PASSING
#ifdef HAVE_SYS_UIO_H
#include <sys/uio.h>
#endif
#endif

#include <sys/time.h>
#endif
#include <pthread.h>
#include <time.h>
#include <errno.h>
#include <stddef.h>

#include "libclamav/clamav.h"
#include "libclamav/str.h"
#include "libclamav/others.h"

#include "shared/optparser.h"
#include "shared/output.h"
#include "shared/misc.h"

#include "others.h"
#include "scanner.h"
#include "server.h"
#include "session.h"
#include "thrmgr.h"

#ifdef HAVE_FD_PASSING
static int recvfd_and_scan(int desc, const struct cl_engine *engine, unsigned int options, const struct optstruct *opts)
{
	struct msghdr msg;
	struct cmsghdr *cmsg;
	unsigned char buf[CMSG_SPACE(sizeof(int))];
	struct iovec iov[1];
	char dummy;
	int ret=-1;

	memset(&msg, 0, sizeof(msg));
	iov[0].iov_base = &dummy;
	iov[0].iov_len = 1;
	msg.msg_iov = iov;
	msg.msg_iovlen = 1;
	msg.msg_control = buf;
	msg.msg_controllen = sizeof(buf);

	if (recvmsg(desc, &msg, 0) == -1) {
	    logg("recvmsg failed: %s!", strerror(errno));
	    return -1;
	}
	if ((msg.msg_flags & MSG_TRUNC) || (msg.msg_flags & MSG_CTRUNC)) {
	    logg("control message truncated");
	    return -1;
	}
	for (cmsg = CMSG_FIRSTHDR(&msg); cmsg != NULL;
	    cmsg = CMSG_NXTHDR(&msg, cmsg)) {
		if (cmsg->cmsg_len == CMSG_LEN(sizeof(int)) &&
		    cmsg->cmsg_level == SOL_SOCKET &&
		    cmsg->cmsg_type == SCM_RIGHTS) {
			int fd = *(int *)CMSG_DATA(cmsg);
			ret = scanfd(fd, NULL, engine, options, opts, desc);
			close(fd);
		}
	}
	return ret;
}

#else
static int recvfd_and_scan(int desc, const struct cl_engine *engine, unsigned int options, const struct optstruct *opts)
{
	mdprintf(desc, "ERROR: FILDES support not compiled in\n");
	return -1;
}
#endif

int command(client_conn_t *conn, int timeout)
{

    int desc = conn->sd;
    char *buff = conn->cmd;
    size_t cmdlen = conn->cmdlen;
    const struct cl_engine *engine = conn->engine;
    unsigned int options = conn->options;
    const struct optstruct *opts = conn->opts;
    const char term = conn->term;

    cli_chomp(buff);
    thrmgr_setactiveengine(engine);

    if(!strncmp(buff, CMD1, strlen(CMD1))) { /* SCAN */
	thrmgr_setactivetask(NULL, CMD1);
	if(scan(buff + strlen(CMD1) + 1, term, NULL, engine, options, opts, desc, TYPE_SCAN) == -2)
	    if(optget(opts, "ExitOnOOM")->enabled)
		return COMMAND_SHUTDOWN;

    } else if(!strncmp(buff, CMD3, strlen(CMD3))) { /* QUIT */
	thrmgr_setactivetask(NULL, CMD3);
	return COMMAND_SHUTDOWN;

    } else if(!strncmp(buff, CMD4, strlen(CMD4))) { /* RELOAD */
	thrmgr_setactivetask(NULL, CMD4);
	/* we'll reload, hide the engine, if we are the last
	 * holding a ref to the engine it'll be freed,
	 * we don't want STATS command to access it */
	thrmgr_setactiveengine(NULL);
	mdprintf(desc, "RELOADING%c", conn->term);
	return COMMAND_RELOAD;

    } else if(!strncmp(buff, CMD5, strlen(CMD5))) { /* PING */
	thrmgr_setactivetask(NULL, CMD5);
	mdprintf(desc, "PONG%c", conn->term);

    } else if(!strncmp(buff, CMD6, strlen(CMD6))) { /* CONTSCAN */
	thrmgr_setactivetask(NULL, CMD6);
	if(scan(buff + strlen(CMD6) + 1, term, NULL, engine, options, opts, desc, TYPE_CONTSCAN) == -2)
	    if(optget(opts, "ExitOnOOM")->enabled)
		return COMMAND_SHUTDOWN;

    } else if(!strncmp(buff, CMD7, strlen(CMD7))) { /* VERSION */
	uint32_t ver;

	thrmgr_setactivetask(NULL, CMD7);
	cl_engine_get(engine, CL_ENGINE_DB_VERSION, &ver);
	if(ver) {
	    char timestr[32];
	    const char *tstr;
	    time_t t;
	    cl_engine_get(engine, CL_ENGINE_DB_TIME, &t);
	    tstr = cli_ctime(&t, timestr, sizeof(timestr));
	    /* cut trailing \n */
	    timestr[strlen(tstr)-1] = '\0';
	    mdprintf(desc, "ClamAV %s/%u/%s%c", get_version(), (unsigned int) ver, tstr, conn->term);
	} else {
	    mdprintf(desc, "ClamAV %s%c", get_version(), conn->term);
	}

    } else if(!strncmp(buff, CMD8, strlen(CMD8))) { /* STREAM */
	thrmgr_setactivetask(NULL, CMD8);
	if(scanstream(desc, NULL, engine, options, opts, conn->term) == CL_EMEM)
	    if(optget(opts, "ExitOnOOM")->enabled)
		return COMMAND_SHUTDOWN;

    } else if(!strncmp(buff, CMD9, strlen(CMD9))) { /* SESSION */
	thrmgr_setactivetask(NULL, CMD9);
	return COMMAND_SESSION;

    } else if(!strncmp(buff, CMD10, strlen(CMD10))) { /* END */
	thrmgr_setactivetask(NULL, CMD10);
	return COMMAND_END;

    } else if(!strncmp(buff, CMD11, strlen(CMD11))) { /* SHUTDOWN */
	thrmgr_setactivetask(NULL, CMD11);
	return COMMAND_SHUTDOWN;

    } else if(!strncmp(buff, CMD13, strlen(CMD13))) { /* MULTISCAN */
	thrmgr_setactivetask(buff+strlen(CMD13)+1, CMD13);
	if(scan(buff + strlen(CMD13) + 1, term, NULL, engine, options, opts, desc, TYPE_MULTISCAN) == -2)
	    if(optget(opts, "ExitOnOOM")->enabled)
		return COMMAND_SHUTDOWN;

    } else if(!strncmp(buff, CMD14, strlen(CMD14))) { /* FILDES */
	thrmgr_setactivetask(NULL, CMD14);
	if(recvfd_and_scan(desc, engine, options, opts) == -2)
	    if(optget(opts, "ExitOnOOM")->enabled)
		return COMMAND_SHUTDOWN;
    } else if(!strncmp(buff, CMD15, strlen(CMD15))) { /* STATS */
	    thrmgr_setactivetask(NULL, CMD15);
	    thrmgr_printstats(desc);
    } else {
	mdprintf(desc, "UNKNOWN COMMAND%c", conn->term);
    }

    return 0; /* no error and no 'special' command executed */
}
