/*--------------------------------------------------------------------
 * FILE:
 *     load_balance.c
 *
 * NOTE:
 *     This file is composed of the functions of load balance modules
 *     with connection pooling or not
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
#include "postgres.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <netdb.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/param.h>
#include <sys/select.h>
#include <arpa/inet.h>
#include <sys/file.h>

#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

#include "replicate_com.h"
#include "pglb.h"

/*--------------------------------------
 * PROTOTYPE DECLARATION
 *--------------------------------------
 */
int PGRload_balance(void);
int PGRload_balance_with_pool(void);
char PGRis_connection_full(ClusterTbl * ptr);
void PGRrelease_connection(ClusterTbl * ptr);
void PGRchild_wait(int sig);

/*--------------------------------------------------------------------
 * SYMBOL
 *    PGRload_balance()
 * NOTES
 *    load balance module that normal connection is used
 * ARGS
 *    void
 * RETURN
 *    OK: STATUS_OK
 *    NG: STATUS_ERROR
 *--------------------------------------------------------------------
 */
int
PGRload_balance(void)
{
	char * func = "PGRload_balance()";
	pid_t pid,pgid;
	int count;
	int status;
	ClusterTbl * cluster_p = NULL;

	PGRsignal(SIGCHLD, PGRchild_wait);
	/* get the least locaded cluster server info */
	cluster_p = PGRscan_cluster();
	count = 0;
	while (cluster_p == NULL )
	{
		if ( count > PGLB_CONNECT_RETRY_TIME)
		{
			show_error("%s:no cluster available",func);
			return STATUS_ERROR;
		}
		cluster_p = PGRscan_cluster();
		if (cluster_p == NULL)
		{
			usleep(20);
		}
		count ++;
	}

	if ((cluster_p == NULL) || (Child_Tbl))
	{
	}
	pgid = getpgid((pid_t)0);
	pid = fork();
	if (pid < 0)
	{
		show_error("%s:fork() failed. (%s)",func,strerror(errno));
		exit(1);
	}
	if (pid == 0)
	{
		int status;
		pid_t mypid;
		setpgid((pid_t)0,pgid);
		CurrentCluster = cluster_p;
		mypid = getpid();

		PGRsem_lock(ClusterSemid,cluster_p->rec_no);
		status = PGRget_child_status(mypid);
		if (status == TBL_END)
		{
			PGRadd_child_tbl(cluster_p, mypid, TBL_USE);
		}
		else
		{
			PGRset_status_to_child_tbl(mypid, TBL_USE);
		}
		PGRsem_unlock(ClusterSemid,cluster_p->rec_no);
		PGRdo_child( NOT_USE_CONNECTION_POOL );
		PGRrelease_connection(cluster_p);
		PGRset_status_to_child_tbl(getpid(), TBL_FREE);
		exit(0);
	}
	else if (pid > 0)
	{
		while ((status = PGRget_child_status(pid)) == TBL_USE)
		{
			usleep(20);
		}
		return STATUS_OK;
	}
	else
	{
		return STATUS_ERROR;
	}
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    PGRload_balance_with_pool()
 * NOTES
 *    load balance module that connection pooling system is used
 * ARGS
 *    void
 * RETURN
 *    OK: STATUS_OK
 *    NG: STATUS_ERROR
 *--------------------------------------------------------------------
 */
int
PGRload_balance_with_pool(void)
{
	char * func = "PGRload_balance_with_pool()";
	int count;
	pid_t pid;
	ClusterTbl * cluster_p = NULL;

	/* get the least locaded cluster server info */
	cluster_p = PGRscan_cluster();
	count = 0;
	while (cluster_p == NULL )
	{
		if ( count > PGLB_CONNECT_RETRY_TIME)
		{
			show_error("%s:no cluster available",func);
       			PGRreturn_no_connection_error();
			return STATUS_ERROR;
		}
		cluster_p = PGRscan_cluster();
		count ++;
	}
	pid = PGRscan_child_tbl(cluster_p);
	if ((pid == 0) || (pid == STATUS_ERROR))
	{
		show_error("%s:no child process available",func);
		return STATUS_ERROR;
	}
	kill(pid,SIGUSR1);

	/* waiting change status to TBL_ACCEPT or TBL_FREE from TBL_USE */
	while ( PGRget_child_status(pid) == TBL_USE)
	{
		usleep(20);
	}

	return STATUS_OK;

}

char
PGRis_connection_full(ClusterTbl * ptr)
{
	char rtn = 1;

	if (ptr == NULL)
	{
		return rtn;
	}
	PGRsem_lock(ClusterSemid,ptr->rec_no);
	if (ptr->max_connect > ptr->use_num)
	{
		rtn = 0;
	}
	PGRsem_unlock(ClusterSemid,ptr->rec_no);
	return rtn;
}

void
PGRrelease_connection(ClusterTbl * ptr)
{
	if (ptr == NULL)
	{
		return;
	}
	PGRsem_lock(ClusterSemid,MAX_DB_SERVER);
	if (ptr->use_num > 0)
	{
		ptr->use_num --;
	}
	PGRsem_unlock(ClusterSemid,MAX_DB_SERVER);
}

void
PGRchild_wait(int sig)
{
	pid_t pid = 0;
	int ret = 0;

	do {
		pid = waitpid(-1,&ret,WNOHANG);
		if ((pid <= 0) && (WTERMSIG(ret) > 0))
		{
			pid = 1;
		}
	} while(pid > 0);
}

/*char PGRis_same_host(char * host1, char * host2)
{
	unsigned int ip1, ip2;

	if ((host1 == NULL) || (host2 == NULL))
	{
		return 0;
	}
	ip1 = PGRget_ip_by_name( host1);
	ip2 = PGRget_ip_by_name( host2);
	if (ip1 == ip2)
	{
		return 1;
	}
	return 0;
}*/
