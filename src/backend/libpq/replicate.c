/*--------------------------------------------------------------------
 * FILE:
 *     replicate.c
 *
 * NOTE:
 *     This file is composed of the functions to call with the source
 *     at backend for the replication.
 *     Low level I/O functions that called by in these functions are 
 *     contained in 'replicate_com.c'.
 *
 *--------------------------------------------------------------------
 */

/*--------------------------------------
 * INTERFACE ROUTINES
 *
 * setup/teardown:
 *      PGR_Init_Replicate_Server_Data
 *      PGR_Set_Replicate_Server_Socket
 *      PGR_delete_shm
 * I/O call:
 *      PGR_Send_Replicate_Command
 * table handling:
 *      PGR_get_replicate_server_info
 * status distinction:
 *      PGR_Is_Replicated_Command
 *      Xlog_Check_Replicatec
 * replicateion main:
 *      PGR_replication 
 *-------------------------------------
 */
#ifdef USE_REPLICATION

#include "postgres.h"

#include <stdio.h>
#include <strings.h>
#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#include <sys/file.h>
#include <netdb.h>

#include "access/transam.h"
#include "bootstrap/bootstrap.h"
#include "libpq/libpq.h"
#include "libpq/pqformat.h"
#include "miscadmin.h"
#include "commands/prepare.h"
#include "nodes/nodes.h"
#include "nodes/print.h"
#include "utils/guc.h"
#include "parser/parser.h"
#include "access/xact.h"
#include "storage/proc.h"
#include "tcop/tcopprot.h"
#include "tcop/utility.h"
#include "postmaster/postmaster.h"
#include "replicate.h"

/* the source of this value is 'access/transam/varsup.c' */
#define VAR_OID_PREFETCH                (8192)

PGR_ReplicationLog_Info ReplicationLog_Info;
bool pgr_skip_in_prepared_query = false;

/*--------------------------------------
 * PROTOTYPE DECLARATION
 *--------------------------------------
 */
static int set_command_args(char argv[PGR_CMD_ARG_NUM][256],char *str);
static bool is_same_replication_server(ReplicateServerInfo * sp1, ReplicateServerInfo * sp2 );
static ReplicateServerInfo * search_new_replication_server ( ReplicateServerInfo * sp , int socket_type );

static int recv_message(int sock,char * buf,int flag);
static int send_replicate_packet(int sock,ReplicateHeader * header, char * query_string);
static bool is_copy_from(char * query);
static int get_words( char words[MAX_WORDS][MAX_WORD_LETTERS] ,char * string,int length,int upper);
static int get_table_name(char * table_name, char * query, int position );
static bool is_not_replication_query(char * query_string, int query_len, char cmdType);
static int Comp_Not_Replicate(PGR_Not_Replicate_Type * nrp1,PGR_Not_Replicate_Type* nrp2);
static bool is_serial_control_query(char cmdType,char * query);
static bool is_select_into_query(char cmdType,char * query);
static int send_response_to_replication_server(const char * notice);
static bool do_not_replication_command(const char * commandTag);
static bool is_create_temp_table(char * query);
static int add_replication_server(char * hostname,char * port, char * recovery_port);
static int change_replication_server(char * hostname,char * port, char * recovery_port);
static int get_new_replication_socket( ReplicateServerInfo * base, ReplicateServerInfo * sp, int socket_type);
static char * get_hostName(char * dest,char * src);
static void set_response_mode(char * mode);
static void PGR_Set_Current_Replication_Query_ID(char *id);
#ifdef CONTROL_LOCK_CONFLICT
static int wait_lock_answer(void);
static int read_trigger(char * result, int buf_size);
#endif /* CONTROL_LOCK_CONFLICT */
static int check_conf_data(void);

static unsigned int get_next_request_id(void);
static bool is_this_query_replicated(char * id);
static int set_replication_id(char * id);
static int return_current_oid(void);
static int sync_oid(char * oid);
static bool is_concerned_with_prepared_select(char cmdType, char * query_string);
static int skip_non_blank(char * ptr, int max);
static int skip_blank(char * ptr, int max);
static int parse_message(char * query_string);
static bool is_prepared_as_select(char * query_string);
static bool is_statement_as_select(char * query_string);

extern ssize_t secure_read(Port *, void *, size_t);
/*--------------------------------------------------------------------
 * SYMBOL
 *    PGR_Init_Replicate_Server_Data()
 * NOTES
 *    Read Configuration file and create ReplicateServerData table
 * ARGS
 *    void
 * RETURN
 *    OK: STATUS_OK
 *    NG: STATUS_ERROR
 *--------------------------------------------------------------------
 */
int
PGR_Init_Replicate_Server_Data(void)
{
	int table_size,str_size;
	ReplicateServerInfo  *sp;
	PGR_Not_Replicate_Type * nrp;
	ConfDataType * conf;
	int rec_no,cnt;
	char HostName[HOSTNAME_MAX_LENGTH];

	memset (HostName,0,sizeof(HostName));
	if (ConfData_Top == (ConfDataType *)NULL)
	{
		return STATUS_ERROR;
	}

	/* allocate replication server information table */
	table_size = sizeof(ReplicateServerInfo) * MAX_SERVER_NUM;
	ReplicateServerShmid = shmget(IPC_PRIVATE,table_size,IPC_CREAT | IPC_EXCL | 0600);
	if (ReplicateServerShmid < 0)
	{
		return STATUS_ERROR;
	}
	ReplicateServerData = (ReplicateServerInfo *)shmat(ReplicateServerShmid,0,0);
	if (ReplicateServerData == (ReplicateServerInfo *)-1)
	{
		return STATUS_ERROR;
	}
	memset(ReplicateServerData,0,table_size);
	sp = ReplicateServerData;

	/* allocate cluster db information table */
	ClusterDBShmid = shmget(IPC_PRIVATE,sizeof(ClusterDBInfo),IPC_CREAT | IPC_EXCL | 0600);
	if (ClusterDBShmid < 0)
	{
		return STATUS_ERROR;
	}
	ClusterDBData = (ClusterDBInfo *)shmat(ClusterDBShmid,0,0);
	if (ClusterDBData == (ClusterDBInfo *)-1)
	{
		return STATUS_ERROR;
	}
	memset(ClusterDBData,0,sizeof(ClusterDBInfo));
	PGR_Set_Cluster_Status(STATUS_REPLICATED);

	/* allocate partial replicate table */
	table_size = sizeof(PGR_Not_Replicate_Type) * MAX_SERVER_NUM;
	PGR_Not_Replicate = malloc(table_size);
	if (PGR_Not_Replicate == (PGR_Not_Replicate_Type*)NULL)
	{
		return STATUS_ERROR;
	}
	memset(PGR_Not_Replicate, 0, table_size);
	nrp = PGR_Not_Replicate;
	cnt = 0;
	conf = ConfData_Top;
	while ((conf != (ConfDataType *)NULL) && (cnt < MAX_SERVER_NUM))
	{
		/* set replication server table */
		if (!strcmp(conf->table,REPLICATION_SERVER_INFO_TAG))
		{
			rec_no = conf->rec_no;
			cnt = rec_no;
			if (!strcmp(conf->key,HOST_NAME_TAG))
			{
				get_hostName((sp + rec_no)->hostName, conf->value);
				conf = (ConfDataType *)conf->next;
				continue;
			}
			if (!strcmp(conf->key,PORT_TAG))
			{
				(sp + rec_no)->portNumber = atoi(conf->value);
				(sp + rec_no)->sock = -1;
				if ((sp + rec_no)->useFlag != DATA_USE)
				{
					PGR_Set_Replication_Server_Status((sp+rec_no), DATA_INIT);
				}
				memset((sp + rec_no + 1)->hostName,0,sizeof(sp->hostName));
				(sp + rec_no + 1)->useFlag = DATA_END;
				conf = (ConfDataType *)conf->next;
				continue;
			}
			if (!strcmp(conf->key,RECOVERY_PORT_TAG))
			{
				(sp + rec_no)->recoveryPortNumber = atoi(conf->value);
				if ((sp + rec_no)->useFlag != DATA_USE)
				{
					PGR_Set_Replication_Server_Status((sp+rec_no), DATA_INIT);
				}
				memset((sp + rec_no + 1)->hostName,0,sizeof(sp->hostName));
				(sp + rec_no + 1)->useFlag = DATA_END;
				conf = (ConfDataType *)conf->next;
				continue;
			}
		}
		/* set part replication table */
		if (!strcmp(conf->table,NOT_REPLICATE_INFO_TAG))
		{
			rec_no = conf->rec_no;
			cnt = rec_no;
			if (PGR_Not_Replicate_Rec_Num < rec_no +1)
			{
				PGR_Not_Replicate_Rec_Num = rec_no +1;
			}
			if (!strcmp(conf->key,DB_NAME_TAG))
			{
				strncpy((nrp + rec_no)->db_name,conf->value,sizeof(nrp->db_name));
				conf = (ConfDataType *)conf->next;
				continue;
			}
			if (!strcmp(conf->key,TABLE_NAME_TAG))
			{
				strncpy((nrp + rec_no)->table_name,conf->value,sizeof(nrp->table_name));
				conf = (ConfDataType *)conf->next;
				continue;
			}
		}
		if (!strcmp(conf->key,HOST_NAME_TAG))
		{
			str_size = sizeof(HostName) ;
			memset(HostName,0,str_size);
			strncpy(HostName,conf->value,str_size-1);
		}
		else if (!strcmp(conf->key,RECOVERY_PORT_TAG))
		{
			RecoveryPortNumber = atoi(conf->value);
		}
		else if (!strcmp(conf->key,RSYNC_PATH_TAG))
		{
			str_size = strlen(conf->value) ;
			RsyncPath = malloc(str_size + 1);
			if (RsyncPath == NULL)
			{
				return STATUS_ERROR;
			}
			memset(RsyncPath,0,str_size + 1);
			strncpy(RsyncPath,conf->value,str_size);
		}
		else if (!strcmp(conf->key,RSYNC_OPTION_TAG))
		{
			str_size = strlen(conf->value) ;
			RsyncOption = malloc(str_size + 1);
			if (RsyncOption == NULL)
			{
				return STATUS_ERROR;
			}
			memset(RsyncOption,0,str_size + 1);
			strncpy(RsyncOption,conf->value,str_size);
		}
		else if (!strcmp(conf->key,RSYNC_COMPRESS_TAG))
		{
			if (!strcmp(conf->value, "yes"))
				RsyncCompress = true;
			else if (!strcmp(conf->value, "no"))
				RsyncCompress = false;
		}
		else if (!strcmp(conf->key,RSYNC_TIMEOUT_TAG))
		{
			PGR_Rsync_Timeout = PGRget_time_value(conf->value);
			if ((PGR_Rsync_Timeout < 1) || (PGR_Rsync_Timeout > 3600))
			{
				fprintf(stderr,"%s is out of range. It should be between 1sec-1hr.\n",RSYNC_TIMEOUT_TAG);
				fflush(stderr);
				return STATUS_ERROR;
			}
		}
		else if (!strcmp(conf->key,RSYNC_BWLIMIT_TAG))
		{
			PGR_Rsync_Bwlimit = PGRget_bw_value(conf->value);
			if ((PGR_Rsync_Bwlimit < 0) || (PGR_Rsync_Bwlimit > 10*1024*1024 ))
			{
				fprintf(stderr,"%s is out of range. It should be between 1KB-10GB.\n",RSYNC_BWLIMIT_TAG);
				fprintf(stderr,"If you want to set more than 10GB. Probably it is not necessary to set bwlimit.\n"); 
				fflush(stderr);
				return STATUS_ERROR;
			}
		}
		else if (!strcmp(conf->key,PING_PATH_TAG))
		{
			str_size = strlen(conf->value) ;
			PingPath = malloc(str_size + 1);
			if (PingPath == NULL)
			{
				return STATUS_ERROR;
			}
			memset(PingPath,0,str_size + 1);
			strncpy(PingPath,conf->value,str_size);
		}
		else if (!strcmp(conf->key,PG_DUMP_PATH_TAG))
		{
			str_size = strlen(conf->value) ;
			PgDumpPath = malloc(str_size + 1);
			if (PgDumpPath == NULL)
			{
				return STATUS_ERROR;
			}
			memset(PgDumpPath,0,str_size + 1);
			strncpy(PgDumpPath,conf->value,str_size);
		}
		else if (!strcmp(conf->key,STAND_ALONE_TAG))
		{
			PGR_Stand_Alone = (PGR_Stand_Alone_Type*)malloc(sizeof(PGR_Stand_Alone_Type));
			if (PGR_Stand_Alone == (PGR_Stand_Alone_Type *)NULL)
			{
				return STATUS_ERROR;
			}
			PGR_Stand_Alone->is_stand_alone = false;
			if (!strcmp(conf->value,READ_WRITE_IF_STAND_ALONE))
			{
				PGR_Stand_Alone->permit = PERMIT_READ_WRITE;
			}
			else
			{
				PGR_Stand_Alone->permit = PERMIT_READ_ONLY;
			}
		}
		else if (!strcmp(conf->key,TIMEOUT_TAG))
		{
			/* get repliaction timeout */
			PGR_Replication_Timeout = PGRget_time_value(conf->value);
			if ((PGR_Replication_Timeout < 1) || (PGR_Replication_Timeout > 3600))
			{
				fprintf(stderr,"%s is out of range. It should be between 1sec-1hr.\n",TIMEOUT_TAG);
				fflush(stderr);
				return STATUS_ERROR;
			}
		}
		else if (!strcmp(conf->key,LIFECHECK_TIMEOUT_TAG))
		{
			/* get lifecheck timeout */
			PGR_Lifecheck_Timeout = PGRget_time_value(conf->value);
			if ((PGR_Lifecheck_Timeout < 1) || (PGR_Lifecheck_Timeout > 3600))
			{
				fprintf(stderr,"%s is out of range. It should be between 1sec-1hr.\n",LIFECHECK_TIMEOUT_TAG);
				fflush(stderr);
				return STATUS_ERROR;
			}
		}
		else if (!strcmp(conf->key,LIFECHECK_INTERVAL_TAG))
		{
			/* get lifecheck interval */
			PGR_Lifecheck_Interval = PGRget_time_value(conf->value);
			if ((PGR_Lifecheck_Interval < 1) || (PGR_Lifecheck_Interval > 3600))
			{
				fprintf(stderr,"%s is out of range. It should between 1sec-1hr.\n",LIFECHECK_INTERVAL_TAG);
				fflush(stderr);
				return STATUS_ERROR;
			}
		}
		conf = (ConfDataType *)conf->next;
	}
	TransactionSock = -1;
	ReplicateCurrentTime = (ReplicateNow *)malloc(sizeof(ReplicateNow));
	if (ReplicateCurrentTime == (ReplicateNow *)NULL)
	{
		return STATUS_ERROR;
	}
	memset(ReplicateCurrentTime,0,sizeof(ReplicateNow));

	PGRCopyData = (CopyData *)malloc(sizeof(CopyData));
	if (PGRCopyData == (CopyData *)NULL)
	{
		return STATUS_ERROR;
	}
	memset(PGRCopyData,0,sizeof(CopyData));

	if (PGR_Not_Replicate_Rec_Num  == 0)
	{
		free(PGR_Not_Replicate);
		PGR_Not_Replicate = NULL;
	}
	else
	{
		qsort((char *)PGR_Not_Replicate,PGR_Not_Replicate_Rec_Num,sizeof(PGR_Not_Replicate_Type), (int (*)(const void*,const void*))Comp_Not_Replicate);
	}

	PGRSelfHostName = malloc(HOSTNAME_MAX_LENGTH);
	if (PGRSelfHostName == NULL)
	{
		return STATUS_ERROR;
	}
	memset(PGRSelfHostName,0,HOSTNAME_MAX_LENGTH);

	PGR_password = malloc(sizeof(PGR_Password_Info));
	if (PGR_password == NULL)
	{
		return STATUS_ERROR;
	}
	memset(PGR_password,0,sizeof(PGR_Password_Info));
	PGR_password->password = malloc(PASSWORD_MAX_LENGTH);
	if (PGR_password->password == NULL)
	{
		return STATUS_ERROR;
	}
	memset(PGR_password->password,0,PASSWORD_MAX_LENGTH);

	if (HostName[0] == 0)
	{
		if (gethostname(HostName,HOSTNAME_MAX_LENGTH) < 0)
		{
			return STATUS_ERROR;
		}
	}
	get_hostName(PGRSelfHostName, HostName);

	if (RsyncPath == NULL)
	{
		RsyncPath = strdup(DEFAULT_RSYNC);
	}
	if (PingPath == NULL)
	{
		PingPath = strdup(DEFAULT_PING);
	}
	if (PgDumpPath == NULL)
	{
		PgDumpPath = strdup(DEFAULT_PG_DUMP);
	}

	return (check_conf_data());
}

static int
check_conf_data(void)
{
	int i = 0;
	ReplicateServerInfo  *sp;
	sp = ReplicateServerData;
	while ((sp + i)->useFlag != DATA_END)
	{
		if (*((sp + i)->hostName) == 0)
		{
			fprintf(stderr,"Hostname of replication server is not valid.\n");
			fflush(stderr);
			return STATUS_ERROR;
		}
		if ((sp + i)->portNumber < 1024)
		{
			fprintf(stderr,"Replication Port of replication server is not valid. It's required larger than 1024.\n");
			fflush(stderr);
			return STATUS_ERROR;
		}
		if ((sp + i)->recoveryPortNumber < 1024)
		{
			fprintf(stderr,"RecoveryPort of replication server is not valid. It's required larger than 1024.\n");
			fflush(stderr);
			return STATUS_ERROR;
		}
		if ((sp + i)->portNumber == (sp + i)->recoveryPortNumber)
		{
			fprintf(stderr,"Replication Port and RecoveryPort is conflicted.\n");
			fflush(stderr);
			return STATUS_ERROR;
		}
		i++;
	}
	if (RecoveryPortNumber < 1024)
	{
		fprintf(stderr,"RecoveryPort of Cluster DB is not valid. It's required larger than 1024.\n");
		fflush(stderr);
		return STATUS_ERROR;
	}
	if (PGR_Stand_Alone == NULL)
	{
		fprintf(stderr,"Stand Alone Mode is not specified.\n");
		fflush(stderr);
		return STATUS_ERROR;
	}
	if (RsyncOption == NULL)
	{
		fprintf(stderr,"Option of rsync command is not specified.\n");
		fflush(stderr);
		return STATUS_ERROR;
	}
	if (strlen(PGRSelfHostName) <= 0)
	{
		fprintf(stderr,"Hostname of Cluster DB is not valid.\n");
		fflush(stderr);
		return STATUS_ERROR;
	}
	if (PGR_Lifecheck_Timeout > PGR_Lifecheck_Interval)
	{
		fprintf(stderr,"The lifecheck timeouti(%d) should be shorter than interval(%d).\n",PGR_Lifecheck_Timeout,PGR_Lifecheck_Interval);
		fflush(stderr);
		return STATUS_ERROR;
	}
	return STATUS_OK;
 }

/*--------------------------------------------------------------------
 * SYMBOL
 *    PGR_Set_Replicate_Server_Socket()
 * NOTES
 *    Create new socket and set ReplicateServerData table
 * ARGS
 *    void
 * RETURN
 *    OK: STATUS_OK
 *    NG: STATUS_ERROR
 *--------------------------------------------------------------------
 */
int
PGR_Set_Replicate_Server_Socket(void)
{
	ReplicateServerInfo * sp;
	if (ReplicateServerData == NULL)
	{
		return STATUS_ERROR;
	}
	sp = ReplicateServerData;
	while (sp->useFlag != DATA_END){
		sp->sock = -1;
		PGR_Create_Socket_Connect(&(sp->sock),sp->hostName,sp->portNumber);
		sp ++;
	}
	return	STATUS_OK;
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    PGR_get_replicate_server_socket()
 * NOTES
 *    search or create a socket to connect with the replication server
 * ARGS
 *    ReplicateServerInfo * sp: replication server data (I)
 *    int socket_type: socket type (I)
 *                       -PGR_TRANSACTION_SOCKET:
 *                       -PGR_QUERY_SOCKET:
 * RETURN
 *    OK: >0(socket)
 *    NG: -1
 *--------------------------------------------------------------------
 */
int
PGR_get_replicate_server_socket ( ReplicateServerInfo * sp , int socket_type )
{
	ReplicateServerInfo * tmp;
	tmp = sp;
	if (tmp == (ReplicateServerInfo *) NULL)
	{
		return -1;
	}
	if (tmp->hostName[0] == '\0')
	{
		return -1;
	}
	elog(NOTICE, "PGGRID: replicate server: %s", tmp->hostName);
	/*
	if ((socket_type != PGR_QUERY_SOCKET) && (TransactionSock != -1))
	*/
	if (TransactionSock != -1)
	{
		return TransactionSock;
	}

	while(PGR_Create_Socket_Connect(&TransactionSock,tmp->hostName,tmp->portNumber) != STATUS_OK)
	{
		close(TransactionSock);
		TransactionSock = -1;
		PGR_Set_Replication_Server_Status(tmp, DATA_ERR);
		usleep(20);
		tmp = PGR_get_replicate_server_info();
		if (tmp == (ReplicateServerInfo *)NULL)
		{
			return -1;
		}
		PGR_Set_Replication_Server_Status(tmp, DATA_USE);
		usleep(10);
	}
	return TransactionSock;
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    PGR_close_replicate_server_socket()
 * NOTES
 *    close the socket connected with the replication server
 * ARGS
 *    ReplicateServerInfo * sp: replication server data (I)
 *    int socket_type: socket type (I)
 *                       -PGR_TRANSACTION_SOCKET:
 *                       -PGR_QUERY_SOCKET:
 * RETURN
 *    OK: STATUS_OK
 *    NG: STATUS_ERROR
 *--------------------------------------------------------------------
 */
int
PGR_close_replicate_server_socket ( ReplicateServerInfo * sp , int socket_type )
{
	if (sp == (ReplicateServerInfo *)NULL )
	{
		return STATUS_ERROR;
	}
	if (sp->hostName[0] == '\0')
	{
		return STATUS_ERROR;
	}
	if (TransactionSock != -1)
	{
		PGR_Close_Sock(&(TransactionSock));
		TransactionSock = -1;
	}
	if (socket_type == PGR_QUERY_SOCKET)
	{
		if (sp->sock != -1)
		{
			PGR_Close_Sock(&(sp->sock));
		}
	}
	sp->sock = -1;
	/*
	PGR_Set_Replication_Server_Status(sp, DATA_INIT);
	*/
	return STATUS_OK;
}

static bool
is_same_replication_server(ReplicateServerInfo * sp1, ReplicateServerInfo * sp2 )
{
	if ((sp1 == NULL) || (sp2 == NULL))
	{
		return false;
	}
	if ((!strcmp(sp1->hostName,sp2->hostName)) &&
		(sp1->portNumber == sp2->portNumber) &&
		(sp1->recoveryPortNumber == sp2->recoveryPortNumber))
	{
		return true;
	}
	return false;
}

static ReplicateServerInfo *
search_new_replication_server ( ReplicateServerInfo * sp , int socket_type )
{
	ReplicateHeader dummy_header;
	ReplicateServerInfo * rs_tbl;
	char command[256];
	int sock = -1;
	int cnt = 0;

	if ((ReplicateServerData == NULL) || ( sp == NULL))
	{
		return NULL;
	}
	rs_tbl = sp;
	PGR_close_replicate_server_socket ( sp , socket_type);
	sp ++;
	if (sp->useFlag == DATA_END)
	{
		sp = ReplicateServerData;
	}
	while (is_same_replication_server(sp,rs_tbl) != true)
	{
		if (sp->useFlag == DATA_END)
		{
			sp = ReplicateServerData;
		}
		sock = PGR_get_replicate_server_socket( sp , socket_type);
		if (sock < 0 )
		{
			if (is_same_replication_server(sp,rs_tbl) == true)
			{
				return NULL;
			}
			else
			{
				sp++;
			}
			continue;
		}
		memset(&dummy_header, 0, sizeof(ReplicateHeader));
		memset(command,0,sizeof(command));
		snprintf(command,sizeof(command)-1,"SELECT %s(%d,%s,%d,%d)",
				PGR_SYSTEM_COMMAND_FUNC,
				PGR_CHANGE_REPLICATION_SERVER_FUNC_NO,
				sp->hostName,
				sp->portNumber,
				sp->recoveryPortNumber);
		dummy_header.cmdSys = CMD_SYS_CALL;
		dummy_header.cmdSts = CMD_STS_NOTICE;
		dummy_header.query_size = htonl(strlen(command));
		if (send_replicate_packet(sock,&dummy_header,command) != STATUS_OK)
		{
			cnt ++;
			PGR_close_replicate_server_socket ( sp , socket_type);
			PGR_Set_Replication_Server_Status(sp, DATA_ERR);
		}
		else
		{
			PGR_Set_Replication_Server_Status(sp, DATA_USE);
			return sp;
		}
		if (cnt > MAX_RETRY_TIMES )
		{
			sp++;
			cnt = 0;
		}
		else
		{
			continue;
		}
	}
	return NULL;
}

static int
get_table_name(char * table_name, char * query, int position )
{
	
	int i,wc;
	char * p;
	char * sp;
	int length;

	if ((table_name == NULL) || (query == NULL) || (position < 1))
	{
		return STATUS_ERROR;
	}
	length = strlen(query);
	p = query;
	wc = 1;
	sp = table_name;
	for (i = 0 ; i < length ; i ++)
	{
		while(isspace(*p))
		{
			p++;
			i++;
		}
		while((*p != '\0') && (! isspace(*p)))
		{
			if ((*p == ';') || (*p == '('))
				break;
			if (wc == position)
			{
				*sp = *p;
				sp++;
			}
			p++;
			i++;
		}
		if (wc == position)
		{
			*sp = '\0';
			break;
		}
		wc++;
	}
	return STATUS_OK;
}

static bool 
is_not_replication_query(char * query_string, int query_len, char cmdType)
{
	PGR_Not_Replicate_Type key;
	PGR_Not_Replicate_Type * ptr = NULL;

	if (PGR_Not_Replicate_Rec_Num <= 0)
		return false;
	if (query_string == NULL)
		return true;
	memset(&key,0,sizeof(PGR_Not_Replicate_Type));
	strncpy(key.db_name ,(char *)(MyProcPort->database_name),sizeof(key.db_name)-1);
	switch (cmdType)
	{
		case CMD_TYPE_INSERT:
			get_table_name(key.table_name,query_string,3);
			break;
		case CMD_TYPE_UPDATE:
			get_table_name(key.table_name,query_string,2);
			break;
		case CMD_TYPE_DELETE:
			get_table_name(key.table_name,query_string,3);
			break;
		case CMD_TYPE_COPY:
			get_table_name(key.table_name,query_string,2);
			break;
		default:
			return false;
	}
	ptr = (PGR_Not_Replicate_Type*)bsearch((void*)&key,(void*)PGR_Not_Replicate,PGR_Not_Replicate_Rec_Num,sizeof(PGR_Not_Replicate_Type), (int (*)(const void*,const void*))Comp_Not_Replicate);
	if (ptr == NULL)
	{
		return false;
	}
	return true;

}

/*--------------------------------------------------------------------
 * SYMBOL
 *    PGR_Send_Replicate_Command()
 * NOTES
 *    create new socket
 * ARGS
 *    char * query_string: query strings (I)
 *    char cmdSts: 
 *    char cmdType:
 * RETURN
 *    OK: result
 *    NG: NULL
 *--------------------------------------------------------------------
 */
char *
//#ifdef PGGRID
PGR_Send_Replicate_Command(char * query_string, int query_len, char cmdSts ,char cmdType, char *sitehostname, uint16_t siteport)
/*#else
PGR_Send_Replicate_Command(char * query_string, int query_len, char cmdSts ,char cmdType)
#endif*/
{
	int sock = -1;
	int cnt = 0;
	ReplicateHeader header;
	char * serverName = NULL;
	int portNumber=0;
	char * result = NULL;
	ReplicateServerInfo * sp = NULL;
	ReplicateServerInfo * base = NULL;
	int socket_type = 0;
	char argv[ PGR_CMD_ARG_NUM ][256];
	int argc = 0;
	int func_no = 0;
	int check_flag =0;
	bool in_transaction = false;
	

	elog(NOTICE,"PGGRID: Entrou na funcao PGR_Send_Replicate_Command");
	//fprintf(stderr, "cmdSts %c\n",cmdSts);
	//fprintf(stderr, "cmdType %c\n",cmdType);
	/*
	 * check query string
	 */
	if ((query_string == NULL)  ||
		(query_len < 0))
	{
		return NULL;
	}
	if (0){
		result = malloc(3);
		strcpy(result, "ok");
		return result;
	}
	/* check not replication query */
	if (is_not_replication_query(query_string, query_len, cmdType) == true)
	{
		PGR_Copy_Data_Need_Replicate = false;
		return NULL;
	}

	if ((cmdSts == CMD_STS_TRANSACTION ) ||
		(cmdSts == CMD_STS_SET_SESSION_AUTHORIZATION ) ||
		(cmdSts == CMD_STS_TEMP_TABLE ))
	{
		socket_type = PGR_TRANSACTION_SOCKET ;
	}
	else
	{
		socket_type = PGR_QUERY_SOCKET ;
	}

	if(cmdSts==CMD_STS_TRANSACTION 
	   && (cmdType!=CMD_TYPE_BEGIN && cmdType!=CMD_TYPE_ROLLBACK))
	{
		in_transaction = true;
	}

	sp = PGR_get_replicate_server_info();
	if (sp == NULL)
	{
		if (Debug_pretty_print)
			elog(DEBUG1,"PGR_get_replicate_server_info get error");
		return NULL;
	}
	sock = PGR_get_replicate_server_socket( sp , socket_type);
	if (sock < 0)
	{
		base = sp;
		sock = get_new_replication_socket( base, sp, socket_type);
		if (sock < 0)
		{
			if (Debug_pretty_print)
				elog(DEBUG1,"PGR_get_replicate_server_socket fail");
			return NULL;
		}
	}
	result = malloc(PGR_MESSAGE_BUFSIZE + 4);
	if (result == NULL)
	{
		fprintf(stderr,"malloc failed %s\n",strerror(errno));
		fflush(stderr);
		return NULL;
	}

	serverName = sp->hostName;
	portNumber = (int)sp->portNumber;
	memset(&header,0,sizeof(ReplicateHeader));

	header.cmdSts = cmdSts;
	header.cmdType = cmdType;
	header.port = htons(PostPortNumber);
	header.pid = htons(getpid());
	header.query_size = htonl(query_len); 
	
	strncpy(header.dbName ,(char *)(MyProcPort->database_name),sizeof(header.dbName)-1);
	strncpy(header.userName , (char *)(MyProcPort->user_name),sizeof(header.userName)-1);
	strncpy(header.password , PGR_password->password, PASSWORD_MAX_LENGTH );
	memcpy(header.md5Salt ,MyProcPort->md5Salt, sizeof(header.md5Salt));
	memcpy(header.cryptSalt ,MyProcPort->cryptSalt, sizeof(header.cryptSalt));
	header.request_id = htonl(get_next_request_id());
	header.rlog = 0;

	if (PGRSelfHostName != NULL)
	{
		strncpy(header.from_host, PGRSelfHostName, HOSTNAME_MAX_LENGTH);
	}

	base = sp;
	PGR_Sock_To_Replication_Server = sock;

retry_send_prereplicate_packet:

	memset(result,0,PGR_MESSAGE_BUFSIZE + 4);
	cnt = 0;
	header.cmdSys=CMD_SYS_PREREPLICATE;

	//#ifdef PGGRID
	elog(NOTICE,"PGGRID: Chegou no pggridhost %s", header.sitehostname);
	strncpy(header.sitehostname ,sitehostname,strlen(sitehostname));
	header.siteport=siteport;

	if (cmdType==CMD_TYPE_OTHER && strcasestr(query_string, "count") ){
		header.pggrid_op_type=PGGRID_OP_DCOUNT;
	}else if (cmdType==CMD_TYPE_OTHER && strcasestr(query_string, "get") ){
		header.pggrid_op_type=PGGRID_OP_DQUERY;
	}else
		header.pggrid_op_type=PGGRID_OP_NORMAL;
	elog(NOTICE,"PGGRID: sitehost: %s", header.sitehostname);
	elog(NOTICE,"PGGRID: siteport: %u", header.siteport);
	elog(NOTICE,"PGGRID: Passou");
	//#endif

	elog(NOTICE,"PGGRID: Enviando Pre replicate");
	while (send_replicate_packet(sock,&header,query_string) != STATUS_OK)
	{
		elog(NOTICE,"PGGRID: Pre replicate falhou");
		cnt++;
		if (cnt >= MAX_RETRY_TIMES )
		{
			sock = get_new_replication_socket( base, sp, socket_type);
			if (sock < 0)
			{
				if (Debug_pretty_print)
					elog(DEBUG1,"all replication servers may be down");
				PGR_Stand_Alone->is_stand_alone = true;
				if (cmdSts == CMD_STS_TRANSACTION )
				{
					strcpy(result,PGR_REPLICATION_ABORT_MSG);
					return result;
				}
				free(result);
				result = NULL;
				fprintf(stderr,"all replication servers may be down\n");
				fflush(stderr);
				return NULL;
				
			}
			if(in_transaction)
			{
				elog(ERROR,"replicate server down during replicating transaction. aborted.");
				free(result);
				return NULL;
			}					 
			PGR_Sock_To_Replication_Server = sock;
			cnt = 0;
		}
	}
	elog(NOTICE,"PGGRID: Passou Pre replicate");

	memset(result,0,PGR_MESSAGE_BUFSIZE);
	if (PGR_recv_replicate_result(sock,result,0) < 0)
	{
		elog(DEBUG1,"PGGRID: erro recebendo resultado rep");
			sock = get_new_replication_socket( base, sp, socket_type);
			if (sock < 0)
			{
					if (Debug_pretty_print)
							elog(DEBUG1,"all replication servers may be down");
					PGR_Stand_Alone->is_stand_alone = true;

					if (cmdSts == CMD_STS_TRANSACTION )
					{
							strcpy(result,PGR_REPLICATION_ABORT_MSG);
							return result;
					}
					if(result!=NULL) {
							free(result);
							result = NULL;
					}
					return NULL;
			}
			PGR_Sock_To_Replication_Server = sock;
			/* replication server should be down */

			if(in_transaction)
			{
				elog(ERROR,"replicate server down during replicating transaction. aborted.");
				free(result);
				return NULL;
			}

			goto retry_send_prereplicate_packet;
	}
	elog(DEBUG1,"PGGRID: Passou recebendo resultado pre");


	argc = set_command_args(argv,result);
	func_no=atoi(argv[0]);
	if(func_no==0) {
			/* this server is not primary replicate server*/
			sock=-1;
			goto retry_send_prereplicate_packet;
	}
retry_send_replicate_packet:


	elog(DEBUG1,"PGGRID: Enviando replicate");
	memset(result,0,PGR_MESSAGE_BUFSIZE + 4);
	cnt = 0;
	header.cmdSys = CMD_SYS_REPLICATE;
	while (send_replicate_packet(sock,&header,query_string) != STATUS_OK)
	{
		elog(NOTICE,"PGGRID: Replicate falhou");
		if (cnt > MAX_RETRY_TIMES )
		{
			sock = get_new_replication_socket( base, sp, socket_type);
			if (sock < 0)
			{
				if (Debug_pretty_print)
				  elog(DEBUG1,"all replication servers may be down");
				PGR_Stand_Alone->is_stand_alone = true;
				if (cmdSts == CMD_STS_TRANSACTION )
				{
				        strcpy(result,PGR_REPLICATION_ABORT_MSG);
				        return result;
				}
				free(result);
				result = NULL;
				return NULL;

			}
			PGR_Sock_To_Replication_Server = sock;
			header.rlog = CONNECTION_SUSPENDED_TYPE;
			cnt = 0;
		}
		cnt ++;
	}

	elog(NOTICE,"PGGRID: Passou enviando replicate");

	/*memset(result,0,PGR_MESSAGE_BUFSIZE);
	if (PGR_recv_replicate_result(sock,result,0) < 0)
	{
		// replication server should be down 
		sock = get_new_replication_socket( base, sp, socket_type);
		if (sock < 0)
		{
			if (Debug_pretty_print)
				elog(DEBUG1,"all replication servers may be down");
			PGR_Stand_Alone->is_stand_alone = true;

			if (cmdSts == CMD_STS_TRANSACTION )
			{
			        strcpy(result,PGR_REPLICATION_ABORT_MSG);
				return result;
			}
			if(result!=NULL) {
			        free(result);
			        result = NULL;
			}
			return NULL;
		}
		PGR_Sock_To_Replication_Server = sock;
		header.rlog = CONNECTION_SUSPENDED_TYPE;

		goto retry_send_replicate_packet;
	}*/

	elog(NOTICE,"PGGRID: Passou recebendo resultado replicate");

	argc = set_command_args(argv,result);
	if (argc >= 1)
	{
		func_no = atoi(argv[0]);
		if (func_no == PGR_SET_CURRENT_TIME_FUNC_NO)
		{
			if(! in_transaction)
				PGR_Set_Current_Time(argv[1],argv[2]);
			set_replication_id(argv[3]);
			set_response_mode(argv[4]);
         		PGR_Set_Current_Replication_Query_ID(argv[5]);
		}
		else if (func_no == PGR_NOTICE_DEADLOCK_DETECTION_FUNC_NO)
		{
			memset(result,0,PGR_MESSAGE_BUFSIZE);
			strcpy(result,PGR_DEADLOCK_DETECTION_MSG);
		}
                else if (func_no == PGR_SET_CURRENT_REPLICATION_QUERY_ID_NO) 
		{
			PGR_Set_Current_Replication_Query_ID(argv[1]);
		}  
		else if (func_no == PGR_QUERY_CONFIRM_ANSWER_FUNC_NO)
		{
			check_flag = atoi(argv[1]);
			if (check_flag == PGR_ALREADY_COMMITTED )
			{
				if(! in_transaction)
					PGR_Set_Current_Time(argv[2],argv[3]);
				set_replication_id(argv[4]);
			}
			else
			{
				if(! in_transaction)
					PGR_Set_Current_Time(argv[1],argv[2]);
				set_replication_id(argv[3]);
				/* this query is not replicated */
				/*
				free(result);
				return NULL;
				*/
			}
		}
	}
	return result;
}

uint32_t
PGRget_replication_id(void)
{
	return (ReplicationLog_Info.PGR_Replicate_ID);
}

static int
set_replication_id(char * id)
{
        uint32_t rid=0;
		uint32_t saved_id;
		if (id == NULL)
		{
				return STATUS_ERROR;
		}

		rid=(uint32_t)atol(id);
		if(rid==0)
				return STATUS_OK;

		needToUpdateReplicateIdOnNextQueryIsDone=true;
		saved_id=ReplicationLog_Info.PGR_Replicate_ID;

		ReplicationLog_Info.PGR_Replicate_ID =rid;


		/*set replicate id in this process */


		if (CurrentReplicateServer == NULL)
		{
				PGR_get_replicate_server_info();
		}
		if (CurrentReplicateServer != NULL)
		{
				/* set replicate id in this system */
				saved_id=CurrentReplicateServer->replicate_id;
				elog(DEBUG1, "replication id set from %d to %d", saved_id, rid); 

				CurrentReplicateServer->replicate_id = (uint32_t)(atol(id));
		}
	
		return STATUS_OK;
}


static unsigned int
get_next_request_id(void)
{
	if (ReplicationLog_Info.PGR_Request_ID +1 < PGR_MAX_COUNTER)
	{
		ReplicationLog_Info.PGR_Request_ID ++;
	}
	else
	{
		ReplicationLog_Info.PGR_Request_ID = 0;
	}
	return ReplicationLog_Info.PGR_Request_ID ;
		
}

static bool
is_this_query_replicated(char * id)
{
	uint32_t replicate_id = 0;
	uint32_t saved_id = 0;
	int32_t diff=0;
	ReplicateServerInfo * replicate_server_info = NULL;

	if (id == NULL)
	{
		return false;
	}
	replicate_id = (uint32_t)atol(id);
	elog(DEBUG1, "check for replication id , input=%u", replicate_id);

	if (CurrentReplicateServer == NULL)
	{
		PGR_get_replicate_server_info();
	}

	if (CurrentReplicateServer != NULL)
	{
		replicate_server_info = CurrentReplicateServer;
	}
        else if (LastReplicateServer != NULL)
	{	 
		replicate_server_info = LastReplicateServer;
	}
	if (replicate_server_info != NULL)
	{

	        saved_id=replicate_server_info->replicate_id;
		saved_id = saved_id < ReplicationLog_Info.PGR_Replicate_ID
		  ? ReplicationLog_Info.PGR_Replicate_ID
		  : saved_id;

		elog(DEBUG1, "check for replication id , now=%u", saved_id);
		/* check replicate_id < saved_id logically 
		 * 
		 * see also:
		 *  backend/transam/transam.c#TransactionIdPrecedes
		 */

	        diff = (int32) (saved_id-replicate_id);
		return (diff > 0);
	}
	elog(DEBUG1, "check for replication id check failed. no replication server");
	return false;
}


static int
get_new_replication_socket( ReplicateServerInfo * base, ReplicateServerInfo * sp, int socket_type)
{
	int sock = -1;

	if (( base == NULL) ||
		( sp == NULL))
	{
		return -1;
	}
	//elog(NOTICE, "PGGRID getnewsocket close");
	PGR_close_replicate_server_socket ( sp , socket_type);
	//elog(NOTICE, "PGGRID getnewsocket status");
	PGR_Set_Replication_Server_Status(sp, DATA_ERR);
	//elog(NOTICE, "PGGRID getnewsocket search");
	sp = search_new_replication_server(base, socket_type);
	if (sp == NULL)
	{
		if (Debug_pretty_print)
			elog(DEBUG1,"all replication servers may be down");
		PGR_Stand_Alone->is_stand_alone = true;
		return -1;
	}
	//elog(NOTICE, "PGGRID getnewsocket chamada");
	sock = PGR_get_replicate_server_socket( sp , socket_type);
	return sock;
}


int
PGR_recv_replicate_result(int sock,char * result,int user_timeout)
{
	fd_set      rmask;
	struct timeval timeout;
	int rtn, rv;
	
	//#ifdef PGGRID
	HeapTuple tuple;
	char *data_pointer;
	TupleTableSlot *slot;
	//#endif

	if (result == NULL)
	{
		return -1;
	}

	/*
	 * Wait for something to happen.
	 */
	for (;;)
	{
		if (user_timeout == 0)
			timeout.tv_sec = PGR_Replication_Timeout;
		else
			timeout.tv_sec = user_timeout;

		timeout.tv_usec = 0;

		FD_ZERO(&rmask);
		FD_SET(sock,&rmask);
		
		start_recv:
		
		rtn = select(sock+1, &rmask, (fd_set *)NULL, (fd_set *)NULL, &timeout);
		if (rtn <= 0)
		{
			if (errno != EINTR)
				return -1;
		}

		else if ((rtn > 0) && (FD_ISSET(sock, &rmask)))
		{
			elog(NOTICE, "PGGRID: recebendo resposta do repserver");
			rv=recv_message(sock, result,0);
			elog(NOTICE, "PGGRID: resposta recebida do repserver: %s", result);
			//receive tuples returned by replication server and store them into temporary table		
			if (rv>0 && PGR_target_temp_rel){
				if (strncmp(PGR_RETURN_TUPLES_MSG, result, strlen(PGR_RETURN_TUPLES_MSG)) ==0){
					elog(NOTICE, "PGGRID: recebendo tuplas do servidor");
					//fills tuple returned by server		
					data_pointer=result+strlen(PGR_RETURN_TUPLES_MSG)+1;
					if (strcmp(data_pointer, "")!= 0) {
						PGR_target_temp_rel_values[PGR_target_temp_rel_att_counter]=(char *)malloc(sizeof(char) * strlen(data_pointer));
						strcpy(PGR_target_temp_rel_values[PGR_target_temp_rel_att_counter], data_pointer);
						
						elog(NOTICE, "PGGRID: reposta q considerei: %d  %s", PGR_target_temp_rel_att_counter, PGR_target_temp_rel_values[PGR_target_temp_rel_att_counter]);
					}else{
						PGR_target_temp_rel_values[PGR_target_temp_rel_att_counter]=(char *)malloc(sizeof(char) * 4);
                                                strcpy(PGR_target_temp_rel_values[PGR_target_temp_rel_att_counter], "0");
					}
					//store tuple in temporary table
					PGR_target_temp_rel_att_counter++;
					if (PGR_target_temp_rel_att_counter >= RelationGetNumberOfAttributes(PGR_target_temp_rel)){
						elog(NOTICE, "PGGRID: montando tupla com o retorno do servidor");
						tuple=BuildTupleFromCStrings(TupleDescGetAttInMetadata(RelationGetDescr(PGR_target_temp_rel)),
							PGR_target_temp_rel_values);
						elog(NOTICE, "PGGRID: inserindo tupla");
						simple_heap_insert(PGR_target_temp_rel, tuple);
						//CatalogUpdateIndexes(PGR_target_temp_rel, tuple);
						slot = MakeSingleTupleTableSlot(RelationGetDescr(PGR_target_temp_rel));
						
						ExecStoreTuple(tuple, slot, InvalidBuffer, false);
						ExecDropSingleTupleTableSlot(slot);
						
						//free temp data
						//elog(NOTICE, "PGGRID: freezing temp data");
						for (PGR_target_temp_rel_att_counter=0;PGR_target_temp_rel_att_counter<RelationGetNumberOfAttributes(PGR_target_temp_rel); ++PGR_target_temp_rel_att_counter)
							free(PGR_target_temp_rel_values[PGR_target_temp_rel_att_counter]);
						//reinitialize counter
						PGR_target_temp_rel_att_counter=0;
					}
					goto start_recv;
				}
				
			}
			elog(NOTICE, "PGGRID: saindo da resposta do repserver");
			return rv;
		}
	}
	return -1;
}

static int
recv_message(int sock,char * buf,int flag)
{
	int cnt = 0;
	int r = 0;
	char * read_ptr;
	int read_size = 0;
	cnt = 0;
	read_ptr = buf;

	for (;;)
	{
		r = recv(sock,read_ptr + read_size ,PGR_MESSAGE_BUFSIZE - read_size, flag); 
		if (r < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				continue;
			} else {
			  elog(DEBUG1, "recv_message():recv failed");
			  return -1;
			}
		} else if (r == 0) {
			elog(DEBUG1, "recv_message():unexpected EOF");
			return -1;
		} else /*if (r > 0)*/ {
			read_size += r;
			if (read_size == PGR_MESSAGE_BUFSIZE)
			{
				return read_size;
			}
		}
	}
	return -1;
}

static int
send_replicate_packet(int sock,ReplicateHeader * header, char * query_string)
{
	int s = 0;
	char * send_ptr = NULL;
	char * buf = NULL;
	int send_size = 0;
	int buf_size = 0;
	int header_size = 0;
	int rtn = 0;
	fd_set      wmask;
	struct timeval timeout;
	int query_size = 0;

	/* check parameter */
	if ((sock < 0) || (header == NULL))
	{
		return STATUS_ERROR;
	}

	query_size = ntohl(header->query_size);
	header_size = sizeof(ReplicateHeader);
	buf_size = header_size + query_size + 4;
	buf = malloc(buf_size);
	if (buf == NULL)
	{
		return STATUS_ERROR;
	}
	memset(buf,0,buf_size);
	buf_size -= 4;
	memcpy(buf,header,header_size);
	if (query_string != NULL)
	{
		memcpy((char *)(buf+header_size),query_string,query_size+1);
	}
	send_ptr = buf;

	/*
	 * Wait for something to happen.
	 */
	rtn = 1;
	for (;;)
	{
		timeout.tv_sec = PGR_Replication_Timeout;
		timeout.tv_usec = 0;

		FD_ZERO(&wmask);
		FD_SET(sock,&wmask);
		rtn = select(sock+1, (fd_set *)NULL, &wmask, (fd_set *)NULL, &timeout);
		//elog(NOTICE, "PGGRID: select command");
		if (rtn < 0)
		{
			if (errno == EINTR)
				continue;
			else
			{
				elog(DEBUG1, "send_replicate_packet():select() failed");
				return STATUS_ERROR;
			}
		}
		else if (rtn && FD_ISSET(sock, &wmask))
		{
			//elog(NOTICE, "PGGRID: send command");

			s = send(sock,send_ptr + send_size,buf_size - send_size ,0);
			if (s < 0){
				if (errno == EINTR || errno == EAGAIN)
				{
					continue;
				}
				elog(DEBUG1, "send_replicate_packet():send error");

				/* EPIPE || ENCONNREFUSED || ENSOCK || EHOSTUNREACH */
				return STATUS_ERROR;
			} else if (s == 0) {
				free(buf);
				buf = NULL;
				elog(DEBUG1, "send_replicate_packet():unexpected EOF");
				return STATUS_ERROR;
			} else /*if (s > 0)*/ {
				send_size += s;
				if (send_size == buf_size)
				{
					free(buf);
					buf = NULL;
					return STATUS_OK;
				}
			}
		}
	}
	if (buf != NULL)
	{
		free(buf);
		buf = NULL;
	}
	return STATUS_ERROR;
}

bool
PGR_Is_Replicated_Command(char * query)
{

	return (PGR_Is_System_Command(query));
}

int
Xlog_Check_Replicate(int operation)
{
	if (PGR_Get_Cluster_Status() == STATUS_RECOVERY)
	{
		return STATUS_OK;
		/* elog(WARNING, "This query is not permitted while recovery db "); */
	}
	else if ((operation == CMD_UTILITY ) ||
		(operation == CMD_INSERT )  ||
		(operation == CMD_UPDATE )  ||
		(operation == CMD_DELETE ))
	{
		return (PGR_Replicate_Function_Call());
	}
	return STATUS_OK;
}

int 
PGR_Replicate_Function_Call(void)
{
	char *result = NULL;
	int status = STATUS_OK;

	if ((PGR_Get_Cluster_Status() == STATUS_RECOVERY) ||
		(PGR_Stand_Alone == NULL))
	{
		 return STATUS_OK;
	}
    if (Query_String != NULL)
    {
		if (PGR_Is_Stand_Alone() == true)
		{
			if (PGR_Stand_Alone->permit == PERMIT_READ_ONLY)
			{
				Query_String = NULL;
				return STATUS_ERROR;
			}
		}
		PGR_Need_Notice = true;
		PGR_Check_Lock.check_lock_conflict = true;
        result = PGR_Send_Replicate_Command(Query_String,strlen(Query_String), CMD_STS_QUERY,CMD_TYPE_SELECT,"0", 0);
		if (result != NULL)
		{
			PGR_Reload_Start_Time();
			if (!strncmp(result,PGR_DEADLOCK_DETECTION_MSG,strlen(PGR_DEADLOCK_DETECTION_MSG)))
			{
				status = STATUS_DEADLOCK_DETECT;
			}
			free(result);
			result = NULL;
		}
		else
		{
			status = STATUS_ERROR;
		}
		Query_String = NULL;
    }
	return status;
}

void
PGR_delete_shm(void)
{

	if (ReplicateServerData != NULL)
	{
		shmdt(ReplicateServerData);
		ReplicateServerData = NULL;
		shmctl(ReplicateServerShmid,IPC_RMID,(struct shmid_ds *)NULL);
	}
	if (ClusterDBData != NULL)
	{
		shmdt(ClusterDBData);
		ClusterDBData = NULL;
		shmctl(ClusterDBShmid,IPC_RMID,(struct shmid_ds *)NULL);
	}

	if (TransactionSock != -1)
	{
		close(TransactionSock);
	}
	
	if (RsyncPath != NULL)
	{
		free(RsyncPath);
		RsyncPath = NULL;
	}
	if (RsyncOption != NULL)
	{
		free(RsyncOption);
		RsyncOption = NULL;
	}
	if (PingPath != NULL)
	{
		free(PingPath);
		PingPath = NULL;
	}

	if (ReplicateCurrentTime != NULL)
	{
		free(ReplicateCurrentTime);
		ReplicateCurrentTime = NULL;
	}

	if (PGRCopyData != NULL)
	{
		free (PGRCopyData);
		PGRCopyData = NULL;
	}

	if (PGR_Stand_Alone != NULL)
	{
		free(PGR_Stand_Alone);
		PGR_Stand_Alone = NULL;
	}

	if (PGR_Not_Replicate != NULL)
	{
		free(PGR_Not_Replicate);
		PGR_Not_Replicate = NULL;
	}
	if (PGRSelfHostName != NULL)
	{
		free(PGRSelfHostName);
		PGRSelfHostName = NULL;
	}
	if (PGR_password != NULL)
	{
		if (PGR_password->password != NULL)
		{
			free(PGR_password->password);
			PGR_password->password = NULL;
		}
		free(PGR_password);
		PGR_password = NULL;
	}
}

ReplicateServerInfo * 
PGR_get_replicate_server_info(void)
{

	ReplicateServerInfo * sp;

	if (ReplicateServerData == NULL)
	{
		return (ReplicateServerInfo *)NULL;
	}
	/* check current using replication server */
	sp = PGR_check_replicate_server_info();
	if (sp != NULL)
	{
		if (CurrentReplicateServer != NULL)
		{
			LastReplicateServer = CurrentReplicateServer;
			CurrentReplicateServer->replicate_id = LastReplicateServer->replicate_id;
		}
		CurrentReplicateServer = sp;
		return sp;
	}
	/* there is no used replication server */
	/* however it may exist still in initial status */
	sp = ReplicateServerData;
	while (sp->useFlag != DATA_END)
	{
		if (sp->useFlag != DATA_ERR )
		{
			if (CurrentReplicateServer != NULL)
			{
				LastReplicateServer = CurrentReplicateServer;
				CurrentReplicateServer->replicate_id = LastReplicateServer-> replicate_id;
			}
			CurrentReplicateServer = sp;
			PGR_Set_Replication_Server_Status(sp, DATA_USE);
			return sp;
		}
		sp++;
	}
	PGR_Stand_Alone->is_stand_alone = true;
	if (CurrentReplicateServer != NULL)
	{
	  LastReplicateServer = CurrentReplicateServer;
	  CurrentReplicateServer->replicate_id = LastReplicateServer-> replicate_id;
	}
	CurrentReplicateServer = NULL;
	return (ReplicateServerInfo *)NULL;
}

ReplicateServerInfo * 
PGR_check_replicate_server_info(void)
{
	ReplicateServerInfo * sp;

	if (ReplicateServerData == NULL)
	{
		return (ReplicateServerInfo *)NULL;
	}
	sp = ReplicateServerData;
	while (sp->useFlag != DATA_END)
	{
		if (sp->useFlag == DATA_USE )
		{
			return sp;
		}
		sp++;
	}
	return NULL;
} 

int
PGR_Send_Copy(CopyData * copy,int end )
{

	char cmdSts,cmdType;
	char * p = NULL;
	char *result = NULL;
	char term[8];
	/*int status = 0; */

	if (copy == NULL)
	{
		return STATUS_ERROR;
	}

	cmdSts = CMD_STS_COPY;

	if (Transaction_Mode > 0)
	{
		cmdSts = CMD_STS_TRANSACTION ;
	}
	if (Session_Authorization_Mode)
	{
		cmdSts = CMD_STS_SET_SESSION_AUTHORIZATION ;
	}
	cmdType = CMD_TYPE_COPY_DATA;

	copy->copy_data[copy->cnt] = '\0';
	if (end)
	{
		memset(term,0,sizeof(term));
		term[0]='\\';
		term[1]='.';
		term[2]='\n';

		cmdType = CMD_TYPE_COPY_DATA_END;
		p = NULL;
		if (copy->cnt > 0)
		{
			copy->copy_data[copy->cnt] = '\0';
			p = strstr(copy->copy_data,term);
			if (p == NULL)
			{
				p = &(copy->copy_data[copy->cnt-1]);
				copy->cnt--;
			}
			else
			{
				p = NULL;
			}
		}
		if (p != NULL)
		{
			strncpy(p,term,sizeof(term));
			copy->cnt += 4;
		}
	}
	result = PGR_Send_Replicate_Command(copy->copy_data, copy->cnt, cmdSts, cmdType, "0",0);
	memset(copy,0,sizeof(CopyData));

	if (result != NULL)
	{
		PGR_Reload_Start_Time();
		free(result);
		result = NULL;
		return STATUS_OK;
	}
	else
	{
		return STATUS_ERROR;
	}
}

CopyData * 
PGR_Set_Copy_Data(CopyData * copy, char *str, int len,int end)
{
	CopyData save;
	int save_len = 0;
	int read_index = 0;
	int send_size = 0;
	int buf_size = 0;
	int rest_len = 0;
	int rest_buf_size = 0;
	int status = STATUS_OK;
	char * ep = NULL;
	char term[4];

	#define BUFF_OFFSET (8)

	if ((PGR_Copy_Data_Need_Replicate == false) ||
		(copy == NULL))
	{
		return (CopyData *)NULL;
	}
	memset(term,0,sizeof(term));
	term[0]='\n';
	term[1]='\\';
	term[2]='.';
	buf_size = COPYBUFSIZ - BUFF_OFFSET;
	read_index = 0;
	rest_len = len;
	rest_buf_size = buf_size - copy->cnt; 
	while ((rest_len > 0) && (rest_buf_size > 0))
	{
		if (rest_buf_size < rest_len)
		{
			send_size = rest_buf_size;
			rest_len -= send_size;
		}
		else
		{
			send_size = rest_len;
			rest_len = 0;
		}
		memcpy(&(copy->copy_data[copy->cnt]) ,str + read_index ,send_size);
		copy->cnt += send_size;
		read_index += send_size;
		rest_buf_size = buf_size - copy->cnt; 
		if (strstr(copy->copy_data,term) != NULL)
		{
			break;
		}
		if (rest_buf_size <= 0)
		{
			ep = strrchr(copy->copy_data,'\n');
			if (ep != NULL)
			{
				*ep = '\0';
				save_len = copy->cnt - strlen(copy->copy_data) -1;
				copy->cnt -= save_len ;
				memset(&save,0,sizeof(CopyData));
				memcpy(save.copy_data,(ep+1),save_len+1);
				save.cnt = save_len;
				*ep = '\n';
				*(ep+1) = '\0';
				status = PGR_Send_Copy(copy,0);
				memset(copy,0,sizeof(CopyData));
				if (save_len > 0)
				{
					memcpy(copy,&save,sizeof(CopyData));
				}
				rest_buf_size = buf_size - copy->cnt; 

			}
			else
			{
				/* one record is bigger than COPYBUFSIZ */
				/* buffer would be over flow*/
				status = PGR_Send_Copy(copy,0);
				memset(copy,0,sizeof(CopyData));
				rest_buf_size = buf_size - copy->cnt; 
			}
		}
	}
	if (end)
	{
		status = PGR_Send_Copy(copy,end);
		memset(copy,0,sizeof(CopyData));
	}
	if (status != STATUS_OK)
	{
		return (CopyData *)NULL;
	}
	return copy;
}

int 
PGR_getcountresult(){
	return 0;	
}

int
//#ifdef PGGRID
PGR_replication(char * query_string, CommandDest dest, Node *parsetree, const char * commandTag , char *sitehostname, uint16_t siteport)
/*#else
PGR_replication(char * query_string, CommandDest dest, Node *parsetree, const char * commandTag)	
#endif*/
{
	char *result = NULL;
	char cmdSts = CMD_STS_OTHER;
	char cmdType = CMD_TYPE_OTHER;
	int query_len = 0;

	if ((query_string == NULL) ||
		(commandTag == NULL))
	{
		
		elog(ERROR,"PGGRID: sem consulta para replicar");
		return STATUS_ERROR;
	}

	Query_String = NULL;
	query_len = strlen(query_string);

	/* save query data for retry */
	PGR_Retry_Query.query_string = query_string;
	PGR_Retry_Query.query_len = query_len;
	PGR_Retry_Query.cmdSts = cmdSts;
	PGR_Retry_Query.cmdType = cmdType;
	PGR_Retry_Query.useFlag = DATA_USE;
	/* set cmdType */
	if (!strcmp(commandTag,"BEGIN")) cmdType = CMD_TYPE_BEGIN ;
	else if (!strcmp(commandTag,"COMMIT")) cmdType = CMD_TYPE_COMMIT ;
	else if (!strcmp(commandTag,"CREATE FRAGMENT")) cmdType = CMD_TYPE_OTHER ;
	else if (!strcmp(commandTag,"COUNT")) cmdType = CMD_TYPE_OTHER ;
	else if (!strcmp(commandTag,"LOAD DATA")) cmdType = CMD_TYPE_OTHER ;
	else if (!strcmp(commandTag,"SELECT")) cmdType = CMD_TYPE_SELECT ;
	else if (!strcmp(commandTag,"INSERT")) cmdType = CMD_TYPE_INSERT ;
	else if (!strcmp(commandTag,"UPDATE")) cmdType = CMD_TYPE_UPDATE ;
	else if (!strcmp(commandTag,"DELETE")) cmdType = CMD_TYPE_DELETE ;
	else if (!strcmp(commandTag,"VACUUM")) cmdType = CMD_TYPE_VACUUM ;
	else if (!strcmp(commandTag,"ANALYZE")) cmdType = CMD_TYPE_ANALYZE ;
	else if (!strcmp(commandTag,"REINDEX")) cmdType = CMD_TYPE_REINDEX ;
	else if (!strcmp(commandTag,"ROLLBACK")) cmdType = CMD_TYPE_ROLLBACK ;
	else if (!strcmp(commandTag,"RESET")) cmdType = CMD_TYPE_RESET ;
	else if (!strcmp(commandTag,"START TRANSACTION")) cmdType = CMD_TYPE_BEGIN ;

	/* only "replication_server" statement-name is replicated for SHOW. */
	/*   see CreateCommandTag() @ backend/tcop/postgres.c      */

	else if (!strcmp(commandTag,"COPY"))
	{
		cmdType = CMD_TYPE_COPY ;
		if (is_copy_from(query_string))
		{
			PGR_Copy_Data_Need_Replicate = true;
		}
		else
		{
			PGR_Copy_Data_Need_Replicate = false;
			return STATUS_NOT_REPLICATE;
		}
	}
	else if (!strcmp(commandTag,"SET")) 
	{
		cmdType = CMD_TYPE_SET;
		/*
		VariableSetStmt *stmt = (VariableSetStmt *)parsetree;
		if (strcmp(stmt->name, "TRANSACTION ISOLATION LEVEL") &&
			strcmp(stmt->name, "datestyle") &&
			strcmp(stmt->name, "autocommit") &&
			strcmp(stmt->name, "client_encoding") &&
			strcmp(stmt->name, "password_encryption") &&
			strcmp(stmt->name, "search_path") &&
			strcmp(stmt->name, "session_authorization") &&
			strcmp(stmt->name, "timezone"))

			return STATUS_NOT_REPLICATE;
		*/
		if (strstr(query_string,SYS_QUERY_1) != NULL)
		{
			return STATUS_NOT_REPLICATE;
		}
	}
	else if (!strcmp(commandTag,"CREATE TABLE")) 
	{
		if (is_create_temp_table(query_string))
		{
			Create_Temp_Table_Mode = true;
		}
	}
	if (Create_Temp_Table_Mode)
	{
		cmdSts = CMD_STS_TEMP_TABLE ;
	}
	if (Transaction_Mode > 0)
	{
		cmdSts = CMD_STS_TRANSACTION ;
	}
	else
	{
		if ((cmdType == CMD_TYPE_COMMIT ) ||
			(cmdType == CMD_TYPE_ROLLBACK ))
		{
			cmdSts = CMD_STS_TRANSACTION ;
			if (ReplicateCurrentTime != NULL)
			{
				ReplicateCurrentTime->useFlag = DATA_INIT;
				ReplicateCurrentTime->use_seed = 0;
			}
		}
	}
	if (Session_Authorization_Mode)
	{
		cmdSts = CMD_STS_SET_SESSION_AUTHORIZATION ;
		if (cmdType == CMD_TYPE_SESSION_AUTHORIZATION_END)
		{
			Session_Authorization_Mode = false;
		}
	}
	if ((cmdSts == CMD_STS_TRANSACTION ) ||
		(cmdSts == CMD_STS_SET_SESSION_AUTHORIZATION ) ||
		(cmdSts == CMD_STS_TEMP_TABLE ))
	{
		/* check partitional replication table */
		if (is_not_replication_query(query_string, query_len, cmdType)== true )
		{
			PGR_Copy_Data_Need_Replicate = false;
			return STATUS_NOT_REPLICATE;
		}
		Query_String = NULL;
		if (( do_not_replication_command(commandTag) == true) &&
			(strcmp(commandTag,"SELECT")))
		{
			return STATUS_NOT_REPLICATE;
		}

		if (Debug_pretty_print)
			elog(DEBUG1,"transaction query send :%s",(char *)query_string);
		PGR_Retry_Query.cmdSts = cmdSts;
		PGR_Retry_Query.cmdType = cmdType;
		result = PGR_Send_Replicate_Command(query_string,query_len, cmdSts,cmdType, sitehostname, siteport);
		if (result != NULL)
		{
			if (!strncmp(result,PGR_DEADLOCK_DETECTION_MSG,strlen(PGR_DEADLOCK_DETECTION_MSG)))
			{
				/*
				PGR_Send_Message_To_Frontend(result);
				*/
				free(result);
				result = NULL;
				return STATUS_DEADLOCK_DETECT;
			}
			else if (!strncmp(result,PGR_REPLICATION_ABORT_MSG,strlen(PGR_REPLICATION_ABORT_MSG)))
			{
				free(result);
				result = NULL;
				return STATUS_REPLICATION_ABORT;
			}
			free(result);
			result = NULL;
			return STATUS_CONTINUE;
		}
		else
		{
			return STATUS_ERROR;
		}
	}
	else
	{
		cmdSts = CMD_STS_QUERY ;
		if ( do_not_replication_command(commandTag) == false)
		{
			Query_String = NULL;
			/* check partitional replication table */
			if (is_not_replication_query(query_string, query_len, cmdType)== true )
			{
				PGR_Copy_Data_Need_Replicate = false;
				return STATUS_NOT_REPLICATE;
			}
			elog(DEBUG1,"sen replicate command :%s",(char *)query_string);
			result = PGR_Send_Replicate_Command(query_string,query_len,cmdSts,cmdType, sitehostname, siteport);
			if (result != NULL)
			{
				elog(DEBUG1,"error sending command :%s",(char *)query_string);
				if (!strncmp(result,PGR_DEADLOCK_DETECTION_MSG,strlen(PGR_DEADLOCK_DETECTION_MSG)))
				{
					free(result);
					result = NULL;
					return STATUS_DEADLOCK_DETECT;
				}
				else if (!strncmp(result,PGR_REPLICATION_ABORT_MSG,strlen(PGR_REPLICATION_ABORT_MSG)))
				{
					free(result);
					result = NULL;
					return STATUS_REPLICATION_ABORT;
				}
				/*
				PGR_Send_Message_To_Frontend(result);
				*/
				free(result);
				result = NULL;
				return STATUS_CONTINUE;
			}
			else
			{
	
				elog(DEBUG1,"erro PGR_Send_Replicate_Command :%s",(char *)query_string);
				return STATUS_ERROR;
			}
		}
		else
		{
			if (( is_serial_control_query(cmdType,query_string) == true) ||
				( is_select_into_query(cmdType,query_string) == true))
			{
				Query_String = NULL;
				PGR_Need_Notice = true;
				PGR_Check_Lock.check_lock_conflict = true;
				elog(DEBUG1,"sen replicate command :%s",(char *)query_string);
				result = PGR_Send_Replicate_Command(query_string,query_len,cmdSts,cmdType, sitehostname, siteport);
				if (result != NULL)
				{
					/*
					PGR_Send_Message_To_Frontend(result);
					*/
					if (!strncmp(result,PGR_DEADLOCK_DETECTION_MSG,strlen(PGR_DEADLOCK_DETECTION_MSG)))
					{
						free(result);
						return STATUS_DEADLOCK_DETECT;
					}
					free(result);
					result = NULL;
					return STATUS_CONTINUE;
				}
				else
				{
					return STATUS_ERROR;
				}
			}
			else
			{
				Query_String = query_string;
				/*PGR_Sock_To_Replication_Server = -1;*/
			}
			return STATUS_CONTINUE_SELECT;
		}
	}
	return STATUS_CONTINUE;
}


bool
PGR_Is_System_Command(char * query)
{
	char * ptr;

	if (query == NULL)
	{
		return false;
	}
	ptr = strstr(query,PGR_SYSTEM_COMMAND_FUNC);
	if (ptr != NULL)
	{
		ptr = strchr(ptr,'(');
		if (ptr == NULL)
			return false;
		return true;
	}
	return false;
}

static int
set_command_args(char argv[ PGR_CMD_ARG_NUM ][256],char *str)
{
	int i,j,cnt,len;
	char * ptr = str;

	if (str == NULL)
	{
		return 0;
	}
	len = strlen(str);
	cnt = j = 0;
	for ( i = 0 ; i < len ; i++,ptr++)
	{
		if (cnt >= PGR_CMD_ARG_NUM)
			break;
		if (( *ptr == ',') || (*ptr == ')'))
		{
			argv[cnt][j] = '\0';
			cnt ++;
			j = 0;
			continue;
		}
		argv[cnt][j] = *ptr;
		j++;
	}
	if (cnt < PGR_CMD_ARG_NUM)
		argv[cnt][j] = '\0';
	cnt ++;

	return cnt;
}

static int
add_replication_server(char * hostname,char * port, char * recovery_port)
{
	int cnt;
	int portNumber;
	int recoveryPortNumber;
	ReplicateServerInfo * sp;

	if ((hostname == NULL) ||
		(port == NULL ) ||
		(recovery_port == NULL ))
	{
		return STATUS_ERROR;
	}
	if (ReplicateServerData == NULL)
	{
		return STATUS_ERROR;
	}
	portNumber = atoi(port);
	recoveryPortNumber = atoi(recovery_port);
	cnt = 0;
	sp = ReplicateServerData;
	while (sp->useFlag != DATA_END){
		if((!strncmp(sp->hostName,hostname,sizeof(sp->hostName))) &&
			(sp->portNumber == portNumber) &&
			(sp->recoveryPortNumber == recoveryPortNumber))
		{
			PGR_Set_Replication_Server_Status(sp, DATA_INIT);
			return STATUS_OK;
		}
		sp ++;
		cnt ++;
	}
	if (cnt < MAX_SERVER_NUM)
	{
		memset(sp->hostName,0,sizeof(sp->hostName));
		get_hostName(sp->hostName,hostname);
		sp->portNumber = portNumber;
		sp->recoveryPortNumber = recoveryPortNumber;
		PGR_Set_Replication_Server_Status(sp, DATA_INIT);
		memset((sp+1),0,sizeof(ReplicateServerInfo));
		(sp + 1)->useFlag = DATA_END;
	}
	else
	{
		return STATUS_ERROR;
	}
	return	STATUS_OK;
}

static int
change_replication_server(char * hostname,char * port, char * recovery_port)
{
	int cnt;
	int portNumber;
	int recoveryPortNumber;
	ReplicateServerInfo * sp;

	if ((hostname == NULL) ||
		(port == NULL ) ||
		(recovery_port == NULL ))
	{
		return STATUS_ERROR;
	}
	if (ReplicateServerData == NULL)
	{
		return STATUS_ERROR;
	}
	portNumber = atoi(port);
	recoveryPortNumber = atoi(recovery_port);
	cnt = 0;
	sp = ReplicateServerData;
	while (sp->useFlag != DATA_END){
		if((!strcmp(sp->hostName,hostname)) &&
			(sp->portNumber == portNumber) &&
			(sp->recoveryPortNumber == recoveryPortNumber))
		{
			PGR_Set_Replication_Server_Status(sp, DATA_USE);
		}
		/*
		else
		{
			if (sp->useFlag == DATA_USE)
			{
				PGR_Set_Replication_Server_Status(sp, DATA_INIT);
			}
		}
		*/
		sp ++;
		cnt ++;
	}
	return	STATUS_OK;
}

int
PGR_Set_Current_Time(char * sec, char * usec)
{
	int rtn = 0;
	struct timeval local_tp;
	struct timezone local_tpz;
	struct timeval tv;

	if ((sec == NULL) ||
		(usec == NULL))
	{
		return STATUS_ERROR;
	}
	rtn = gettimeofday(&local_tp, &local_tpz);
	tv.tv_sec = atol(sec);
	tv.tv_usec = atol(usec);
	ReplicateCurrentTime->offset_sec = local_tp.tv_sec - tv.tv_sec;
	ReplicateCurrentTime->offset_usec = local_tp.tv_usec - tv.tv_usec;
	ReplicateCurrentTime->tp.tv_sec = tv.tv_sec;
	ReplicateCurrentTime->tp.tv_usec = tv.tv_usec;
	ReplicateCurrentTime->useFlag = DATA_USE;
	ReplicateCurrentTime->use_seed = 0;

	return	STATUS_OK;
}

static void
PGR_Set_Current_Replication_Query_ID(char *id) {
      MyProc->replicationId=atol(id);
      return;
}

static void
set_response_mode(char * mode)
{
	int response_mode = 0;

	if (mode == NULL)
		return;
	response_mode = atoi(mode);
	if (response_mode < 0)
		return;
	if (CurrentReplicateServer == NULL)
	{
		PGR_get_replicate_server_info();
		if (CurrentReplicateServer == NULL)
		{
			return;
		}
	}
	if (CurrentReplicateServer->response_mode != response_mode)
	{
		CurrentReplicateServer->response_mode = response_mode;
	}
}

int
PGR_Call_System_Command(char * command)
{
	char * ptr;
	char * args;
	char argv[ PGR_CMD_ARG_NUM ][256];
	int argc = 0;
	int func_no;
	char * hostName = NULL;

	if ((command == NULL) || (ReplicateCurrentTime == NULL))
	{
		return STATUS_ERROR;
	}
	ptr = strstr(command,PGR_SYSTEM_COMMAND_FUNC);
	if (ptr == NULL)
		return STATUS_ERROR;
	ptr = strchr(ptr,'(');
	if (ptr == NULL)
		return STATUS_ERROR;
	args = ptr+1;
	ptr = strchr(ptr,')');
	if (ptr == NULL)
		return STATUS_ERROR;
	*ptr = '\0';
	argc = set_command_args(argv,args);
	if (argc < 1)
		return STATUS_ERROR;
	func_no = atoi(argv[0]);
	switch (func_no)
	{
		/* set current system time */
		case PGR_SET_CURRENT_TIME_FUNC_NO:
			if (atol(argv[1]) == 0)
			{
				/* before 8.3 */
				/*
				CreateCheckPoint(false,true);
				*/
				/* in 8.3 */
				RequestCheckpoint(CHECKPOINT_CAUSE_XLOG);
			}
			else
			{
			  /*
			  if ((atoi(argv[3]) > 0) &&
				(is_this_query_replicated(argv[3]) == true))
			    {				
			      return STATUS_SKIP_QUERY;
			    }
			  */
				PGR_Set_Current_Time(argv[1],argv[2]);
				set_replication_id(argv[3]);
				set_response_mode(argv[4]);
				PGR_Set_Current_Replication_Query_ID(argv[5]);

			}
			break;
		/* add new replication server data */
		case PGR_STARTUP_REPLICATION_SERVER_FUNC_NO:
			hostName = strdup(argv[1]);
			hostName = get_hostName(hostName, argv[1]);
			add_replication_server(hostName,argv[2],argv[3]);
			free(hostName);
			break;
		/* change new replication server */
		case PGR_CHANGE_REPLICATION_SERVER_FUNC_NO:
			hostName = strdup(argv[1]);
			hostName = get_hostName(hostName, argv[1]);
			change_replication_server(hostName,argv[2],argv[3]);
			free(hostName);
			break;
        	case PGR_SET_CURRENT_REPLICATION_QUERY_ID_NO:
		  PGR_Set_Current_Replication_Query_ID(argv[1]);
			break;
		case PGR_QUERY_CONFIRM_ANSWER_FUNC_NO:
			if ((atoi(argv[3]) > 0) &&
				(is_this_query_replicated(argv[3]) == true))
			{
				/* skip this query */
			  return STATUS_SKIP_QUERY;
			}
			else
			{
				PGR_Set_Current_Time(argv[1],argv[2]);
				set_replication_id(argv[3]);
			}
			break;
		/* get current oid */
		case PGR_GET_OID_FUNC_NO:
			return_current_oid();
			break;
		/* set current oid */
		case PGR_SET_OID_FUNC_NO:
			sync_oid(argv[1]);
			break;
		/* set noticed session abort */
		case PGR_NOTICE_ABORT_FUNC_NO:
			PGR_Noticed_Abort = true;
			break;
	}
	return STATUS_OK;
}

int
PGR_GetTimeOfDay(struct timeval *tp, struct timezone *tpz)
{

	int rtn;

	rtn = gettimeofday(tp, tpz);
	if (ReplicateCurrentTime == NULL)
	{
		return rtn;
	}
	if (ReplicateCurrentTime->useFlag == DATA_USE)
	{
		if (ReplicateCurrentTime->use_seed != 0)
		{
			tp->tv_sec -= ReplicateCurrentTime->offset_sec;
			if (tp->tv_usec < ReplicateCurrentTime->offset_usec)
			{
				tp->tv_usec += (1000000 -  ReplicateCurrentTime->offset_usec);
				tp->tv_sec -= 1;
			}
			else
			{
				tp->tv_usec -= ReplicateCurrentTime->offset_usec;
			}
		}
		else
		{
			tp->tv_sec = ReplicateCurrentTime->tp.tv_sec;
			tp->tv_usec = ReplicateCurrentTime->tp.tv_usec;
		}
		rtn = 0;
	}
	return rtn;
}

long
PGR_Random(void)
{
	double rtn;
	if (ReplicateCurrentTime != NULL)
	{
		if ( ReplicateCurrentTime->use_seed == 0)
		{
			srand( ReplicateCurrentTime->tp.tv_usec );
			ReplicateCurrentTime->use_seed = 1;
		}
	}
	rtn = random();
	return rtn;
}

char *
PGR_scan_terminate( char * str)
{
	char * p;
	int sflag = 0;
	int dflag = 0;
	int lflag = 0;
	int i = 0;
	char tag[256];

	if (str == NULL)
		return NULL;
	p = str;
	memset(tag,0,sizeof(tag));
	while ( *p != '\0' )
	{
		if ((!strncmp(p,"--",2)) ||
			(!strncmp(p,"//",2)))
		{
			while (( *p != '\n') && (*p != '\0'))
			{
				p++;
			}
			continue;
		}

		switch (*p)
		{
			case '\'':
				sflag ^= 1;
				break;
			case '\"':
				dflag ^= 1;
				break;
			case '$':
				i = 0;
				p++;
				while (( *p != '\n') && (*p != '\0'))
				{
					if (isalnum(*p) == 0)
					{
						if (*p == '$')
						{
							lflag ^= 1;
						}
						break;
					}
					else
					{
						if (i >= sizeof(tag))
							break;
						if (lflag == 0)
						{
							tag[i] = *p;
						}
						else
						{
							if (tag[i] != *p)
							{
								break;
							}
						}
						i++;
					}
					p++;
				}
				break;
			case '\\':
				p +=2;
				continue;
				break;
			case ';':
				if ((!sflag) && (!dflag) && (!lflag))
					return p;
				break;
		}
		p++;
	}
	return NULL;
}

static bool
is_copy_from(char * query)
{
	char * p;
	int i;
	char buf[12];
	int c_flag = 0;
	if (query == NULL)
		return false;
	p = query;
	for ( i = 0 ; i <= 1 ; i ++)
	{
		/* get 'copy table_name' string */
		while(isspace(*p))
			p++;
		while ((*p != '\0') && (*p  != '(') && (!isspace(*p)))
			p++;
	}
	while(isspace(*p))
		p++;
	/* skip table column */
	if (*p == '(')
	{
		c_flag = 1;
		p++;
		while (*p != '\0') 
		{
			if (*p == '(')
				c_flag ++;
			if (*p == ')')
				c_flag --;
			if (c_flag == 0)
			{
				p++;
				break;
			}
			p++;
		}
		while(isspace(*p))
			p++;
	}
	/* get 'from' or 'to' */
	i = 0;
	memset(buf,0,sizeof(buf));
	while ((*p != '\0') && (!isspace(*p)) && ( i < sizeof(buf)-1))
	{
		buf[i] = (char)toupper(*p);
		p++;
		i++;
	}
	if (!strcmp(buf,"FROM"))
	{
		return true;
	}
	else
	{
		return false;
	}
}

static bool
is_create_temp_table(char * query)
{
	int len,wc;
	char buf[MAX_WORDS][MAX_WORD_LETTERS];

	if (query == NULL)
		return false;
	len = strlen(query);
	wc = get_words(buf,query,len,1);
	if (wc < 4)
		return false;
	if ((!strncmp(buf[0],"CREATE", strlen("CREATE"))) &&
		(!strncmp(buf[1],"TEMP",strlen("TEMP"))) &&
		(!strncmp(buf[2],"TABLE",strlen("TABLE"))))
	{
		return true;
	}
	return false;
}

static int
get_words( char words[MAX_WORDS][MAX_WORD_LETTERS] ,char * string,int length,int upper)
{
	int i,wc,lc;
	char * p = NULL;
	char * buf = NULL;

	if (string == NULL)
		return STATUS_ERROR;
	buf = malloc(length);
	if (buf == NULL)
		return STATUS_ERROR;

	memset(buf,0,length);
	p = string;
	wc = 0;
	for (i = 0 ; i < length ; i ++)
	{
		if ((*p == '\0') || (wc >= MAX_WORDS))
			break;
		while (isspace(*p))
		{
			p++;
			i++;
		}
		lc = 0;
		while ((*p != '\0') && (! isspace(*p)))
		{
			if (upper)
				*(buf+lc) = (char)toupper(*p);
			else
				*(buf+lc) = *p;

			p++;
			i++;
			lc++;
		}
		memset(words[wc],0,MAX_WORD_LETTERS);
		memcpy(words[wc],buf,lc);
		memset(buf,0,length);
		wc++;
	}
	free(buf);
	buf = NULL;
	return wc;
}

static int
Comp_Not_Replicate(PGR_Not_Replicate_Type * nrp1,PGR_Not_Replicate_Type* nrp2)
{
	int rtn;

	if ((nrp1 == NULL) ||
		(nrp2 == NULL))
	{
		return 0;
	}
	rtn = strcasecmp(nrp1->table_name,nrp2->table_name);
	if (rtn == 0)
	{
		rtn = strcasecmp(nrp1->db_name,nrp2->db_name);
	}
	return rtn;
}

bool
PGR_Is_Stand_Alone(void)
{
	ReplicateServerInfo * sp = NULL;

	if (PGR_Stand_Alone == NULL)
		return true;
	if (PGR_Stand_Alone->is_stand_alone == true)
	{
		sp = PGR_get_replicate_server_info();
		if (sp == NULL)
		{
			return true;
		}
	}
	return false;
}

void
PGR_Send_Message_To_Frontend(char * msg)
{
	StringInfoData msgbuf;

	pq_beginmessage(&msgbuf, 'N');

	if (PG_PROTOCOL_MAJOR(FrontendProtocol) >= 3)
	{
		/* New style with separate fields */
		char		tbuf[12];
		int			ssval;
		int			i;

		pq_sendbyte(&msgbuf, PG_DIAG_SEVERITY);
		pq_sendstring(&msgbuf, "NOTICE" );

		/* unpack MAKE_SQLSTATE code */
		ssval = ERRCODE_WARNING ;
		for (i = 0; i < 5; i++)
		{
			tbuf[i] = PGUNSIXBIT(ssval);
			ssval >>= 6;
		}
		tbuf[i] = '\0';

		pq_sendbyte(&msgbuf, PG_DIAG_SQLSTATE);
		pq_sendstring(&msgbuf, tbuf);

		/* M field is required per protocol, so always send something */
		pq_sendbyte(&msgbuf, PG_DIAG_MESSAGE_PRIMARY);
		if (msg)
			pq_sendstring(&msgbuf, msg);
		else
			pq_sendstring(&msgbuf, _("missing error text"));

		pq_sendbyte(&msgbuf, '\0');		/* terminator */
	}
	else
	{
		/* Old style --- gin up a backwards-compatible message */
		StringInfoData buf;

		initStringInfo(&buf);

		appendStringInfo(&buf, "%s:  ", "NOTICE");

		if (msg)
			appendStringInfoString(&buf, msg);
		else
			appendStringInfoString(&buf, _("missing error text"));

		appendStringInfoChar(&buf, '\n');

		pq_sendstring(&msgbuf, buf.data);

		pfree(buf.data);
	}

	pq_endmessage(&msgbuf);

	/*
	 * This flush is normally not necessary, since postgres.c will flush out
	 * waiting data when control returns to the main loop. But it seems best
	 * to leave it here, so that the client has some clue what happened if the
	 * backend dies before getting back to the main loop ... error/notice
	 * messages should not be a performance-critical path anyway, so an extra
	 * flush won't hurt much ...
	 */
	pq_flush();
}

static bool
is_serial_control_query(char cmdType,char * query)
{
	char * buf = NULL;
	int len = 0;
	int i = 0;
	char * p = NULL;

	if ((cmdType != CMD_TYPE_SELECT ) ||
		( query == NULL))
	{
		return false;
	}

	p = query;
	len = strlen(query) +1;
	buf = malloc(len);
	if (buf == NULL)
		return false;

	memset(buf,0,len);
	for ( i = 0 ; i < len ; i ++)
	{
		*(buf+i) = toupper(*(query+i));
	}
	if ((strstr(buf,"NEXTVAL") != NULL) ||
		(strstr(buf,"SETVAL") != NULL))
	{
		free(buf);
		buf = NULL;
		return true;
	}
	free(buf);
	buf = NULL;
	return false;
}

static bool
is_select_into_query(char cmdType,char * query)
{
	char * buf = NULL;
	int len = 0;
	int i = 0;
	char * p = NULL;

	if ((cmdType != CMD_TYPE_SELECT ) ||
		( query == NULL))
	{
		return false;
	}

	p = query;
	len = strlen(query) +1;
	buf = malloc(len);
	if (buf == NULL)
		return false;

	memset(buf,0,len);
	for ( i = 0 ; i < len ; i ++)
	{
		*(buf+i) = toupper(*(query+i));
	}
	if (strstr(buf,"INTO") != NULL)
	{
		free(buf);
		buf = NULL;
		return true;
	}
	if (strstr(buf,"CREATE") != NULL)
	{
		free(buf);
		buf = NULL;
		return true;
	}
	free(buf);
	buf = NULL;
	return false;
}

static int
send_response_to_replication_server(const char * notice)
{
	ReplicateHeader header;
	int status;

	if (PGR_Lock_Noticed)
	{
		return STATUS_OK;
	}
	if ((notice == NULL) ||
		(PGR_Sock_To_Replication_Server < 0))
	{
		return STATUS_ERROR;
	}

	memset(&header,0,sizeof(ReplicateHeader));
	header.cmdSys = CMD_SYS_CALL;
	header.cmdSts = CMD_STS_RESPONSE;
	if (!strcmp(notice,PGR_QUERY_ABORTED_NOTICE_CMD))
	{
		header.cmdType = CMD_TYPE_FRONTEND_CLOSED;
	}
	header.query_size = htonl(strlen(notice));
	status = send_replicate_packet(PGR_Sock_To_Replication_Server,&header,(char *)notice);
	return status;
}

void
PGR_Notice_Transaction_Query_Done(void)
{
	send_response_to_replication_server(PGR_QUERY_DONE_NOTICE_CMD);
}

void
PGR_Notice_Transaction_Query_Aborted(void)
{
	send_response_to_replication_server(PGR_QUERY_ABORTED_NOTICE_CMD);
}

int
PGR_Notice_Conflict(void)
{
	const char * msg = NULL ;
	int rtn = STATUS_OK;

	msg = PGR_LOCK_CONFLICT_NOTICE_CMD ;
	if (PGR_Check_Lock.deadlock == true)
	{
		msg = PGR_DEADLOCK_DETECT_NOTICE_CMD ;
	}
	if (PGR_Check_Lock.dest == TO_FRONTEND)
	{
		ReadyForQuery(DestRemote);
		EndCommand(msg,DestRemote);
#ifdef CONTROL_LOCK_CONFLICT
		rtn = wait_lock_answer();
#endif /* CONTROL_LOCK_CONFLICT */
	}
	else
	{
		send_response_to_replication_server(msg);
#ifdef CONTROL_LOCK_CONFLICT
		rtn = PGR_Recv_Trigger (PGR_Replication_Timeout);
#endif /* CONTROL_LOCK_CONFLICT */
	}
	return rtn;
}

#ifdef CONTROL_LOCK_CONFLICT
static int
wait_lock_answer(void)
{
	char result[PGR_MESSAGE_BUFSIZE+4];
	int rtn = 0;

	memset(result,0,sizeof(result));
	rtn = read_trigger(result, PGR_MESSAGE_BUFSIZE);
	if (rtn < 0)
		return STATUS_ERROR;
	return STATUS_OK;
}

static int
read_trigger(char * result, int buf_size)
{
	int i = 0;
	char c;
	int r = 0;

	if ((result == NULL) || (buf_size <= 0 ))
	{
		return EOF;
	}
	/*
	pq_getbytes(result,buf_size);
	*/
	while ((r = pq_getbytes(&c,1)) == 0)
	{
		if (i < buf_size -1)
		{
			*(result + i) = c;
		}
		else
		{
			break;
		}
		if (c == '\0')
			break;
		i++;
	}

	return r;
}
#endif /* CONTROL_LOCK_CONFLICT */

int
PGR_Recv_Trigger (int user_timeout)
{
	char result[PGR_MESSAGE_BUFSIZE];
	int rtn = 0;
	int func_no = 0;

	
	if (PGR_Lock_Noticed)
	{
		return STATUS_OK;
	}
	if (PGR_Sock_To_Replication_Server < 0)
		return STATUS_ERROR;
	memset(result,0,sizeof(result));
	rtn = PGR_recv_replicate_result(PGR_Sock_To_Replication_Server,result,user_timeout);
	if (rtn > 0)
	{
		func_no = atoi(result);
		if (func_no  <= 0)
		{
			func_no = STATUS_OK;
		}
		return func_no;
	}
	else 
	{
		if (user_timeout == 0)
		{
			PGR_Set_Replication_Server_Status(CurrentReplicateServer, DATA_ERR);
		}
		return STATUS_ERROR;
	}
	return STATUS_OK;
}


int
PGR_Set_Transaction_Mode(int mode,const char * commandTag)
{
	if (commandTag == NULL)
	{
		return mode;
	}
	if ((!strcmp(commandTag,"BEGIN")) ||
		(!strcmp(commandTag,"START TRANSACTION")) )
	{
		return (++mode);
	}
	if (mode > 0)
	{
		if ((!strncmp(commandTag,"COMMIT",strlen("COMMIT"))) ||
			(!strncmp(commandTag,"ROLLBACK",strlen("ROLLBACK"))))
		{
			return (--mode);
		}
	}
	return mode;
}

static bool
do_not_replication_command(const char * commandTag)
{
	if (commandTag == NULL)
	{
		return true;
	}
	if ((!strcmp(commandTag,"SELECT")) ||
		(!strcmp(commandTag,"CLOSE CURSOR")) ||
		(!strcmp(commandTag,"MOVE")) ||
		(!strcmp(commandTag,"FETCH")) ||
		(!strcmp(commandTag,"EXPLAIN")))
	{
		return true;
	}
	else
	{
		return false;
	}
}

void
PGR_Set_Replication_Server_Status( ReplicateServerInfo * sp, int status)
{
	if (sp == NULL)
	{
		return;
	}
	if (sp->useFlag != status)
	{
		sp->useFlag = status;
	}
}

int
PGR_Is_Skip_Replication(char * query)
{
	char skip_2[256];

	if ((query == NULL) ||
		(MyProcPort == NULL))
	{
		return -1;
	}
	snprintf(skip_2,sizeof(skip_2),SKIP_QUERY_2,MyProcPort->user_name);
	if ((strncmp(query,SKIP_QUERY_1,strlen(SKIP_QUERY_1)) == 0) ||
		(strncmp(query,skip_2,strlen(skip_2)) == 0))
	{
		return 3;
	}
	if ((strncmp(query,SKIP_QUERY_3,strlen(SKIP_QUERY_3)) == 0) ||
		(strncmp(query,SKIP_QUERY_4,strlen(SKIP_QUERY_4)) == 0))
	{
		return 1;
	}
	return 0;
}

bool
PGR_Did_Commit_Transaction(void)
{

	int sock = -1;
	int cnt = 0;
	ReplicateHeader header;
	char * serverName = NULL;
	int portNumber=0;
	char * result = NULL;
	ReplicateServerInfo * sp = NULL;
	ReplicateServerInfo * base = NULL;
	int socket_type = 0;
	char argv[ PGR_CMD_ARG_NUM ][256];
	int argc = 0;
	int func_no = 0;

	if (ReplicateCurrentTime->useFlag != DATA_USE)
	{
		return false;
	}
	sp = PGR_get_replicate_server_info();
	if (sp == NULL)
	{
		if (Debug_pretty_print)
			elog(DEBUG1,"PGR_get_replicate_server_info get error");
		return false;
	}
	sock = PGR_get_replicate_server_socket( sp , PGR_QUERY_SOCKET);
	if (sock < 0)
	{
		if (Debug_pretty_print)
			elog(DEBUG1,"PGR_get_replicate_server_socket fail");
		return false;
	}
	result = malloc(PGR_MESSAGE_BUFSIZE);
	if (result == NULL)
	{
		return false;
	}
	memset(result,0,PGR_MESSAGE_BUFSIZE);

	serverName = sp->hostName;
	portNumber = (int)sp->portNumber;
	header.cmdSys = CMD_SYS_CALL;
	header.cmdSts = CMD_STS_TRANSACTION_ABORT;
	header.cmdType = CMD_TYPE_COMMIT_CONFIRM;
	header.port = htons(PostPortNumber);
	header.pid = htons(getpid());
	header.tv.tv_sec = htonl(ReplicateCurrentTime->tp.tv_sec);
	header.tv.tv_usec = htonl(ReplicateCurrentTime->tp.tv_usec);
	header.query_size = htonl(0); 
	strncpy(header.dbName ,(char *)(MyProcPort->database_name),sizeof(header.dbName)-1);
	strncpy(header.userName , (char *)(MyProcPort->user_name),sizeof(header.userName)-1);
	strncpy(header.password , PGR_password->password, PASSWORD_MAX_LENGTH );
	memcpy(header.md5Salt ,MyProcPort->md5Salt, sizeof(header.md5Salt));
	memcpy(header.cryptSalt ,MyProcPort->cryptSalt, sizeof(header.cryptSalt));
	if (PGRSelfHostName != NULL)
	{
		strncpy(header.from_host, PGRSelfHostName, HOSTNAME_MAX_LENGTH);
	}
	header.replicate_id = htonl(ReplicationLog_Info.PGR_Replicate_ID);
	header.request_id = 0;

	base = sp;
	PGR_Sock_To_Replication_Server = sock;

	cnt = 0;
	while (send_replicate_packet(sock,&header,"") != STATUS_OK)
	{
		if (cnt > MAX_RETRY_TIMES )
		{
			sock = get_new_replication_socket( base, sp, socket_type);
			if (sock < 0)
			{
				if (Debug_pretty_print)
					elog(DEBUG1,"all replication servers may be down");
				PGR_Stand_Alone->is_stand_alone = true;
				free(result);
				result = NULL;
				return false;
			}
			PGR_Sock_To_Replication_Server = sock;
			cnt = 0;
		}
		cnt ++;
	}

	if (PGR_recv_replicate_result(sock,result,6) < 0)
	{
		free(result);
		result = NULL;
		return false;
	}
	/* read answer */
	argc = set_command_args(argv,result);
	if (argc >= 1)
	{
		func_no = atoi(argv[0]);
		if (func_no == PGR_TRANSACTION_CONFIRM_ANSWER_FUNC_NO)
		{
			/* the transaction was commited in other server */
			if (atoi(argv[1]) == PGR_ALREADY_COMMITTED)
			{
				free(result);
				result = NULL;
				return true;
			}
		}
	}
	free(result);
	result = NULL;
	return false;
}

int
PGRsend_system_command(char cmdSts, char cmdType)
{
	ReplicateServerInfo * sp = NULL;
	int sock = -1;
	int socket_type = 0;
	char * result = NULL;
	char * serverName = NULL;
	int portNumber=0;
	ReplicateHeader header;
	int cnt = 0;
	ReplicateServerInfo * base = NULL;

	sp = PGR_get_replicate_server_info();
	if (sp == NULL)
	{
		if (Debug_pretty_print)
			elog(DEBUG1,"PGR_get_replicate_server_info get error");
		return STATUS_ERROR;
	}
	sock = PGR_get_replicate_server_socket( sp , PGR_QUERY_SOCKET);
	if (sock < 0)
	{
		if (Debug_pretty_print)
			elog(DEBUG1,"PGR_get_replicate_server_socket fail");
		return STATUS_ERROR;
	}
	result = malloc(PGR_MESSAGE_BUFSIZE);
	if (result == NULL)
	{
		return STATUS_ERROR;
	}
	memset(result,0,PGR_MESSAGE_BUFSIZE);

	serverName = sp->hostName;
	portNumber = (int)sp->portNumber;
	header.cmdSys = CMD_SYS_CALL;
	header.cmdSts = cmdSts;
	header.cmdType = cmdType;
	header.port = htons(PostPortNumber);
	header.pid = htons(getpid());
	header.tv.tv_sec = htonl(ReplicateCurrentTime->tp.tv_sec);
	header.tv.tv_usec = htonl(ReplicateCurrentTime->tp.tv_usec);
	header.query_size = htonl(0); 
	strncpy(header.dbName ,(char *)(MyProcPort->database_name),sizeof(header.dbName)-1);
	strncpy(header.userName , (char *)(MyProcPort->user_name),sizeof(header.userName)-1);
	strncpy(header.password , PGR_password->password, PASSWORD_MAX_LENGTH );
	memcpy(header.md5Salt ,MyProcPort->md5Salt, sizeof(header.md5Salt));
	memcpy(header.cryptSalt ,MyProcPort->cryptSalt, sizeof(header.cryptSalt));
	if (PGRSelfHostName != NULL)
	{
		strncpy(header.from_host, PGRSelfHostName, HOSTNAME_MAX_LENGTH);
	}
	header.replicate_id = htonl(ReplicationLog_Info.PGR_Replicate_ID);
	header.request_id = 0;

	base = sp;
	PGR_Sock_To_Replication_Server = sock;
	cnt = 0;
	while (send_replicate_packet(sock,&header,"") != STATUS_OK)
	{
		if (cnt > MAX_RETRY_TIMES )
		{
			sock = get_new_replication_socket( base, sp, socket_type);
			if (sock < 0)
			{
				if (Debug_pretty_print)
					elog(DEBUG1,"all replication servers may be down");
				PGR_Stand_Alone->is_stand_alone = true;
				free(result);
				result = NULL;
				return STATUS_ERROR;
			}
			PGR_Sock_To_Replication_Server = sock;
			cnt = 0;
		}
		cnt ++;
	}
	free(result);
	result = NULL;
	return STATUS_OK;
}

static char *
get_hostName(char *dest, char * src)
{
	char * top = NULL;
	char * p = NULL;
	unsigned int ip = 0;

	p = src;
	while ( *p != '\0')
	{
		if (*p == '\'') 
		{
			*p = '\0';
			p++;
			if (top == NULL)
			{
				top = p;
			}
		}
		if (*p == '(') 
		{
			*p = '\0';
		}
		p++;
	}
	if (top == NULL)
		top = src;
	ip = PGRget_ip_by_name(top);
	sprintf(dest,
				 "%d.%d.%d.%d",
				 (ip      ) & 0xff ,
				 (ip >>  8) & 0xff ,
				 (ip >> 16) & 0xff ,
				 (ip >> 24) & 0xff );
	return dest;
}

char *
PGR_Remove_Comment(char * str)
{
	char * p = NULL;
	p = str;
	while( *p != '\0')
	{
		while(isspace(*p))
		{
			p++;
		}
		if ((!memcmp(p,"--",2)) ||
			(!memcmp(p,"//",2)))
		{
			while((*p != '\n') && (*p != '\0'))
			{
				p++;
			}
			continue;
		}
		break;
	}
	return p;
}

void
PGR_Force_Replicate_Query(void)
{
	if (PGR_Retry_Query.useFlag == DATA_USE)
	{
		PGR_Send_Replicate_Command(PGR_Retry_Query.query_string,
			PGR_Retry_Query.query_len,
			PGR_Retry_Query.cmdSts,
			PGR_Retry_Query.cmdType, "0",0);
	}
}

void
PGR_Notice_DeadLock(void)
{
	ReplicateHeader header;

	memset(&header,0,sizeof(ReplicateHeader));
	header.cmdSys = CMD_SYS_CALL;
	header.cmdSts = CMD_STS_NOTICE;
	header.cmdType = CMD_TYPE_DEADLOCK_DETECT;
	header.query_size = 0;
	send_replicate_packet(PGR_Sock_To_Replication_Server,&header,(char *)NULL);
}

void
PGR_Set_Cluster_Status(int status)
{
	if (ClusterDBData != NULL)
	{
		if (ClusterDBData->status != status)
		{
			ClusterDBData->status = status;
		}
	}
}

bool
PGR_Is_Life_Check(char * query)
{
	if (!strcmp(query,PING_QUERY))
	{
		return true;
	}
	return false;
}

int
PGR_Get_Cluster_Status(void)
{
	if (ClusterDBData != NULL)
	{
		return (ClusterDBData->status);
	}
	return 0;
}

int
PGR_Check_Replicate_Server_Status(ReplicateServerInfo * sp)
{
	ReplicateHeader header;
	char * result = NULL;
	int status;
	int fdP;

	result = malloc(PGR_MESSAGE_BUFSIZE + 4);
	if (result == NULL)
	{
		if (Debug_pretty_print)
			elog(DEBUG1,"malloc failed in PGR_Check_Replicate_Server_Status()");
		return STATUS_ERROR;
	}

	memset(&header, 0, sizeof(ReplicateHeader));
	memset(result,  0, PGR_MESSAGE_BUFSIZE + 4);

	header.cmdSys = CMD_SYS_PREREPLICATE;
	header.cmdSts = CMD_STS_OTHER;
	header.cmdType = CMD_TYPE_OTHER;
	header.port = htons(PostPortNumber);
	header.pid = htons(getpid());
	header.query_size = 0;
	strncpy(header.dbName ,(char *)(MyProcPort->database_name),sizeof(header.dbName)-1);
	strncpy(header.userName , (char *)(MyProcPort->user_name),sizeof(header.userName)-1);
	strncpy(header.password , PGR_password->password, PASSWORD_MAX_LENGTH );
	memcpy(header.md5Salt ,MyProcPort->md5Salt, sizeof(header.md5Salt));
	memcpy(header.cryptSalt ,MyProcPort->cryptSalt, sizeof(header.cryptSalt));
	header.request_id = htonl(get_next_request_id());
	header.rlog = 0;
	if (PGRSelfHostName != NULL) {
		strncpy(header.from_host, PGRSelfHostName, HOSTNAME_MAX_LENGTH);
	}

	/* open a new socket for lifecheck */
	if ((status = PGR_Create_Socket_Connect(&fdP, sp->hostName, sp->portNumber)) == STATUS_ERROR) {
		if (Debug_pretty_print) {
			elog(DEBUG1,"create socket failed in PGR_Check_Replicate_Server_Status()");
		}
		
	/* status = STATUS_OK */
	} else {
		if ((status = send_replicate_packet(fdP, &header, (char *)NULL)) == STATUS_OK) {
			/* receive result to check for possible deadlock */
			status = (0 >= PGR_recv_replicate_result(fdP, result ,0))
				? STATUS_OK : STATUS_ERROR;
		}
	}

	free(result);
	PGR_Close_Sock(&fdP);

	return status;
}

static int
return_current_oid(void)
{
	char msg[PGR_MESSAGE_BUFSIZE];

	LWLockAcquire(OidGenLock, LW_EXCLUSIVE);

	if (ShmemVariableCache->nextOid < ((Oid) FirstBootstrapObjectId))
	{
		ShmemVariableCache->nextOid = FirstBootstrapObjectId;
		ShmemVariableCache->oidCount = 0;
	}

	if (ShmemVariableCache->oidCount == 0)
	{
		XLogPutNextOid(ShmemVariableCache->nextOid + VAR_OID_PREFETCH);
		ShmemVariableCache->oidCount = VAR_OID_PREFETCH;
	}
	LWLockRelease(OidGenLock);

	memset(msg,0,sizeof(msg));
	snprintf(msg, sizeof(msg), "%u", ShmemVariableCache->nextOid);
	if (PGR_Check_Lock.dest == TO_FRONTEND)
	{
		pq_puttextmessage('C',msg);
		pq_flush();
	}
	else
	{
		send_response_to_replication_server(msg);
	}
	return STATUS_OK;
}

static int
sync_oid(char * oid)
{
	uint32_t next_oid = 0;
	int offset = 0;
	char msg[PGR_MESSAGE_BUFSIZE];

	LWLockAcquire(OidGenLock, LW_EXCLUSIVE);

	next_oid =  strtoul(oid, NULL, 10);
	if (next_oid <= 0)
		return STATUS_ERROR;
	next_oid ++;
	offset = next_oid - ShmemVariableCache->nextOid ;
	if (offset <= 0)
		return STATUS_ERROR;

	if (next_oid < FirstBootstrapObjectId)
	{
		ShmemVariableCache->nextOid = FirstBootstrapObjectId;
		ShmemVariableCache->oidCount = 0;
	}

	/* If we run out of logged for use oids then we must log more */
	while (ShmemVariableCache->oidCount - offset <= 0)
	{
		offset -= (ShmemVariableCache->oidCount) ;
		(ShmemVariableCache->nextOid) += (ShmemVariableCache->oidCount);
		XLogPutNextOid(ShmemVariableCache->nextOid + VAR_OID_PREFETCH);
		ShmemVariableCache->oidCount = VAR_OID_PREFETCH;
	}

	(ShmemVariableCache->nextOid) += offset;
	(ShmemVariableCache->oidCount) -= offset;
	
	LWLockRelease(OidGenLock);

	memset(msg,0,sizeof(msg));
	snprintf(msg, sizeof(msg), "%u", ShmemVariableCache->nextOid);
	if (PGR_Check_Lock.dest == TO_FRONTEND)
	{
		pq_puttextmessage('C',msg);
		pq_flush();
	}
	else
	{
		send_response_to_replication_server(msg);
	}
	return STATUS_OK;
}

int
PGR_lo_import(char * filename)
{
	char * result = NULL;
	LOArgs *lo_args;
	int len = 0;
	int buf_size = 0;
	
	if ((PGR_Is_Replicated_Query == true) ||
		(PGR_Retry_Query.cmdSts == CMD_STS_TRANSACTION))
	{
		return STATUS_OK;
	}
	if ((PGR_Retry_Query.cmdSts != CMD_STS_QUERY) ||
		(PGR_Retry_Query.cmdType != CMD_TYPE_SELECT))
	{
		return STATUS_OK;
	}

	len = strlen(filename);
	buf_size = sizeof(LOArgs) + len;
	lo_args = (LOArgs *)malloc(buf_size + 4);
	if (lo_args == (LOArgs *)NULL)
	{
		return STATUS_ERROR;
	}
	memset(lo_args, 0, buf_size + 4);
	lo_args->arg1 = htonl((uint32_t)len);
	memcpy(lo_args->buf, filename, len);

	result = PGR_Send_Replicate_Command((char *)lo_args,
		buf_size,
		CMD_STS_LARGE_OBJECT,
		CMD_TYPE_LO_IMPORT, "0",0);

	free(lo_args);
	if (result != NULL)
	{
		free(result);
		return STATUS_OK;
	}
	
	return STATUS_ERROR;
}

int
PGR_lo_create(int flags)
{
	char * result = NULL;
	LOArgs lo_args;
	
	if ((PGR_Is_Replicated_Query == true) ||
		(PGR_Retry_Query.cmdSts == CMD_STS_TRANSACTION))
	{
		return STATUS_OK;
	}
	if ((PGR_Retry_Query.cmdSts != CMD_STS_QUERY) ||
		(PGR_Retry_Query.cmdType != CMD_TYPE_SELECT))
	{
		return STATUS_OK;
	}
	memset(&lo_args, 0, sizeof(LOArgs));
	lo_args.arg1 = htonl(flags);

	result = PGR_Send_Replicate_Command((char *)&lo_args,
		sizeof(LOArgs),
		CMD_STS_LARGE_OBJECT,
		CMD_TYPE_LO_CREATE, "0",0);

	if (result != NULL)
	{
		free(result);
		return STATUS_OK;
	}
	
	return STATUS_ERROR;
}

int
PGR_lo_open(Oid lobjId,int32 mode)
{
	char * result = NULL;
	LOArgs lo_args;
	
	if ((PGR_Is_Replicated_Query == true) ||
		(PGR_Retry_Query.cmdSts == CMD_STS_TRANSACTION))
	{
		return STATUS_OK;
	}
	if ((PGR_Retry_Query.cmdSts != CMD_STS_QUERY) ||
		(PGR_Retry_Query.cmdType != CMD_TYPE_SELECT))
	{
		return STATUS_OK;
	}
	memset(&lo_args, 0, sizeof(LOArgs));
	lo_args.arg1 = htonl((uint32_t)lobjId);
	lo_args.arg2 = htonl((uint32_t)mode);

	result = PGR_Send_Replicate_Command((char *)&lo_args,
		sizeof(LOArgs),
		CMD_STS_LARGE_OBJECT,
		CMD_TYPE_LO_OPEN,"0",0);
	
	if (result != NULL)
	{
		free(result);
		return STATUS_OK;
	}
	
	return STATUS_ERROR;
}

int
PGR_lo_close(int32 fd)
{
	char * result = NULL;
	LOArgs lo_args;
	
	if ((PGR_Is_Replicated_Query == true) ||
		(PGR_Retry_Query.cmdSts == CMD_STS_TRANSACTION))
	{
		return STATUS_OK;
	}
	if ((PGR_Retry_Query.cmdSts != CMD_STS_QUERY) ||
		(PGR_Retry_Query.cmdType != CMD_TYPE_SELECT))
	{
		return STATUS_OK;
	}
	memset(&lo_args, 0, sizeof(LOArgs));
	lo_args.arg1 = htonl((uint32_t)fd);

	result = PGR_Send_Replicate_Command((char *)&lo_args,
		sizeof(LOArgs),
		CMD_STS_LARGE_OBJECT,
		CMD_TYPE_LO_CLOSE, "0",0);

	if (result != NULL)
	{
		free(result);
		return STATUS_OK;
	}
	
	return STATUS_ERROR;
}

int
PGR_lo_write(int fd, char *buf, int len)
{
	char * result = NULL;
	LOArgs *lo_args = NULL;
	int buf_size = 0;
	
	if ((PGR_Is_Replicated_Query == true) ||
		(PGR_Retry_Query.cmdSts == CMD_STS_TRANSACTION))
	{
		return STATUS_OK;
	}
	if ((PGR_Retry_Query.cmdSts != CMD_STS_QUERY) ||
		(PGR_Retry_Query.cmdType != CMD_TYPE_SELECT))
	{
		return STATUS_OK;
	}
	buf_size = sizeof(LOArgs) + len;
	lo_args = malloc(buf_size + 4);
	if (lo_args == (LOArgs *)NULL)
	{
		return STATUS_ERROR;
	}
	memset(lo_args, 0, buf_size + 4);
	lo_args->arg1 = htonl((uint32_t)fd);
	lo_args->arg2 = htonl((uint32_t)len);
	memcpy(lo_args->buf, buf, len);
	result = PGR_Send_Replicate_Command((char *)lo_args,
		buf_size,
		CMD_STS_LARGE_OBJECT,
		CMD_TYPE_LO_WRITE, "0",0);

	free(lo_args);
	if (result != NULL)
	{
		free(result);
		return STATUS_OK;
	}
	
	return STATUS_ERROR;
}

int
PGR_lo_lseek(int32 fd, int32 offset, int32 whence)
{
	char * result = NULL;
	LOArgs lo_args;
	
	if ((PGR_Is_Replicated_Query == true) ||
		(PGR_Retry_Query.cmdSts == CMD_STS_TRANSACTION))
	{
		return STATUS_OK;
	}
	if ((PGR_Retry_Query.cmdSts != CMD_STS_QUERY) ||
		(PGR_Retry_Query.cmdType != CMD_TYPE_SELECT))
	{
		return STATUS_OK;
	}
	memset(&lo_args, 0, sizeof(LOArgs));
	lo_args.arg1 = htonl((uint32_t)fd);
	lo_args.arg2 = htonl((uint32_t)offset);
	lo_args.arg3 = htonl((uint32_t)whence);

	result = PGR_Send_Replicate_Command((char *)&lo_args,
		sizeof(LOArgs),
		CMD_STS_LARGE_OBJECT,
		CMD_TYPE_LO_LSEEK, "0",0);

	if (result != NULL)
	{
		free(result);
		return STATUS_OK;
	}
	
	return STATUS_ERROR;
}

int
PGR_lo_unlink(Oid lobjId)
{
	char * result = NULL;
	LOArgs lo_args;
	
	if ((PGR_Is_Replicated_Query == true) ||
		(PGR_Retry_Query.cmdSts == CMD_STS_TRANSACTION))
	{
		return STATUS_OK;
	}
	if ((PGR_Retry_Query.cmdSts != CMD_STS_QUERY) ||
		(PGR_Retry_Query.cmdType != CMD_TYPE_SELECT))
	{
		return STATUS_OK;
	}
	memset(&lo_args, 0, sizeof(LOArgs));
	lo_args.arg1 = htonl((uint32_t)lobjId);

	result = PGR_Send_Replicate_Command((char *)&lo_args,
		sizeof(LOArgs),
		CMD_STS_LARGE_OBJECT,
		CMD_TYPE_LO_UNLINK, "0",0);

	if (result != NULL)
	{
		free(result);
		return STATUS_OK;
	}
	
	return STATUS_ERROR;
}

Oid
PGRGetNewObjectId(Oid last_id)
{
	Oid newId = 0;

	if (last_id == 0)
	{
		newId = (Oid)PGRget_replication_id();
	}
	else
	{
		newId = last_id + 1;
	}
	return newId;
}

int
PGR_Send_Input_Message(char cmdType,StringInfo input_message)
{
	int len = 0;
	char * ptr = NULL;
	char * result = NULL;

	if (input_message == NULL)
	{
		return STATUS_ERROR;
	}
	if (PGR_Is_Replicated_Query == true)
	{
		return STATUS_OK;
	}
	len = input_message->len+1;
	ptr = input_message->data;

	/* check setting of configuration value */
	if ( PGRnotReplicatePreparedSelect == true)
	{
		if (is_concerned_with_prepared_select(cmdType, ptr+1) == true)
		{
			return STATUS_OK;
		}
	}
	result = PGR_Send_Replicate_Command(ptr,len, CMD_STS_PREPARE,cmdType,"0",0);
	if (result != NULL)
	{
		PGR_Reload_Start_Time();
		free(result);
		result = NULL;
		return STATUS_OK;
	}
	else
	{
		return STATUS_ERROR;
	}
}

static bool
is_concerned_with_prepared_select(char cmdType, char * query_string)
{
	if (cmdType == CMD_TYPE_P_PARSE)
	{
		switch (parse_message(query_string))
		{
			case PGR_MESSAGE_SELECT:
				pgr_skip_in_prepared_query = true;
				break;	
			case PGR_MESSAGE_PREPARE:
				if (is_prepared_as_select(query_string) == true)
				{
					pgr_skip_in_prepared_query = true;
				}
				break;	
			case PGR_MESSAGE_EXECUTE:
			case PGR_MESSAGE_DEALLOCATE:
				if (is_statement_as_select(query_string) == true)
				{
					pgr_skip_in_prepared_query = true;
				}
				break;	
		}
		if (pgr_skip_in_prepared_query == true)
		{
			return true;
		}
	}
	if (pgr_skip_in_prepared_query == true)
	{
		if (cmdType == CMD_TYPE_P_SYNC)
		{
			pgr_skip_in_prepared_query = false;
		}
		return true;
	}
	return false;
}

static int
skip_non_blank(char * ptr, int max)
{
	int i= 0;
	while(!isspace(*(ptr+i)))
	{
		if ((*(ptr+1) == '(') || (*(ptr+1) == ')'))
		{
			return i;
		}
		i++;
		if (i > max)
			return -1;
	}
	return i;
}

static int
skip_blank(char * ptr, int max)
{
	int i = 0;
	while(isspace(*(ptr+i)))
	{
		i++;
		if (i > max)
			return -1;
	}
	return i;
}

static int
parse_message(char * query_string)
{
	char * ptr =NULL;
	int rtn = 0;
	int i = 0;
	int len = 0;
	if (query_string == NULL)
	{
		return PGR_MESSAGE_OTHER;
	}
	len = strlen (query_string);
	if (len <= 0)
	{
		return PGR_MESSAGE_OTHER;
	}
	ptr = (char *)query_string;
	i = 0;
	/* skip space */
	rtn = skip_blank(ptr+i, len-i);
	if (rtn < 0)
		return PGR_MESSAGE_OTHER;
	i += rtn;

	if (!strncasecmp(ptr+i,"SELECT",strlen("SELECT")))
	{
		return PGR_MESSAGE_SELECT;
	}
	if (!strncasecmp(ptr+i,"PREPARE",strlen("PREPARE")))
	{
		return PGR_MESSAGE_PREPARE;
	}
	if (!strncasecmp(ptr+i,"EXECUTE",strlen("EXECUTE")))
	{
		return PGR_MESSAGE_EXECUTE;
	}
	if (!strncasecmp(ptr+i,"DEALLOCATE",strlen("DEALLOCATE")))
	{
		return PGR_MESSAGE_DEALLOCATE;
	}
	return PGR_MESSAGE_OTHER;
}

static bool
is_prepared_as_select(char * query_string)
{
	char * ptr =NULL;
	int rtn = 0;
	int i = 0;
	int len = 0;
	int args =0;
	if (query_string == NULL)
	{
		return false;
	}
	ptr = (char *)query_string;
	len = strlen (query_string);
	i = 0;
	/* skip "PREPARE" word */
	rtn = skip_non_blank(ptr+i, len-i);
	if (rtn < 0)
		return false;
	i += rtn;
	/* skip space */
	rtn = skip_blank(ptr+i, len-i);
	if (rtn < 0)
		return false;
	i += rtn;
	/* skip plan_name */
	rtn = skip_non_blank(ptr+i, len-i);
	if (rtn < 0)
		return false;
	i += rtn;
	/* skip space */
	rtn = skip_blank(ptr+i, len-i);
	if (rtn < 0)
		return false;
	i += rtn;
	/* skip args */
	args = 0;
	if (*(ptr+i) == '(')
	{
		args ++;
		i++;
		while(args > 0)
		{
			if (*(ptr+i) == ')')
				args --;
			else if (*(ptr+i) == '(')
				args ++;
			i++;
			if (i >= len) 
				return false;
		}
		/* skip space */
		rtn = skip_blank(ptr+i, len-i);
		if (rtn < 0)
			return false;
		i += rtn;
	}
	/* skip "AS" word */
	i += strlen("AS");
	if (i >= len) 
		return false;
	/* skip space */
	rtn = skip_blank(ptr+i, len-i);
	if (rtn < 0)
		return false;
	i += rtn;
	/* check "SELECT" word */
	if (len-i < strlen("SELECT"))
		return false;
	if (!strncasecmp(ptr+i,"SELECT",strlen("SELECT")))
	{
		return true;
	}
	return false;
	
}

static bool
is_statement_as_select(char * query_string)
{
	char * ptr =NULL;
	int rtn = 0;
	int i = 0;
	int j = 0;
	int len = 0;
	bool result = false;
	PrepareStmt stmt;
	char * name = NULL;
	if (query_string == NULL)
	{
		return false;
	}
	ptr = (char *)query_string;
	len = strlen (query_string);
	i = 0;
	/* skip "EXECUTE" or "DEALLOCATE" word */
	rtn = skip_non_blank(ptr+i, len-i);
	if (rtn < 0)
		return false;
	i += rtn;
	/* skip space */
	rtn = skip_blank(ptr+i, len-i);
	if (rtn < 0)
		return false;
	i += rtn;
	if ((name = malloc(len)) == NULL)
		return false;
	memset(name,0,len);
	j = 0;
	while(isalnum(*(ptr+i)))
	{
		*(name+j) = *(ptr+i);
		i++;
		j++;
		if (i > len)
			return false;
	}
	stmt.name = name;
	result = PGR_is_select_prepared_statement(&stmt);
	free(name);
	return result;
}

bool
PGR_is_select_prepare_query(void)
{
	if (debug_query_string == NULL)
	{
		return false;
	}
	return (is_prepared_as_select((char *)debug_query_string));
}

char *
PGR_get_md5salt(char * md5Salt, char * string)
{
	char buf[24];
	char * ptr = NULL;
	int len = 0;
	int i = 0;
	int cnt = 0;
	int index = 0;
	bool set_flag = false;

	ptr = (char *)md5Salt;
	len = strlen(string);
	for ( i = 0 ; i < len ; i ++)
	{
		if (*(string+i) == ')')
		{
			buf[index++] = '\0';
			*ptr = (char)atoi(buf);
			set_flag = false;
		}
		if (set_flag)
		{
			buf[index++] = *(string+i);
		}
		if (*(string+i) == '(')
		{
			set_flag = true;
			index = 0;
			ptr = (char *)(md5Salt + cnt);
			cnt++;
		}
	}
	return md5Salt;
}

#endif /* USE_REPLICATION */
