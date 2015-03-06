/*--------------------------------------------------------------------
 * FILE:
 *     pgreplicate.h
 *
 * Portions Copyright (c) 2003-2008, Atsushi Mitani
 *--------------------------------------------------------------------
 */
#ifndef PGREPLICATE_H
#define PGREPLICATE_H

#define PGREPLICATE_VERSION		"1.9.0rc5"

#include "lib/dllist.h"
#include "lib/stringinfo.h"
#include "../libpgc/libpgc.h"

/* cascade packet id */
#define	CMD_SYS_CASCADE	'C'
#define CMD_STS_TO_UPPER 'U'
#define CMD_STS_TO_LOWER 'L'
#define CMD_TYPE_ADD 'A'
#define CMD_TYPE_DELTE 'D'
#define CMD_TYPE_UPDATE_ALL 'A'

/* log packet id */
#define	CMD_SYS_LOG	'L'
#define CMD_STS_DELETE_QUERY 'q'
#define CMD_STS_DELETE_TRANSACTION 't'
#define CMD_STS_UPDATE_QUERY 'r'
#define CMD_STS_UPDATE_TRANSACTION 'u'

#define INIT_TRANSACTION_TBL_NUM (12)
#define FILENAME_MAX_LENGTH	(256)
#define MAX_DB_SERVER	(32)
#define	MAX_CONNECTIONS	(128)
#define MAX_QUEUE_FILE_SIZE (0x40000000)
#define PGR_MAX_TICKETS (0x7FFFFFFF)
#define PGR_MAX_QUERY_ID (0x7FFFFFFF)
#define PGR_CONNECT_RETRY_TIME  (3)
#define PGR_EXEC_RETRY_TIME  (5)
#define DB_TBL_FREE	(0)
#define DB_TBL_INIT	(1)
#define DB_TBL_USE	(2)
#define DB_TBL_ERROR	(-1)
#define DB_TBL_TOP	(10)
#define DB_TBL_END	(11)
#define RECOVERY_FILE_MTYPE	(1)
#define QUERY_LOG_MTYPE (2)
#define PGREPLICATE_CONF_FILE	"pgreplicate.conf"
#define PGREPLICATE_LOG_FILE	"pgreplicate.log"
#define PGREPLICATE_STATUS_FILE	"pgreplicate.sts"
#define PGREPLICATE_PID_FILE	"pgreplicate.pid"
#define PGREPLICATE_RID_FILE	"pgreplicate.rid"
#define RECOVERY_QUEUE_FILE	"pgr_recovery"
/* setup data tag of the configuration file */
#define CLUSTER_SERVER_TAG	"Cluster_Server_Info"
#define LOAD_BALANCE_SERVER_TAG	"LoadBalance_Server_Info"
#define REPLICATE_PORT_TAG	"Replication_Port"
#define RECOVERY_PORT_TAG	"Recovery_Port"
#define LIFECHECK_PORT_TAG	"LifeCheck_Port"
#define RLOG_PORT_TAG		"RLOG_Port"
#define RESPONSE_MODE_TAG	"Response_Mode"
#define	RESPONSE_MODE_FAST	"fast"
#define	RESPONSE_MODE_NORMAL	"normal"
#define	RESPONSE_MODE_RELIABLE	"reliable"
#define	USE_REPLICATION_LOG_TAG	"Use_Replication_Log"
#define	RESERVED_CONNECTIONS_TAG	"Reserved_Connections"
/* semapho numner of recovery queue */
#define SEM_NUM_OF_RECOVERY	(1)
#define SEM_NUM_OF_RECOVERY_QUEUE	(2)
/* semapho numner of lock tickets */
#define SEM_NUM_OF_LOCK	(1)
#define STATUS_LOCK_CONFLICT (2)
#define STATUS_DEADLOCK_DETECT (3)
#define STATUS_ABORTED (4)
#define STATUS_NOT_YET_REPLICATE (5)
#define STATUS_ALREADY_REPLICATED (6)
#define STATUS_SKIP_REPLICATE (7)
#define PGR_NOWAIT_ANSWER (0)
#define PGR_WAIT_ANSWER (1)
#define LOOP_CONTINUE	(0)
#define LOOP_END	(1)
#define LOWER_CASCADE	(1)
#define UPPER_CASCADE	(2)
#define ALL_CASCADE	(3)
#define NOTICE_SYSTEM_CALL_TYPE (10)
#define RECOVERY_QUERY_TYPE (20)

#define PGR_TIME_OUT	(60)
#define PGR_LOGIN_TIMEOUT	(15)
#define PGR_SEND_RETRY_CNT (100)
#define PGR_SEND_WAIT_MSEC (500)
#define PGR_RECV_RETRY_CNT (100)
#define PGR_RECV_WAIT_MSEC (500)
#define PGR_SEM_UNLOCK_WAIT_MSEC (100)
#define PGR_SEM_LOCK_WAIT_MSEC (500)
#define PGR_RECOVERY_RETRY_CNT	(6000)
#define PGR_RECOVERY_WAIT_MSEC	(500)
#define PGR_CHECK_POINT    (300)

#define PGR_RECOVERY_1ST_STAGE	(1)
#define PGR_RECOVERY_2ND_STAGE	(2)

#define IDLE_MODE	(0)
#define BUSY_MODE	(1)

/*
 * connection table for transaction query
 */
typedef struct {
	int useFlag;
	int lock;
	int transaction_count;
	unsigned short port;
	unsigned short pid;
	unsigned int hostIP;
	unsigned int srcHostIP;
	char host[HOSTNAME_MAX_LENGTH];
	char srcHost[HOSTNAME_MAX_LENGTH];
	char dbName[DBNAME_MAX_LENGTH];
	PGconn	* conn;
	bool in_transaction;
	bool exec_copy;
}TransactionTbl;

/*
 * cluster server table
 */
typedef struct {
	int useFlag;
	char hostName[HOSTNAME_MAX_LENGTH];
	char resolvedName[24];
	int port;
	int recoveryPort;
	int hostNum;
	int transaction_count;
	int retry_count;
}HostTbl;


typedef struct {
	FILE * queue_fp;
	int current_queue_no;
} RecoveryQueueInf;


/*
 * host table for recovery request
 */
typedef struct {
	char hostName[HOSTNAME_MAX_LENGTH];
	char resolvedName[24];
	int port;
	int recoveryPort;
	int sock;
	int recovery_sock;
} RecoveryTbl;

/*
 * status table for recovery
 */
typedef struct {
	int useFlag;
	int transaction_count;
	int recovery_status;
	unsigned int replication_id;
	HostTbl target_host;
	int read_queue_no;
	int write_queue_no;
	int check_point;
	unsigned int file_size;
	char write_file[FILENAME_MAX_LENGTH];
	char read_file[FILENAME_MAX_LENGTH];
} RecoveryStatusInf;

typedef struct {
	long mtype;
	char mdata[1];
} RecoveryQueueFile;

typedef struct {
	long mtype;
	unsigned int replicationId;
	char mdata[1];
} RecoveryQueueQuery;

typedef struct {
	unsigned int entry_ticket;
	unsigned int lock_wait_queue_length;
	int overflow;
} LockWaitInf;

typedef struct {
	int response_mode;
	int current_cluster;
} ResponseInf;

typedef struct {
	ReplicateHeader * header;
	char * query;
	char * next;
	char * last;
} QueryLogType;

typedef struct {
	ReplicateServerInfo * top;
	ReplicateServerInfo * end;
	ReplicateServerInfo * lower;
	ReplicateServerInfo * upper;
	ReplicateServerInfo * myself;
	int useFlag;
} CascadeInf;

typedef struct {
	union 
	{
		int useFlag;
		int commit_log_num;
	} inf;
	ReplicateHeader header;
} CommitLogInf;

typedef struct {
	int useFlag;
	char * RLog_Sock_Path;
	uint16_t RLog_Port_Number;
	int r_log_sock;
	ReplicateHeader * header;
	char * query;
} ReplicationLogInf;

typedef struct {
	char hostName[HOSTNAME_MAX_LENGTH];
	uint16_t port;
	uint16_t pid;
	uint32_t request_id;
} QueryLogID; 

typedef struct {
	QueryLogID query_log_id;
	char * last;
	char * next;
} ConfirmQueryList;

typedef struct {
	ReplicateHeader * header;
	char * query;
	int dest;
	int current_cluster;
	int transaction_count;
	HostTbl * host_ptr;
	TransactionTbl *transaction_tbl;
	//#ifdef PGGRID
	bool returningTuples;
	//#endif
} ThreadArgInf;

/* replication server data */
extern char * ResolvedName;
extern uint16_t Port_Number;
extern uint16_t LifeCheck_Port_Number;
extern uint16_t Recovery_Port_Number;
extern int Reserved_Connections;
extern bool PGR_Parse_Session_Started;
extern int PGR_Replication_Timeout;

/* global tables */
extern HostTbl * Host_Tbl_Begin;
extern Dllist * Transaction_Tbl_Begin;
extern TransactionTbl * Transaction_Tbl_End;
extern RecoveryTbl * LoadBalanceTbl;
extern RecoveryStatusInf * Recovery_Status_Inf;
extern LockWaitInf * Lock_Wait_Tbl;
extern ReplicateHeader * PGR_Log_Header;
extern ReplicateServerInfo * Cascade_Tbl;
extern CascadeInf * Cascade_Inf;
extern CommitLogInf * Commit_Log_Tbl;
extern QueryLogType * Query_Log_Top;
extern QueryLogType * Query_Log_End;
extern ReplicationLogInf * Replicateion_Log;
extern int RecoveryShmid;
extern int ReplicateSerializationShmid;
extern int RecoveryMsgShmid;
extern int *RecoveryMsgid;
extern int HostTblShmid;
extern int LockWaitTblShmid;
extern int CascadeTblShmid;
extern int CascadeInfShmid;
extern int CommitLogShmid;
extern int MaxBackends;
extern char * PGR_Result;
extern int SemID;
extern int RecoverySemID;
extern int CascadeSemID;
extern int LockSemID;
extern int VacuumSemID;
extern char * PGR_Data_Path;
extern char * PGR_Write_Path;
extern FILE * LogFp;
extern FILE * StatusFp;
extern FILE * RidFp;
extern FILE * QueueFp;
extern int Log_Print;
extern int Debug_Print;
extern char * Function;
extern int IS_SESSION_AUTHORIZATION;
extern ResponseInf * PGR_Response_Inf;
extern bool StartReplication[MAX_DB_SERVER];
extern bool PGR_Cascade;
extern bool	PGR_Use_Replication_Log;
extern bool	PGR_AutoCommit;
extern unsigned int * PGR_ReplicateSerializationID;
extern unsigned int * PGR_Send_Query_ID;
extern unsigned int PGR_Query_ID;
extern volatile bool exit_processing;
extern RecoveryQueueInf RecoveryQueue;
extern int pgreplicate_pid;
extern char * PGRuserName;
extern int exit_signo;

extern int ReplicateSock;

/* smart shutdown */
extern int Idle_Flag;
extern volatile bool Exit_Request;

/*
 * external prototype in main.c
 */
extern void child_wait(SIGNAL_ARGS);

/*
 * external prototype in conf.c
 */
extern int PGRget_Conf_Data(char * path);

/*
 * external prototype in replicate.c
 */
extern int PGRset_replication_id(uint32_t rid);
//extern bool PGRis_same_host(char * host1, unsigned short port1 , char * host2, unsigned short port2);
extern HostTbl * PGRadd_HostTbl(HostTbl *  conf_data, int useFlag);
extern HostTbl * PGRget_master(void);
extern void PGRset_recovery_status(int status);
extern int PGRget_recovery_status(void);
extern int PGRcheck_recovered_host(void);
extern int PGRset_recovered_host(HostTbl * target,int useFlag);
extern int PGRinit_recovery(void);
extern void PGRexit_subprocess(int signo);
extern void PGRreplicate_exit(int exit_status);
extern int PGRsend_replicate_packet_to_server( HostTbl * host_ptr, ReplicateHeader * header, char *query , char * result,unsigned int replicationId, bool recovery);
extern int PGRreplicate_packet_send_each_server( HostTbl * ptr, bool return_response, ReplicateHeader * header, char * query,int dest);
extern HostTbl * PGRget_HostTbl(char * hostName,int port);
extern int PGRset_queue(ReplicateHeader * header,char * query);
extern int PGRset_host_status(HostTbl * host_ptr,int status);
extern void PGRclear_connections(void);
extern void PGRdestroy_transaction_table(void);
extern void PGRsem_unlock( int semid, short sem_num );
extern void PGRsem_lock( int semid, short sem_num );
extern int PGRdo_replicate(int sock,ReplicateHeader *header, char * query);
extern int PGRreturn_result(int dest, char * result, int wait);
extern int PGRreplicate_packet_send( ReplicateHeader * header, char * query,int dest,int recovery_status);
extern char * PGRread_packet(int sock, ReplicateHeader *header);
extern void PGRnotice_replication_server(char * hostName, unsigned short portNumber,unsigned short recoveryPortNumber, unsigned short lifecheckPortNumber, char * userName);
extern char * PGRread_query(int sock, ReplicateHeader *header);
extern int PGRsync_oid(ReplicateHeader *header);
extern unsigned int PGRget_next_query_id(void);
extern int PGRinit_transaction_table(void);
extern int replicate_packet_send_internal(ReplicateHeader * header, char * query,int dest,int recovery_status,bool isHeldLock);
extern int PGRsync_oid(ReplicateHeader *header);
extern int PGRload_replication_id(void);
extern PGconn * PGRcreateConn( char * host, char * port,char * database, char * userName, char * password, char * md5Salt, char * cryptSalt ,int timeout);
/*
 * external prototype in recovery.c
 */
extern int PGRsend_load_balance_packet(RecoveryPacket * packet);
extern void PGRrecovery_main(int fork_wait_time);
extern FILE * PGRget_recovery_queue_file_for_write(void);
extern FILE * PGRget_recovery_queue_file_for_read(int next);

/*
 * external prototype in rlog.c
 */
extern int PGRwrite_rlog(ReplicateHeader * header, char * query);
extern ReplicateHeader * PGRget_requested_query(ReplicateHeader * header);
extern void PGRreconfirm_commit(int sock, ReplicateHeader * header);
extern void PGRset_rlog(ReplicateHeader * header, char * query);
extern void PGRunset_rlog(ReplicateHeader * header, char * query);
extern int PGRresend_rlog_to_db(void);
extern void PGRreconfirm_query(int sock, ReplicateHeader * header);
extern pid_t  PGR_RLog_Main(void);
extern int PGRcreate_send_rlog_socket(void);
extern int PGRsend_rlog_packet(int sock,ReplicateHeader * header, const char * query_string);
extern int PGRrecv_rlog_result(int sock,void * result, int size);
extern int PGRsend_rlog_to_local(ReplicateHeader * header,char * query);
extern int PGRget_rlog_header(ReplicateHeader * header);

/*
 * external prototype in cascade.c
 */
extern int PGRstartup_cascade(void);
extern int PGRsend_lower_cascade(ReplicateHeader * header, char * query);
extern int PGRsend_upper_cascade(ReplicateHeader * header, char * query);
extern int PGRwait_answer_cascade(int  sock);
extern ReplicateServerInfo * PGRget_lower_cascade(void);
extern ReplicateServerInfo * PGRget_upper_cascade(void);
extern void PGRset_cascade_server_status(ReplicateServerInfo * cascade, int status);
extern ReplicateServerInfo * PGRrecv_cascade_answer(ReplicateServerInfo * cascade,ReplicateHeader * header);
extern int PGRsend_cascade(int sock , ReplicateHeader * header, char * query);
extern int PGRcascade_main(int sock, ReplicateHeader * header, char * query);
extern int PGRwait_notice_rlog_done(void);
extern int PGRsend_notice_rlog_done(int sock);
extern int PGRsend_notice_quit(void);

/*
 * external prototype in pqformat.c
 */
extern const char * pq_getmsgstring(StringInfo msg);
extern unsigned int pq_getmsgint(StringInfo msg, int b);
extern void pq_copymsgbytes(StringInfo msg, char *buf, int datalen);
extern const char * pq_getmsgbytes(StringInfo msg, int datalen);
extern int pq_getmsgbyte(StringInfo msg);

/*
 * external prototype in lifecheck.c
 */
extern int PGRlifecheck_main(int fork_wait_time);

#endif /* PGREPLICATE_H */
