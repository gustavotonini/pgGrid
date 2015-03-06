/*--------------------------------------------------------------------
 * FILE:
 *     lifecheck.c
 *
 * NOTE:
 *     This file is composed of the functions to call with the source
 *     at pgreplicate for the lifecheck.
 *
 * Portions Copyright (c) 2003-2008, Atsushi Mitani
 *--------------------------------------------------------------------
 */
#include "postgres.h"
#include "postgres_fe.h"

#include <pthread.h>
#include <stdio.h>
#include <stdarg.h>
#include <sys/types.h>
#include <fcntl.h>
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <signal.h>

/*
#include "libpq/pqsignal.h"
#include "utils/guc.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "tcop/tcopprot.h"
#include "postmaster/postmaster.h"
*/

#include "libpq-fe.h"
#include "libpq-int.h"
#include "fe-auth.h"

#include <sys/socket.h>
#include <unistd.h>
#include <netdb.h>
#include <arpa/inet.h>

#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif


#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif


#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

#include "access/xact.h"
#include "lib/dllist.h"
#include "libpq/pqformat.h"
#include "replicate_com.h"
#include "pgreplicate.h"

#define PING_DB		"template1"
#define PING_QUERY	"SELECT 1"

/*--------------------------------------
 * PROTOTYPE DECLARATION
 *--------------------------------------
 */
int PGRlifecheck_main(int fork_wait_time);

static bool is_started_replication(void);
static int lifecheck_loop(void);
static int ping_cluster(PGconn * conn);
static void set_host_status( HostTbl * host_ptr , int status );

int
PGRlifecheck_main(int fork_wait_time)
{
	bool started = false;
	pid_t pgid = 0;
	pid_t pid = 0;

	pgid = getpgid(0);
	pid = fork();
	if (pid != 0)
	{
		return STATUS_OK;
	}

	/*
	 * in child process,
	 * call recovery module
	 */
	setpgid(0,pgid);

	PGRsignal(SIGHUP, PGRexit_subprocess);
	PGRsignal(SIGTERM, PGRexit_subprocess);
	PGRsignal(SIGINT, PGRexit_subprocess);
	PGRsignal(SIGQUIT, PGRexit_subprocess);

	if (fork_wait_time > 0) {
		sleep(fork_wait_time);
	}

	if (PGRuserName == NULL)
	{
		PGRuserName = getenv("LOGNAME");
		if (PGRuserName == NULL)
		{
			PGRuserName = getenv("USER");
			if (PGRuserName == NULL)
				PGRuserName = "postgres";
		}
	}

	for (;;)
	{
		started = is_started_replication();
		if (!started)
		{
			/* wait next lifecheck as interval */
			sleep(PGR_Lifecheck_Interval);
			continue;
		}

		/* life check to all cluster dbs */
		lifecheck_loop();

		/* wait next lifecheck as interval */
		sleep(PGR_Lifecheck_Interval);
	}
	return STATUS_OK;
}

static bool
is_started_replication(void)
{
	HostTbl * host_ptr = (HostTbl*)NULL;

	host_ptr = Host_Tbl_Begin;
	while(host_ptr->useFlag != DB_TBL_END)
	{
		if (host_ptr->useFlag == DB_TBL_USE)
		{
			return true;
		}
		host_ptr ++;
	}
	return false;
}

static int
lifecheck_loop(void)
{
	HostTbl * host_ptr = (HostTbl*)NULL;
	char	   port[8];
	char * host = NULL;
	PGconn * conn = NULL;

	host_ptr = Host_Tbl_Begin;
	if (host_ptr == NULL)
	{
		return STATUS_ERROR;
	}
	while(host_ptr->useFlag != DB_TBL_END)
	{
		/*
		 * check the status of the cluster DB
		 */
		if (host_ptr->useFlag != DB_TBL_USE)
		{
			host_ptr ++;
			continue;
		}
		snprintf(port,sizeof(port),"%d", host_ptr->port);
		host = (char *)(host_ptr->resolvedName);
		
		/* connect DB */
		conn = PGRcreateConn(host,port, PING_DB ,PGRuserName,"","","",PGR_Lifecheck_Timeout);
		if ((conn != NULL) &&
			(ping_cluster(conn) == STATUS_OK))
		{
			set_host_status(host_ptr, DB_TBL_USE);
		}
		else
		{
			set_host_status(host_ptr, DB_TBL_ERROR);
		}

		PQfinish(conn);
		conn = NULL;
		host_ptr ++;
	}

	return STATUS_OK;
}

static int
ping_cluster(PGconn * conn)
{
	int status = 0;
	PGresult * res = (PGresult *)NULL;

	res = PQexec(conn, PING_QUERY );

	status = PQresultStatus(res);
	if (res != NULL)
	{
		PQclear(res);
	}
	if ((status == PGRES_NONFATAL_ERROR ) ||
		(status == PGRES_FATAL_ERROR ))
	{
		return STATUS_ERROR;
	}
	return STATUS_OK;
}

static void
set_host_status( HostTbl * host_ptr , int status )
{
	if (host_ptr == NULL)
		return;
	if (status == DB_TBL_ERROR)
	{
		host_ptr->retry_count ++;
		if (host_ptr->retry_count > PGR_CONNECT_RETRY_TIME )
		{
			PGRset_host_status(host_ptr, status);
		}
	}
	else
	{
		host_ptr->retry_count = 0;
		PGRset_host_status(host_ptr, status);
	}
}

