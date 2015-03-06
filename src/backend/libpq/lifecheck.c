/*--------------------------------------------------------------------
 * FILE:
 *     lifecheck.c
 *
 * NOTE:
 *     This file is composed of the functions to call with the source
 *     at backend for the lifecheck.
 *     Low level I/O functions that called by in these functions are 
 *     contained in 'replicate_com.c'.
 *
 *--------------------------------------------------------------------
 */

#ifdef USE_REPLICATION

#include "postgres.h"

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <ctype.h>
#include <time.h>
#include <pwd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <netdb.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/param.h>
#include <sys/select.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/file.h>
#include <dirent.h>

#include "libpq/pqsignal.h"
#include "utils/guc.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "tcop/tcopprot.h"
#include "postmaster/postmaster.h"

#include "replicate.h"

#ifdef WIN32
#include "win32.h"
#else
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#endif

#ifndef HAVE_STRDUP
#include "strdup.h"
#endif
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

static void set_replication_server_status(int status);
static int send_lifecheck(int sock);
static int recv_lifecheck(int sock);
static void set_timeout(SIGNAL_ARGS);
static void exit_lifecheck(SIGNAL_ARGS);

ReplicateServerInfo * PGR_Replicator_4_Lifecheck = NULL;

int
PGR_Lifecheck_Main(void)
{
	int status = STATUS_OK;
	int sock = -1;
	int pid = 0;

	if ((pid = fork()) != 0 )
	{
		return pid;
	}

	pqsignal(SIGHUP, exit_lifecheck);
	pqsignal(SIGTERM, exit_lifecheck);
	pqsignal(SIGINT, exit_lifecheck);
	pqsignal(SIGQUIT, exit_lifecheck);
	pqsignal(SIGALRM, set_timeout);
	PG_SETMASK(&UnBlockSig);

	for (;;)
	{
		
		if (PGR_Replicator_4_Lifecheck == NULL)
		{
			PGR_Replicator_4_Lifecheck = PGR_check_replicate_server_info();
			if (PGR_Replicator_4_Lifecheck == NULL)
			{
				alarm(0);
				sleep(PGR_Lifecheck_Interval);
				continue;
			}
		}
		/* get replication server information */
		PGR_Replicator_4_Lifecheck = PGR_get_replicate_server_info();
		if (PGR_Replicator_4_Lifecheck == NULL)
		{
			if (Debug_pretty_print)
			{
				elog(DEBUG1,"not found replication server");
			}
			alarm(0);
			sleep(PGR_Lifecheck_Interval);
			continue;
		}
		sock = PGR_get_replicate_server_socket( PGR_Replicator_4_Lifecheck , PGR_QUERY_SOCKET );
		if (sock < 0)
		{
			set_replication_server_status(DATA_ERR);
			if (Debug_pretty_print)
				elog(DEBUG1,"get_replicate_server_socket failed");
			alarm(0);
			sleep(PGR_Lifecheck_Interval);
			continue;
		}

		/* set alarm as lifecheck timeout */
		alarm(PGR_Lifecheck_Timeout * 2);

		/* send lifecheck to replication server */
		status = send_lifecheck(sock);
		if (status != STATUS_OK)
		{
			set_replication_server_status(DATA_ERR);
			PGR_close_replicate_server_socket( PGR_Replicator_4_Lifecheck , PGR_QUERY_SOCKET );
			if (Debug_pretty_print)
				elog(DEBUG1,"send life check failed");
			alarm(0);
			sleep(PGR_Lifecheck_Interval);
			continue;
		}

		/* receive lifecheck response */
		status = recv_lifecheck(sock);
		if (status != STATUS_OK)
		{
			set_replication_server_status(DATA_ERR);
			PGR_close_replicate_server_socket( PGR_Replicator_4_Lifecheck , PGR_QUERY_SOCKET );
			if (Debug_pretty_print)
				elog(DEBUG1,"receive life check failed");
			alarm(0);
			sleep(PGR_Lifecheck_Interval);
			continue;
		}
		
		/* stop alarm */
		alarm(0);
		set_replication_server_status(DATA_USE);
		PGR_close_replicate_server_socket( PGR_Replicator_4_Lifecheck , PGR_QUERY_SOCKET );

		/* wait next lifecheck as interval */
		sleep(PGR_Lifecheck_Interval);
	}
}

static void
set_replication_server_status(int status)
{
	if (status == DATA_ERR)
	{
		PGR_Replicator_4_Lifecheck->retry_count ++;
		if (PGR_Replicator_4_Lifecheck->retry_count > MAX_RETRY_TIMES)
		{
			PGR_Set_Replication_Server_Status(PGR_Replicator_4_Lifecheck, status);
		}
	}
	else
	{
		PGR_Replicator_4_Lifecheck->retry_count = 0;
		PGR_Set_Replication_Server_Status(PGR_Replicator_4_Lifecheck, status);
	}
}

static int
send_lifecheck(int sock)
{
	ReplicateHeader header;
	fd_set	  wmask;
	struct timeval timeout;
	int send_size = 0;
	int buf_size = 0;
	char * send_ptr = (char *)&header;
	int s = 0;
	int rtn = 0;

	timeout.tv_sec = PGR_Lifecheck_Timeout;
	timeout.tv_usec = 0;

	memset(&header,0,sizeof(ReplicateHeader));
	header.cmdSys = CMD_SYS_LIFECHECK;
	header.cmdSts = CMD_STS_CLUSTER;
	buf_size = sizeof(ReplicateHeader);

	for (;;)
	{
		FD_ZERO(&wmask);
		FD_SET(sock,&wmask);
		rtn = select(sock+1, (fd_set *)NULL, &wmask, (fd_set *)NULL, &timeout);
		if (rtn < 0)
		{
			if (errno == EINTR)
			{
				return STATUS_OK;
			}
			else
			{
				elog(DEBUG1, "send_lifecheck():select() failed");
				return STATUS_ERROR;
			}
		}
		else if (rtn && FD_ISSET(sock, &wmask))
		{
			s = send(sock,send_ptr + send_size,buf_size - send_size ,0);
			if (s < 0){
				if (errno == EINTR)
				{
					return STATUS_OK;
				}
				if (errno == EAGAIN)
				{
					continue;
				}
				elog(DEBUG1, "send_replicate_packet():send error");
	
				/* EPIPE || ENCONNREFUSED || ENSOCK || EHOSTUNREACH */
				return STATUS_ERROR;
			} else if (s == 0) {
				elog(DEBUG1, "send_lifecheck():unexpected EOF");
				return STATUS_ERROR;
			} else /*if (s > 0)*/ {
				send_size += s;
				if (send_size == buf_size)
				{
					return STATUS_OK;
				}
			}
		}
	}
}

static int
recv_lifecheck(int sock)
{
	int status = STATUS_OK;
	char result[PGR_MESSAGE_BUFSIZE];

	memset(result,0,PGR_MESSAGE_BUFSIZE);
	status = PGR_recv_replicate_result(sock,result, PGR_Lifecheck_Timeout);
	return ((status >= 0) ?STATUS_OK:STATUS_ERROR);
}

static void
set_timeout(SIGNAL_ARGS)
{
	if (PGR_Replicator_4_Lifecheck != NULL)
	{
		set_replication_server_status(DATA_ERR);
		if (Debug_pretty_print)
			elog(DEBUG1,"time out is occured in life check");
	}
}

static void
exit_lifecheck(SIGNAL_ARGS)
{
	fprintf(stderr,"lifecheck stopped\n");
	exit(0);
}

#endif /* USE_REPLICATION */
