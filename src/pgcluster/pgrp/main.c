/*--------------------------------------------------------------------
 * FILE:
 *    main.c
 *    Replication server for PostgreSQL
 *
 * NOTE:
 *    This is the main module of the replication server.
 *
 * Portions Copyright (c) 2003-2008, Atsushi Mitani
 *--------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <signal.h>
#include <sys/wait.h>
#include <ctype.h>
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
#include <arpa/inet.h>
#include <sys/file.h>
#include <pthread.h>

#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif

#include "miscadmin.h"
#include "nodes/nodes.h"

#include "libpq-fe.h"
#include "libpq/libpq-fs.h"
#include "libpq-int.h"
#include "fe-auth.h"


#include "access/xact.h"
#include "replicate_com.h"
#include "pgreplicate.h"

#ifdef WIN32
#include "win32.h"
#endif
#include <arpa/inet.h>
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

/*--------------------------------------
 * GLOBAL VARIABLE DECLARATION
 *--------------------------------------
 */
/* for replicate_com.h */

ConfDataType * ConfData_Top = (ConfDataType *)NULL;
ConfDataType * ConfData_End = (ConfDataType *)NULL;

/* replication server data */
char * ResolvedName = NULL;
uint16_t Port_Number = 0;
uint16_t LifeCheck_Port_Number = 0;
uint16_t Recovery_Port_Number = 0;
bool PGR_Parse_Session_Started = false;
int PGR_Replication_Timeout = 60;
int PGR_Lifecheck_Timeout = 3;
int PGR_Lifecheck_Interval = 15;

/* global table data */
HostTbl *Host_Tbl_Begin = NULL;
Dllist * Transaction_Tbl_Begin = NULL;
TransactionTbl * Transaction_Tbl_End = NULL;
RecoveryTbl * LoadBalanceTbl = NULL;
RecoveryStatusInf * Recovery_Status_Inf = NULL;
ReplicateHeader * PGR_Log_Header = NULL;
ReplicateServerInfo * Cascade_Tbl = NULL;;
CommitLogInf * Commit_Log_Tbl = NULL;
QueryLogType * Query_Log_Top = NULL;
QueryLogType * Query_Log_End = NULL;
CascadeInf * Cascade_Inf = NULL;
ReplicationLogInf * Replicateion_Log = NULL;
/* IPC's id data */
int RecoveryShmid = 0;
int ReplicateSerializationShmid=0;
int RecoveryMsgShmid = 0;
int *RecoveryMsgid = NULL;
int HostTblShmid = 0;
int LockWaitTblShmid = 0;
int LoadBalanceTblShmid = 0;
int CascadeTblShmid = 0;
int CascadeInfShmid = 0;
int CommitLogShmid = 0;
int QueryLogMsgid = 0;
int QueryLogAnsMsgid = 0;
int PGconnMsgid = 0;
int MaxBackends = 0;
char * PGR_Result = NULL;
int SemID = 0;
int RecoverySemID= 0;
int RecovErysemid = 0;
int VacuumSemID = 0;
int CascadeSemID= 0;
char * PGR_Data_Path = NULL;
char * PGR_Write_Path = NULL;
int IS_SESSION_AUTHORIZATION = 0;
ResponseInf * PGR_Response_Inf = NULL; 
bool StartReplication[MAX_DB_SERVER]; 
bool PGR_Cascade = false;
bool PGR_Use_Replication_Log = false;
bool	PGR_AutoCommit = true;
unsigned int * PGR_Send_Query_ID = NULL;
unsigned int PGR_Query_ID = 0;
volatile bool exit_processing = false;
int pgreplicate_pid = 0;

int ReplicateSock = -1;
int exit_signo = SIGTERM;

RecoveryQueueInf RecoveryQueue;
char * Backend_Socket_Dir = NULL;

unsigned int * PGR_ReplicateSerializationID = NULL;

int Log_Print = 0;
int Debug_Print = 0;
FILE * LogFp = (FILE *)NULL;
FILE * StatusFp = (FILE *)NULL;
FILE * RidFp = (FILE *)NULL;
FILE * QueueFp = (FILE *)NULL;

extern char *optarg;
char * PGRuserName = NULL;

int fork_wait_time = 0;
int Idle_Flag = IDLE_MODE;
volatile bool Exit_Request = false;

pthread_mutex_t transaction_table_mutex;
//#ifdef PGGRID
pthread_mutex_t return_value_mutex;
//#endif

/*--------------------------------------
 * PROTOTYPE DECLARATION
 *--------------------------------------
 */
static void startup_replication_server(void);
static int replicate_loop(int fd);
static void replicate_main(void);
//static void quick_exit(SIGNAL_ARGS);
static void daemonize(void);
static void write_pid_file(void);
static void stop_pgreplicate(void);
static bool is_exist_pid_file(void);
static void usage(void);
static void set_exit_processing(int signo);

/*--------------------------------------------------------------------
 * SYMBOL
 *    replicate_loop()
 * NOTES
 *   replication module
 * ARGS
 *    int fd :
 * RETURN
 *    OK: STATUS_OK
 *    NG: STATUS_ERROR
 *--------------------------------------------------------------------
 */
static int
replicate_loop(int fd)
{
	char * func = "replicate_loop()";
	pid_t pgid = 0;
	pid_t pid = 0;
	int sock = -1;
	int rtn = 0;
	int cnt = 0;
	int result;
	bool exist_sys_log=false;
	bool exist_replicate=false;
	bool clear_connection = false;


	result = PGR_Create_Acception(fd,&sock,"",Port_Number);
	if (result == STATUS_ERROR)
	{
		show_error("%s: accept failed (%s)", func, strerror(errno));
		if (sock != -1)
			close(sock);
		return 1;
	}

	pgid = getpgid(0);
	pid = fork();
	if (pid <0)
	{
		show_error("%s:fork failed (%s)",func,strerror(errno));
		PGRreplicate_exit(0);
	}
	if (pid == 0)
	{
		int status = LOOP_CONTINUE;
		bool PGR_Cascade = false;
		ReplicateHeader  header;
		ReplicateHeader  header_save_for_recovering;
		char * query = NULL;

		if (fork_wait_time > 0) {
			sleep(fork_wait_time);
		}

		close(fd);

		PGRsignal(SIGHUP, quick_exit);	
		PGRsignal(SIGINT, quick_exit);	
		PGRsignal(SIGQUIT, quick_exit);	
		PGRsignal(SIGTERM, quick_exit);	
		PGRsignal(SIGALRM, SIG_IGN); 
		PGRsignal(SIGPIPE, SIG_IGN); 
		setpgid(0,pgid);
		
		if (PGRinit_transaction_table() != STATUS_OK)
		{
			show_error("transaction table memory allocate failed");
			PGR_Close_Sock(&sock);
			exit(1);
		}

		pthread_mutex_init(&transaction_table_mutex, NULL);
		pthread_mutex_init(&return_value_mutex, NULL);

		//show_debug ("Chegou no child loop",func);
		/* child loop */
		for (;;)
		{
			fd_set	  rmask;
			struct timeval timeout;

			timeout.tv_sec = PGR_Replication_Timeout;
			timeout.tv_usec = 0;
			
			if (query != NULL)
			{
				free(query);
				query = NULL;
			}
			/*
			 * Wait for something to happen.
			 */
			//show_debug ("Esperando algo acontecer",func);
			FD_ZERO(&rmask);
			FD_SET(sock,&rmask);
			rtn = select(sock+1, &rmask, (fd_set *)NULL, (fd_set *)NULL, &timeout);
			if (rtn < 0)
			{
				if (errno == EINTR)
					continue;
			}

			if (rtn && FD_ISSET(sock, &rmask))
			{
				//show_debug ("Recebendo pacote",func);
				query = NULL;
				query = PGRread_packet(sock,&header);				
				if ((query == NULL))
				{

					if (exist_sys_log)
					{
						show_error("%s:upper cascade closed? , errno=%d(%s)",func,errno,strerror(errno));
						memset(&header, 0, sizeof(ReplicateHeader));
						header.cmdSys = CMD_SYS_CALL;
						header.cmdSts = CMD_STS_QUERY_SUSPEND;
						header.query_size = htonl(0);
						PGRsend_rlog_to_local(&header, NULL);
						exist_sys_log = false;
					}
					else
					{
						if (exist_replicate)
						{
							show_debug ("Existe replicate",func);
							PGRclear_connections();
							clear_connection = true;
							header_save_for_recovering.cmdSts=CMD_TYPE_OTHER;
							header_save_for_recovering.cmdType=CMD_TYPE_CONNECTION_CLOSE;
							header_save_for_recovering.query_size = htonl(21);
							PGRdo_replicate(sock,&header_save_for_recovering,"PGR_CLOSE_CONNECTION");
						}
						show_debug ("Quit",func);
						PGRsend_notice_quit();
					}
					break;
				}
				cnt = 0;
				show_debug ("Processando header",func);
				switch (header.cmdSys)
				{
				case CMD_SYS_LIFECHECK:
					PGRreturn_result(sock,"1", PGR_NOWAIT_ANSWER);
					break;
				case CMD_SYS_PREREPLICATE:
					if(Cascade_Inf!=NULL ||
						Cascade_Inf->upper == NULL) 
					{
						/* 1 means "I am primary replicate server." */
						PGRreturn_result(sock,"1", PGR_NOWAIT_ANSWER);
					}
					else
					{
						/* 0 means "I am not primary replicate server." */
						PGRreturn_result(sock,"0", PGR_NOWAIT_ANSWER);
					}
					break;
				case CMD_SYS_REPLICATE:
					show_debug ("Identificou replicando",func);
					if (exist_replicate == false)
					{
						exist_replicate=true;
						memcpy(&header_save_for_recovering,
							&header,
							sizeof(ReplicateHeader));
					}
					status = PGRdo_replicate(sock,&header,query);
					break;
				case CMD_SYS_LOG:
					exist_sys_log = true;
					PGRsend_rlog_to_local(&header, query);
					/* set own replicate id by rlog */
					PGRset_replication_id(ntohl(header.replicate_id));
					PGRsend_notice_rlog_done(sock);
					break;
				case  CMD_SYS_CASCADE:
					PGR_Cascade = true;
					PGRcascade_main(sock,&header,query);
					break;
				case  CMD_SYS_CALL:
					if (header.cmdSts == CMD_STS_TRANSACTION_ABORT)
					{
						PGRreconfirm_commit(sock,&header);
					}
					else if (header.cmdSts == CMD_STS_NOTICE)
					{

					}
					else if (header.cmdSts == CMD_STS_RESPONSE)
					{
						if (header.cmdType == CMD_TYPE_FRONTEND_CLOSED)
						{
							PGRsend_notice_rlog_done(sock);
							status = LOOP_END;
						}
					}
					break;
				default:
					show_error("WARNING: unknown Header->cmdSys %c",header.cmdSys);
				}
			}
			if (status == LOOP_END)
			{
				break;
			}
		}

		PGR_Close_Sock(&sock);
		if (query != NULL)
		{
			free(query);
			query = NULL;
		}
		if (!clear_connection)
			PGRclear_connections();
		PGRdestroy_transaction_table();
		pthread_mutex_destroy(&transaction_table_mutex);
		pthread_mutex_destroy(&return_value_mutex);
		exit(0);
	}
	else
	{
		PGR_Close_Sock(&sock);
		return 0;
	}
}

static void
startup_replication_server(void)
{
	ReplicateHeader  header;
	char hostName[HOSTNAME_MAX_LENGTH];
	char userName[USERNAME_MAX_LENGTH];
	char query[256];

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
	memset(&header,0,sizeof(ReplicateHeader));
	memset(query,0,sizeof(query));
	memset(hostName,0,sizeof(hostName));
	memset(userName,0,sizeof(userName));
	if (ResolvedName != NULL)
	{
		strncpy(hostName,ResolvedName,ADDRESS_LENGTH);
	}
	else
	{
		gethostname(hostName,sizeof(hostName)-1);
	}
	strncpy(userName ,PGRuserName,sizeof(userName)-1);
	snprintf(query,sizeof(query)-1,"SELECT %s(%d,'%s',%d,%d)",
			PGR_SYSTEM_COMMAND_FUNC,
			PGR_STARTUP_REPLICATION_SERVER_FUNC_NO,
			hostName,
			Port_Number,
			Recovery_Port_Number);
	header.cmdSts = CMD_STS_NOTICE;
	header.query_id = htonl(PGRget_next_query_id());
	header.query_size = htonl(strlen(query));
	memcpy(header.from_host,hostName,sizeof(header.from_host));
	memcpy(header.userName,userName,sizeof(header.userName));
	strcpy(header.dbName,"template1");
	
	memset(header.sitehostname,0,sizeof(header.sitehostname));
	replicate_packet_send_internal( &header, query,-1,PGRget_recovery_status(),true);
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    replicate_main()
 * NOTES
 *    Replication main module
 * ARGS
 *    void
 * RETURN
 *    none
 *--------------------------------------------------------------------
 */
static void
replicate_main(void)
{
#ifdef PRINT_DEBUG
	char * func = "replicate_main()";
#endif			
	int status;
	int rtn;
	show_debug ("%s:entering replicate_main",func);

	/* cascade start up notice */
	if (Cascade_Inf->upper != NULL)
	{
		show_debug("initialize cascade information");
		PGRstartup_cascade();
	}

	status = PGR_Create_Socket_Bind(&ReplicateSock, ResolvedName, Port_Number);

	if (status != STATUS_OK)
	{
		show_debug("%s %d port bind failed. quit.",func,Port_Number);
		stop_pgreplicate();
		PGRreplicate_exit(0);
	}
#ifdef PRINT_DEBUG
	show_debug("%s %d port bind OK",func,Port_Number);
#endif			
	

	/* replication start up notice */
	startup_replication_server();

	for (;;)
	{
		//show_debug("Entrou no loop",func,Port_Number);
		fd_set	  rmask;
		struct timeval timeout;

		if (exit_processing == true)
			PGRreplicate_exit(0);

		timeout.tv_sec = PGR_Replication_Timeout;
		timeout.tv_usec = 0;


		/*
		 * Wait for something to happen.
		 */
		FD_ZERO(&rmask);
		FD_SET(ReplicateSock,&rmask);
		rtn = select(ReplicateSock+1, &rmask, (fd_set *)NULL, (fd_set *)NULL, &timeout);
		if (rtn < 0)
			continue;

		if (rtn && FD_ISSET(ReplicateSock, &rmask))
		{
			//show_debug("Algo aconteceu",func,Port_Number);
			/*
			 * get recovery status.
			 */
			PGRcheck_recovered_host();

			if (exit_processing == true)
				break;

			/*
			 * call replication module
			 */
			replicate_loop(ReplicateSock);
		}
	}
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    quick_exit()
 * NOTES
 *    Exit child process
 * ARGS
 *    SIGNAL_ARGS: receive signal number(I)
 * RETURN
 *    none
 *--------------------------------------------------------------------
 */
/*static void
quick_exit(SIGNAL_ARGS)
{
#ifdef PRINT_DEBUG
	show_debug("quick_exit:signo = %d", postgres_signal_arg);
#endif
	exit(0);
}*/

/*--------------------------------------------------------------------
 * SYMBOL
 *    daemonize()
 * NOTES
 *    Daemonize this process
 * ARGS
 *    void
 * RETURN
 *    none
 *--------------------------------------------------------------------
 */
static void 
daemonize(void)
{
	char * func = "daemonize()";
	int		i;
	pid_t		pid;

	pid = fork();
	if (pid == (pid_t) -1)
	{
		show_error("%s:fork() failed. reason: %s",func, strerror(errno));
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
		show_error("%s:setsid() failed. reason:%s", func,strerror(errno));
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
 *    write_pid_file()
 * NOTES
 *    The process ID is written in the file.
 *    This process ID is used when finish pglb.
 * ARGS
 *    void
 * RETURN
 *    none
 *--------------------------------------------------------------------
 */
static void 
write_pid_file(void)
{
	char * func = "write_pid_file()";
	FILE *fd;
	char fname[256];
	char pidbuf[128];

	snprintf(fname, sizeof(fname), "%s/%s", PGR_Write_Path, PGREPLICATE_PID_FILE);
	fd = fopen(fname, "w");
	if (!fd)
	{
		show_error("%s:could not open pid file as %s. reason: %s",
				   func, fname, strerror(errno));
		exit(1);
	}
	snprintf(pidbuf, sizeof(pidbuf), "%d", getpid());
	fwrite(pidbuf, strlen(pidbuf), 1, fd);
	if (fclose(fd))
	{
		show_error("%s:could not write pid file as %s. reason: %s",
				   func,fname, strerror(errno));
		exit(1);
	}
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    stop_pgreplicate()
 * NOTES
 *    Stop the pgreplicate process
 * ARGS
 *    void
 * RETURN
 *    none
 *--------------------------------------------------------------------
 */
static void 
stop_pgreplicate(void)
{
	char * func = "stop_pgreplicate()";
	FILE *fd;
	char fname[256];
	char pidbuf[128];
	pid_t pid;

	snprintf(fname, sizeof(fname), "%s/%s", PGR_Write_Path, PGREPLICATE_PID_FILE);
	fd = fopen(fname, "r");
	if (!fd)
	{
		show_error("%s:could not open pid file as %s. reason: %s",
				   func,fname, strerror(errno));
		exit(1);
	}
	memset(pidbuf,0,sizeof(pidbuf));
	fread(pidbuf, sizeof(pidbuf), 1, fd);
	fclose(fd);
	pid = atoi(pidbuf);

	if (kill (pid,SIGTERM) == -1)
	{
		show_error("%s:could not stop pid: %d, reason: %s",func,pid,strerror(errno));
		exit(1);
	}
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    is_exist_pid_file()
 * NOTES
 *    Check existence of pid file.
 * ARGS
 *    void
 * RETURN
 *    1: the pid file is exist
 *    0: the pid file is not exist
 *--------------------------------------------------------------------
 */
static bool
is_exist_pid_file(void)
{
	char fname[256];
	struct stat buf;

	snprintf(fname, sizeof(fname), "%s/%s", PGR_Write_Path, PGREPLICATE_PID_FILE);
	if (stat(fname,&buf) == 0)
	{
		/* pid file is exist */
		return true;
	}
	else
	{
		/* pid file is not exist */
		return false;
	}
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    child_wait()
 * NOTES
 *    Waiting for hung up a child
 * ARGS
 *    int signal_args: signal number (expecting the SIGCHLD)
 * RETURN
 *    none
 *--------------------------------------------------------------------
 */
void
child_wait(SIGNAL_ARGS)
{
	pid_t pid = 0;

	do {
		int ret;
		pid = waitpid(-1,&ret,WNOHANG);
	} while(pid > 0);
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    usage()
 * NOTES
 *    show usage of pglb
 * ARGS
 *    void
 * RETURN
 *    none
 *--------------------------------------------------------------------
 */
static void
usage(void)
{
	char * path;

	path = getenv("PGDATA");
	if (path == NULL)
		path = ".";
	fprintf(stderr,"PGReplicate version [%s]\n",PGREPLICATE_VERSION);
	fprintf(stderr,"A replication server for cluster DB servers (based on PostgreSQL)\n\n");
	fprintf(stderr,"usage: pgreplicate [-D path_of_config_file] [-W path_of_work_files] [-U login user][-l][-n][-v][-h][stop]\n");
	fprintf(stderr,"    config file default path: %s/%s\n",path, PGREPLICATE_CONF_FILE);
	fprintf(stderr,"    -l: print error logs in the log file.\n");
	fprintf(stderr,"    -n: don't run in daemon mode.\n");
	fprintf(stderr,"    -v: debug mode. need '-n' flag\n");
	fprintf(stderr,"    -h: print this help\n");
	fprintf(stderr,"    stop: stop pgreplicate\n");
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    main()
 * NOTES
 *    main module of pgreplicate
 * ARGS
 *    int argc: number of parameter
 *    char ** argv: value of parameter
 * RETURN
 *    none
 *--------------------------------------------------------------------
 */
int
main(int argc, char * argv[])
{
	char * func = "main()";
	int opt = 0;
	char * r_path = NULL;
	char * w_path = NULL;
	bool detach = true;
	pid_t rlog_pid;

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
				detach = false;
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

	if (optind == (argc-1) && !strncasecmp(argv[optind],"stop",4))
	{
		stop_pgreplicate();
		exit(0);
	}
	else if (optind == argc)
	{
		if (is_exist_pid_file())
		{
			fprintf(stderr,"pid file %s/%s found. is another pgreplicate running?", PGR_Write_Path, PGREPLICATE_PID_FILE);
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

	PGR_Under_Replication_Server = true;
	write_pid_file();
	pgreplicate_pid = getpid();

	PGRsignal(SIGINT, set_exit_processing);
	PGRsignal(SIGQUIT, set_exit_processing);
	PGRsignal(SIGTERM, set_exit_processing);
	PGRsignal(SIGCHLD, child_wait);
	PGRsignal(SIGPIPE, SIG_IGN);

	if (PGRget_Conf_Data(PGR_Data_Path) != STATUS_OK)
	{
		show_error("%s:PGRget_Conf_Data error",func);
		PGRreplicate_exit(0);
	}
	if (PGRinit_recovery() != STATUS_OK)
	{
		show_error("%s:PGRinit_recovery error",func);
		PGRreplicate_exit(0);
	}
	if (PGRload_replication_id() != STATUS_OK)
	{
		show_error("%s:PGRload_replication_id error",func);
		PGRreplicate_exit(0);
	}

	if ( PGR_Use_Replication_Log == true )
	{
#ifdef PRINT_DEBUG
		show_debug("Use Replication Log. Start PGR_RLog_Main()");
#endif
		rlog_pid = PGR_RLog_Main();
		if (rlog_pid < 0)
		{
			show_error("%s:PGR_RLog_Main failed",func);
			PGRreplicate_exit(0);
		}
	}

	/*
	 * fork recovery process
	 */
	PGRrecovery_main(fork_wait_time);

	/*
	 * fork lifecheck process
	 */
	PGRlifecheck_main(fork_wait_time);

	/*
	 * call replicate module
	 */
	Replicateion_Log->r_log_sock =-1;

	if (fork_wait_time > 0) {
#ifdef PRINT_DEBUG
		show_debug("replicate process: wait fork(): pid = %d", getpid());
#endif		
		sleep(fork_wait_time);
	}

	replicate_main();

	PGRreplicate_exit(0);
	return STATUS_OK;
}

static void
set_exit_processing(int signo)
{
	exit_signo = signo;
	exit_processing = true;
	PGRsignal(signo, SIG_IGN);
}

