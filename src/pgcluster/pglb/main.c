/*--------------------------------------------------------------------
 * FILE:
 *		 main.c
 *
 * NOTE:
 *		 This file is composed of the main function of pglb.
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
 * suitability of this software for any purpose.	It is provided "as
 * is" without express or implied warranty.
 *
*/
#include "postgres.h"
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/param.h>
#include <sys/select.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/file.h>
#include <arpa/inet.h>

#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include "replicate_com.h"
#include "pglb.h"



#define IPC_NMAXSEM (32)
/*--------------------------------------
 * GLOBAL VARIABLE DECLARATION
 *--------------------------------------
 */
/* for replicate_com.h */
ConfDataType * ConfData_Top = (ConfDataType *)NULL;
ConfDataType * ConfData_End = (ConfDataType *)NULL;
int MapTableShmid = -1;
int LifeCheckStartShmid = -1;
char * LifeCheckStartFlag = NULL;
int LifeCheckTimeOut = 10;
FILE * StatusFp = (FILE *)NULL;
char * PGRStatusFileName = NULL;
char * PGRLogFileName = NULL;
char * PGRuserName = NULL;
int Log_Print = 0;
int Debug_Print = 0;

char * ResolvedName = NULL;
int Recv_Port_Number = 0;
int Recovery_Port_Number = 0;
uint16_t LifeCheck_Port_Number = 0;
int Use_Connection_Pool = 0;
int Max_Pool = 1;
int Connection_Life_Time = 0;
int Max_DB_Server = 0;
int MaxBackends = 0;
ClusterTbl * Cluster_Tbl = (ClusterTbl *)NULL;
int ClusterNum = 0;
int ClusterShmid = 0;
int ClusterSemid = 0;
ChildTbl * Child_Tbl = (ChildTbl *)NULL;
int ChildShmid = 0;
char * PGR_Data_Path = NULL;
char * PGR_Write_Path = NULL;
char * Backend_Socket_Dir = NULL;
FrontSocket Frontend_FD;
ClusterTbl * CurrentCluster = NULL;
int PGR_Lifecheck_Timeout = 3;
int PGR_Lifecheck_Interval = 15;

int fork_wait_time = 0;

extern char *optarg;

/*--------------------------------------
 * PROTOTYPE DECLARATION
 *--------------------------------------
 */
static int init_pglb(char * path);
static void pglb_exit(int signal_args);
static void load_balance_main(void);
static void daemonize(void);
static void write_pid_file(void);
static int is_exist_pid_file(void);
static ClusterTbl * scan_cluster_by_pid(pid_t pid);
static void usage(void);
static void close_child(int signal_args);

void PGRrecreate_child(int signal_args);
void PGRexit_subprocess(int sig);

/*--------------------------------------------------------------------
 * SYMBOL
 *		init_pglb()
 * NOTES
 *		Reading of the setup file
 *		and the initialization of the memory area.
 * ARGS
 *		char * path: path of the setup file (I)
 * RETURN
 *		OK: STATUS_OK
 *		NG: STATUS_ERROR
 *--------------------------------------------------------------------
 */
static int
init_pglb(char * path)
{
	char * func = "init_pglb()";

	ConfDataType * conf;
	ClusterTbl cluster_tbl[MAX_DB_SERVER];
	int size = 0;
	int rec_no = 0;
	int i;
	int max_connect = 0;
	union semun sem_arg;
	char fname[256];

	/*
	 * read configuration file
	 */
	if (path == NULL)
	{
		path = ".";
	}
	if (PGR_Get_Conf_Data(path,PGLB_CONF_FILE) != STATUS_OK)
	{
		show_error("%s:PGR_Get_Conf_Data failed",func);
		return STATUS_ERROR;
	}
	
	size = sizeof(LogFileInf);
	LogFileData = (LogFileInf *) malloc(size);
	if (LogFileData == NULL)
	{
		show_error("%s:malloc() failed. reason: %s", func,strerror(errno));
		return STATUS_ERROR;
	}
	memset(LogFileData,0,size);

	/* cluster db status file open */
	if (PGRStatusFileName == NULL)
	{
		snprintf(fname,sizeof(fname),"%s/%s",PGR_Write_Path,PGLB_STATUS_FILE);
	}
	else
	{
		memcpy(fname,PGRStatusFileName,sizeof(fname));
	}
	StatusFp = fopen(fname, "a");
	if (StatusFp == NULL)
	{
		show_error("%s:open() %s file failed. (%s)",
					 func,fname, strerror(errno));
		exit(1);
	}

	Backend_Socket_Dir = malloc(128);
	if (Backend_Socket_Dir == NULL)
	{
		show_error("%s:malloc() failed. (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
	memset(Backend_Socket_Dir,0,128);
	/* set initiarize data */
	strcpy(Backend_Socket_Dir,"/tmp");
	Max_Pool = 1;
	Connection_Life_Time = 0;
	Use_Connection_Pool = 0;

	conf = ConfData_Top;
	while (conf != (ConfDataType *)NULL) 
	{
		/* get cluster db servers name */
		if (!strcmp(conf->table,CLUSTER_SERVER_TAG))
		{
			rec_no = conf->rec_no;
			if (!strcmp(conf->key,HOST_NAME_TAG))
			{
				memcpy(cluster_tbl[rec_no].hostName,conf->value,sizeof(cluster_tbl[rec_no].hostName));
				conf = (ConfDataType*)conf->next;
				continue;
			}
			if (!strcmp(conf->key,PORT_TAG))
			{
				cluster_tbl[rec_no].port = atoi(conf->value);
				conf = (ConfDataType*)conf->next;
				continue;
			}
			if (!strcmp(conf->key,MAX_CONNECT_TAG))
			{
				cluster_tbl[rec_no].max_connect = atoi(conf->value);
				conf = (ConfDataType*)conf->next;
				continue;
			}
		}
		/* get logging file data */
		else if (!strcmp(conf->table, LOG_INFO_TAG))
		{
			if (!strcmp(conf->key, FILE_NAME_TAG))
			{
				strncpy(LogFileData->file_name, conf->value ,sizeof(LogFileData->file_name));
				LogFileData->fp = NULL;
				conf = (ConfDataType*)conf->next;
				continue;
			}
			if (!strcmp(conf->key, FILE_SIZE_TAG))
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
			if (!strcmp(conf->key, LOG_ROTATION_TAG))
			{
				LogFileData->rotation = atoi(conf->value);
				conf = (ConfDataType*)conf->next;
				continue;
			}
		}
		else
		{
			if (!strcmp(conf->key,HOST_NAME_TAG))
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
					 (ip			) & 0xff ,
					 (ip >>	8) & 0xff ,
					 (ip >> 16) & 0xff ,
					 (ip >> 24) & 0xff );
				conf = (ConfDataType*)conf->next;
				continue;
			}
			/* get port number for receive querys */
			else if (!strcmp(conf->key,RECV_PORT_TAG))
			{
				Recv_Port_Number = atoi(conf->value);
				conf = (ConfDataType*)conf->next;
				continue;
			}
			/* get port number for recovery session */
			else if (!strcmp(conf->key,RECOVERY_PORT_TAG))
			{
				Recovery_Port_Number = atoi(conf->value);
				conf = (ConfDataType*)conf->next;
				continue;
			}
			else if (!strcmp(conf->key,MAX_CLUSTER_TAG))
			{
				Max_DB_Server = atoi(conf->value);
				conf = (ConfDataType*)conf->next;
				continue;
			}
			else if (!strcmp(conf->key,USE_CONNECTION_POOL_TAG))
			{
				if (!strcmp(conf->value,"yes"))
				{
					Use_Connection_Pool = 1;
				}
				conf = (ConfDataType*)conf->next;
				continue;
			}
			else if (!strcmp(conf->key,MAX_POOL_TAG))
			{
				Max_Pool = atoi(conf->value);
				if (Max_Pool < 0)
					Max_Pool = 1;
				conf = (ConfDataType*)conf->next;
				continue;
			}
			else if (!strcmp(conf->key,CONNECTION_LIFE_TIME))
			{
				Connection_Life_Time = PGRget_time_value(conf->value);
				if (Connection_Life_Time < 0)
					Connection_Life_Time = 0;
				conf = (ConfDataType*)conf->next;
				continue;
			}
			else if (!strcmp(conf->key,BACKEND_SOCKET_DIR_TAG))
			{
				strncpy(Backend_Socket_Dir,conf->value,128);
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
	if (Max_DB_Server <= 0)
	{
		show_error("%s:Max_DB_Server is wrong value. %s/%s file should be broken",func, path, PGLB_CONF_FILE);
		exit(1);
	}
	/* shared memory allocation for cluster table */
	size = sizeof(ClusterTbl) * Max_DB_Server;

	ClusterShmid = shmget(IPC_PRIVATE,size,IPC_CREAT | IPC_EXCL | 0600);
	if (ClusterShmid < 0)
	{
		show_error("%s:ClusterShm shmget() failed. (%s)", func,strerror(errno));
		return STATUS_ERROR;
	}
	Cluster_Tbl = (ClusterTbl *)shmat(ClusterShmid,0,0);
	if (Cluster_Tbl == (ClusterTbl *)-1)
	{
		show_error("%s:shmat() failed. (%s)", func,strerror(errno));
		return STATUS_ERROR;
	}
	memset(Cluster_Tbl,0,size);

	if ((ClusterSemid = semget(IPC_PRIVATE,MAX_DB_SERVER+1,IPC_CREAT | IPC_EXCL | 0600)) < 0)
	{
		show_error("%s:semget() failed. (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
	for ( i = 0 ; i <= MAX_DB_SERVER ; i ++)
	{
		semctl(ClusterSemid, i, GETVAL, sem_arg);
		sem_arg.val = 1;
		semctl(ClusterSemid, i, SETVAL, sem_arg);
	}
	ClusterNum = 0;
	/* set cluster db server name into cluster db server table */
	for ( i = 0 ; i < Max_DB_Server ; i ++)
	{
		(Cluster_Tbl + i)->rec_no = i;
	}
	(Cluster_Tbl + i)->useFlag = TBL_END;
	max_connect = 0;
	for ( i = 0 ; i <= rec_no ; i ++)
	{
		cluster_tbl[i].use_num = 0;
		cluster_tbl[i].rate = 0;
		if (cluster_tbl[i].max_connect < 0)
		{
			cluster_tbl[i].max_connect = 0;
		}
		if (max_connect < cluster_tbl[i].max_connect)
		{
			max_connect = cluster_tbl[i].max_connect;
		}
		PGRadd_cluster_tbl(&cluster_tbl[i]);
	}

	/* shared memory allocation for children table */
	size = sizeof(ChildTbl) * (Max_DB_Server + 1) * max_connect * Max_Pool;
#ifdef PRINT_DEBUG
	show_debug("%s:Child_Tbl size is[%d]",func,size);
#endif			

	ChildShmid = shmget(IPC_PRIVATE,size,IPC_CREAT | IPC_EXCL | 0600);
	if (ChildShmid < 0)
	{
		show_error("%s:ChildShm shmget() failed. (%s)",func, strerror(errno));
		return STATUS_ERROR;
	}
	Child_Tbl = (ChildTbl *)shmat(ChildShmid,0,0);
	if (Child_Tbl == (ChildTbl *)-1)
	{
		show_error("%s:shmat() failed. (%s)", func,strerror(errno));
		return STATUS_ERROR;
	}
	memset(Child_Tbl, 0, size);
	(Child_Tbl + ( Max_DB_Server * max_connect * Max_Pool) -1)->useFlag = TBL_END;

	PGR_Free_Conf_Data();

	return STATUS_OK;
}

/*--------------------------------------------------------------------
 * SYMBOL
 *		pglb_exit()
 * NOTES
 *		Closing of pglb process
 * ARGS
 *		int signal_args: signal number (I)
 * RETURN
 *		none
 *--------------------------------------------------------------------
 */
static void
pglb_exit(int signal_args)
{
	char fname[256];
	int rtn;
	
	Child_Tbl->useFlag = TBL_END;
	PGRsignal(SIGCHLD,SIG_IGN);
	PGRsignal(signal_args,SIG_IGN);
	kill (0,signal_args);
	while (wait(NULL) > 0 )
		;

	if (ClusterShmid > 0)
	{
		rtn = shmdt((char *)Cluster_Tbl);
		shmctl(ClusterShmid,IPC_RMID,(struct shmid_ds *)NULL);
		ClusterShmid = 0;
		Cluster_Tbl = NULL;
	}
	if (ChildShmid > 0)
	{
		rtn = shmdt((char *)Child_Tbl);
		shmctl(ChildShmid,IPC_RMID,(struct shmid_ds *)NULL);
		ChildShmid = 0;
		Child_Tbl = NULL;
	}
	if (ClusterSemid > 0)
	{
		semctl(ClusterSemid, 0, IPC_RMID);
		ClusterSemid = 0;
	}
	
	if (StatusFp != NULL)
	{
		fflush(StatusFp);
		fclose(StatusFp);
	}
	if (Frontend_FD.unix_fd != 0)
	{
		close(Frontend_FD.unix_fd);
		Frontend_FD.unix_fd = 0;
		snprintf(fname, sizeof(fname), "%s/.s.PGSQL.%d", Backend_Socket_Dir,Recv_Port_Number);
		unlink(fname);
	}
	if (Frontend_FD.inet_fd != 0)
	{
		close(Frontend_FD.inet_fd);
		Frontend_FD.inet_fd = 0;
	}
	/*
	PGRsyn_quit();
	*/
	snprintf(fname, sizeof(fname), "%s/%s", PGR_Write_Path, PGLB_PID_FILE);
	unlink(fname);

	if (ResolvedName != NULL)
	{
		free(ResolvedName);
		ResolvedName = NULL;
	}
	exit(0);
}

/*--------------------------------------------------------------------
 * SYMBOL
 *		load_balance_main()
 * NOTES
 *		This is a main module of load balance function
 * ARGS
 *		void
 * RETURN
 *		none
 *--------------------------------------------------------------------
 */
static void
load_balance_main(void)
{
	char * func = "load_balance_main()";
	int status;
	int rtn;
	int count = 0;

	Frontend_FD.unix_fd = PGRcreate_unix_domain_socket(Backend_Socket_Dir, Recv_Port_Number);
	if (Frontend_FD.unix_fd < 0)
	{
		show_error("%s:PGRcreate_unix_domain_socket failed",func);
		pglb_exit(SIGTERM);
	}
	Frontend_FD.inet_fd = PGRcreate_recv_socket(ResolvedName, Recv_Port_Number);
	if (Frontend_FD.inet_fd < 0)
	{
		show_error("%s:PGRcreate_recv_socket failed",func);
		pglb_exit(SIGTERM);
	}
	if (Use_Connection_Pool)
	{
		PGRsignal(SIGCHLD,PGRrecreate_child);
		rtn = PGRpre_fork_children(Cluster_Tbl);
		if (rtn != STATUS_OK)
		{
			show_error("%s:PGRpre_fork_children failed",func);
			pglb_exit(SIGTERM);
		}
	}
	
	for (;;)
	{
		fd_set		rmask;
		struct timeval timeout;

		timeout.tv_sec = 60;
		timeout.tv_usec = 0;

		/*
		 * Wait for something to happen.
		 */
		FD_ZERO(&rmask);
		FD_SET(Frontend_FD.unix_fd,&rmask);
		if(Frontend_FD.inet_fd)
			FD_SET(Frontend_FD.inet_fd,&rmask);
		rtn = select(Max(Frontend_FD.unix_fd, Frontend_FD.inet_fd) + 1, &rmask, (fd_set *)NULL, (fd_set *)NULL, &timeout);
		if( rtn > 0)
		{
			if(PGRis_cluster_alive() == STATUS_ERROR) {
				show_error("%s:all clusters were dead.",func);
				PGRreturn_no_connection_error();				
				count=0;
			}
			else 
			{
				if (Use_Connection_Pool)
				{
					status = PGRload_balance_with_pool();
				}
				else
				{
					status = PGRload_balance();
				}
				if (status != STATUS_OK)
				{
					show_error("%s:load balance process failed",func);
					if ( count > PGLB_CONNECT_RETRY_TIME)
					{
						show_error("%s:no cluster available",func);
						PGRreturn_connection_full_error();
						count = 0;
					}
					count ++;
				}
				else
				{
					count = 0;
				}
			}
		}
	}
}

/*--------------------------------------------------------------------
 * SYMBOL
 *		daemonize()
 * NOTES
 *		Daemonize this process
 * ARGS
 *		void
 * RETURN
 *		none
 *--------------------------------------------------------------------
 */
static void 
daemonize(void)
{
	char *	func = "daemonize()";
	int		i;
	pid_t		pid;

	pid = fork();
	if (pid == (pid_t) -1)
	{
		show_error("%s:fork() failed. (%s)",func, strerror(errno));
		exit(1);
		return;					/* not reached */
	}
	else if (pid > 0)
	{			/* parent */
		exit(0);
	}

#ifdef HAVE_SETSID
	if (setsid() < 0)
	{
		show_error("%s:setsid() failed. (%s)", func,strerror(errno));
		exit(1);
	}
#endif

	i = open("/dev/null", O_RDWR);
	dup2(i, 0);
	dup2(i, 1);
	dup2(i, 2);
	close(i);
}


/*--------------------------------------------------------------------
 * SYMBOL
 *		write_pid_file()
 * NOTES
 *		The process ID is written in the file.
 *		This process ID is used when finish pglb.
 * ARGS
 *		void
 * RETURN
 *		none
 *--------------------------------------------------------------------
 */
static void 
write_pid_file(void)
{
	char * func = "write_pid_file()";
	FILE *fd;
	char fname[256];
	char pidbuf[128];

	snprintf(fname, sizeof(fname), "%s/%s", PGR_Write_Path, PGLB_PID_FILE);
	fd = fopen(fname, "w");
	if (!fd)
	{
		show_error("%s:open() %s file failed. (%s)",
					 func,fname, strerror(errno));
		exit(1);
	}
	snprintf(pidbuf, sizeof(pidbuf), "%d", getpid());
	fwrite(pidbuf, strlen(pidbuf), 1, fd);
	if (fclose(fd))
	{
		show_error("%s:fwrite() %s file failed. (%s)",
					 func,fname, strerror(errno));
		exit(1);
	}
}


/*--------------------------------------------------------------------
 * SYMBOL
 *		PGRsend_signal()
 * NOTES
 *		Stop the pglb process
 * ARGS
 *		void
 * RETURN
 *		none
 *--------------------------------------------------------------------
 */
void 
PGRsend_signal(int sig)
{
	char * func = "PGRsend_signal()";
	FILE *fd;
	char fname[256];
	char pidbuf[128];
	pid_t pid;

	if (PGR_Write_Path == NULL)
	{
		PGR_Write_Path = ".";
	}
	snprintf(fname, sizeof(fname), "%s/%s", PGR_Write_Path, PGLB_PID_FILE);
	fd = fopen(fname, "r");
	if (!fd)
	{
		show_error("%s:open() %s file failed. (%s)",
					 func,fname, strerror(errno));
		exit(1);
	}
	memset(pidbuf,0,sizeof(pidbuf));
	fread(pidbuf, sizeof(pidbuf), 1, fd);
	fclose(fd);
	pid = atoi(pidbuf);
	if (kill (pid,sig) == -1)
	{
		show_error("%s:could not send signal: %d (%s)",func,sig,strerror(errno));
		exit(1);
	}
}

/*--------------------------------------------------------------------
 * SYMBOL
 *		is_exist_pid_file()
 * NOTES
 *		Check existence of pid file.
 * ARGS
 *		void
 * RETURN
 *		1: the pid file is exist
 *		0: the pid file is not exist
 *--------------------------------------------------------------------
 */
static int
is_exist_pid_file(void)
{
	char fname[256];
	struct stat buf;

	snprintf(fname, sizeof(fname), "%s/%s", PGR_Write_Path, PGLB_PID_FILE);
	if (stat(fname,&buf) == 0)
	{
		/* pid file is exist */
		return 1;
	}
	else
	{
		/* pid file is not exist */
		return 0;
	}
}


/*--------------------------------------------------------------------
 * SYMBOL
 *		PGRrecreate_child()
 * NOTES
 *		create the child process again which it hunged up
 * ARGS
 *		int signal_args: signal number (expecting the SIGCHLD)
 * RETURN
 *		none
 *--------------------------------------------------------------------
 */
void
PGRrecreate_child(int signal_args)
{
	pid_t pid = 0;
	int status;
	ClusterTbl * cluster_p;

ReWait:

	errno = 0;
#ifdef HAVE_WAITPID
	while ((pid = waitpid(-1, &status, WNOHANG)) > 0)
	{
#else
	while ((pid = wait3(&status, WNOHANG, NULL)) > 0)
	{
#endif
		cluster_p = scan_cluster_by_pid(pid);
		pid = PGRcreate_child(cluster_p);	
	}
	if ((pid < 0) && (errno == EINTR))
		goto ReWait;
}

/*--------------------------------------------------------------------
 * SYMBOL
 *		close_child()
 * NOTES
 *		Hung up child process 
 * ARGS
 *		int signal_args: signal number (expecting the SIGUSR2)
 * RETURN
 *		none
 *--------------------------------------------------------------------
 */
static void
close_child(int signal_args)
{
	char * func = "close_child()";
	ClusterTbl * cluster;
	int rec_no = -1;

	if (( Cluster_Tbl == NULL) || (Child_Tbl == NULL))
	{
		show_error("%s:Cluster_Tbl or Child_Tbl is not initialize",func);
		return ;
	}
	cluster = Cluster_Tbl;
	while(cluster->useFlag != TBL_END)
	{
		if (cluster->useFlag == TBL_ERROR_NOTICE) 
		{
			rec_no = cluster->rec_no;
			if (rec_no < 0)
			{
				cluster++;
				continue;
			}
			PGRquit_children_on_cluster(rec_no);
			PGRset_status_on_cluster_tbl(TBL_ERROR,cluster);
		}
		cluster++;
	}
	PGRsignal(SIGUSR2, close_child);
}

/*--------------------------------------------------------------------
 * SYMBOL
 *		scan_cluster_by_pid()
 * NOTES
 *		get cluster server record from child process id
 * ARGS
 *		pid_t pid: child process id (I)
 * RETURN
 *		OK: pointer of cluster table
 *		NG: NULL
 *--------------------------------------------------------------------
 */
static ClusterTbl *
scan_cluster_by_pid(pid_t pid)
{
	char * func = "scan_cluster_by_pid()";
	ChildTbl * child_p;
	ClusterTbl * cluster_p;
	int cnt;

	child_p = Child_Tbl;
	if (child_p == NULL)
	{
		show_error("%s:Child Table is not initialize",func);
		return NULL;
	}
	cluster_p = Cluster_Tbl;
	if (cluster_p == NULL)
	{
		show_error("%s:Cluster Table is not initialize",func);
		return NULL;
	}
	
	while (child_p->useFlag != TBL_END)
	{
		if (child_p->pid == pid)
		{
			break;
		}
		child_p++;
	}
	if (child_p->useFlag == TBL_END)
	{
		show_error("%s:pid:%d not found in child table",func,pid);
		return NULL;
	}

	cnt = 0;
	while ((cluster_p->useFlag != TBL_END) && (cnt < ClusterNum))
	{
		if (cluster_p->rec_no == child_p->rec_no)
		{
			return cluster_p;
		}
		cluster_p++;
		cnt ++;
	}
	return NULL;
}

/*--------------------------------------------------------------------
 * SYMBOL
 *		usage()
 * NOTES
 *		show usage of pglb
 * ARGS
 *		void
 * RETURN
 *		none
 *--------------------------------------------------------------------
 */
static void
usage(void)
{
	char * path;

	path = getenv("PGDATA");
	if (path == NULL)
		path = ".";
	fprintf(stderr,"pglb version [%s]\n",PGLB_VERSION);
	fprintf(stderr,"A load balancer for PostgreSQL\n\n");
	fprintf(stderr,"usage: pglb [-D path_of_config_file] [-W path_of_work_files] [-n][-v][-h][stop | restart]\n");
	fprintf(stderr,"		config file default path: %s/%s\n",path, PGLB_CONF_FILE);
	fprintf(stderr,"		-l: print error logs in the log file.\n");
	fprintf(stderr,"		-n: don't run in daemon mode.\n");
	fprintf(stderr,"		-v: debug mode. need '-n' flag\n");
	fprintf(stderr,"		-h: print this help\n");
	fprintf(stderr,"		stop: stop pglb\n");
	fprintf(stderr,"		restart: restart pglb\n");
}

/*--------------------------------------------------------------------
 * SYMBOL
 *		main()
 * NOTES
 *		main module of pglb
 * ARGS
 *		int argc: number of parameter
 *		char ** argv: value of parameter
 * RETURN
 *		none
 *--------------------------------------------------------------------
 */
int
main(int argc, char ** argv)
{
	int opt = 0;
	char * r_path = NULL;
	char * w_path = NULL;
	int detach = 1;

	PGRsignal(SIGHUP, pglb_exit);
	PGRsignal(SIGINT, pglb_exit);	
	PGRsignal(SIGQUIT, pglb_exit);
	PGRsignal(SIGTERM, pglb_exit);
	PGRsignal(SIGALRM, SIG_IGN); /* ignored */
	PGRsignal(SIGPIPE, SIG_IGN); /* ignored */
	PGRsignal(SIGTTIN, SIG_IGN); /* ignored */
	PGRsignal(SIGTTOU, SIG_IGN); /* ignored */
	PGRsignal(SIGCHLD,PGRchild_wait);
	PGRsignal(SIGUSR1, SIG_IGN); /* ignored */
	PGRsignal(SIGUSR2, close_child); /* close child process */
	r_path = getenv("PGDATA");
	if (r_path == NULL)
		r_path = ".";

	while ((opt = getopt(argc, argv, "U:D:W:w:lvnh")) != -1)
	{
		switch (opt)
		{
			case 'U':
				if (!optarg)
				{
					usage();
					exit(1);
				}
				PGRuserName = strdup(optarg);
				break;
			case 'D':
				if (!optarg)
				{
					usage();
					exit(1);
				}
				r_path = optarg;
				break;
			case 'W':
				if (!optarg)
				{
					usage();
					exit(1);
				}
				w_path = optarg;
				break;
			case 'w':
				fork_wait_time = atoi(optarg);
				if (fork_wait_time < 0)
					fork_wait_time = 0;
				break;
			case 'l':
				Log_Print = 1;
				break;
			case 'v':
				Debug_Print = 1;
				break;
			case 'n':
				detach = 0;
				break;
			case 'h':
				usage();
				exit(0);
				break;
			default:
				usage();
				exit(1);
		}
	}
	PGR_Data_Path = r_path;
	if (w_path == NULL)
	{
		PGR_Write_Path = PGR_Data_Path;
	}
	else
	{
		PGR_Write_Path = w_path;
	}

	if (optind == (argc-1) &&
			((!strcmp(argv[optind],"stop")) ||
			(!strcmp(argv[optind],"restart"))))
	{
		PGRsend_signal(SIGTERM);
		if (!strcmp(argv[optind],"stop"))
		{
			exit(0);
		}
	}
	else if (optind == argc)
	{
		if (is_exist_pid_file())
		{
			fprintf(stderr,"pid file %s/%s found. is another pglb running?", PGR_Write_Path, PGLB_PID_FILE);
			exit(1);
		}
	}
	else if (optind < argc)
	{
		usage();
		exit(1);
	}

	PGRinitmask();

	if (detach)
	{
		daemonize();
	}
	write_pid_file();
	
	if (init_pglb(PGR_Data_Path) != STATUS_OK)
	{
		exit(0);
	}

	/* call recovery process */
	PGRrecovery_main(fork_wait_time);

	/* call lifecheck process */
	PGRlifecheck_main(fork_wait_time);

	/* start loadbalance module */
	load_balance_main();
	pglb_exit(0);
	return STATUS_OK;
}

void
PGRexit_subprocess(int sig)
{
	pglb_exit(sig);
}
