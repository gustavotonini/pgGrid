/*-------------------------------------------------------------------------
 *
 * replicate.h
 *	  Primary include file for replicate server .c files
 *
 * This should be the first file included by replicate modules.
 *
 *-------------------------------------------------------------------------
 */
#ifndef REPLICATE_H
#define	REPLICATE_H

#ifndef _SYS_TIME_H
#include <sys/time.h>
#endif
#include "tcop/dest.h"
#include "storage/proc.h"
#include "lib/stringinfo.h"
#include "utils/rel.h"
#include "replicate_com.h"

#define STAND_ALONE_TAG			"When_Stand_Alone"
#define NOT_REPLICATE_INFO_TAG	"Not_Replicate_Info"
#define DB_NAME_TAG				"DB_Name"
#define TABLE_NAME_TAG			"Table_Name"
#define RSYNC_PATH_TAG			"Rsync_Path"
#define RSYNC_OPTION_TAG		"Rsync_Option"
#define RSYNC_COMPRESS_TAG		"Rsync_Compress"
#define RSYNC_TIMEOUT_TAG		"Rsync_Timeout"
#define RSYNC_BWLIMIT_TAG		"Rsync_Bwlimit"
#define PG_DUMP_PATH_TAG		"Pg_Dump_Path"
#define PING_PATH_TAG			"Ping_Path"

#define CLUSTER_CONF_FILE		"cluster.conf"
#define DEFAULT_RSYNC			"/usr/bin/rsync"
#define DEFAULT_PING			"/bin/ping"
#define DEFAULT_PG_DUMP			"/usr/local/pgsql/bin/pg_dump"
#define	NOT_SESSION_AUTHORIZATION	(0)
#define SESSION_AUTHORIZATION_BEGIN	(1)
#define SESSION_AUTHORIZATION_END	(2)

#define READ_ONLY_IF_STAND_ALONE	"read_only"
#define READ_WRITE_IF_STAND_ALONE	"read_write"
#define PERMIT_READ_ONLY		(1)
#define PERMIT_READ_WRITE		(2)
#define STATUS_REPLICATED		(3)
#define STATUS_CONTINUE			(4)
#define STATUS_CONTINUE_SELECT	(5)
#define STATUS_NOT_REPLICATE	(6)
#define STATUS_SKIP_QUERY		(7)
#define STATUS_RECOVERY			(11)
#define STATUS_REPLICATION_ABORT	(98)
#define STATUS_DEADLOCK_DETECT	(99)

#define TO_REPLICATION_SERVER	(0)
#define TO_FRONTEND				(1)

#define PGR_DEADLOCK_DETECTION_MSG "deadlock detected!"
#define PGR_REPLICATION_ABORT_MSG "replication aborted!"
#define SKIP_QUERY_1 "begin; select getdatabaseencoding(); commit"
#define SKIP_QUERY_2 "BEGIN; SELECT usesuper FROM pg_catalog.pg_user WHERE usename = '%s'; COMMIT"
#define SKIP_QUERY_3 "SET autocommit TO 'on'"
#define SKIP_QUERY_4 "SET search_path = public"
#define SYS_QUERY_1 "set pgr_force_loadbalance to on" 

#define PGR_1ST_RECOVERY (1)
#define PGR_2ND_RECOVERY (2)
#define PGR_COLD_RECOVERY (1)
#define PGR_HOT_RECOVERY (2)
#define PGR_WITHOUT_BACKUP (3)

#define PGR_MESSAGE_OTHER (0)
#define PGR_MESSAGE_SELECT (1)
#define PGR_MESSAGE_PREPARE (2)
#define PGR_MESSAGE_EXECUTE (3)
#define PGR_MESSAGE_DEALLOCATE (4)

typedef struct
{
	bool is_stand_alone;
	int  permit;
} PGR_Stand_Alone_Type;

typedef struct
{
	char db_name[DBNAME_MAX_LENGTH];
	char table_name[TABLENAME_MAX_LENGTH];
} PGR_Not_Replicate_Type;

typedef struct
{
	bool check_lock_conflict;
	bool deadlock;
	int status_lock_conflict;
	int dest;
} PGR_Check_Lock_Type;

typedef struct
{
	char * query_string;
	int query_len;
	char cmdSts;
	char cmdType;
	char useFlag;
} PGR_Retry_Query_Type;


/* replicaition log */
typedef struct {
	uint32_t PGR_Replicate_ID;
	uint32_t PGR_Request_ID;
} PGR_ReplicationLog_Info;

typedef struct {
	char * password;
	char md5Salt[4];
	char cryptSalt[2];
} PGR_Password_Info;

extern char * Query_String;
extern int TransactionQuery;
extern int Transaction_Mode;
extern bool PGR_Noticed_Abort;
extern bool Session_Authorization_Mode;
extern bool Create_Temp_Table_Mode;
extern int RecoveryPortNumber;
extern char * RsyncPath;
extern bool RsyncCompress;
extern char * RsyncOption;
extern int PGR_Rsync_Timeout;
extern int PGR_Rsync_Bwlimit;
extern char * PgDumpPath;
extern char * PingPath;
extern int TransactionSock;
extern ReplicateNow * ReplicateCurrentTime;
extern CopyData * PGRCopyData;
extern bool PGR_Copy_Data_Need_Replicate;
extern PGR_Stand_Alone_Type * PGR_Stand_Alone;
extern PGR_Not_Replicate_Type * PGR_Not_Replicate;
extern int PGR_Not_Replicate_Rec_Num;
extern bool autocommit;
extern bool PGR_Is_Replicated_Query;

//#ifdef PGGRID
extern Relation PGR_target_temp_rel;
extern char** PGR_target_temp_rel_values;
extern unsigned int PGR_target_temp_rel_att_counter;
//#endif
extern PGR_Check_Lock_Type PGR_Check_Lock;
extern int PGR_Sock_To_Replication_Server;
extern bool PGR_Need_Notice;
extern bool PGR_Lock_Noticed;
extern bool PGR_Recovery_Option;
extern int PGR_recovery_mode;
extern ReplicateServerInfo * CurrentReplicateServer;
extern ReplicateServerInfo * LastReplicateServer;
extern char * PGRSelfHostName;
extern int PGR_Pending_Sem_Num;
extern int PGR_Response_Mode;
extern bool PGR_Reliable_Mode_Wait;
extern PGR_Retry_Query_Type PGR_Retry_Query;
extern bool needToUpdateReplicateIdOnNextQueryIsDone;
extern PGR_ReplicationLog_Info ReplicationLog_Info;
extern bool PGR_Not_Replication_Query;
extern bool PGR_Is_Sync_OID;
extern PGR_Password_Info * PGR_password;

/* backend/utils/misc/guc.c */
extern bool PGRforceLoadBalance;
extern bool	PGRcheckConstraintWithLock;
extern bool	PGRautoLockTable;
extern bool	PGRnotReplicatePreparedSelect;

/* in backend/libpq/replicate.c */
extern int PGR_Init_Replicate_Server_Data(void);
extern int PGR_Set_Replicate_Server_Socket(void);
extern int PGR_get_replicate_server_socket ( ReplicateServerInfo * sp , int socket_type );
extern ReplicateServerInfo * PGR_get_replicate_server_info(void);
extern ReplicateServerInfo * PGR_check_replicate_server_info(void);
extern char * 
//#ifdef PGGRID
PGR_Send_Replicate_Command(char * query_string, int query_len, char cmdSts ,char cmdType, char *sitehostname, uint16_t siteport);
/*#else
PGR_Send_Replicate_Command(char * query_string, int query_len, char cmdSts ,char cmdType);
#endif*/
extern bool PGR_Is_Replicated_Command(char * query);
extern int Xlog_Check_Replicate(int operation);
extern int PGR_Replicate_Function_Call(void);
extern void PGR_delete_shm(void);
extern int 
//#ifdef PGGRID
PGR_replication(char * query_string, CommandDest dest, Node *parsetree, const char * commandTag , char *sitehostname, uint16_t siteport);
/*#else
PGR_replication(char * query_string, CommandDest dest, Node *parsetree, const char * commandTag);
#endif*/

extern int PGR_getcountresult();

extern bool PGR_Is_System_Command(char * query);
extern int PGR_Call_System_Command(char * command);
extern int PGR_GetTimeOfDay(struct timeval *tp,struct timezone *tpz);
extern long PGR_Random(void);
extern int PGR_Set_Current_Time(char * sec, char * usec);
extern int PGR_Send_Copy(CopyData * copy, int end);
extern CopyData * PGR_Set_Copy_Data(CopyData * copy, char *str, int len, int end);
extern char * PGR_scan_terminate( char * str);
extern bool PGR_Is_Stand_Alone(void);
extern void PGR_Send_Message_To_Frontend(char * msg);
extern void PGR_Notice_Transaction_Query_Done(void);
extern void PGR_Notice_Transaction_Query_Aborted(void);
extern int PGRsend_system_command(char cmdSts, char cmdType);
extern int PGR_Notice_Conflict(void);
extern int PGR_Recv_Trigger (int user_timeout);
extern void PGR_Set_Replication_Server_Status( ReplicateServerInfo * sp, int status);
extern int PGR_Is_Skip_Replication(char * query);
extern bool PGR_Did_Commit_Transaction(void);
extern int PGR_Set_Transaction_Mode(int mode,const char * commandTag);
extern char * PGR_Remove_Comment(char * str);
extern void PGR_Force_Replicate_Query(void);
extern void PGR_Notice_DeadLock(void);
extern void PGR_Set_Cluster_Status(int status);
extern int PGR_Get_Cluster_Status(void);
extern bool PGR_Is_Life_Check(char * query);
extern int PGR_Check_Replicate_Server_Status(ReplicateServerInfo * sp);
extern int PGR_lo_import(char * filename);
extern int PGR_lo_create(int flags);
extern int PGR_lo_open(Oid lobjId,int32 mode);
extern int PGR_lo_close(int32 fd);
extern int PGR_lo_write(int fd, char *buf, int len);
extern int PGR_lo_lseek(int32 fd, int32 offset, int32 whence);
extern int PGR_lo_unlink(Oid lobjId);
extern uint32_t PGRget_replication_id(void);
extern Oid PGRGetNewObjectId(Oid last_id);
extern int PGR_Send_Input_Message(char cmdType,StringInfo input_message);
extern bool PGR_is_select_prepare_query(void);
extern char * PGR_get_md5salt(char * md5Salt, char * string);
extern int PGR_recv_replicate_result(int sock,char * result,int user_timeout);
extern int PGR_close_replicate_server_socket ( ReplicateServerInfo * sp , int socket_type );

/* in backend/libpq/recovery.c */
extern int PGR_Master_Main(void);
extern int PGR_Recovery_Main(int mode);
extern int PGR_recovery_error_send(void);
extern int PGR_recovery_finish_send(void);
extern int PGR_recovery_queue_data_req(void);
extern int PGR_measure_bandwidth(char * target);

/* in backend/libpq/lifecheck.c */
extern int PGR_Lifecheck_Main(void);

/* in backend/access/transam/xact.c */
extern void PGR_Reload_Start_Time(void);
#endif /* REPLICATE_H */
