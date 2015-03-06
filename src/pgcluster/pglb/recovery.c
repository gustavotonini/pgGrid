/*--------------------------------------------------------------------
 * FILE:
 *     recovery.c
 *
 * NOTE:
 *     This file is composed of the functions to call with the source
 *     at pglb for the recovery.
 *
 * Portions Copyright (c) 2003-2008, Atsushi Mitani
 *--------------------------------------------------------------------
 * pglb is based on pgpool.
 * pgpool: a language independent connection pool server for PostgreSQL 
 * written by Tatsuo Ishii
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of the
 * author not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The author makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
*/
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ipc.h>
#include <sys/msg.h>
#include <netdb.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/param.h>
#include <arpa/inet.h>
#include <sys/file.h>

#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif
#include "replicate_com.h"
#include "pglb.h"


/*--------------------------------------
 * PROTOTYPE DECLARATION
 *--------------------------------------
 */
void PGRrecovery_main(int fork_wait_time);

static int set_recovery(RecoveryPacket *packet);
static int receive_recovery(int fd);


/*--------------------------------------------------------------------
 * SYMBOL
 *    PGRrecovery_main()
 * NOTES
 *    main module of recovery function
 * ARGS
 *    void
 * RETURN
 *    none
 *--------------------------------------------------------------------
 */
void
PGRrecovery_main(int fork_wait_time)
{
	char * func = "PGRrecovery_main()";
	int fd = -1;
	int rtn;
	pid_t pgid = 0;
	pid_t pid = 0;

	pgid = getpgid(0);
	pid = fork();
	if (pid != 0)
	{
		return;
	}

	PGRsignal(SIGCHLD, SIG_DFL);
	PGRsignal(SIGHUP, PGRexit_subprocess);	
	PGRsignal(SIGINT, PGRexit_subprocess);	
	PGRsignal(SIGQUIT, PGRexit_subprocess);	
	PGRsignal(SIGTERM, PGRexit_subprocess);	
	PGRsignal(SIGPIPE, SIG_IGN);	
	/*
	 * in child process,
	 * call recovery module
	 */
	setpgid(0,pgid);

	if (fork_wait_time > 0) {
#ifdef PRINT_DEBUG
		show_debug("recovery process: wait fork(): pid = %d", getpid());
#endif		
		sleep(fork_wait_time);
	}

	fd = PGRcreate_recv_socket(ResolvedName, Recovery_Port_Number);
	if (fd < 0)
	{
		show_error("%s:PGRcreate_recv_socket failed",func);
		exit(1);
	}
	
	for (;;)
	{
		fd_set	  rmask;
		struct timeval timeout;

		timeout.tv_sec = 60;
		timeout.tv_usec = 0;

		/*
		 * Wait for something to happen.
		 */
		FD_ZERO(&rmask);
		FD_SET(fd,&rmask);
		rtn = select(fd+1, &rmask, (fd_set *)NULL, (fd_set *)NULL, &timeout);
		if (rtn && FD_ISSET(fd, &rmask))
		{
			receive_recovery(fd);
		}
	}
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    set_recovery()
 * NOTES
 *    check a recovery request from replication server
 * ARGS
 *    void
 * RETURN
 *    none
 *--------------------------------------------------------------------
 */
static int
set_recovery(RecoveryPacket *packet)
{
#ifdef PRINT_DEBUG
	char * func = "set_recovery()";
#endif			
	int status = STATUS_OK;
	ClusterTbl key;
	ClusterTbl * ptr;

	PGRset_key_of_cluster(&key,packet);
#ifdef PRINT_DEBUG
	show_debug("%s:received no:%d",func, ntohs(packet->packet_no));
#endif			
	switch (ntohs(packet->packet_no))
	{
	case RECOVERY_PREPARE_REQ:
		/* add cluster db */
#ifdef PRINT_DEBUG
		show_debug("%s:add_db host:%s port:%d max:%d",
			func, packet->hostName,ntohs(packet->port),ntohs(packet->max_connect));
#endif			
		ptr = PGRsearch_cluster_tbl(&key);
		if (ptr == NULL)
		{
			ptr = PGRadd_cluster_tbl(&key);
		}
		if (ptr != NULL)
		{
			PGRset_status_on_cluster_tbl(TBL_STOP,ptr);
			if (Use_Connection_Pool)
			{
				signal(SIGCHLD,PGRrecreate_child);
				status = PGRpre_fork_child(ptr);
			}
		}
		else
		{
			show_error("cluster table is full");
		}
		break;
	case RECOVERY_FINISH:
		/* start cluster db */
		ptr = PGRsearch_cluster_tbl(&key);
		if (ptr != NULL)
		{
#ifdef PRINT_DEBUG
			show_debug("%s:start_db host:%s port:%d max:%d",
				func,packet->hostName,ntohs(packet->port),ntohs(packet->max_connect));
#endif			
			PGRset_status_on_cluster_tbl(TBL_INIT,ptr);
		}
		break;
	case RECOVERY_PGDATA_ANS:
		/* stop cluster db */
		ptr = PGRsearch_cluster_tbl(&key);
		if (ptr != NULL)
		{
#ifdef PRINT_DEBUG
			show_debug("%s:stop_db host:%s port:%d max:%d",
				func, packet->hostName,ntohs(packet->port),ntohs(packet->max_connect));
#endif			
			PGRset_status_on_cluster_tbl(TBL_STOP,ptr);
		}
		break;
	case RECOVERY_ERROR:
		/* delete cluster db */
		ptr = PGRsearch_cluster_tbl(&key);
		if (ptr != NULL)
		{
			PGRset_status_on_cluster_tbl(TBL_FREE,ptr);
			if (Use_Connection_Pool)
			{
				PGRquit_children_on_cluster(ptr->rec_no);
			}
		}
		break;
	/* cluster db has error */
	case RECOVERY_ERROR_CONNECTION:
		/* set error cluster db */
		ptr = PGRsearch_cluster_tbl(&key);
		if (ptr != NULL)
		{
			PGRset_status_on_cluster_tbl(TBL_ERROR_NOTICE,ptr);
			PGRsend_signal(SIGUSR2);
		}
		break;
	}
	return STATUS_OK;
}

static int
receive_recovery(int fd)
{
	int status = STATUS_ERROR;
	int r_size = -1;
	int recv_sock = -1;
	RecoveryPacket packet;

	recv_sock = PGRcreate_acception(fd,ResolvedName,Recovery_Port_Number);
	if (recv_sock >= 0 )
	{
		memset(&packet,0, sizeof(RecoveryPacket));
		r_size = PGRread_byte(recv_sock,(char *)&packet,sizeof(RecoveryPacket),MSG_WAITALL);
		if ( r_size == sizeof(RecoveryPacket) )
		{
			status = set_recovery(&packet);
		}
	}
	PGRclose_sock(&recv_sock);
	return status;
}
