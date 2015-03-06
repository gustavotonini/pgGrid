/*--------------------------------------------------------------------
 * FILE:
 *     cluster_tbl.c
 *
 * NOTE:
 *     This file is composed of the functions to use a cluster table.
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
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
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
int PGRis_cluster_alive(void) ;
ClusterTbl * PGRscan_cluster(void);
void PGRset_key_of_cluster(ClusterTbl * ptr, RecoveryPacket * packet);
ClusterTbl * PGRadd_cluster_tbl (ClusterTbl * conf_data);
ClusterTbl * PGRset_status_on_cluster_tbl (int status, ClusterTbl * ptr);
ClusterTbl * PGRsearch_cluster_tbl(ClusterTbl * conf_data);

static int set_cluster_tbl(ClusterTbl * ptr , ClusterTbl * conf_data);
static ClusterTbl * search_free_cluster_tbl(void );
static void write_cluster_status_file(ClusterTbl * ptr);
static int check_cluster_num(void);

int PGRis_cluster_alive(void) 
{
	return check_cluster_num() == 0 ? STATUS_ERROR : STATUS_OK;
}

ClusterTbl * 
PGRscan_cluster(void)
{
	char * func = "PGRscan_cluster";
	ClusterTbl * ptr = NULL;
	ClusterTbl * rtn = NULL;
	int min_use_rate = 100;
	int use_rate = 0;
	int cnt = 0;


	ptr = Cluster_Tbl;
	if (ptr == NULL)
	{
		show_error("%s:Cluster Table is not initialize",func);
		return (ClusterTbl *)NULL;
	}
#ifdef PRINT_DEBUG
	show_debug("%s:%d ClusterDB can be used",func,ClusterNum);
#endif			
	PGRsem_lock(ClusterSemid,MAX_DB_SERVER);
	while ((cnt < Max_DB_Server) && (ptr->useFlag != TBL_END))
	{
#ifdef PRINT_DEBUG
		show_debug("%s:%s [%d],useFlag->%d max->%d use_num->%d\n",
			func, ptr->hostName,ptr->port,ptr->useFlag,ptr->max_connect,ptr->use_num);
#endif			
		cnt ++;
		if ((ptr->useFlag != TBL_USE) && (ptr->useFlag != TBL_INIT))
		{
			ptr ++;
			continue;
		}
		if (ptr->max_connect <= ptr->use_num)
		{
			ptr ++;
			continue;
		}
		if (ptr->use_num > 0)
		{
			use_rate = ptr->use_num * 100 / ptr->max_connect ;
		}
		else
		{
			use_rate = 0;
			rtn = ptr;
			break;
		}
		if (min_use_rate > use_rate)
		{
			min_use_rate = use_rate;
			rtn = ptr;
		}
		ptr ++;
	}
	if (rtn != NULL)
	{
		rtn->use_num ++;
		if (rtn->useFlag == TBL_INIT)
		{
			PGRset_status_on_cluster_tbl (TBL_USE,rtn);
		}
	}
	PGRsem_unlock(ClusterSemid,MAX_DB_SERVER);
	return rtn;
}

void
PGRset_key_of_cluster(ClusterTbl * ptr, RecoveryPacket * packet)
{
	int max_connect = 0;
	int port = 0;

	memset(ptr,0,sizeof(ClusterTbl));
	memcpy(ptr->hostName,packet->hostName,sizeof(ptr->hostName));
	max_connect = ntohs(packet->max_connect);
	if (max_connect >= 0)
	{
		ptr->max_connect = max_connect;
	}
	else
	{
		ptr->max_connect = DEFAULT_CONNECT_NUM;
	}
	port = ntohs(packet->port);
	if ( port >= 0)
	{
		ptr->port = port;
	}
	else
	{
		ptr->port = DEFAULT_PORT;
	}
}

ClusterTbl *
PGRadd_cluster_tbl (ClusterTbl * conf_data)
{
	char * func = "PGRadd_cluster_tbl()";
	ClusterTbl * ptr;

	ptr = PGRsearch_cluster_tbl(conf_data);
	if ((ptr != NULL) && 
		((ptr->useFlag == TBL_USE ) || ((ptr->useFlag == TBL_INIT))))
	{
		ptr->max_connect = conf_data->max_connect;
		ptr->use_num = 0;
		ptr->rate = 0;
		return ptr;
	}
	ptr = search_free_cluster_tbl();
	if (ptr == (ClusterTbl *) NULL)
	{
		show_error("%s:no more free space in cluster table",func);
		return (ClusterTbl *)NULL;
	}

	set_cluster_tbl( ptr, conf_data);
	return ptr;
}

ClusterTbl *
PGRset_status_on_cluster_tbl (int status, ClusterTbl * ptr)
{
#ifdef PRINT_DEBUG
	char * func = "PGRset_status_on_cluster_tbl()";
#endif			

	if (ptr != (ClusterTbl*)NULL)
	{
		if (ptr->useFlag != status)
		{
#ifdef PRINT_DEBUG
			show_debug("%s:host:%s port:%d max:%d use:%d status%d",
				func, ptr->hostName,ptr->port,ptr->max_connect,ptr->useFlag,status);
#endif			
			if ((ptr->useFlag != TBL_ERROR) || 
				(status != TBL_ERROR_NOTICE))
			{
				ptr->useFlag = status;
				write_cluster_status_file(ptr);
				ClusterNum = check_cluster_num();
				if (status == TBL_ERROR_NOTICE)	
				{
					/* disconnect failed cluster */
					PGRsend_signal(SIGUSR2);
				}
			}
		}
	}
	return ptr;
}

static int
check_cluster_num(void)
{
	ClusterTbl *ptr;
	int cnt = 0;
	int rec_num = 0;

	ptr = Cluster_Tbl;
	PGRsem_lock(ClusterSemid,0);
	while ((ptr->useFlag != TBL_END) && (rec_num < Max_DB_Server))
	{
		if ((ptr->useFlag == TBL_USE) || (ptr->useFlag == TBL_INIT))
		{
			cnt ++;
		}
		rec_num ++;
		ptr ++;
	}
	PGRsem_unlock(ClusterSemid,0);
	return cnt;
}

static void
write_cluster_status_file(ClusterTbl * ptr)
{
	switch( ptr->useFlag)
	{
		case TBL_FREE:
			PGRwrite_log_file(StatusFp,"port(%d) host:%s free",
					ptr->port,
					ptr->hostName);
			break;
		case TBL_INIT:
			PGRwrite_log_file(StatusFp,"port(%d) host:%s initialize",
					ptr->port,
					ptr->hostName);
			break;
		case TBL_USE:
			PGRwrite_log_file(StatusFp,"port(%d) host:%s start use",
					ptr->port,
					ptr->hostName);
			break;
		case TBL_ERROR:
			PGRwrite_log_file(StatusFp,"port(%d) host:%s error",
					ptr->port,
					ptr->hostName);
			break;
		case TBL_END:
			PGRwrite_log_file(StatusFp,"port(%d) host:%s end",
					ptr->port,
					ptr->hostName);
			break;
	}
}

ClusterTbl *
PGRsearch_cluster_tbl(ClusterTbl * conf_data)
{
	ClusterTbl *ptr;
	int cnt = 0;
	int rec_num = 0;

	ptr = Cluster_Tbl;
	while ((cnt <= ClusterNum) && (rec_num < Max_DB_Server))
	{
		if (ptr->port > 0)
		{
			if ((!strcmp(ptr->hostName,conf_data->hostName)) &&
				(ptr->port == conf_data->port))
			{
				return ptr;
			}
			if ((ptr->useFlag == TBL_USE) || (ptr->useFlag == TBL_INIT))
			{
				cnt ++;
			}
		}
		ptr ++;
		rec_num ++;
	}
	return (ClusterTbl *)NULL;
}

static int
set_cluster_tbl(ClusterTbl * ptr , ClusterTbl * conf_data)
{
	int rec_no;

	rec_no = ptr->rec_no;
	memcpy(ptr->hostName,conf_data->hostName,sizeof(ptr->hostName));
	ptr->max_connect = conf_data->max_connect;
	ptr->port = conf_data->port;
	ptr->use_num = conf_data->use_num;
	ptr->rate = conf_data->rate;
	PGRset_status_on_cluster_tbl (TBL_INIT, ptr);

	return STATUS_OK;
}

static ClusterTbl *
search_free_cluster_tbl(void )
{
	ClusterTbl *ptr;
	int cnt = 0;

	ptr = Cluster_Tbl;
	while ((ptr->useFlag != TBL_END) && (cnt < Max_DB_Server))
	{
		if ((ptr->useFlag == TBL_FREE) || (ptr->useFlag == TBL_ERROR))
		{
			return ptr;
		}
		cnt ++;
		ptr ++;
	}
	return (ClusterTbl *)NULL;
}

