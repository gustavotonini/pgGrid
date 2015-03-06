/*--------------------------------------------------------------------
 * FILE:
 *    conf.c
 *    Replication server for PostgreSQL
 *
 * NOTE:
 *    Read and set configuration data in this modul.
 *
 * Portions Copyright (c) 2003-2008, Atsushi Mitani
 *--------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <netdb.h>
#include <errno.h>
#include <sys/file.h>



#include "libpq-fe.h"
#include "libpq-int.h"
#include "fe-auth.h"

#include "replicate_com.h"
#include "pgreplicate.h"

/*--------------------------------------------------------------------
 * SYMBOL
 *    PGRget_Conf_Data()
 * NOTES
 *    Initialize mamory and tables
 * ARGS
 *    char * path: path of the setup file (I)
 * RETURN
 *    OK: STATUS_OK
 *    NG: STATUS_ERROR
 *--------------------------------------------------------------------
 */
int
PGRget_Conf_Data(char * path)
{
	char * func = "PGRget_Conf_Data()";
	HostTbl host_tbl[MAX_DB_SERVER];
	ConfDataType * conf = NULL;
	int cnt = 0;
	int lb_cnt = 0;
	int cascade_cnt = 0;
	int rec_no = 0;
	int lb_rec_no = 0;
	int cascade_rec_no = -1;
	int i = 0;
	int size = 0;
	char fname[256];
	union semun sem_arg;

	/*
	 * open log file
	 */
	if (path == NULL)
	{
		path = ".";
	}
	size = sizeof(LogFileInf);
	LogFileData = (LogFileInf *) malloc(size);
	if (LogFileData == NULL)
	{
		show_error("%s:malloc() failed. reason: %s", func,strerror(errno));
		return STATUS_ERROR;
	}
	memset(LogFileData,0,size);

	snprintf(fname,sizeof(fname),"%s/%s",path,PGREPLICATE_STATUS_FILE);
	StatusFp = fopen(fname,"a");
	if (StatusFp == NULL)
	{
		show_error("%s:fopen failed: (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}

	snprintf(fname,sizeof(fname),"%s/%s",path,PGREPLICATE_RID_FILE);
	RidFp = fopen(fname,"r+");
	if (RidFp == NULL)
	{
		RidFp = fopen(fname,"w+");
		if (RidFp == NULL)
		{
			show_error("%s:fopen failed: (%s)",func,strerror(errno));
			return STATUS_ERROR;
		}
	}

	/*
	 * read configuration file
	 */
	if (PGR_Get_Conf_Data(path,PGREPLICATE_CONF_FILE) != STATUS_OK)
	{
		show_error("%s:PGR_Get_Conf_Data failed",func);
		return STATUS_ERROR;
	}
#ifdef PRINT_DEBUG
	show_debug("PGR_Get_Conf_Data ok");
#endif			

	/* allocate response information table */
	PGR_Response_Inf = (ResponseInf *)malloc(sizeof(ResponseInf));
	if (PGR_Response_Inf == NULL)
	{
		show_error("%s:malloc() failed. reason: %s", func,strerror(errno));
		return STATUS_ERROR;
	}
	PGR_Response_Inf->response_mode = PGR_NORMAL_MODE;
	PGR_Response_Inf->current_cluster = 0;

	/*
	 * memory allocate load balance table buffer
	 */
	LoadBalanceTbl = (RecoveryTbl *)malloc(sizeof(RecoveryTbl)*MAX_DB_SERVER);
	if (LoadBalanceTbl == (RecoveryTbl *)NULL)
	{
		show_error("%s:malloc failed: (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
#ifdef PRINT_DEBUG
	show_debug("LoadBalanceTbl allocate ok");
#endif			

	/*
	 * memory allocate cascade server table buffer
	 */
	size = sizeof(ReplicateServerInfo) * MAX_DB_SERVER;
	CascadeTblShmid = shmget(IPC_PRIVATE,size,IPC_CREAT | IPC_EXCL | 0600);
	if (CascadeTblShmid < 0)
	{
		show_error("%s:shmget() failed. reason: %s", func,strerror(errno));
		return STATUS_ERROR;
	}
#ifdef PRINT_DEBUG
	show_debug("%s:CascadeTbl shmget ok",func);
#endif			
	Cascade_Tbl = (ReplicateServerInfo *)shmat(CascadeTblShmid,0,0);
	if (Cascade_Tbl == (ReplicateServerInfo *)-1)
	{
		show_error("%s:shmat() failed. reason: %s", func,strerror(errno));
		return STATUS_ERROR;
	}
#ifdef PRINT_DEBUG
	show_debug("%s:CascadeTbl shmat ok",func);
#endif			
	memset(Cascade_Tbl , 0 , size );

	/*
	 * memory allocate cascade index 
	 */
	size = sizeof(CascadeInf);
	CascadeInfShmid = shmget(IPC_PRIVATE,size,IPC_CREAT | IPC_EXCL | 0600);
	if (CascadeInfShmid < 0)
	{
		show_error("%s:shmget() failed. reason: %s", func,strerror(errno));
		return STATUS_ERROR;
	}
#ifdef PRINT_DEBUG
	show_debug("%s:CascadeInf shmget ok",func);
#endif			
	Cascade_Inf = (CascadeInf *)shmat(CascadeInfShmid,0,0);
	if (Cascade_Inf == (CascadeInf *)-1)
	{
		show_error("%s:shmat() failed. reason: %s",func, strerror(errno));
		return STATUS_ERROR;
	}
#ifdef PRINT_DEBUG
	show_debug("%s:CascadeInf shmat ok",func);
#endif			
	memset(Cascade_Inf , 0 , size );

	/*
	 * memory allocate replication commit log buffer
	 */
	size = sizeof(CommitLogInf) * MAX_DB_SERVER * MAX_CONNECTIONS;
	CommitLogShmid = shmget(IPC_PRIVATE,size,IPC_CREAT | IPC_EXCL | 0600);
	if (CommitLogShmid < 0)
	{
		show_error("%s:shmget() failed. reason: %s", func, strerror(errno));
		return STATUS_ERROR;
	}
#ifdef PRINT_DEBUG
	show_debug("%s:CommitLog shmget ok",func);
#endif			
	Commit_Log_Tbl = (CommitLogInf *)shmat(CommitLogShmid,0,0);
	if (Commit_Log_Tbl == (CommitLogInf *)-1)
	{
		show_error("%s:shmat() failed. reason: %s",func, strerror(errno));
		return STATUS_ERROR;
	}
#ifdef PRINT_DEBUG
	show_debug("%s:Commit_Log_Tbl shmat ok",func);
#endif			
	memset(Commit_Log_Tbl , 0 , size );
	(Commit_Log_Tbl + (MAX_DB_SERVER * MAX_CONNECTIONS) -1)->inf.useFlag = DB_TBL_END;

	/* create semapho */
	if ((SemID = semget(IPC_PRIVATE,2,IPC_CREAT | IPC_EXCL | 0600)) < 0)
	{
		show_error("%s:semget() failed. (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
	for ( i = 0 ; i < 2 ; i ++)
	{
		semctl(SemID, i, GETVAL, sem_arg);
		sem_arg.val = 1;
		semctl(SemID, i, SETVAL, sem_arg);
	}

	/* create semapho */
	if ((CascadeSemID = semget(IPC_PRIVATE,2,IPC_CREAT | IPC_EXCL | 0600)) < 0)
	{
		show_error("%s:semget() failed. (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
	for ( i = 0 ; i < 2 ; i ++)
	{
		semctl(CascadeSemID, i, GETVAL, sem_arg);
		sem_arg.val = 1;
		semctl(CascadeSemID, i, SETVAL, sem_arg);
	}


	if ((VacuumSemID = semget(IPC_PRIVATE,2,IPC_CREAT | IPC_EXCL | 0600)) < 0)
	{
		show_error("%s:semget() failed. (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
	for ( i = 0 ; i < 2 ; i ++)
	{
		semctl(VacuumSemID, i, GETVAL, sem_arg);
		sem_arg.val = 1;
		semctl(VacuumSemID, i, SETVAL, sem_arg);
	}
	size = sizeof(ReplicationLogInf);
	Replicateion_Log = malloc(size);
	if (Replicateion_Log == NULL)
	{
		show_error("%s:malloc failed: (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
	memset(Replicateion_Log , 0 , size );
	Replicateion_Log->RLog_Sock_Path = NULL;
#ifdef PRINT_DEBUG
	show_debug("%s:RLog Memory Allocation ok",func);
#endif			


	/*
	 * set each datas into the tables
	 */
	conf = ConfData_Top;
	while (conf != (ConfDataType *)NULL) 
	{
		show_debug("registering (key,value)=(%s,%s)",conf->key,conf->value);
		/* get cluster db data */
		if (!STRCMP(conf->table,CLUSTER_SERVER_TAG))
		{
			rec_no = conf->rec_no;
			if (cnt < rec_no)
			{
				cnt = rec_no;
				if (cnt >= MAX_DB_SERVER)
				{
					continue;
				}
			}
			if (!STRCMP(conf->key,HOST_NAME_TAG))
			{
				int ip;
				strncpy(host_tbl[rec_no].hostName,conf->value,sizeof(host_tbl[rec_no].hostName));
				show_debug("registering hostname %s",host_tbl[rec_no].hostName);
				ip=PGRget_ip_by_name(conf->value);

				sprintf(host_tbl[rec_no].resolvedName,
					 "%d.%d.%d.%d",
					 (ip      ) & 0xff ,
					 (ip >>  8) & 0xff ,
					 (ip >> 16) & 0xff ,
					 (ip >> 24) & 0xff );
				show_debug("resolved name is %s",host_tbl[rec_no].resolvedName);

				conf = (ConfDataType*)conf->next;
				continue;
			}
			if (!STRCMP(conf->key,PORT_TAG))
			{
				host_tbl[rec_no].port = atoi(conf->value);
				conf = (ConfDataType*)conf->next;
				continue;
			}
			if (!STRCMP(conf->key,RECOVERY_PORT_TAG))
			{
				host_tbl[rec_no].recoveryPort = atoi(conf->value);
				conf = (ConfDataType*)conf->next;
				continue;
			}
		}
		/* get cascade server data */
		else if (!STRCMP(conf->table, REPLICATION_SERVER_INFO_TAG))
		{
			cascade_rec_no = conf->rec_no ;
			if (cascade_cnt < cascade_rec_no)
			{
				cascade_cnt = cascade_rec_no;
				if (cascade_cnt >= MAX_DB_SERVER)
				{
					continue;
				}
			}
			if (!STRCMP(conf->key,HOST_NAME_TAG))
			{
				strncpy((Cascade_Tbl+cascade_rec_no)->hostName,conf->value,sizeof(Cascade_Tbl->hostName));
				conf = (ConfDataType*)conf->next;
				continue;
			}
			if (!STRCMP(conf->key,PORT_TAG))
			{
				if (atoi(conf->value) > 0)
				{
					(Cascade_Tbl+cascade_rec_no)->portNumber = atoi(conf->value);
				}
				else
				{
					(Cascade_Tbl+cascade_rec_no)->portNumber = DEFAULT_PGRP_PORT;
				}
				(Cascade_Tbl+cascade_rec_no)->sock = -1;

				conf = (ConfDataType*)conf->next;
				PGRset_cascade_server_status(Cascade_Tbl+cascade_rec_no,DB_TBL_USE);
				if (cascade_rec_no == 0)
				{
					Cascade_Inf->top = Cascade_Tbl;
				}
				continue;
			}
			if (!STRCMP(conf->key,RECOVERY_PORT_TAG))
			{
				if (atoi(conf->value) > 0)
				{
					(Cascade_Tbl+cascade_rec_no)->recoveryPortNumber = atoi(conf->value);
				}
				else
				{
					(Cascade_Tbl+cascade_rec_no)->recoveryPortNumber = DEFAULT_PGRP_RECOVERY_PORT;
				}
				(Cascade_Tbl+cascade_rec_no)->rlog_sock=-1;
				(Cascade_Tbl+cascade_rec_no +1)->useFlag = DB_TBL_END;
				conf = (ConfDataType*)conf->next;
				continue;
			}
		}
		/* get loadbalancer table data */
		else if (!STRCMP(conf->table,LOAD_BALANCE_SERVER_TAG))
		{
			lb_rec_no = conf->rec_no;
			if (lb_cnt < lb_rec_no)
			{
				lb_cnt = lb_rec_no;
				if (lb_cnt >= MAX_DB_SERVER)
				{
					continue;
				}
			}
			if (!STRCMP(conf->key,HOST_NAME_TAG))
			{
				strncpy((LoadBalanceTbl + lb_rec_no)->hostName, conf->value,sizeof(LoadBalanceTbl->hostName));
				conf = (ConfDataType*)conf->next;
				continue;
			}
			if (!STRCMP(conf->key,RECOVERY_PORT_TAG))
			{
				(LoadBalanceTbl + lb_rec_no)->recoveryPort = atoi(conf->value);
				(LoadBalanceTbl + lb_rec_no)->sock = -1;
				(LoadBalanceTbl + lb_rec_no)->recovery_sock = -1;
				conf = (ConfDataType*)conf->next;
				continue;
			}
		}
		/* get logging file data */
		else if (!STRCMP(conf->table, LOG_INFO_TAG))
		{
			if (!STRCMP(conf->key, FILE_NAME_TAG))
			{
				strncpy(LogFileData->file_name, conf->value ,sizeof(LogFileData->file_name));
				LogFileData->fp = NULL;
				conf = (ConfDataType*)conf->next;
				continue;
			}
			if (!STRCMP(conf->key, FILE_SIZE_TAG))
			{
				int i,len;
				char * ptr;
				int unit = 1;
				len = strlen(conf->value);
				ptr = conf->value;
				for (i = 0; i < len ; i ++,ptr++)
				{
					if ((! isdigit(*ptr)) && (! isspace(*ptr)))
					{
						switch (*ptr)
						{
							case 'K':
							case 'k':
								unit = 1024;
								break;
							case 'M':
							case 'm':
								unit = 1024*1024;
								break;
							case 'G':
							case 'g':
								unit = 1024*1024*1024;
								break;
						}
						*ptr = '\0';
						break;
					}
				}
				LogFileData->max_size = atoi(conf->value) * unit;
				conf = (ConfDataType*)conf->next;
				continue;
			}
			if (!STRCMP(conf->key, LOG_ROTATION_TAG))
			{
				LogFileData->rotation = atoi(conf->value);
				conf = (ConfDataType*)conf->next;
				continue;
			}
		}
		else
		{
			if (!STRCMP(conf->key,HOST_NAME_TAG))
			{
			    int ip;
				ip=PGRget_ip_by_name(conf->value);
				if (ResolvedName == NULL)
				{
					ResolvedName = malloc(ADDRESS_LENGTH);
				}
				if (ResolvedName == NULL)
				{
					continue;
				}
				else
				{
					memset(ResolvedName,0,ADDRESS_LENGTH);
				}

				sprintf(ResolvedName,
					 "%d.%d.%d.%d",
					 (ip      ) & 0xff ,
					 (ip >>  8) & 0xff ,
					 (ip >> 16) & 0xff ,
					 (ip >> 24) & 0xff );
				conf = (ConfDataType*)conf->next;
				continue;
			}
			else if (!STRCMP(conf->key,REPLICATE_PORT_TAG))
			{
				Port_Number = atoi(conf->value);
				conf = (ConfDataType*)conf->next;
				continue;
			}
			/* get port number for recovery cluster db server */
			else if (!STRCMP(conf->key,RECOVERY_PORT_TAG))
			{
				if (atoi(conf->value) > 0)
				{
					Recovery_Port_Number = atoi(conf->value);
				}
				else
				{
					Recovery_Port_Number =DEFAULT_PGRP_RECOVERY_PORT;
				}
				conf = (ConfDataType*)conf->next;
				continue;
			}
			else if (!STRCMP(conf->key,LIFECHECK_PORT_TAG))
			{
				if (atoi(conf->value) > 0)
				{
					LifeCheck_Port_Number = atoi(conf->value);
				}
				else
				{
					LifeCheck_Port_Number = DEFAULT_PGRP_LIFECHECK_PORT;
				}
				conf = (ConfDataType*)conf->next;
				continue;
			}
			else if (!STRCMP(conf->key,RLOG_PORT_TAG))
			{
				if (atoi(conf->value) > 0)
				{
					Replicateion_Log->RLog_Port_Number = atoi(conf->value);
				}
				else
				{
					Replicateion_Log->RLog_Port_Number = DEFAULT_PGRP_RLOG_PORT;
				}
				conf = (ConfDataType*)conf->next;
				continue;
			}
			/* get response mode */
			else if (!STRCMP(conf->key,RESPONSE_MODE_TAG))
			{
				if (!STRCMP(conf->value,RESPONSE_MODE_RELIABLE))
				{
					PGR_Response_Inf->response_mode = PGR_RELIABLE_MODE;
				}
				else if (!STRCMP(conf->value,RESPONSE_MODE_FAST))
				{
					PGR_Response_Inf->response_mode = PGR_FAST_MODE;
				}
				else
				{
					PGR_Response_Inf->response_mode = PGR_NORMAL_MODE;
				}
				conf = (ConfDataType*)conf->next;
				continue;
			}
			/* get replication log use or not */
			else if (!STRCMP(conf->key,USE_REPLICATION_LOG_TAG))
			{
				if (!STRCMP(conf->value,"yes"))
				{
					PGR_Use_Replication_Log = true;
				}
				conf = (ConfDataType*)conf->next;
				continue;
			}
			/* get replication timeout */
			else if (!STRCMP(conf->key,TIMEOUT_TAG))
			{
				/* get repliaction timeout */
				PGR_Replication_Timeout = PGRget_time_value(conf->value);
				if ((PGR_Replication_Timeout < 1) || (PGR_Replication_Timeout > 3600))
				{
					fprintf(stderr,"%s is out of range. It should be between 1sec-1hr.\n",TIMEOUT_TAG);
					return STATUS_ERROR;
				}
				conf = (ConfDataType*)conf->next;
				continue;
			}
			else if (!STRCMP(conf->key,LIFECHECK_TIMEOUT_TAG))
			{
				/* get lifecheck timeout */
				PGR_Lifecheck_Timeout = PGRget_time_value(conf->value);
				if ((PGR_Lifecheck_Timeout < 1) || (PGR_Lifecheck_Timeout > 3600))
				{
					show_error("%s is out of range. It should be between 1sec-1hr.\n",LIFECHECK_TIMEOUT_TAG);
					return STATUS_ERROR;
				}
				conf = (ConfDataType*)conf->next;
				continue;
			}
			else if (!STRCMP(conf->key,LIFECHECK_INTERVAL_TAG))
			{
				/* get lifecheck interval */
				PGR_Lifecheck_Interval = PGRget_time_value(conf->value);
				if ((PGR_Lifecheck_Interval < 1) || (PGR_Lifecheck_Interval > 3600))
				{
					show_error("%s is out of range. It should between 1sec-1hr.\n",LIFECHECK_INTERVAL_TAG);
					return STATUS_ERROR;
				}
				conf = (ConfDataType*)conf->next;
				continue;
			}
		}
		conf = (ConfDataType*)conf->next;
	}

	/* create cluster db server table */
	Host_Tbl_Begin = (HostTbl *)NULL;

	size = sizeof(HostTbl) * MAX_DB_SERVER;
	HostTblShmid = shmget(IPC_PRIVATE,size,IPC_CREAT | IPC_EXCL | 0600);
	if (HostTblShmid < 0)
	{
		show_error("%s:shmget() failed. reason: %s", func,strerror(errno));
		return STATUS_ERROR;
	}
#ifdef PRINT_DEBUG
	show_debug("%s:HostTbl shmget ok",func);
#endif			
	Host_Tbl_Begin = (HostTbl *)shmat(HostTblShmid,0,0);
	if (Host_Tbl_Begin == (HostTbl *)-1)
	{
		show_error("%s:shmat() failed. reason: %s", func, strerror(errno));
		return STATUS_ERROR;
	}
#ifdef PRINT_DEBUG
	show_debug("%s:HostTbl shmat ok",func);
#endif			
	memset(Host_Tbl_Begin , 0 , size );
	Host_Tbl_Begin -> useFlag = DB_TBL_END;

	for ( i = 0 ; i <= cnt ; i ++)
	{
		PGRadd_HostTbl(&host_tbl[i],DB_TBL_INIT);
	}
	/* set load balance table */
	for ( i = 0 ; i <= lb_cnt ; i ++)
	{
		(LoadBalanceTbl + i)->port = -1;
		(LoadBalanceTbl + i)->sock = -1;
	}
	memset((LoadBalanceTbl + i),0,sizeof(RecoveryTbl));
	PGR_Free_Conf_Data();

	/* allocate result buffer of query */
	PGR_Result = malloc(PGR_MESSAGE_BUFSIZE);
	if (PGR_Result == NULL)
	{
		show_error("%s:malloc() failed. reason: %s", func, strerror(errno));
		return STATUS_ERROR;
	}
	memset(PGR_Result,0,PGR_MESSAGE_BUFSIZE);

	/* allocate log_data */
	PGR_Log_Header = malloc(sizeof(ReplicateHeader));
	if (PGR_Log_Header == NULL)
	{
		show_error("%s:malloc() failed. reason: %s", func, strerror(errno));
		return STATUS_ERROR;
	}
	memset(PGR_Log_Header,0,sizeof(ReplicateHeader));

	/* allocate send query id */
	size = sizeof(unsigned int) * (MAX_DB_SERVER +1);
	PGR_Send_Query_ID = malloc (size);
	if (PGR_Send_Query_ID == NULL)
	{
		show_error("%s:malloc() failed. reason: %s", func, strerror(errno));
		return STATUS_ERROR;
	}
	memset(PGR_Send_Query_ID, 0, size);
	for ( i = 0 ; i < MAX_DB_SERVER ; i ++)
	{
		StartReplication[i] = true;
	}

	/* set self data into cascade table */

	cascade_rec_no ++;
	if (ResolvedName != NULL)
	{
		strncpy((Cascade_Tbl+cascade_rec_no)->hostName,ResolvedName,ADDRESS_LENGTH);
	}
	else
	{

		gethostname((Cascade_Tbl+cascade_rec_no)->hostName,sizeof(Cascade_Tbl->hostName));
	}
	(Cascade_Tbl+cascade_rec_no)->portNumber = Port_Number;
	(Cascade_Tbl+cascade_rec_no)->recoveryPortNumber = Recovery_Port_Number;
	(Cascade_Tbl+cascade_rec_no)->sock = -1;

	PGRset_cascade_server_status(Cascade_Tbl+cascade_rec_no,DB_TBL_USE);
	/* terminate */
	(Cascade_Tbl+(cascade_rec_no+1))->useFlag = DB_TBL_END;

	Cascade_Inf->top = Cascade_Tbl;
	Cascade_Inf->end = Cascade_Tbl+cascade_rec_no;
      	Cascade_Inf->upper = NULL;
	Cascade_Inf->lower = NULL;
	if (cascade_rec_no >= 1)
	{
		Cascade_Inf->upper = (Cascade_Tbl+cascade_rec_no - 1);
	}
	(Cascade_Tbl+(cascade_rec_no+1))->useFlag = DB_TBL_END;

	Cascade_Inf->myself = (Cascade_Tbl+cascade_rec_no);
	Cascade_Inf->useFlag = DB_TBL_USE;

	PGR_Response_Inf->response_mode = PGR_NORMAL_MODE;

	return STATUS_OK;
}

