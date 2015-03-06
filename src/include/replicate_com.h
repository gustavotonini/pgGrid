/*-------------------------------------------------------------------------
 *
 * replicate.h
 *	  Primary include file for replicate server .c files
 *
 * This should be the first file included by replicate modules.
 *
 *-------------------------------------------------------------------------
 */
#ifndef REPLICATE_COM_H
#define	REPLICATE_COM_H 1

#ifndef _SYS_TYPES_H
#include <sys/types.h>
#endif
#ifndef _INTTYPES_H
#include <inttypes.h>
#endif
#ifndef _NETINET_IN_H
#include <netinet/in.h>
#endif

#include "c.h"
#include "pg_config.h"

/* default values */
#define DEFAULT_PGLB_PORT	(6001)
#define DEFAULT_PGLB_RECOVERY_PORT	(6101)
#define DEFAULT_PGLB_LIFECHECK_PORT	(6201)
#define DEFAULT_CLUSTER_PORT	(5432)
#define DEFAULT_CLUSTER_RECOVERY_PORT	(7101)
#define DEFAULT_CLUSTER_LIFECHECK_PORT	(7201)
#define DEFAULT_PGRP_PORT	(8001)
#define DEFAULT_PGRP_RECOVERY_PORT	(8101)
#define DEFAULT_PGRP_LIFECHECK_PORT	(8201)
#define DEFAULT_PGRP_RLOG_PORT	(8301)
#define MAX_DB_SERVER	(32)

/**************************
*                         *
*   Packet ID definition  *
*                         *
***************************/
/*=========================
	Replication packet id
===========================*/
#define	CMD_SYS_REPLICATE	'R'
/*-------------------------
	Simple Query
--------------------------*/
#define CMD_STS_SET_SESSION_AUTHORIZATION	'S'
#define	CMD_STS_TRANSACTION	'T'
#define	CMD_STS_TEMP_TABLE	'E'
#define	CMD_STS_QUERY	'Q'
#define	CMD_STS_OTHER	'O'

#define CMD_TYPE_VACUUM	'V'
#define CMD_TYPE_ANALYZE	'A'
#define CMD_TYPE_REINDEX	'N'
#define CMD_TYPE_SELECT	'S'
#define CMD_TYPE_EXPLAIN	'X'
#define CMD_TYPE_SET	'T'
#define CMD_TYPE_RESET	't'
#define CMD_TYPE_INSERT	'I'
#define CMD_TYPE_DELETE	'D'
#define CMD_TYPE_EXECUTE	'U'
#define CMD_TYPE_UPDATE	'U'
#define CMD_TYPE_BEGIN	'B'
#define CMD_TYPE_COMMIT	'E'
#define CMD_TYPE_ROLLBACK	'R'
#define CMD_TYPE_CONNECTION_CLOSE	'x'
#define CMD_TYPE_SESSION_AUTHORIZATION_BEGIN	'a'
#define CMD_TYPE_SESSION_AUTHORIZATION_END	'b'
#define CMD_TYPE_SAVEPOINT	's'
#define CMD_TYPE_ROLLBACK_TO_SAVEPOINT	'r'
#define CMD_TYPE_RELEASE_SAVEPOINT	'l'
#define CMD_TYPE_OTHER	'O'

/*=========================
	System call packet id
===========================*/
#define CMD_SYS_CALL		'S'
#define CMD_SYS_PREREPLICATE		'Z'

#define	CMD_STS_NOTICE	'N'
#define	CMD_STS_RESPONSE	'R'
#define	CMD_STS_TRANSACTION_ABORT	'A'
#define	CMD_STS_QUERY_SUSPEND	'P'
#define	CMD_STS_QUERY_DONE	'D'

#define CMD_TYPE_COMMIT_CONFIRM	'c'
#define CMD_TYPE_QUERY_CONFIRM	'q'
#define CMD_TYPE_DEADLOCK_DETECT	'd'
#define CMD_TYPE_FRONTEND_CLOSED	'x'

/*----------------------------
	Copy Command
------------------------------*/
#define	CMD_STS_COPY	'C'

#define CMD_TYPE_COPY	'C'
#define CMD_TYPE_COPY_DATA	'd'
#define CMD_TYPE_COPY_DATA_END	'e'

/*----------------------------
	Large Object
------------------------------*/
#define	CMD_STS_LARGE_OBJECT	'L'

#define CMD_TYPE_LO_IMPORT	'I'
#define CMD_TYPE_LO_CREATE	'C'
#define CMD_TYPE_LO_OPEN	'O'
#define CMD_TYPE_LO_WRITE	'W'
#define CMD_TYPE_LO_LSEEK	'S'
#define CMD_TYPE_LO_CLOSE	'X'
#define CMD_TYPE_LO_UNLINK	'U'

/*-------------------------
	Prepare/Params Query
--------------------------*/
#define CMD_STS_PREPARE	'P'

#define CMD_TYPE_P_PARSE	'P'
#define CMD_TYPE_P_BIND		'B'
#define CMD_TYPE_P_EXECUTE	'E'
#define CMD_TYPE_P_FASTPATH	'F'
#define CMD_TYPE_P_CLOSE	'C'
#define CMD_TYPE_P_DESCRIBE	'D'
#define CMD_TYPE_P_FLUSH	'H'
#define CMD_TYPE_P_SYNC		'S'

/*=========================
	Lifecheck packet id
===========================*/
#define CMD_SYS_LIFECHECK		'W'
#define	CMD_STS_LOADBALANCER	'A'
#define	CMD_STS_CLUSTER			'B'
#define	CMD_STS_REPLICATOR		'C'

#define PGR_TRANSACTION_SOCKET	(0)
#define PGR_QUERY_SOCKET	(1)

#define	DATA_FREE	(0)
#define	DATA_INIT	(1)
#define	DATA_USE	(2)
#define	DATA_ERR	(90)
#define	DATA_END	(-1)
#define HOSTNAME_MAX_LENGTH     (128)
#define DBNAME_MAX_LENGTH       (128)
#define USERNAME_MAX_LENGTH     (128)
#define PASSWORD_MAX_LENGTH		(128)
#define TABLENAME_MAX_LENGTH     (128)
#define PATH_MAX_LENGTH        (256)
#define MAX_SERVER_NUM         (128)
#define MAX_RETRY_TIMES	(3)
#define MAX_SOCKET_QUEUE	(100000)
#define TRANSACTION_ERROR_RESULT	"TRANSACTION_ERROR"
#define REPLICATE_SERVER_SHM_KEY (1020)
/* target -> replicate */
#define RECOVERY_PREPARE_REQ	(1)
/* replicate  -> master */
#define RECOVERY_PGDATA_REQ	(2)
/* master -> replicate */
#define RECOVERY_PGDATA_ANS	(3)
/* replicate -> target */
#define RECOVERY_PREPARE_ANS	(4)
/* target -> replicate */
#define RECOVERY_START_REQ	(5)
/* replicate  -> master */
#define RECOVERY_FSYNC_REQ	(6)
/* master -> replicate */
#define RECOVERY_FSYNC_ANS	(7)
/* replicate -> target */
#define RECOVERY_START_ANS	(8)
/* target -> replicate */
#define RECOVERY_QUEUE_DATA_REQ	(9)
/* replicate -> target */
#define RECOVERY_QUEUE_DATA_ANS	(10)
/* target -> replicate */
#define RECOVERY_FINISH	(11)

#define RECOVERY_ERROR_OCCUPIED	(100)
#define RECOVERY_ERROR_CONNECTION	(101)
#define RECOVERY_ERROR_TARGET_ONLY	(102)
#define RECOVERY_ERROR_ANS	(200)

/* lifecheck ask from cluster db */
#define LIFECHECK_ASK_FROM_CLUSTER	(1)
/* lifecheck response from replication server */
#define LIFECHECK_RES_FROM_REPLICATOR	(2)
/* lifecheck ask from replication server */
#define LIFECHECK_ASK_FROM_REPLICATOR	(3)
/* lifecheck response from cluster db */
#define LIFECHECK_RES_FROM_CLUSTER	(4)

#define REPLICATION_SERVER_INFO_TAG "Replicate_Server_Info"
#define HOST_NAME_TAG	"Host_Name"
#define PORT_TAG	"Port"
#define RECOVERY_PORT_TAG	"Recovery_Port"
#define LIFECHECK_PORT_TAG  "LifeCheck_Port"
#define TIMEOUT_TAG  "Replication_Timeout"
#define LIFECHECK_TIMEOUT_TAG  "LifeCheck_Timeout"
#define LIFECHECK_INTERVAL_TAG  "LifeCheck_Interval"

#define RECOVERY_INIT	(0)
#define RECOVERY_PREPARE_START	(1)
#define RECOVERY_START_1	(2)
#define RECOVERY_CLEARED	(3)
#define RECOVERY_WAIT_CLEAN (10)
#define RECOVERY_ERROR	(99)

/* response mode */
#define PGR_FAST_MODE	(0)
#define PGR_NORMAL_MODE	(1)
#define PGR_RELIABLE_MODE	(2)

#define RECOVERY_TIMEOUT	(600)
#ifndef COMPLETION_TAG_BUFSIZE
#define	COMPLETION_TAG_BUFSIZE (128)
#endif

/* replicate log type */
#define FROM_R_LOG_TYPE	(1)
#define FROM_C_DB_TYPE	(2)
#define CONNECTION_SUSPENDED_TYPE	(3)

#define PGR_SYSTEM_COMMAND_FUNC	"PGR_SYSTEM_COMMAND_FUNCTION"
#define PGR_STARTUP_REPLICATION_SERVER_FUNC_NO	(1)
#define PGR_CHANGE_REPLICATION_SERVER_FUNC_NO	(2)
#define PGR_SET_CURRENT_TIME_FUNC_NO	(3)
#define PGR_NOTICE_DEADLOCK_DETECTION_FUNC_NO	(4)
#define PGR_TRANSACTION_CONFIRM_ANSWER_FUNC_NO	(5)
#define PGR_RELIABLE_MODE_DONE_FUNC_NO		(6)
#define PGR_NOTICE_ABORT_FUNC_NO		(7)
#define PGR_SET_CURRENT_REPLICATION_QUERY_ID_NO (8)
#define PGR_QUERY_CONFIRM_ANSWER_FUNC_NO	(9)
#define PGR_GET_OID_FUNC_NO		(10)
#define PGR_SET_OID_FUNC_NO		(11)
//#ifdef PGGRID
#define PGR_RETURN_TUPLES_NO		(12)
#define PGR_RETURN_TUPLES_MSG  "12"
//#endif

#define PGR_CMD_ARG_NUM	(10)
#define PGR_LOCK_CONFLICT_NOTICE_CMD	"PGR_LOCK_CONFLICT_NOTICE_CMD"
#define PGR_DEADLOCK_DETECT_NOTICE_CMD	"PGR_DEADLOCK_DETECT_NOTICE_CMD"
#define PGR_QUERY_DONE_NOTICE_CMD		"PGR_QUERY_DONE_NOTICE_CMD"
#define PGR_QUERY_ABORTED_NOTICE_CMD	"PGR_QUERY_ABORTED_NOTICE_CMD"
#define PGR_RETRY_LOCK_QUERY_CMD	"PGR_RETRY_LOCK_QUERY_CMD"
#define PGR_NOT_YET_REPLICATE_NOTICE_CMD	"PGR_NOT_YET_REPLICATE_NOTICE_CMD"
#define PGR_ALREADY_REPLICATED_NOTICE_CMD	"PGR_ALREADY_REPLICATED_NOTICE_CMD"
#define PGR_NOT_YET_COMMIT		(0)
#define PGR_ALREADY_COMMITTED	(1)

#define COPYBUFSIZ	(8192)
#define MAX_WORDS	(24)
#define MAX_WORD_LETTERS	(48)
#define PGR_MESSAGE_BUFSIZE	(128)
#define INT_LENGTH	(12)
#define PGR_MAX_COUNTER	(0x0FFFFFFF)
#define PGR_GET_OVER_FLOW_FILTER	(0xF0000000)
#define PGR_GET_DATA_FILTER	(0x0FFFFFFF)
#define PGR_SET_OVER_FLOW	(0x10000000)
#define PGR_MIN_COUNTER (0x0000000F)

#define STRCMP(x,y)	(strncmp(x,y,strlen(y)))

/* life check target */
#define SYN_TO_LOAD_BALANCER	(0)
#define SYN_TO_CLUSTER_DB		(1)
#define SYN_TO_REPLICATION_SERVER	(2)
#define LIFE_CHECK_TRY_COUNT	(2)
#define LIFE_CHECK_STOP		(0)
#define LIFE_CHECK_START	(1)

#define PING_DB		"template1"
#define PING_QUERY	"SELECT 1"

//pggrid operation types
/*Normal pgcluster operation*/
#define PGGRID_OP_NORMAL 0
/*Distributed query*/
#define PGGRID_OP_DQUERY 1
/*Distributed count query*/
#define PGGRID_OP_DCOUNT 2

#ifndef HAVE_UNION_SEMUN
union semun {
	int val;
	struct semid_ds *buf;
	unsigned short int *array;
	struct seminfo *__buf;
};
#endif

typedef struct ReplicateHeaderType
{
	char cmdSys;
	char cmdSts;	/*
						Q:query 
						T:transaction
					*/
	char cmdType;	/*
						S:select
						I:insert
						D:delete
						U:update
						B:begin
						E:commit/rollback/end
						O:others
					*/
	char rlog;		/*
					-- kind of replication log --
						1: send from replication log
						2: send from cluster db (should be retry)
						3: connection suspended
					*/
	uint16_t port;
	uint16_t pid;
	uint32_t query_size;
	char from_host[HOSTNAME_MAX_LENGTH];
	char dbName[DBNAME_MAX_LENGTH];
	char userName[USERNAME_MAX_LENGTH];
	struct timeval tv;
	uint32_t query_id;
    int isAutoCommit; /* 0 if autocommit is off. 1 if autocommit is on */
	uint32_t request_id;
	uint32_t replicate_id;
	char password[PASSWORD_MAX_LENGTH];
	char md5Salt[4];
	char cryptSalt[2];
	char dummySalt[2];
	
	//fragment parameters
	//#ifdef PGGRID
	char sitehostname[HOSTNAME_MAX_LENGTH];
	uint16_t siteport;
	uint16_t siterecport;
	char pggrid_op_type; //see PGGRID_OP_*
	//#endif
} ReplicateHeader;

typedef struct RecoveryPacketType
{
	uint16_t packet_no;	/*	
					1:start recovery prepare
					2:ask pgdata
					3:ans pgdata
					4:send master info
					5:start queueing query
					6:requst fsync
					7:ready to fsync
					8:pepared master
					9:finished rsync
					*/
	uint16_t max_connect;
	uint16_t port;
	uint16_t recoveryPort;
	char hostName[HOSTNAME_MAX_LENGTH];
	char pg_data[PATH_MAX_LENGTH]; 
	char userName[USERNAME_MAX_LENGTH];
} RecoveryPacket;

typedef struct
{
	char table[128];
	int rec_no;
	char key[128];
	char value[128];
	char * last;
	char * next;
} ConfDataType;


typedef struct ReplicateServerInfoType
{
	uint32_t useFlag;
	char hostName[HOSTNAME_MAX_LENGTH];
	uint16_t portNumber;
	uint16_t recoveryPortNumber;
	uint16_t lifecheckPortNumber;
	uint16_t RLogPortNumber;
	int32_t sock;
	int32_t rlog_sock;
	uint32_t replicate_id;
	uint16_t response_mode;
	uint16_t retry_count;
} ReplicateServerInfo;


typedef struct ReplicateNowType
{
	uint32_t replicate_id;
	int useFlag;
	int use_seed;
	int use_time;
	int offset_sec;
	int offset_usec;
	struct timeval tp;
} ReplicateNow;

typedef struct CopyDataType
{
	int cnt;
	char copy_data[COPYBUFSIZ];
} CopyData;

typedef struct ClusterDBInfoType
{
	int status;
} ClusterDBInfo;

typedef struct
{
	uint32_t arg1;
	uint32_t arg2;
	uint32_t arg3;
	char buf[1];
} LOArgs;

typedef struct
{
	int length;
	char data[1];
} ArrayData;

extern ConfDataType * ConfData_Top;
extern ConfDataType * ConfData_End;
extern ReplicateServerInfo * ReplicateServerData;
extern ClusterDBInfo * ClusterDBData;
extern int ReplicateServerShmid;
extern int ClusterDBShmid;
extern bool PGR_Under_Replication_Server;
extern int PGR_Replication_Timeout;
extern int PGR_Lifecheck_Timeout;
extern int PGR_Lifecheck_Interval;

/* in backend/libpq/replicate_com.c */
extern int PGR_Create_Socket_Connect(int * fdP, char * hostName , unsigned short portNumber);
extern void PGR_Close_Sock(int * sock);
extern int PGR_Create_Socket_Bind(int * fdP, char * hostName , unsigned short portNumber);
extern int PGR_Create_Acception(int fd, int * sockP, char * hostName , unsigned short portNumber);
extern int PGR_Free_Conf_Data(void);
extern int PGR_Get_Conf_Data(char * dir , char * fname);
extern void PGRset_recovery_packet_no(RecoveryPacket * packet, int packet_no);
extern unsigned int PGRget_ip_by_name(char * host);
extern int PGRget_time_value(char *str);
extern int PGRget_bw_value(char *str);

extern void PGRwrite_log_file(FILE * fp, const char * fmt,...);
extern void show_debug(const char * fmt,...);
extern void show_error(const char * fmt,...);



#endif /* REPLICATE_COM_H */
