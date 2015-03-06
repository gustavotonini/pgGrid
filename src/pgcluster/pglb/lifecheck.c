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
 * pglb is based on pgpool.
 * pgpool: a language independent connection pool server for PostgreSQL 
 * written by Tatsuo Ishii
 *
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
#include "pglb.h"

static ClusterTbl * PGR_Cluster_DB_4_Lifecheck = (ClusterTbl*)NULL;

/*--------------------------------------
 * PROTOTYPE DECLARATION
 *--------------------------------------
 */
int PGRlifecheck_main(int fork_wait_time);
PGconn * PGRcreateConn( char * host, char * port,char * database, char * userName, char * password, char * md5Salt, char * cryptSalt );

static bool is_started_loadbalance(void);
static void set_timeout(SIGNAL_ARGS);
static int lifecheck_loop(void);
static int ping_cluster(PGconn * conn);
static void set_cluster_status(ClusterTbl * host_ptr, int status);

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
	PGRsignal(SIGALRM, set_timeout);

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
		started = is_started_loadbalance();
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
is_started_loadbalance(void)
{
	ClusterTbl * host_ptr = (ClusterTbl*)NULL;

	host_ptr = Cluster_Tbl;
	if (host_ptr == NULL)
	{
		return false;
	}
	while(host_ptr->useFlag != TBL_END)
	{
		if (host_ptr->useFlag == TBL_USE)
		{
			return true;
		}
		host_ptr ++;
	}
	return false;
}

static void 
set_timeout(SIGNAL_ARGS)
{
	if (PGR_Cluster_DB_4_Lifecheck != NULL)
	{
		set_cluster_status( PGR_Cluster_DB_4_Lifecheck, TBL_ERROR_NOTICE);
	}
	PGRsignal(SIGALRM, set_timeout);
}

static int
lifecheck_loop(void)
{
	ClusterTbl * host_ptr = (ClusterTbl*)NULL;
	char	   port[8];
	char * host = NULL;
	PGconn * conn = NULL;

	host_ptr = Cluster_Tbl;
	if (host_ptr == NULL)
	{
		return STATUS_ERROR;
	}
	alarm(0);
	while(host_ptr->useFlag != TBL_END)
	{
		/*
		 * check the status of the cluster DB
		 */
		if ((host_ptr->useFlag != TBL_USE) || (host_ptr->useFlag != TBL_INIT))
		{
			host_ptr ++;
			continue;
		}
		snprintf(port,sizeof(port),"%d", host_ptr->port);
		host = (char *)(host_ptr->hostName);
		/* set host data */
		PGR_Cluster_DB_4_Lifecheck = host_ptr;
		
		/* set alarm as lifecheck timeout */
		alarm(PGR_Lifecheck_Timeout);

		/* connect DB */
		conn = PGRcreateConn(host,port, PING_DB ,PGRuserName,"","","");
		if ((conn != NULL) &&
			(ping_cluster(conn) == STATUS_OK))
		{
			set_cluster_status(host_ptr,TBL_USE);
		}
		else
		{
			set_cluster_status(host_ptr,TBL_ERROR_NOTICE);
		}
		/* reset alarm */
		alarm(0);

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

PGconn *
PGRcreateConn( char * host, char * port,char * database, char * userName, char * password, char * md5Salt, char * cryptSalt )
{
	int cnt = 0;
	PGconn * conn = NULL;
	char pwd[256];

	memset(pwd,0,sizeof(pwd));
	if (*password != '\0')
	{
		if ((strncmp(password,"md5",3) == 0) && (md5Salt != NULL))
		{
			sprintf(pwd,"%s(%d)(%d)(%d)(%d)",password,
				*md5Salt,*(md5Salt+1),*(md5Salt+2),*(md5Salt+3));
		}
		else
		{
			strncpy(pwd,password,sizeof(pwd));
		}
	}
	conn = PQsetdbLogin(host, port, NULL, NULL, database, userName, pwd);
	/* check to see that the backend Connection was successfully made */
	cnt = 0;
	while (PQstatus(conn) == CONNECTION_BAD)
	{
		if (conn != NULL)
		{
			PQfinish(conn);
			conn = NULL;
		}
		conn = PQsetdbLogin(host, port, NULL, NULL, database, userName, pwd);
		if (cnt > PGLB_CONNECT_RETRY_TIME )
		{
			if (conn != NULL)
			{
				PQfinish(conn);
				conn = NULL;
			}
			return (PGconn *)NULL;
		}		
		
		if(PQstatus(conn) == CONNECTION_BAD && h_errno==2)
		{
		    usleep(PGR_SEND_WAIT_MSEC);
			cnt ++;
		}
		else if(!strncasecmp(PQerrorMessage(conn),"FATAL:  Sorry, too many clients already",30) ||
			!strncasecmp(PQerrorMessage(conn),"FATAL:  Non-superuser connection limit",30) ) 
		{
		    usleep(PGR_SEND_WAIT_MSEC);
			cnt ++;
		}
		else if(!strncasecmp(PQerrorMessage(conn),"FATAL:  The database system is starting up",40)   )
		{
		    usleep(PGR_SEND_WAIT_MSEC);
		}
		else
		{
		    usleep(PGR_SEND_WAIT_MSEC);
			cnt ++;
		}
	}
	return conn;
}

static void
set_cluster_status(ClusterTbl * host_ptr, int status)
{
	if (host_ptr == NULL)
		return;
	if (status == TBL_ERROR_NOTICE)
	{
		host_ptr->retry_count ++;
		if (host_ptr->retry_count > PGLB_CONNECT_RETRY_TIME )
		{
			PGRset_status_on_cluster_tbl(status, host_ptr);
		}
	}
	else
	{
		host_ptr->retry_count = 0;
		PGRset_status_on_cluster_tbl(status, host_ptr);
	}
}
