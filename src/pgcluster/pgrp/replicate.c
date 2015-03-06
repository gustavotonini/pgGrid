/*--------------------------------------------------------------------
 * FILE:
 *     replicate.c
 *
 * NOTE:
 *     This file is composed of the functions to call with the source
 *     at pgreplicate for the replication.
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

//#ifdef PGGRID
//#include "access/htup.h"
//#include "access/tupdesc.h"
//#endif


#define IPC_NMAXSEM (32)

/*--------------------------------------
 * PROTOTYPE DECLARATION
 *--------------------------------------
 */
static void exit_waiting_loop(int sig);
static TransactionTbl * setTransactionTbl(HostTbl * host_ptr, ReplicateHeader * header);
static TransactionTbl * insertTransactionTbl( HostTbl * host_ptr, TransactionTbl * datap);
static TransactionTbl * getTransactionTbl( HostTbl * host_ptr, ReplicateHeader * header);
static void deleteTransactionTbl(HostTbl * host_ptr,ReplicateHeader * header);

static HostTbl * deleteHostTbl(HostTbl * ptr);
static bool is_master_in_recovery(char * host, int port,int recovery_status);
static void sem_quit(int semid);
static int send_cluster_status_to_load_balance(HostTbl * host_ptr,int status);
static void set_transaction_status(int status);
static void check_transaction_status(ReplicateHeader * header,TransactionTbl *transaction);
static HostTbl * check_host_transaction_status(ReplicateHeader * header,HostTbl *host );
static void clearHostTbl(void);
static bool is_need_sync_time(ReplicateHeader * header);
static bool is_need_wait_answer(ReplicateHeader * header);
static void write_host_status_file(HostTbl * host_ptr);

static void delete_template(HostTbl * ptr, ReplicateHeader * header);
static char * check_copy_command(char * query);
static int read_answer(int dest);
static bool is_autocommit_off(char * query);
static bool is_autocommit_on(char * query);
static unsigned int get_host_ip_from_tbl(char * host);
static unsigned int get_srcHost_ip_from_tbl(char * srcHost);

static int next_replication_id(void);
static void check_replication_id(void);
static bool is_need_use_rlog(ReplicateHeader * header);
static bool is_need_queue_jump( ReplicateHeader * header,char * query);
static int check_delete_transaction (HostTbl * host_ptr, ReplicateHeader * header);

static bool is_executed_query_in_origin( ReplicateHeader *header );
static bool is_executed_query( PGconn *conn,ReplicateHeader *header );

static void * thread_send_source(void * arg);
static void * thread_send_cluster(void * arg);

static int send_replicate_packet_to_server( TransactionTbl * transaction_tbl, int current_cluster, HostTbl * host_ptr, ReplicateHeader * header, char *query , char * result,unsigned int replicationId, bool recovery, void *arg);
static int check_result( PGresult * res );
static bool compare_results(int *results, int size, int source_id);

static int send_func(HostTbl * host_ptr,ReplicateHeader * header, char * func,char * result);
static uint32_t get_oid(HostTbl * host_ptr,ReplicateHeader * header);
static int set_oid(HostTbl * host_ptr,ReplicateHeader * header, uint32_t oid);
static int replicate_lo( PGconn * conn, ReplicateHeader * header, LOArgs * query);
static int notice_abort(HostTbl * host_ptr,ReplicateHeader * header);
static FILE * create_queue_file(void);
static int add_queue_file(char * data, int size);

static int send_p_parse (PGconn * conn, StringInfo input_message);
static int send_p_bind (PGconn * conn, StringInfo input_message);
static int send_p_describe (PGconn * conn, StringInfo input_message);
static int send_p_execute (PGconn * conn, StringInfo input_message);
static int send_p_sync (PGconn * conn, StringInfo input_message);
static int send_p_close (PGconn * conn, StringInfo input_message);
static void set_string_info(StringInfo input_message, ReplicateHeader * header, char * query);

int replicate_packet_send_internal(ReplicateHeader * header, char * query,int dest,int recovery_status,bool isHeldLock);
bool PGRis_same_host(char * host1, unsigned short port1 , char * host2, unsigned short port2);
HostTbl * PGRadd_HostTbl(HostTbl *  conf_data, int useFlag);
HostTbl * PGRget_master(void);
void PGRset_recovery_status(int status);
int PGRget_recovery_status(void);
int PGRcheck_recovered_host(void);
int PGRset_recovered_host(HostTbl * target,int useFlag);
int PGRinit_recovery(void);
void PGRexit_subprocess(int signo);
void PGRreplicate_exit(int exit_status);
int PGRsend_replicate_packet_to_server( HostTbl * host_ptr, ReplicateHeader * header, char *query , char * result,unsigned int replicationId, bool recovery);
HostTbl * PGRget_HostTbl(char * resolvedName,int port);
int PGRset_queue(ReplicateHeader * header,char * query);
int PGRset_host_status(HostTbl * host_ptr,int status);
void PGRclear_transactions(void);
void PGRclear_connections();
int PGRset_replication_id(uint32_t id);
int PGRdo_replicate(int sock,ReplicateHeader *header, char * query);
int PGRreturn_result(int dest, char * result,int wait);
int PGRreplicate_packet_send( ReplicateHeader * header, char * query,int dest,int recovery_status);
char * PGRread_packet(int sock, ReplicateHeader *header);
char * PGRread_query(int sock, ReplicateHeader *header);
PGconn * PGRcreateConn( char * host, char * port,char * database, char * userName, char * password, char * md5Salt, char * cryptSalt ,int timeout);

unsigned int PGRget_next_query_id(void);
int PGRinit_transaction_table(void);
int PGRsync_oid(ReplicateHeader *header);
int PGRload_replication_id(void);
extern pthread_mutex_t transaction_table_mutex;

//#ifdef PGGRID
char *response_value;
extern pthread_mutex_t return_value_mutex;
//#endif

static sigjmp_buf local_sigjmp_buf;

static PGresult *PGGRID_execute_distributed_query(ReplicateHeader *header, PGconn *conn, char *query, void *arg);

/*bool
PGRis_same_host(char * host1, unsigned short port1 , char * host2, unsigned short port2)
{
#ifdef PRINT_DEBUG
	char * func = "PGRis_same_host()";
#endif			
	unsigned int ip1, ip2;

	if ((host1[0] == '\0' ) || (host2[0] == '\0') ||
		( port1 != port2 ))
	{
#ifdef PRINT_DEBUG
		show_debug("%s:target host",func);
#endif			
		return false;
	}
	ip1 = PGRget_ip_by_name( host1);
	ip2 = PGRget_ip_by_name( host2);

	if ((ip1 == ip2) && (port1 == port2))
	{
		return true;
	}
	return false;
}*/

PGconn *
PGRcreateConn( char * host, char * port,char * database, char * userName, char * password, char * md5Salt, char * cryptSalt ,int timeout)
{
	char * func = "PGRcreateConn()";
	int cnt = 0;
	PGconn * conn = NULL;
	char pwd[256];

	if (timeout > 0)
	{
		PGRsignal(SIGALRM,exit_waiting_loop);
		alarm(timeout);
		if (sigsetjmp(local_sigjmp_buf,1) == 1)
		{
			alarm(0);
		    show_error("%s: timeout was occured",func);
			return NULL;
		}
	}

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
		if (cnt > PGR_CONNECT_RETRY_TIME )
		{
			if (conn != NULL)
			{
				PQfinish(conn);
				conn = NULL;
			}
			alarm(0);
			return (PGconn *)NULL;
		}		
		
		if(PQstatus(conn) == CONNECTION_BAD && h_errno==2)
		{
		    show_error("gethostbyname() failed. sleep and retrying...");
		    usleep(PGR_SEND_WAIT_MSEC);
			cnt ++;
		}
		else if(!strncasecmp(PQerrorMessage(conn),"FATAL:  Sorry, too many clients already",30) ||
			!strncasecmp(PQerrorMessage(conn),"FATAL:  Non-superuser connection limit",30) ) 
		{
		    usleep(PGR_SEND_WAIT_MSEC);
		    show_error("Connection overflow. sleep and retrying...");
			cnt ++;
		}
		else if(!strncasecmp(PQerrorMessage(conn),"FATAL:  The database system is starting up",40)   )
		{
#ifdef PRINT_DEBUG
			show_debug("waiting for starting up...");
#endif			
		    usleep(PGR_SEND_WAIT_MSEC);
		}
		else
		{
#ifdef PRINT_DEBUG
			show_error("%s:Retry. h_errno is %d,reason is '%s'",func,h_errno,PQerrorMessage(conn));
#endif			
		  
		    usleep(PGR_SEND_WAIT_MSEC);
			cnt ++;
		}
	}
	alarm(0);
	return conn;
}

static void
exit_waiting_loop(int sig)
{
	siglongjmp(local_sigjmp_buf,1);
}

static TransactionTbl *
setTransactionTbl(HostTbl * host_ptr, ReplicateHeader * header)
{
	char * func = "setTransactionTbl()";
	TransactionTbl * ptr = NULL;
	TransactionTbl work ;
	char port[8];
	char * hostName = NULL;
	char * dbName = NULL;
	char * userName = NULL;
	char * password = NULL;
	char * md5Salt = NULL;
	char * cryptSalt = NULL;

	if ((host_ptr == NULL) || (header == NULL))
	{
		return (TransactionTbl *)NULL;
	}
	dbName = (char *)header->dbName;
	snprintf(port,sizeof(port),"%d", host_ptr->port);
	userName = (char *)(header->userName);
	password = (char *)(header->password);
	md5Salt = (char *)(header->md5Salt);
	cryptSalt = (char *)(header->cryptSalt);
	hostName = (char *)(host_ptr->resolvedName);

	ptr = getTransactionTbl(host_ptr,header);
	if (ptr != NULL)
	{
		ptr->transaction_count = 0;
		ptr->conn = PGRcreateConn(hostName,port,dbName,userName,password,md5Salt,cryptSalt, PGR_LOGIN_TIMEOUT );
		if (ptr->conn == NULL)
		{
			show_error("%s:Transaction is pooling but PGRcreateConn failed",func);
			deleteTransactionTbl(host_ptr, header);
			PGRset_host_status(host_ptr,DB_TBL_ERROR);
			ptr = NULL;
		}
		return ptr;
	}

	memset(&work,0,sizeof(work));
	strncpy(work.host, hostName, sizeof(work.host));
	strncpy(work.srcHost, header->from_host, sizeof(work.srcHost));
	work.hostIP = PGRget_ip_by_name(hostName);
	work.port = host_ptr->port;
	work.srcHostIP = PGRget_ip_by_name(header->from_host);
	work.pid = ntohs(header->pid);
	strncpy(work.dbName,header->dbName,sizeof(work.dbName));
	work.conn = PGRcreateConn(hostName,port,dbName,userName,password,md5Salt,cryptSalt, PGR_LOGIN_TIMEOUT);
	if (work.conn == NULL)
	{
#ifdef PRINT_DEBUG
		show_debug("%s: %s@%s is not ready",func,port,hostName);
#endif
		return (TransactionTbl *)NULL;
	}
	work.useFlag = DB_TBL_USE ;
	work.in_transaction = false;
	work.transaction_count = 0;
	ptr = insertTransactionTbl(host_ptr,&work);
	if (ptr == (TransactionTbl *)NULL)
	{
		show_error("%s:insertTransactionTbl failed",func);
		return (TransactionTbl *)NULL;
	}
	return ptr;
}

static TransactionTbl *
insertTransactionTbl( HostTbl * host_ptr, TransactionTbl * datap)
{
	char * func = "insertTransactionTbl()";
	TransactionTbl * workp = NULL;

	pthread_mutex_lock(&transaction_table_mutex);
	if ((host_ptr == (HostTbl *)NULL) || (datap == (TransactionTbl*)NULL))
	{
		show_error("%s:host table or transaction table is NULL",func);
		pthread_mutex_unlock(&transaction_table_mutex);

		return (TransactionTbl *)NULL;
	}
	if (Transaction_Tbl_Begin == NULL)
	{
		if (PGRinit_transaction_table() != STATUS_OK)
		{
			pthread_mutex_unlock(&transaction_table_mutex);

			return (TransactionTbl *)NULL;
		}
	}

	workp = (TransactionTbl *)malloc(sizeof(TransactionTbl));
	memset(workp,0,sizeof(TransactionTbl));
	Transaction_Tbl_End = workp;
	workp->hostIP = datap->hostIP;
	workp->port = datap->port;
	workp->pid = datap->pid;
	workp->srcHostIP = datap->srcHostIP;
	strncpy(workp->host,datap->host,sizeof(workp->host));
	strncpy(workp->srcHost,datap->srcHost,sizeof(workp->srcHost));
	strncpy(workp->dbName,datap->dbName,sizeof(workp->dbName));
	workp->conn = datap->conn;
	workp->useFlag = DB_TBL_USE;
	workp->lock = STATUS_OK;
	workp->in_transaction =datap->in_transaction;
	workp->transaction_count =datap->transaction_count;
	DLAddTail(Transaction_Tbl_Begin, DLNewElem(workp));

	pthread_mutex_unlock(&transaction_table_mutex);

	return workp;
}

static TransactionTbl *
getTransactionTbl( HostTbl * host_ptr, ReplicateHeader * header)
{
	Dlelem * ptr = NULL;
	unsigned int host_ip,srcHost_ip;
	unsigned short pid = 0;

	if (Transaction_Tbl_Begin == (Dllist *) NULL)
	{
		return (TransactionTbl * )NULL;
	}
	if ((host_ptr == (HostTbl *)NULL) ||
		(header == (ReplicateHeader *)NULL))
	{
		return (TransactionTbl * )NULL;
	}
	host_ip = get_host_ip_from_tbl(host_ptr->resolvedName);
	if (host_ip == 0)
	{
		host_ip = PGRget_ip_by_name(host_ptr->resolvedName);
	}
	srcHost_ip = get_srcHost_ip_from_tbl(header->from_host);
	if (srcHost_ip == 0)
	{
		srcHost_ip = PGRget_ip_by_name(header->from_host);
	}
	pid = ntohs(header->pid);

	pthread_mutex_lock(&transaction_table_mutex);

	ptr = DLGetHead(Transaction_Tbl_Begin);
	while (ptr)
	{
		TransactionTbl *transaction = DLE_VAL(ptr);
		if ((transaction->useFlag == DB_TBL_USE) &&
			(transaction->hostIP == host_ip) &&
			(transaction->port == host_ptr->port) &&
			(transaction->srcHostIP == srcHost_ip) &&
			(!strncasecmp(transaction->dbName,header->dbName,sizeof(transaction->dbName))) &&
			(transaction->pid == pid))
		{
			pthread_mutex_unlock(&transaction_table_mutex);
			return transaction;
		}
		ptr = DLGetSucc(ptr);
	}
	pthread_mutex_unlock(&transaction_table_mutex);

	return (TransactionTbl * )NULL;
}

static void
deleteTransactionTbl(HostTbl * host_ptr,ReplicateHeader * header)
{
	TransactionTbl *ptr = NULL;
	Dlelem *elem;

	ptr = getTransactionTbl(host_ptr,header);
      
	pthread_mutex_lock(&transaction_table_mutex);

	if (ptr != NULL)
	{
		/*
		if (ptr->in_transaction)
		{
			if (host_ptr->transaction_count > 0)
				host_ptr->transaction_count--;
		}
		*/

		if (ptr->conn != NULL)
		{
			PQfinish(ptr->conn);
		}
		elem = DLGetHead(Transaction_Tbl_Begin);
		while (elem)
		{
			TransactionTbl *transaction = DLE_VAL(elem);
			if (transaction == ptr) {
				free(ptr);
				DLRemove(elem);
				DLFreeElem(elem);
				pthread_mutex_unlock(&transaction_table_mutex);
			  	return;
			}
			elem = DLGetSucc(elem);
		}
	}
	pthread_mutex_unlock(&transaction_table_mutex);
}

static HostTbl *
deleteHostTbl(HostTbl * ptr)
{
	if (ptr != (HostTbl*)NULL)
	{
		memset(ptr,0,sizeof(HostTbl));
	}
	return ++ptr;
}

HostTbl *
PGRadd_HostTbl(HostTbl *conf_data, int useFlag)
{
	HostTbl * ptr = NULL;
	int cnt = 0;

	ptr = PGRget_HostTbl(conf_data->resolvedName, conf_data->port);
	if (ptr != (HostTbl*)NULL)
	{
		PGRset_host_status(ptr,useFlag);
		return ptr;
	}

	ptr = Host_Tbl_Begin;
	cnt = 1;
	while (ptr->useFlag != DB_TBL_END)
	{
		if (ptr->useFlag == DB_TBL_FREE)
		{
			break;
		}
		ptr ++;
		cnt ++;
	}
	if (cnt >= MAX_DB_SERVER)
	{
		return (HostTbl*)NULL;
	}
	if (ptr->useFlag == DB_TBL_END)
	{
		(ptr + 1) -> useFlag = DB_TBL_END;
	}
	memset(ptr,0,sizeof(HostTbl));
	ptr->hostNum = cnt;
	memcpy(ptr->hostName,conf_data->hostName,sizeof(ptr->hostName));
	memcpy(ptr->resolvedName,conf_data->resolvedName,sizeof(ptr->resolvedName));
	ptr->port = conf_data->port;
	ptr->recoveryPort = conf_data->recoveryPort;
	ptr->transaction_count = 0;
	PGRset_host_status(ptr,useFlag);

	return ptr;
}

HostTbl *
PGRget_master(void)
{
	HostTbl * host_tbl = NULL;

	host_tbl = Host_Tbl_Begin;
	while(host_tbl->useFlag != DB_TBL_END)
	{
		if (host_tbl->useFlag == DB_TBL_USE)
		{
			return host_tbl;
		}
		host_tbl ++;
	}
	return (HostTbl *)NULL;
}

void
PGRset_recovery_status(int status)
{
	if (RecoverySemID <= 0)
		return;
	PGRsem_lock(RecoverySemID,SEM_NUM_OF_RECOVERY);
	if (Recovery_Status_Inf != (RecoveryStatusInf *)NULL)
	{
		Recovery_Status_Inf->recovery_status = status;
		
	}
	PGRsem_unlock(RecoverySemID,SEM_NUM_OF_RECOVERY);
}

int
PGRget_recovery_status(void)
{
	int status = -1;

	if (RecoverySemID <= 0)
		return -1;
	PGRsem_lock(RecoverySemID, SEM_NUM_OF_RECOVERY);
	if (Recovery_Status_Inf != (RecoveryStatusInf *)NULL)
	{
		status = Recovery_Status_Inf->recovery_status;
	}
	PGRsem_unlock(RecoverySemID, SEM_NUM_OF_RECOVERY);
	return status;

}

static void
set_transaction_status(int status)
{
	if (RecoverySemID <= 0)
		return ;
	PGRsem_lock(RecoverySemID, SEM_NUM_OF_RECOVERY);
	if (Recovery_Status_Inf != (RecoveryStatusInf *)NULL)
	{
		Recovery_Status_Inf->recovery_status = status;
	}
	PGRsem_unlock(RecoverySemID, SEM_NUM_OF_RECOVERY);
}

#if 0
static int
get_transaction_status(void)
{
	int status = 0;

	if (RecoverySemID <= 0)
		return 0;
	PGRsem_lock(RecoverySemID, SEM_NUM_OF_RECOVERY);
	if (Recovery_Status_Inf != (RecoveryStatusInf *)NULL)
	{
		status = Recovery_Status_Inf->recovery_status;
		PGRsem_unlock(RecoverySemID, SEM_NUM_OF_RECOVERY);
		return status;
	}
	PGRsem_unlock(RecoverySemID, SEM_NUM_OF_RECOVERY);
	return 0;
}
#endif

int
PGRcheck_recovered_host(void)
{
	char * func = "PGRcheck_recovered_host()";
	HostTbl * ptr = NULL;
	int rtn = STATUS_OK;

	if (RecoverySemID <= 0)
		return STATUS_ERROR;
	PGRsem_lock(RecoverySemID, SEM_NUM_OF_RECOVERY);
	if (Recovery_Status_Inf != (RecoveryStatusInf *)NULL)
	{
		if (Recovery_Status_Inf->useFlag != DB_TBL_FREE)
		{
			ptr = PGRadd_HostTbl((HostTbl *)&(Recovery_Status_Inf->target_host),Recovery_Status_Inf->useFlag);
			if (ptr == (HostTbl *) NULL)
			{
				show_error("%s:PGRadd_HostTbl failed",func);
				rtn = STATUS_ERROR;
			}
			Recovery_Status_Inf->useFlag = DB_TBL_FREE;
			memset((HostTbl *)&(Recovery_Status_Inf->target_host),0,sizeof(HostTbl));

		}
	}
	PGRsem_unlock(RecoverySemID, SEM_NUM_OF_RECOVERY);
	return rtn;
}

int
PGRset_recovered_host(HostTbl * target, int useFlag)
{
	if (RecoverySemID <= 0)
		return -1;
	PGRsem_lock(RecoverySemID, SEM_NUM_OF_RECOVERY);
	if (Recovery_Status_Inf != (RecoveryStatusInf *)NULL)
	{
		Recovery_Status_Inf->useFlag = useFlag;
		if (target != (HostTbl*)NULL)
		{
			memcpy((HostTbl *)&(Recovery_Status_Inf->target_host),target,sizeof(HostTbl));
			PGRset_host_status(target,useFlag);
		}

	}
	PGRsem_unlock(RecoverySemID, SEM_NUM_OF_RECOVERY);
	return 0;
}

static bool
is_master_in_recovery(char * host , int port,int recovery_status)
{
	HostTbl * master = NULL;

	int status = PGRget_recovery_status();
	if (status == RECOVERY_CLEARED)
	{
		master = PGRget_master();
		if (master == (HostTbl *)NULL)
		{
			return false;
		}
		return (PGRis_same_host(host, port , master->hostName, master->port));
	}
	return false;
}

int
PGRinit_recovery(void)
{
	char * func = "PGRinit_recovery()";
	int size = 0;
	union semun sem_arg;
	int i = 0;

	if ((RecoverySemID = semget(IPC_PRIVATE,4,IPC_CREAT | IPC_EXCL | 0600)) < 0)
	{
		show_error("%s:semget() failed. (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
	for ( i = 0 ; i < 4 ; i ++)
	{
		semctl(RecoverySemID, i, GETVAL, sem_arg);
		sem_arg.val = 1;
		semctl(RecoverySemID, i, SETVAL, sem_arg);
	}

	size = sizeof(RecoveryStatusInf);
	RecoveryShmid = shmget(IPC_PRIVATE,size,IPC_CREAT | IPC_EXCL | 0600);
	if (RecoveryShmid < 0)
	{
		show_error("%s:shmget() failed. (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
	Recovery_Status_Inf = (RecoveryStatusInf *)shmat(RecoveryShmid,0,0);
	if (Recovery_Status_Inf == (RecoveryStatusInf *)-1)
	{
		show_error("%s:shmat() failed. (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
	memset(Recovery_Status_Inf,0,size);
	Recovery_Status_Inf->check_point = PGR_CHECK_POINT ;

	size = sizeof(unsigned int);
	ReplicateSerializationShmid = shmget(IPC_PRIVATE,size,IPC_CREAT | IPC_EXCL | 0600);
	if (ReplicateSerializationShmid < 0)
	{
		show_error("%s:shmget() failed. (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}

	PGR_ReplicateSerializationID = (unsigned int *)shmat(ReplicateSerializationShmid,0,0); 
	if( PGR_ReplicateSerializationID == (unsigned int *)-1) {
		show_error("%s:shmat() failed. (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
	memset(PGR_ReplicateSerializationID,0,size);
	PGRset_recovery_status(RECOVERY_INIT);
	PGRset_recovered_host((HostTbl *)NULL, DB_TBL_FREE);
	set_transaction_status(0);

	/*
	 * create message queue
	 */
	RecoveryMsgShmid = shmget(IPC_PRIVATE,size,IPC_CREAT | IPC_EXCL | 0600);
	if (RecoveryMsgShmid < 0)
	{
		show_error("%s:shmget() failed. (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}

	RecoveryMsgid = (int *)shmat(RecoveryMsgShmid,0,0);
	if( RecoveryMsgid < 0) {
		show_error("%s:shmat() failed. (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
	*RecoveryMsgid = msgget (IPC_PRIVATE, 00666 | IPC_CREAT );
	if (*RecoveryMsgid < 0)
	{
		show_error("%s:msgget() failed. (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}


	return STATUS_OK;
}

static void
clearHostTbl(void)
{

	HostTbl * ptr = NULL;

	if (Host_Tbl_Begin == NULL)
		return;
	/* normal socket close */
	ptr = Host_Tbl_Begin;
	while(ptr && ptr->useFlag != DB_TBL_END)
	{
		ptr = deleteHostTbl(ptr);
	}	
}

void
PGRexit_subprocess(int signo)
{
	exit_signo = signo;
	PGRreplicate_exit(1);
}

void
PGRreplicate_exit(int exit_status)
{
	char fname[256];
	int rtn = 0;
	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGQUIT);
	sigaddset(&mask, SIGCHLD);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	kill (0, exit_signo);

	child_wait(0);

	if (RidFp != NULL)
	{
		rewind(RidFp);
		if (Recovery_Status_Inf != NULL)
		{
			PGRwrite_log_file(RidFp,"%u",Recovery_Status_Inf->replication_id);
		}
		fflush(RidFp);
		fclose(RidFp);
		RidFp = NULL;
	}

	if (ReplicateSock > 0)
		close(ReplicateSock);

	/* recovery status clear */	
	if (RecoverySemID > 0)
		Recovery_Status_Inf->recovery_status = RECOVERY_INIT;

	/* normal socket close */
	clearHostTbl();

	if (Host_Tbl_Begin != (HostTbl *)NULL)
	{
		rtn = shmdt((char *)Host_Tbl_Begin);
		shmctl(HostTblShmid,IPC_RMID,(struct shmid_ds *)NULL);
	}

	if (Cascade_Tbl != (ReplicateServerInfo *)NULL)
	{
		rtn = shmdt((char *)Cascade_Tbl);
		shmctl(CascadeTblShmid,IPC_RMID,(struct shmid_ds *)NULL);
	}

	if (Cascade_Inf != (CascadeInf *)NULL)
	{
		rtn = shmdt((char *)Cascade_Inf);
		shmctl(CascadeInfShmid,IPC_RMID,(struct shmid_ds *)NULL);
	}

	if (Commit_Log_Tbl != (CommitLogInf *)NULL)
	{
		rtn = shmdt((char *)Commit_Log_Tbl);
		shmctl(CommitLogShmid,IPC_RMID,(struct shmid_ds *)NULL);
	}

	if (Recovery_Status_Inf != (RecoveryStatusInf *)NULL)
	{
		rtn = shmdt((char *)Recovery_Status_Inf);
		shmctl(RecoveryShmid,IPC_RMID,(struct shmid_ds *)NULL);
	}
	if (PGR_ReplicateSerializationID!=NULL) 
	{
	    shmdt(PGR_ReplicateSerializationID);
	    shmctl(ReplicateSerializationShmid,IPC_RMID,(struct shmid_ds *)NULL);
	}

	if (RecoveryMsgid)
	{
		if (*RecoveryMsgid >= 0)
			msgctl(*RecoveryMsgid,IPC_RMID,(struct msqid_ds *)NULL);

		shmdt(RecoveryMsgid);
		shmctl(RecoveryMsgShmid, IPC_RMID, NULL);
	}

	if (StatusFp != NULL)
	{
		fflush(StatusFp);
		fclose(StatusFp);
		StatusFp = NULL;
	}
	if (LogFp != NULL)
	{
		fflush(LogFp);
		fclose(LogFp);
		LogFp = NULL;
	}

	if (PGR_Result != NULL)
	{
		free(PGR_Result);
		PGR_Result = NULL;
	}
	if (PGR_Response_Inf != NULL)
	{
		free(PGR_Response_Inf);
		PGR_Response_Inf = NULL;
	}

	if (LoadBalanceTbl != NULL)
	{
		free(LoadBalanceTbl);
		LoadBalanceTbl = NULL;
	}

	if (PGR_Log_Header != NULL)
	{
		free(PGR_Log_Header);
		PGR_Log_Header = NULL;
	}

	if (PGR_Send_Query_ID != NULL)
	{
		free(PGR_Send_Query_ID);
		PGR_Send_Query_ID = NULL;
	}

	if (CascadeSemID > 0)
	{
		sem_quit(CascadeSemID);
		CascadeSemID = 0;
	}
	if (SemID > 0)
	{
		sem_quit(SemID);
		SemID = 0;
	}
	if (RecoverySemID > 0)
	{
		sem_quit(RecoverySemID);
		RecoverySemID = 0;
	}
	if (VacuumSemID > 0)
	{
		sem_quit(VacuumSemID);
	}

	snprintf(fname, sizeof(fname), "%s/%s", PGR_Write_Path, PGREPLICATE_PID_FILE);
	unlink(fname);

	/* close socket between rlog process */
	
	if (Replicateion_Log->r_log_sock >= 0)
	{
		close(Replicateion_Log->r_log_sock);
		Replicateion_Log->r_log_sock = -1;
	}
	if (Replicateion_Log->RLog_Sock_Path != NULL)
	{
		unlink(Replicateion_Log->RLog_Sock_Path);
		free(Replicateion_Log->RLog_Sock_Path);
		Replicateion_Log->RLog_Sock_Path = NULL;
	}

	if (ResolvedName != NULL)
	{
		free(ResolvedName);
		ResolvedName = NULL;
	}
	exit(exit_status);
}

static int
send_cluster_status_to_load_balance(HostTbl * host_ptr,int status)
{
	RecoveryPacket packet;
	int rtn = 0;

	memset(&packet,0,sizeof(RecoveryPacket));
	packet.packet_no = htons(status);
	strncpy(packet.hostName,host_ptr->hostName,sizeof(packet.hostName));
	packet.port = htons(host_ptr->port);
	rtn = PGRsend_load_balance_packet(&packet);
	return rtn;
}

int
PGRset_host_status(HostTbl * host_ptr,int status)
{
	if (host_ptr == NULL)
	{
		return STATUS_ERROR;
	}
	if (host_ptr->useFlag != status)
	{
		host_ptr->useFlag = status;
		if (status == DB_TBL_ERROR )
		{
			host_ptr->transaction_count = 0;
			send_cluster_status_to_load_balance(host_ptr,RECOVERY_ERROR_CONNECTION);
		}
		write_host_status_file(host_ptr);
	}
	return STATUS_OK;
}

static void
write_host_status_file(HostTbl * host_ptr)
{
	switch( host_ptr->useFlag)
	{
		case DB_TBL_FREE:
			PGRwrite_log_file(StatusFp,"port(%d) host:%s free",
					host_ptr->port,
					host_ptr->hostName);
			break;
		case DB_TBL_INIT:
			PGRwrite_log_file(StatusFp,"port(%d) host:%s initialize",
					host_ptr->port,
					host_ptr->hostName);
			break;
		case DB_TBL_USE:
			PGRwrite_log_file(StatusFp,"port(%d) host:%s start use",
					host_ptr->port,
					host_ptr->hostName);
			break;
		case DB_TBL_ERROR:
			PGRwrite_log_file(StatusFp,"port(%d) host:%s error",
					host_ptr->port,
					host_ptr->hostName);
			break;
		case DB_TBL_END:
			PGRwrite_log_file(StatusFp,"port(%d) host:%s end",
					host_ptr->port,
					host_ptr->hostName); 
			break;
	}
}

static int
check_result( PGresult * res )
{
	int status = 0;

	status = PQresultStatus(res);
	if ((status == PGRES_NONFATAL_ERROR ) ||
		(status == PGRES_FATAL_ERROR ))
	{
		return STATUS_ERROR;
	}
	return STATUS_OK;
}

static bool
compare_results(int *results, int size, int source_id)
{
	int i, prev = 0;

	for (i = 0; i < size; i++)
	{
		if (i != source_id)
		{
			prev = results[i];
			break;
		}
	}

	for (; i < size; i++)
	{
		if (i == source_id)
			continue;
		if (prev != results[i])
			return false;
		prev = results[i];
	}
	return true;
}

/*--------------------------------------------------
 * SYMBOL
 *     PGRsend_replicate_packet_to_server()
 * NOTES
 *     Send query data to the cluster DB and recieve result data. 
 * ARGS
 *     HostTbl * host_ptr: the record of cluster DB table (target)
 *     ReplicateHeader * header: header data
 *     char *query: query data 
 *     char * result: returned result data 
 * RETURN
 *     STATUS_OK: OK
 *     STATUS_ERROR: NG
 *     STATUS_LOCK_CONFLICT: Lock conflicted
 *---------------------------------------------------
 */
int
PGRsend_replicate_packet_to_server( HostTbl * host_ptr, ReplicateHeader * header, char *query , char * result,unsigned int replicationId, bool recovery)
{
	char * func = "PGRsend_replicate_packet_to_server()";
	TransactionTbl * transaction_tbl = NULL;
	char *database = NULL;
	char port[8];
	char *userName = NULL;
	char * password = NULL;
	char * host = NULL;
	char * md5Salt = NULL;
	char * cryptSalt = NULL;
	int rtn = 0;
	int current_cluster = 0;
	int query_size = 0;

	if ((query == NULL) || (header == NULL))
	{
		show_error("%s: query is broken",func);
		return STATUS_ERROR;
	}
	query_size = ntohl(header->query_size);
	if (query_size < 0)
	{
		show_error("%s: query size is broken",func);
		return STATUS_ERROR;
	}
	if (host_ptr == NULL)
	{
		return STATUS_ERROR;
	}

	if (PGR_Response_Inf != NULL)
	{
		current_cluster = PGR_Response_Inf->current_cluster;
	}

	/*
	 * set up the connection
	 */
	database = (char *)header->dbName;
	snprintf(port,sizeof(port),"%d", host_ptr->port);
	userName = (char *)(header->userName);
	password = (char *)(header->password);
	md5Salt = (char *)(header->md5Salt);
	cryptSalt = (char *)(header->cryptSalt);
	host = (char *)(host_ptr->resolvedName);
	/*
	 * get the transaction table data
	 * it has the connection data with each cluster DB
	 */
	transaction_tbl = getTransactionTbl(host_ptr,header);
	/*
	 * if the transaction process is new one, 
	 * create connection data and add the transaction table
	 */
	if (transaction_tbl == (TransactionTbl *)NULL)
	{
		if (recovery == true)
		{
			int cnt = 0;
			while(transaction_tbl == (TransactionTbl *)NULL)
			{
				transaction_tbl = setTransactionTbl(host_ptr, header);
				if (cnt > RECOVERY_TIMEOUT)
				{
					break;
				}
				cnt ++;
				sleep(1);
			}
		}
		else
		{
			transaction_tbl = setTransactionTbl(host_ptr, header);
		}
		if (transaction_tbl == (TransactionTbl *)NULL)
		{
			show_error("%s:setTransactionTbl failed",func);
			if ( header->cmdSts != CMD_STS_NOTICE )
			{
				PGRset_host_status(host_ptr,DB_TBL_ERROR);
			}
			return STATUS_ERROR;
		}
		StartReplication[current_cluster] = true;
	}
	else
	{
		/*
		 * re-use the connection data
		 */
		if ((transaction_tbl->conn != (PGconn *)NULL) &&
			(transaction_tbl->conn->sock > 0))
		{
			StartReplication[current_cluster] = false;
		}
		else
		{
			if (transaction_tbl->conn != (PGconn *)NULL)
			{
				PQfinish(transaction_tbl->conn);
				transaction_tbl->conn = NULL;
			}
		 	transaction_tbl->conn = PGRcreateConn(host,port,database,userName,password,md5Salt,cryptSalt, PGR_LOGIN_TIMEOUT);
			StartReplication[current_cluster] = true;
		}
	}
	if(header->cmdSts==CMD_STS_OTHER &&
	   header->cmdType==CMD_TYPE_CONNECTION_CLOSE) 
	{
		check_delete_transaction(host_ptr, header);
		return STATUS_OK;
	}
#ifdef PRINT_DEBUG
	show_debug("%s:connect db:%s port:%s user:%s host:%s query:%s",
		func, database,port,userName,host,query);
#endif			
	 rtn = send_replicate_packet_to_server( transaction_tbl, current_cluster, host_ptr, header, query ,result ,replicationId, recovery, NULL);
	return rtn;
}

static int
send_replicate_packet_to_server( TransactionTbl * transaction_tbl, int current_cluster, HostTbl * host_ptr, ReplicateHeader * header, char *query , char * result,unsigned int replicationId, bool recovery, void *arg)
{
	char * func = "send_replicate_packet_to_server()";
	PGconn * conn = (PGconn *)NULL;
	PGresult * res = (PGresult *)NULL;
	char sync_command[256];
	bool sync_command_flg = false;
	char * str = NULL;
	int rtn = 0;
	int query_size = 0;
	int hostNum = 0;
	StringInfoData input_message;

	if (( transaction_tbl == (TransactionTbl *)NULL) ||
		( host_ptr == (HostTbl *) NULL) ||
		(header == (ReplicateHeader *) NULL) ||
		(query == NULL) ||
		( result == NULL))
	{
		show_error("%s:unexpected NULL variable",func);
		return STATUS_ERROR;
	}

	query_size = ntohl(header->query_size);
	if (query_size < 0)
	{
		show_error("%s: query size is broken",func);
		return STATUS_ERROR;
	}

/*
	if(header->cmdSts == CMD_STS_OTHER &&
	   header->cmdType == CMD_TYPE_CONNECTION_CLOSE) 
	{
			 check_delete_transaction(host_ptr,header);
			 return STATUS_OK;
	}
*/
	conn = transaction_tbl->conn;
	if (conn == NULL)
	{
		show_error("%s:[%d@%s] may be down",func,host_ptr->port,host_ptr->hostName);
		if ( header->cmdSts != CMD_STS_NOTICE )
		{
			PGRset_host_status(host_ptr,DB_TBL_ERROR);
		}
		return STATUS_ERROR;
	}
	hostNum = host_ptr->hostNum;

	/*
	 * When the query is transaction query...
	 */
	if (is_need_sync_time(header) == true)
	{
		if (transaction_tbl->transaction_count >1 )
		{
			sync_command_flg = false;
		}
		else
		{
			sync_command_flg = true;
		}
	}
	if ((header->cmdSts == CMD_STS_TRANSACTION ) ||
		(header->cmdSts == CMD_STS_SET_SESSION_AUTHORIZATION ))
	{
		if ((header->cmdSts == CMD_STS_TRANSACTION ) &&
			((header->cmdType != CMD_TYPE_BEGIN)     ||
			(transaction_tbl->transaction_count >1 )))
		{
			sync_command_flg = false;
		}
	}

	/*
	 * execute query
	 */

	if (header->rlog > 0 )
	{

		if (is_executed_query( conn, header) == true)
		{
			return STATUS_OK;
		}
		else 
		{
#ifdef PRINT_DEBUG
		  show_debug("%s:check replication log issue , id=%d,rlog=%d,query=%s status=not_replicated",func,ntohl(header->replicate_id),header->rlog,query);
#endif
		}
	}
	if (( header->cmdSts != CMD_STS_NOTICE ) && 
		( header->cmdSts != CMD_STS_PREPARE ) &&
		((sync_command_flg == true)           ||
		 (StartReplication[current_cluster] == true)))
	{
		snprintf(sync_command,sizeof(sync_command),
			"SELECT %s(%d,%u,%u,%u,%d,%u) ",
			PGR_SYSTEM_COMMAND_FUNC,
			PGR_SET_CURRENT_TIME_FUNC_NO,
			(unsigned int)ntohl(header->tv.tv_sec),
			(unsigned int)ntohl(header->tv.tv_usec),
			(unsigned int)ntohl(PGR_Log_Header->replicate_id),
			PGR_Response_Inf->response_mode,
			 *PGR_ReplicateSerializationID);
#ifdef PRINT_DEBUG
		show_debug("%s:sync_command(%s)",func,sync_command);
#endif			
		
		res = PQexec(conn, sync_command);
		
		if (res != NULL)
			PQclear(res);
		StartReplication[current_cluster] = false;
	}

	res = NULL;
	if ((header->cmdType == CMD_TYPE_COPY_DATA) ||
		(header->cmdType == CMD_TYPE_COPY_DATA_END))
	{
		/* copy data replication */
		rtn =PQputnbytes(conn, query,query_size);
		if (header->cmdType == CMD_TYPE_COPY_DATA_END)
		{
			rtn = PQendcopy(conn);
			if (rtn == 1) /* failed */
			{
				if (transaction_tbl->conn != NULL)
				{
					PQfinish(transaction_tbl->conn);
					transaction_tbl->conn = (PGconn *)NULL;
					StartReplication[current_cluster] = true;
				}
			}
		}
		*(PGR_Send_Query_ID + hostNum ) = ntohl(header->query_id);
		return STATUS_OK;
	}
	else if (header->cmdSts == CMD_STS_LARGE_OBJECT)
	{
		replicate_lo(conn, header,(LOArgs *)query);
		return STATUS_OK;
	}

	else if (header->cmdSts == CMD_STS_PREPARE)
	{

		if ( !PGR_Parse_Session_Started)
		{
			snprintf(sync_command,sizeof(sync_command),
				"SELECT %s(%d,%u,%u,%u,%d,%u) ",
				PGR_SYSTEM_COMMAND_FUNC,
				PGR_SET_CURRENT_TIME_FUNC_NO,
				(unsigned int)ntohl(header->tv.tv_sec),
				(unsigned int)ntohl(header->tv.tv_usec),
				(unsigned int)ntohl(PGR_Log_Header->replicate_id),
				PGR_Response_Inf->response_mode,
				 *PGR_ReplicateSerializationID);
			res = PQexec(conn, sync_command);
			if (res != NULL)
			{
				PQclear(res);
				res = NULL;
			}
			while ((res = PQgetResult(conn)) != NULL)
			{
				if (res->resultStatus == PGRES_COPY_IN)
				{
					PQclear(res);
					return STATUS_ERROR;
				}
				else if (res->resultStatus == PGRES_COPY_OUT)
				{
					conn->asyncStatus = PGASYNC_BUSY;
				}
				else if (conn->status == CONNECTION_BAD)
				{
					PQclear(res);
					return STATUS_ERROR;
				}
				PQclear(res);
			}
		}
		set_string_info(&input_message,header,query);
		switch (header->cmdType)
		{
			case CMD_TYPE_P_PARSE :
				if (send_p_parse(conn, &input_message) != STATUS_OK)
				{
					pqHandleSendFailure(conn);
					PGR_Parse_Session_Started = false;
					return STATUS_ERROR;
				}
				break;
			case CMD_TYPE_P_BIND :
				if (send_p_bind(conn, &input_message) != STATUS_OK)
				{
					pqHandleSendFailure(conn);
					PGR_Parse_Session_Started = false;
					return STATUS_ERROR;
				}
				break;
			case CMD_TYPE_P_DESCRIBE :
				if (send_p_describe(conn, &input_message) != STATUS_OK)
				{
					pqHandleSendFailure(conn);
					PGR_Parse_Session_Started = false;
					return STATUS_ERROR;
				}
				break;
			case CMD_TYPE_P_EXECUTE :
				if (send_p_execute(conn,&input_message) != STATUS_OK)
				{
					pqHandleSendFailure(conn);
					PGR_Parse_Session_Started = false;
					return STATUS_ERROR;
				}
				break;
			case CMD_TYPE_P_SYNC :
				if (send_p_sync(conn, &input_message) != STATUS_OK)
				{
					pqHandleSendFailure(conn);
					PGR_Parse_Session_Started = false;
					return STATUS_ERROR;
				}
				break;
			case CMD_TYPE_P_CLOSE :
				if (send_p_close(conn, &input_message) != STATUS_OK)
				{
					pqHandleSendFailure(conn);
					PGR_Parse_Session_Started = false;
					return STATUS_ERROR;
				}
				break;
			default :
				break;
		}
		return STATUS_OK;
	}
	else
	{
		if (transaction_tbl->lock != STATUS_OK)
		{
#ifdef PRINT_DEBUG
	show_debug("%s:[%d]transaction_tbl->lock is [%d]",func,current_cluster,transaction_tbl->lock );
#endif
			transaction_tbl->lock = STATUS_OK;
		}
		snprintf(sync_command,sizeof(sync_command),
			"SELECT %s(%d,%u,%u,%d) ",
			PGR_SYSTEM_COMMAND_FUNC,
			PGR_SET_CURRENT_REPLICATION_QUERY_ID_NO,
		        replicationId,
			0,
			PGR_Response_Inf->response_mode);
		res = PQexec(conn, sync_command);
		if (res != NULL)
		{
			PQclear(res);
			res = NULL;
		}	

		if (header->pggrid_op_type == PGGRID_OP_DQUERY){
			//send data returned to the source site
			show_debug("PGGRID: distributed query");
			res=PGGRID_execute_distributed_query(header, conn, query, arg);
		}else
			res = PQexec(conn, query);
		rtn = check_result(res);
#ifdef PRINT_DEBUG
	show_debug("%s:PQexec send :%s",func,query);
#endif	

	}

	if (res == NULL)
	{
		StartReplication[current_cluster] = true;
		return STATUS_ERROR;
	}
		
	str = PQcmdStatus(res);
#ifdef PRINT_DEBUG
	show_debug("%s:PQexec returns :%s",func,str);
#endif	
	if ((str == NULL) || (*str == '\0'))
	{
		if ((result != NULL) && (res != NULL) && (res->errMsg != NULL))
		{
			snprintf(result,PGR_MESSAGE_BUFSIZE,"E%s",res->errMsg);
		}
		else
		{
			strcpy(result,"E");
		}
		StartReplication[current_cluster] = true;
	}
	else
	{
		if (!strncasecmp(str,PGR_LOCK_CONFLICT_NOTICE_CMD,strlen(PGR_LOCK_CONFLICT_NOTICE_CMD)))
		{
#ifdef PRINT_DEBUG
			show_debug("%s:LOCK CONFLICT from PQexec",func);
#endif			
			if (res != NULL)
				PQclear(res);
			
			transaction_tbl->lock = STATUS_LOCK_CONFLICT;
			return STATUS_LOCK_CONFLICT;
		}
		else if (!strncasecmp(str,PGR_DEADLOCK_DETECT_NOTICE_CMD,strlen(PGR_DEADLOCK_DETECT_NOTICE_CMD)))
		{
#ifdef PRINT_DEBUG
			show_debug("%s:DEADLOCK DETECTED from PQexec",func);
#endif			
			if (res != NULL)
				PQclear(res);
			transaction_tbl->lock = STATUS_DEADLOCK_DETECT;
			return STATUS_DEADLOCK_DETECT;
		}
		snprintf(result,PGR_MESSAGE_BUFSIZE,"C%s",str);
	}
	if (res != NULL)
		PQclear(res);

	/* set send query id */
	*(PGR_Send_Query_ID + hostNum ) = ntohl(header->query_id);

	/*
	 * if the query is end transaction process...
	 */
	check_delete_transaction(host_ptr,header);

	return STATUS_OK;
}

static PGresult *
PGGRID_execute_distributed_query(ReplicateHeader *header, PGconn *conn, char *query, void *arg){
	PGresult *rv;
	int result;
	const char *SELECT="select * from";
	//command comes in the format "get <table name>
	//removing word "get" will give us the target table name
	char *tablename=query+4*sizeof(char);
	int ntuples, nfields, i, j;
	int command_size=strlen(SELECT) + strlen(tablename) +11;
	ThreadArgInf *thread_arg = (ThreadArgInf *)arg;
	
	
	char *command=malloc(command_size);
	snprintf(command, command_size, "%s %s %s", SELECT, tablename, "");

	show_debug("PGGRID: sending dquery: %s", command);
	rv=PQexec(conn, command);
	result=check_result(rv);
	if (result==STATUS_OK){
		ntuples=PQntuples(rv);
		nfields=PQnfields(rv);

		//show_debug("PGGRID: receiving %d tuples", ntuples);

		//critical region
		//each site returns result tuples
		pthread_mutex_lock(&return_value_mutex);
		thread_arg->returningTuples = true;
		
		for (i=1; i<=ntuples;++i){
			 //Build a tuple to return
			for (j=1; j<=nfields;++j){
				response_value=PQgetvalue(rv,i-1,j-1);
				show_debug("PGGRID: sendind value %d %d", i,j);
				thread_send_source(arg);
			}
		}
		thread_arg->returningTuples = false;
		pthread_mutex_unlock(&return_value_mutex);
	}else{
		show_debug("PGGRID: erro ao enviar dquery %d", result);
	}
	
	free(command);
	
	return rv;
}

static int
check_delete_transaction (HostTbl * host_ptr, ReplicateHeader * header)
{
	char	   *database = NULL;

	if ((host_ptr == NULL) || (header == NULL))
	{
		return STATUS_ERROR;
	}
	database = (char *)header->dbName;
	if(header->cmdSts == CMD_STS_OTHER &&
	   header->cmdType == CMD_TYPE_CONNECTION_CLOSE) 
	{
		notice_abort(host_ptr, header);
		deleteTransactionTbl(host_ptr,header);
	}
	
	delete_template(host_ptr, header);
	return STATUS_OK;
}

static void
check_transaction_status(ReplicateHeader * header,
						 TransactionTbl *transaction)
{
	if (header == (ReplicateHeader *)NULL)
	{
		return;
	}
	if (header->cmdSts == CMD_STS_TRANSACTION )
	{
		if (header->cmdType == CMD_TYPE_BEGIN )
		{
			if (transaction != NULL)
			{
				transaction->in_transaction = true;
				transaction->transaction_count ++;
			}
		}
		else if ((header->cmdType == CMD_TYPE_COMMIT) ||
				 (header->cmdType == CMD_TYPE_ROLLBACK))
		{
			if (transaction != NULL)
			{
				if (transaction->transaction_count > 0)
				{
					transaction->transaction_count --;
				}	
				if (transaction->transaction_count == 0)
				{
					transaction->in_transaction = false;
				}
			}
		}
	}
	else 
	{ 
		if ( header->cmdType == CMD_TYPE_COPY ) 
		{
			if (transaction != NULL)
			{
				transaction->exec_copy = true;
			}
		}
		else if (header->cmdType == CMD_TYPE_COPY_DATA_END) 
		{
			if (transaction != NULL)
			{
				transaction->exec_copy = false;						
			}
		}
	}
}

static HostTbl *
check_host_transaction_status(ReplicateHeader * header,
						 HostTbl *host)
{
	int recovery_status = 0;

	if ((header == (ReplicateHeader *)NULL) || (host == (HostTbl *)NULL))
	{
		return NULL;
	}
	if (header->cmdType == CMD_TYPE_BEGIN )
	{
		host->transaction_count++;
	}
	else if ((header->cmdType == CMD_TYPE_COMMIT) ||
			 (header->cmdType == CMD_TYPE_ROLLBACK))
	{
		if (host->transaction_count > 0)
			host->transaction_count--;
	}

	recovery_status = PGRget_recovery_status();
	if ((recovery_status == RECOVERY_PREPARE_START) &&
		(host->transaction_count > 0))
	{
		PGRset_recovery_status(RECOVERY_WAIT_CLEAN);
	}
	else if ((recovery_status == RECOVERY_PREPARE_START) &&
		(host->transaction_count==0))
	{
		PGRset_recovery_status(RECOVERY_CLEARED);
	}
	else if ((recovery_status == RECOVERY_WAIT_CLEAN) &&
		(host->transaction_count==0))
	{
		PGRset_recovery_status(RECOVERY_CLEARED);
	}
	return host;
}

static FILE *
create_queue_file(void)
{
	char * func = "create_queue_file()";
	FILE * fp = NULL;
	struct timeval tv;
	char fname[FILENAME_MAX_LENGTH];
	int size = 0;
	int rtn = 0;
	RecoveryQueueFile * msg = NULL;

	if (*RecoveryMsgid < 0)
	{
		return (FILE *)NULL;
	}
	/* create uniq file name */
	gettimeofday(&tv,NULL);
	memset(fname,0,sizeof(fname));
	snprintf(fname,sizeof(fname),"%s/%s_%u.%u",
		PGR_Data_Path,
		RECOVERY_QUEUE_FILE,
		(uint32_t)tv.tv_sec,
		(uint32_t)tv.tv_usec);

	size = sizeof(fname) + sizeof(RecoveryQueueFile);
	msg = (RecoveryQueueFile *)malloc(size);
	if (msg == NULL)
	{
		show_error("%s:malloc() failed. reason: %s", func, strerror(errno));
		return (FILE *)NULL;
	}
	memset(msg,0,size);
	msg->mtype = RECOVERY_FILE_MTYPE;
	strncpy(msg->mdata,fname,sizeof(fname));

	fp = fopen(fname,"a");
	if (fp == NULL)
	{
		show_error("%s:fopen failed: (%s)",func,strerror(errno));
		return (FILE *)NULL;
	}

	rtn = msgsnd(*RecoveryMsgid, msg, sizeof(fname), IPC_NOWAIT);
	if (rtn < 0)
	{
		show_error("%s:msgsnd failed. reason: %s", func, strerror(errno));
		free(msg);
		msgctl(*RecoveryMsgid, IPC_RMID, NULL);
		*RecoveryMsgid = msgget (IPC_PRIVATE, 00666 | IPC_CREAT );
		return (FILE *)NULL;
	}

	strncpy(Recovery_Status_Inf->write_file,fname,sizeof(Recovery_Status_Inf->write_file));
	return fp;
}

static int
add_queue_file(char * data,int size)
{
	int cnt = 0;

	if ((QueueFp == NULL) || (data == NULL) || (size < 0))
	{
		return STATUS_ERROR;
	}
	/*fseek(QueueFp,0,SEEK_END);*/
	while (fwrite(data, size,1,QueueFp) <= 0)
	{
		fclose(QueueFp);
		QueueFp = NULL;
		if (cnt > MAX_RETRY_TIMES)
		{
			return STATUS_ERROR;
		}
		QueueFp = create_queue_file();
		cnt ++;
	}
	Recovery_Status_Inf->file_size += size;
	return STATUS_OK;
}

/*
 * set query in queue 
 */
int
PGRset_queue(ReplicateHeader * header,char * query)
{
	char * func = "PGRset_queue()";
	int header_size = 0;
	int query_size = 0;

	if ((Recovery_Status_Inf == NULL) || (header == NULL))
	{
		show_error("%s:header is null",func);
		return STATUS_ERROR;
	}

	query_size = ntohl(header->query_size);
	if (query_size < 0)
	{
		show_error("%s:query size less than 0",func);
		return STATUS_ERROR;
	}
	header_size = sizeof(ReplicateHeader);

	if (RecoverySemID <= 0)
	{
		show_error("%s:RecoverySemID is not initialized",func);
		return STATUS_ERROR;
	}
	PGRsem_lock(RecoverySemID, SEM_NUM_OF_RECOVERY_QUEUE);
	/* check existance of queue file */
	if (Recovery_Status_Inf->write_file[0] == '\0')
	{
		/* create new queue file */
		Recovery_Status_Inf->file_size = 0;
		QueueFp = create_queue_file();
	}
	else
	{
		/* check size of queue file */
		if (Recovery_Status_Inf->file_size + header_size + query_size > MAX_QUEUE_FILE_SIZE)
		{
			/* if the file size is over the limit, create new queue file */
			memset(Recovery_Status_Inf->write_file,0,sizeof(Recovery_Status_Inf->write_file));
			fclose(QueueFp);
			Recovery_Status_Inf->file_size = 0;
			QueueFp = create_queue_file();
		}
		else
		{
			QueueFp= fopen(Recovery_Status_Inf->write_file,"a");
		}
	}
	if (QueueFp == (FILE *)NULL)
	{
		PGRsem_unlock(RecoverySemID, SEM_NUM_OF_RECOVERY_QUEUE);
		show_error("%s:QueueFp open failed. error is %s",func,strerror(errno));
		return STATUS_ERROR;
	}
	header->replicate_id = htonl(*PGR_ReplicateSerializationID);
	if (add_queue_file((char *)header,header_size) != STATUS_OK)
	{
		PGRsem_unlock(RecoverySemID, SEM_NUM_OF_RECOVERY_QUEUE);
		show_error("%s:header add failed into queue file",func);
		return STATUS_ERROR;
	}
	if (query_size > 0)
	{
		if (add_queue_file((char *)query,query_size) != STATUS_OK)
		{
			PGRsem_unlock(RecoverySemID, SEM_NUM_OF_RECOVERY_QUEUE);
			show_error("%s:queue add failed into queue file",func);
			return STATUS_ERROR;
		}
	}
	fflush(QueueFp);
	fclose(QueueFp);
	PGRsem_unlock(RecoverySemID, SEM_NUM_OF_RECOVERY_QUEUE);

	return STATUS_OK;	
}

HostTbl *
PGRget_HostTbl(char * resolvedName, int port)
{
	HostTbl * ptr = NULL;
	int len = 0;

	if (Host_Tbl_Begin == NULL)
	{
		return NULL;
	}
	len = strlen(resolvedName);
	ptr = Host_Tbl_Begin;
	if (len > sizeof(ptr->resolvedName))
	{
		len = sizeof(ptr->resolvedName);
	}
	while(ptr->useFlag != DB_TBL_END)
	{
		if ((! memcmp(ptr->resolvedName,resolvedName,len)) &&
			(ptr->port == port))
		{
			return ptr;
		}
		ptr ++;
	}
	return (HostTbl*)NULL;
}

static void
sem_quit(int semid)
{
	semctl(semid, 0, IPC_RMID);
}

void
PGRclear_connections(void)
{
	Dlelem *ptr = NULL;

	pthread_mutex_lock(&transaction_table_mutex);
	ptr = DLGetHead(Transaction_Tbl_Begin);
	while (ptr)
	{
		TransactionTbl *transaction = DLE_VAL(ptr);
		if (transaction->conn != NULL)
		{
			PQfinish(transaction->conn);
			transaction->conn = NULL;
		}
		ptr = DLGetSucc(ptr);
	}
	pthread_mutex_unlock(&transaction_table_mutex);
}

void
PGRdestroy_transaction_table(void)
{
	Dlelem *ptr = NULL, *next;
	pthread_mutex_lock(&transaction_table_mutex);
	ptr = DLGetHead(Transaction_Tbl_Begin);
	while (ptr)
	{
		next = DLGetSucc(ptr);
		DLRemove(ptr);
		DLFreeElem(ptr);
		ptr = next;
	}
	DLFreeList(Transaction_Tbl_Begin);
	Transaction_Tbl_Begin = NULL;
	pthread_mutex_unlock(&transaction_table_mutex);
}

static bool
is_need_sync_time(ReplicateHeader * header)
{
	bool rtn = false;

	if (header->cmdSts == CMD_STS_PREPARE)
	{
		rtn = false;
	}
	else if ((header->cmdType == CMD_TYPE_COPY) ||
		(header->cmdType == CMD_TYPE_COPY_DATA) ||
		(header->cmdType == CMD_TYPE_COPY_DATA_END))
	{
		rtn = false;
	}
	if ((header->cmdSts == CMD_STS_QUERY ) &&
		((header->cmdType == CMD_TYPE_INSERT) || 
		 (header->cmdType == CMD_TYPE_UPDATE) || 
		 (header->cmdType == CMD_TYPE_DELETE) || 
		 (header->cmdType == CMD_TYPE_SET) || 
		 (header->cmdType == CMD_TYPE_EXECUTE)))
	{
		rtn = true;	
	}
	else
	{
		if ((header->cmdType == CMD_TYPE_COPY) ||
			(header->cmdType == CMD_TYPE_SELECT) ||
			(header->cmdType == CMD_TYPE_VACUUM) ||
			(header->cmdType == CMD_TYPE_ANALYZE) ||
			(header->cmdType == CMD_TYPE_BEGIN))
		{
			rtn = true;
		}
		if ((header->cmdSts == CMD_STS_TRANSACTION ) &&
			(header->cmdType != CMD_TYPE_BEGIN))
		{
			rtn = false;
		}
	}
	return rtn;
}

static bool
is_need_wait_answer(ReplicateHeader * header)
{
	bool rtn = false;

	if (header->cmdSts == CMD_STS_PREPARE)
	{
		rtn = false;
	}
	else if ((header->cmdType == CMD_TYPE_COPY) ||
		(header->cmdType == CMD_TYPE_COPY_DATA) ||
		(header->cmdType == CMD_TYPE_COPY_DATA_END))
	{
		rtn = false;
	}
	else if ((header->cmdSts == CMD_STS_QUERY ) &&
		((header->cmdType == CMD_TYPE_INSERT) || 
		 (header->cmdType == CMD_TYPE_UPDATE) || 
		 (header->cmdType == CMD_TYPE_DELETE) || 
		 (header->cmdType == CMD_TYPE_VACUUM) || 
		 (header->cmdType == CMD_TYPE_ANALYZE) || 
		 (header->cmdType == CMD_TYPE_EXECUTE)))
	{
		rtn = true;
	}
	else if ((header->cmdSts == CMD_STS_TRANSACTION ) ||
			(header->cmdSts == CMD_STS_SET_SESSION_AUTHORIZATION ) ||
			(header->cmdSts == CMD_STS_TEMP_TABLE ) ||
			(header->cmdType == CMD_TYPE_SELECT))
	{
		rtn = true;
	}

	return rtn;
}

static void
delete_template(HostTbl * ptr, ReplicateHeader * header)
{
	if ((ptr == (HostTbl *)NULL ) ||
		(header == (ReplicateHeader *)NULL) )
	{
		return;
	}

	if ((! strncmp(header->dbName,"template1",9)) ||
		(! strncmp(header->dbName,"template0",9)))
	{
		if ((header->cmdSts != CMD_STS_TRANSACTION ) &&
			( header->cmdSts != CMD_STS_SET_SESSION_AUTHORIZATION ) &&
			( header->cmdSts != CMD_STS_TEMP_TABLE ))
		{
			deleteTransactionTbl(ptr,header);
		}
	}
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    check_copy_command()
 * NOTES
 *    check the query which it is copy command or not 
 *    when the query is 'copy from', set 'stdin' after 'from' 
 * ARGS
 *    char * query: query strings(I)
 * RETURN
 *    copy command : changed copy command
 *    other command : NULL
 *--------------------------------------------------------------------
 */
static char *
check_copy_command(char * query)
{
	char * p;
	char * p1, *p2, *wp;
	char * buf;
	int size;

	if (query == NULL)
		return NULL;
	size = strlen(query) + strlen("  stdin  ");
	p = p1 = query;
	wp = strstr(p,"FROM");
	if (wp == NULL)
		wp = strstr(p,"from");
	
	if (wp != NULL)
	{
		p = wp + strlen("FROM");
		*p = '\0';
		p ++;
		while ((isspace(*p)) && (*p != '\0')) p++;
		while ((!isspace(*p)) && (*p != '\0')) p++;
		p2 = p;
		buf = malloc(size);
		if (buf == NULL)
		{
			return NULL;
		}
		snprintf(buf,size,"%s stdin %s",p1,p2);
		return buf;
	}
	return NULL;
}

static int
next_replication_id(void)
{
	char * func = "next_replication_id()";

	if (Recovery_Status_Inf == (RecoveryStatusInf *)NULL)
	{
		show_error("%s: Recovery_Status_Inf is NULL",func);
		return -1;
	}
	Recovery_Status_Inf->replication_id ++;
	Recovery_Status_Inf->check_point --;
	return (Recovery_Status_Inf->replication_id);
}

static void
check_replication_id(void)
{
	char * func = "check_replication_id()";

	if (Recovery_Status_Inf == (RecoveryStatusInf *)NULL)
	{
		show_error("%s: Recovery_Status_Inf is NULL",func);
		return ;
	}
	if (Recovery_Status_Inf->check_point < 0)
	{
		Recovery_Status_Inf->check_point = PGR_CHECK_POINT ;
		rewind(RidFp);
		PGRwrite_log_file(RidFp,"%u",Recovery_Status_Inf->replication_id + PGR_CHECK_POINT );
	}
}

int
PGRset_replication_id(uint32_t id)
{
	Recovery_Status_Inf->replication_id = id;
	return (Recovery_Status_Inf->replication_id);
}

int 
PGRdo_replicate(int sock,ReplicateHeader *header, char * query)
{

	char * func = "PGRdo_replicate()";

	struct timeval tv;
	int status = STATUS_OK;
	int recovery_status = 0;
	char * query_string = NULL;

	if (header->cmdType == CMD_TYPE_COPY)
	{
		query_string = check_copy_command(query);
		if (query_string == NULL)
		{
			return LOOP_CONTINUE;
		}
	}
	else
	{
		query_string = query;
		if (header->cmdType == CMD_TYPE_SET)
		{
			if (is_autocommit_off(query_string) == true)
			{
				PGR_AutoCommit = false;
			}
			else if (is_autocommit_on(query_string) == true)
			{
				PGR_AutoCommit = true;
			}
		}
	}
	header->isAutoCommit=PGR_AutoCommit ? 1 : 0;
	gettimeofday(&tv,NULL);
	header->tv.tv_sec = htonl(tv.tv_sec);
	header->tv.tv_usec = htonl(tv.tv_usec);
#ifdef PRINT_DEBUG
	show_debug("%s:query :: %s",func,query_string);
#endif			

	/* set query id */
	header->query_id = htonl(PGRget_next_query_id());

	/* save header for logging */
	if (is_need_sync_time(header) == true)
	{
		if (PGR_Log_Header != NULL)
		{
			memcpy(PGR_Log_Header,header,sizeof(ReplicateHeader));
			if (header->rlog == 0)
			{
				PGR_Log_Header->replicate_id = htonl(next_replication_id());
			}
		}
	}
	/* check rlog */
	if (header->rlog == CONNECTION_SUSPENDED_TYPE )
	{
		if (PGRget_rlog_header(header) == STATUS_OK)
		{
			header->rlog = CONNECTION_SUSPENDED_TYPE;
			
		}
	}
	
	/* check recovery mode */

	recovery_status = PGRget_recovery_status();
	PGRcheck_recovered_host();

	/* send replication packet */
	status = PGRreplicate_packet_send( header,query_string,sock,recovery_status);

	if ((header->cmdType == CMD_TYPE_COPY) &&
		(query_string != NULL))
	{
		free(query_string);
		query_string = NULL;
	}
	
	if (status == STATUS_ABORTED )
	{
#ifdef PRINT_DEBUG
		show_debug("%s:status is STATUS_ABORTED",func);
#endif			
		return LOOP_END;
	}
	if (status == STATUS_DEADLOCK_DETECT) 
	{
#ifdef PRINT_DEBUG
		show_debug("%s:status is STATUS_DEADLOCK_DETECT",func);
#endif			
		return LOOP_END;
	}
	return LOOP_CONTINUE;
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    PGRreturn_result()
 * NOTES
 *    Return result of execution 
 * ARGS
 *    int dest: socket of destination server (I)
 *    char *result: result data(I)
 *    int wait: wait flag (I)
 * RETURN
 *    OK: STATUS_OK
 *    NG: STATUS_ERROR
 *    NG: STATUS_LOCK_CONFLICT
 *    NG: STATUS_DEADLOCK_DETECT
 *--------------------------------------------------------------------
 */
int
PGRreturn_result(int dest, char * result, int wait)
{
	char * func = "PGRreturn_result()";
	fd_set	  wmask;
	struct timeval timeout;
	int rtn = 0;
	char * send_ptr = NULL;
	int send_size= 0;
	int buf_size = 0;
	int s = 0;
	int status = 0;
	int flag = 0;

	//show_debug("PGGRID: retornando %s", result);
	
	if (result == NULL)
	{
		show_error("%s:result is not initialize",func);
		return STATUS_ERROR;
	}
	if (dest < 0)
	{
		return STATUS_ERROR;
	}
	send_ptr = result;
	buf_size = PGR_MESSAGE_BUFSIZE;
	if (buf_size < 1)
		buf_size = 1;

	/*
	 * Wait for something to happen.
	 */
#ifdef MSG_DONTWAIT
	flag |= MSG_DONTWAIT;
#endif
#ifdef MSG_NOSIGNAL
	flag |= MSG_NOSIGNAL;
#endif

	for (;;)
	{
		timeout.tv_sec = PGR_Replication_Timeout;
		timeout.tv_usec = 0;

		FD_ZERO(&wmask);
		FD_SET(dest,&wmask);

		rtn = select(dest+1, (fd_set *)NULL, &wmask, (fd_set *)NULL, &timeout);
		if (rtn < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;

			show_error("%s:select failed ,errno is %s",func , strerror(errno));
			return STATUS_ERROR;
		}
		else if (rtn && FD_ISSET(dest, &wmask))
		{
			s = send(dest,send_ptr + send_size,buf_size - send_size ,flag); 
			if (s < 0)
			{
				if (errno == EINTR || errno == EAGAIN)
					continue;
				else
				{
					show_error("%s:send error: %d(%s)", func, errno, strerror(errno));
					memset(send_ptr, 0, PGR_MESSAGE_BUFSIZE);
					return STATUS_ERROR;
				}
			}
			else if (s > 0)
			{
				send_size += s;
				if (send_size == buf_size)
				{

					status = STATUS_OK;
					if (wait == PGR_WAIT_ANSWER)
					{
						status = read_answer(dest);
					}
					return status;
				}
			}
			else /* s == 0 */
			{
				show_error("%s:unexpected EOF", func);
				memset(send_ptr, 0, PGR_MESSAGE_BUFSIZE);
				return STATUS_ERROR;
			}
		}
	}
	memset(send_ptr, 0, PGR_MESSAGE_BUFSIZE);
	return STATUS_ERROR;
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    read_answer()
 * NOTES
 *    Receive answer packet
 * ARGS
 *    int dest: socket of destination server (I)
 * RETURN
 *    OK: STATUS_OK
 *    NG: STATUS_ERROR
 *    NG: STATUS_LOCK_CONFLICT
 *    NG: STATUS_DEADLOCK_DETECT
 *--------------------------------------------------------------------
 */
static int
read_answer(int dest)
{
	char * func = "read_answer()";
	fd_set	  rmask;
	struct timeval timeout;
	int rtn;
	ReplicateHeader header;
	char * answer = NULL;
	int status = STATUS_ERROR;

	for(;;)
	{
		if (answer != NULL)
		{
			free(answer);
			answer = NULL;
		}
		timeout.tv_sec = PGR_Replication_Timeout;
		timeout.tv_usec = 0;
		FD_ZERO(&rmask);
		FD_SET(dest,&rmask);
		rtn = select(dest+1, &rmask, (fd_set *)NULL, (fd_set *)NULL, &timeout);
		if (rtn < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;

			show_error("%s:select failed ,errno is %s",func , strerror(errno));
			return STATUS_ERROR;
		}
		else if (rtn && FD_ISSET(dest, &rmask))
		{
			memset(&header,0,sizeof(ReplicateHeader));
			answer = PGRread_packet(dest,&header);
			if (answer == NULL)
			{
				status = STATUS_ERROR;
				break;
			}
			if ((header.cmdSts != CMD_STS_RESPONSE) && 
				(header.cmdSts != CMD_STS_NOTICE))
			{
				show_error("%s:none response packet received",func);
				free(answer);
				answer = NULL;
				status = STATUS_ERROR;
				break;
			}
#ifdef PRINT_DEBUG
			show_debug("%s:answer[%s]",func,answer);
#endif			
			if (answer != NULL)
			{
				if (!strncasecmp(answer,PGR_QUERY_DONE_NOTICE_CMD,strlen(PGR_QUERY_DONE_NOTICE_CMD)))
				{
#ifdef PRINT_DEBUG
					show_debug("%s:QUERY DONE",func);
#endif			
					status = STATUS_OK;
				}
				else if (!strncasecmp(answer,PGR_QUERY_ABORTED_NOTICE_CMD,strlen(PGR_QUERY_ABORTED_NOTICE_CMD)))
				{
#ifdef PRINT_DEBUG
					show_debug("%s:QUERY ABORTED",func);
#endif			
					status = STATUS_ABORTED;
				}
				else if (!strncasecmp(answer,PGR_LOCK_CONFLICT_NOTICE_CMD,strlen(PGR_LOCK_CONFLICT_NOTICE_CMD)))
				{
#ifdef PRINT_DEBUG
					show_debug("%s:LOCK CONFLICT !!",func);
#endif			
					status = STATUS_LOCK_CONFLICT;
				}
				else if (!strncasecmp(answer,PGR_DEADLOCK_DETECT_NOTICE_CMD,strlen(PGR_DEADLOCK_DETECT_NOTICE_CMD)))
				{
#ifdef PRINT_DEBUG
					show_debug("%s:DEADLOCK DETECT !!",func);
#endif			
					status = STATUS_DEADLOCK_DETECT;
				}
				free(answer);
				answer = NULL;
			}
			return status;
		}
	}
	return status;
}

/*--------------------------------------------------
 * SYMBOL
 *     PGRreplicate_packet_send()
 * NOTES
 *     Send query to each cluster DB servers and return result.
 * ARGS 
 *     ReplicateHeader * header : packet header (I)
 *     char * query : query for replication (I)
 *     int dest : destination socket for return result (I)
 * RETURN
 *     OK : STATUS_OK
 *     NG : STATUS_ERROR
 *     DEADLOCK : STATUS_DEADLOCK_DETECT
 *---------------------------------------------------
 */
int
PGRreplicate_packet_send( ReplicateHeader * header, char * query,int dest,int recovery_status) {
	return replicate_packet_send_internal(header,query,dest,recovery_status,false);
}


int
replicate_packet_send_internal(ReplicateHeader * header, char * query,int dest,int recovery_status,bool isHeldLock)
{
	char * func = "replicate_packet_send_internal()";
	HostTbl * host_ptr;
	HostTbl * source_host_ptr = (HostTbl*)NULL;
	int only_one_host=(strcmp(header->sitehostname, "0")!=0) && (strcmp(header->sitehostname, "")!=0);
	host_ptr = (HostTbl*)NULL;
	
	int status = STATUS_OK;
	int sem_cnt = 0;
	int sem_id = 0;
	char	   *database = NULL;
	char	   port[8];
	char	   *userName = NULL;
	char	   *password = NULL;
	char * md5Salt = NULL;
	char * cryptSalt = NULL;
	char * host = NULL;
	char result[PGR_MESSAGE_BUFSIZE];

	pthread_attr_t attr;
	int rc = 0;
	int t = 0;
	int t_cnt = 0;
	int source_t_cnt = -1;
	int transaction_count = 0;
	int *results_from_thread;
	bool reliable_mode = true;

	pthread_t thread[MAX_DB_SERVER];
	ThreadArgInf thread_arg[MAX_DB_SERVER];


#ifdef PRINT_DEBUG
	show_debug("cmdSts=%c",header->cmdSts);
	if(header->cmdType!='\0')
		show_debug("cmdType=%c",header->cmdType);
	show_debug("rlog=%d",header->rlog);
	show_debug("port=%d",ntohs(header->port));
	show_debug("pid=%d",ntohs(header->pid));
	show_debug("from_host=%s",header->from_host);
	show_debug("dbName=%s",header->dbName);
	show_debug("userName=%s",header->userName);
	show_debug("recieve sec=%u",ntohl(header->tv.tv_sec));
	show_debug("recieve usec=%u",ntohl(header->tv.tv_usec));
	show_debug("query_size=%d",ntohl(header->query_size));
	show_debug("request_id=%d",ntohl(header->request_id));
	show_debug("replicate_id=%d",ntohl(header->replicate_id));
	show_debug("recovery_status=%d",recovery_status);
	if (only_one_host){
		show_debug("sitehostname=%s",header->sitehostname);
		show_debug("siteport=%d",header->siteport);
		show_debug("siterecport=%d",header->siterecport);
	}
	if (header->cmdSts != CMD_STS_PREPARE)
		show_debug("query=%s",query);

#endif

	/* check rlog type */
	if (header->rlog == FROM_R_LOG_TYPE)
	{
		if (is_executed_query_in_origin(header) == false)
		{
#ifdef PRINT_DEBUG
			show_debug("this query is not yet done in source cluster db. so it wait for receive re-replicate request");
#endif
			/* wait re-replicate request */
			return STATUS_SKIP_REPLICATE;
		}
	}
	/*
	 * loop while registrated cluster DB exist 
	 */
	if (Host_Tbl_Begin == NULL)
	{
		return STATUS_ERROR;
	}
	host_ptr = Host_Tbl_Begin;
	
	PGR_Response_Inf->current_cluster = 0;
	memset(result,0,sizeof(result));
	sem_cnt = 1;

	if (is_need_queue_jump(header,query) == false)
	{
		sem_id = SemID;
	}
	else
	{
		sem_id = VacuumSemID;
	}
	if(!isHeldLock) {
#ifdef PRINT_DEBUG
		show_debug("sem_lock [%d] req",sem_cnt);
#endif

		PGRsem_lock(sem_id,sem_cnt);
#ifdef PRINT_DEBUG
		show_debug("sem_lock [%d] got it",sem_cnt);
#endif
	}		
	++*PGR_ReplicateSerializationID;

	/* set replication log */
	if (is_need_use_rlog(header) == true)
	{
		PGRset_rlog(header,query);
	}

	pthread_attr_init(&attr);
	pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
	PGR_Response_Inf->current_cluster = 0;
	t_cnt = 0;
	while(host_ptr->useFlag != DB_TBL_END)
	{
		/*
		 * check the status of the cluster DB
		 */
		if ((host_ptr->useFlag != DB_TBL_USE) &&
			(host_ptr->useFlag != DB_TBL_INIT))
		{
			host_ptr ++;
			continue;
		}
		/*
		 * skip loop during recover and the host name is master DB
		 */
		if (is_master_in_recovery(host_ptr->hostName, host_ptr->port,recovery_status) == true)
		{
			if (PGRset_queue(header,query) != STATUS_OK)
			{
				show_error("%s:failed to put query to queue.abort to recovery",func);
				PGRset_recovery_status(RECOVERY_INIT);
			}
#ifdef PRINT_DEBUG
			show_debug("%s master is using for recovery",func);
#endif
			host_ptr ++;
			continue;
		}
		show_debug("checking transaction status");
		host_ptr = check_host_transaction_status(header, host_ptr);
		/*
		 *  compare with the host name and the exceptional host name
		 */
		thread_arg[t_cnt].header = header;
		thread_arg[t_cnt].query = query;
		thread_arg[t_cnt].returningTuples = false;
		thread_arg[t_cnt].dest = dest;
		thread_arg[t_cnt].host_ptr = host_ptr;
		thread_arg[t_cnt].current_cluster = t_cnt;
		thread_arg[t_cnt].transaction_tbl = (TransactionTbl *)NULL;


		if (PGRis_same_host(header->from_host,ntohs(header->port),host_ptr->resolvedName, host_ptr->port) == true)
		{
#ifdef PRINT_DEBUG
			show_debug("source host");
#endif
			/* replication to source cluster db */
			source_host_ptr = host_ptr;
			source_t_cnt = t_cnt;

			if (header->rlog == FROM_R_LOG_TYPE )
			{
#ifdef PRINT_DEBUG
				show_debug("%s: This simple query was suspended. Therefore this query is not re-replicated to source cluster db.",func);
#endif
			}
			check_transaction_status(header, thread_arg[t_cnt].transaction_tbl);
			t_cnt++;
		}
		/* replication to other cluster db */
		else
		{

			if (only_one_host && 
				!(PGRis_same_host(header->sitehostname,header->siteport,host_ptr->resolvedName, host_ptr->port) == true)){
				//It's not the host. Cancel
				show_debug("ignore host");
				host_ptr ++;
				continue;
			}


			if ((header->rlog == CONNECTION_SUSPENDED_TYPE ) &&
			    (header->cmdSts == CMD_STS_TRANSACTION) )
			{
#ifdef PRINT_DEBUG
				show_debug("%s: This transaction query was suspended. Therefore this query is not replicated to other cluster dbs.",func);
#endif
			}
			else
			{
				/*
				 * get the transaction table data
				 * it has the connection data with each cluster DB
				 */
				thread_arg[t_cnt].transaction_tbl = getTransactionTbl(host_ptr,header);
				/*
				 * if the transaction process is new one, 
				 * create connection data and add the transaction table
				 */
				if (thread_arg[t_cnt].transaction_tbl == (TransactionTbl *)NULL)
				{
					thread_arg[t_cnt].transaction_tbl = setTransactionTbl(host_ptr, header);
					if (thread_arg[t_cnt].transaction_tbl == (TransactionTbl *)NULL)
					{
						show_error("%s:setTransactionTbl failed",func);
						if ( header->cmdSts != CMD_STS_NOTICE )
						{
							PGRset_host_status(host_ptr,DB_TBL_ERROR);
						}
						host_ptr ++;
						continue;
					}
					StartReplication[t_cnt] = true;
				}
				else
				{
					/*
					 * re-use the connection data
					 */
					if ((thread_arg[t_cnt].transaction_tbl->conn != (PGconn *)NULL) &&
					    (thread_arg[t_cnt].transaction_tbl->conn->sock > 0))
					{
						/*
						  memset(thread_arg[t_cnt].transaction_tbl->conn->inBuffer,0,thread_arg[t_cnt].transaction_tbl->conn->inBufSize);
						  memset(thread_arg[t_cnt].transaction_tbl->conn->outBuffer,0,thread_arg[t_cnt].transaction_tbl->conn->outBufSize);
						*/
						StartReplication[t_cnt] = false;
					}
					else
					{
						if (thread_arg[t_cnt].transaction_tbl->conn != (PGconn *)NULL)
						{
							PQfinish(thread_arg[t_cnt].transaction_tbl->conn);
							thread_arg[t_cnt].transaction_tbl->conn = NULL;
						}

						database = (char *)(header->dbName);
						snprintf(port,sizeof(port),"%d", host_ptr->port);
						userName = (char *)(header->userName);
						password = (char *)(header->password);
						md5Salt = (char *)(header->md5Salt);
						cryptSalt = (char *)(header->cryptSalt);
						host = (char *)(host_ptr->hostName);

						//show_debug("PGGRID: creating connection");

						thread_arg[t_cnt].transaction_tbl->conn = PGRcreateConn(host,port,database,userName,password,md5Salt,cryptSalt, PGR_LOGIN_TIMEOUT);
						StartReplication[t_cnt] = true;
#ifdef PRINT_DEBUG
						show_debug("%s:connect db:%s port:%s user:%s host:%s query:%s",
							   func, database,port,userName,host,query);
#endif
					}
				}
				show_debug("PGGRID: creating send thread");
				check_transaction_status(header, thread_arg[t_cnt].transaction_tbl);
				transaction_count = thread_arg[t_cnt].transaction_tbl->transaction_count;
				rc = pthread_create(&thread[t_cnt], &attr, thread_send_cluster, (void*)&thread_arg[t_cnt]);	  

				if (rc)
				{
					show_error("pthread_create error");
				}
				t_cnt++;
				show_debug("PGGRID: send thread running");
			}
		}
		/*
		 * send replication query to each cluster server
		 */
		if (host_ptr->useFlag != DB_TBL_USE) 
		{
			PGRset_host_status(host_ptr,DB_TBL_USE);
		}

		status = STATUS_OK;	
		
		host_ptr++;		
		PGR_Response_Inf->current_cluster ++;
	}    


	//show_debug("PGGRID: saiu do while");
	/* When the query is SELECT, source cluster would not need to wait other cluster's result */
	if ((header->cmdType == CMD_TYPE_SELECT) && (header->cmdSts != CMD_STS_PREPARE))
	{
		thread_send_source( (void*)&thread_arg[source_t_cnt]);
		reliable_mode = false;
	}

	pthread_attr_destroy(&attr);
	//show_debug("PGGRID: verificando resultado");
	results_from_thread = malloc(t_cnt * sizeof(int));
	for ( t = 0 ; t < t_cnt; )
	{
		int result;
		if (t == source_t_cnt)
		{
			t++;
			continue;
		}
		rc = pthread_join(thread[t], (void **)&result);
		if ((rc != 0) && (errno == EINTR))
		{
			usleep(100);
			continue;
		}
		results_from_thread[t] = (int)result;
		pthread_detach(thread[t]);
		t++;
	}
	//show_debug("PGGRID: comparando resultado");

	if (compare_results(results_from_thread, t_cnt, source_t_cnt) == false)
	show_error("query results discrepancy between cluster servers: %s", query);
	free(results_from_thread);

	thread_arg[source_t_cnt].transaction_count = transaction_count;
	/*
	 * send replication query to source cluster server.
	 */
	if ((source_t_cnt >= 0) && ( reliable_mode == true ))
	{
		//show_debug("PGGRID: enviando resposta ao source");
		thread_send_source( (void*)&thread_arg[source_t_cnt]);
	}
	/* unset replication log */
	if (is_need_use_rlog(header) == true)
	{
		PGRunset_rlog(header,query);
	}

	check_replication_id();
	if (header->cmdSts == CMD_STS_PREPARE)
	{
		if (header->cmdType != CMD_TYPE_P_SYNC)
		{
			if (PGR_Parse_Session_Started == false)
			{
				PGR_Parse_Session_Started = true;
			}
		}
	}
	else
	{
		PGR_Parse_Session_Started = false;
	}

	if(!isHeldLock) {
#ifdef PRINT_DEBUG
		show_debug("sem_unlock[%d]",sem_cnt);
#endif
		PGRsem_unlock(sem_id,sem_cnt);
	}

	//show_debug("PGGRID: saindo da funcao");
	return status;
}

static void *
thread_send_source(void * arg)
{
	char * func = "thread_send_source()";
	ThreadArgInf * thread_arg = NULL;
	ReplicateHeader * header = (ReplicateHeader*)NULL;
	char * query = NULL;
	int dest = 0;
	HostTbl * host_ptr = (HostTbl*)NULL;
	uintptr_t status = STATUS_OK;
	int transaction_count = 0;
	char result[PGR_MESSAGE_BUFSIZE];
	bool sync_command_flg = false;

	if (arg == NULL)
	{
		show_error("%s:arg is NULL",func);
		status = (uintptr_t)STATUS_ERROR;
		pthread_exit((void *) status);
	}
	thread_arg = (ThreadArgInf *)arg;
	header = thread_arg->header;
	query = thread_arg->query;
	dest = thread_arg->dest;
	host_ptr = thread_arg->host_ptr;
	transaction_count = thread_arg->transaction_count;

	if(header->cmdSts==CMD_STS_OTHER &&
	   header->cmdType==CMD_TYPE_CONNECTION_CLOSE) 
	{
			return (void *)0;
	}

	if (header->rlog == FROM_R_LOG_TYPE )
	{
		/* It is not necessary to return rlog to source DB. */
#ifdef PRINT_DEBUG
	        show_debug("%s: It is not necessary to return rlog to source DB",func);
#endif
		status = (uintptr_t)STATUS_OK;
		return (void *)status;
	}

	/**
	 * NOTE: 
	 * We can use PGR_ReplicateSerializationID here , because 
	 * all queries from cluster server isn't recovery query.
	 *
	 */
	if (is_need_sync_time(header) == true)
	{
		if (transaction_count >1 )
		{
			sync_command_flg = false;
		}
		else
		{
			sync_command_flg = true;
		}
	}

	if (thread_arg->returningTuples == true){
		//show_debug("PGGRID: Retornando tuplas para o site");
		snprintf(result,PGR_MESSAGE_BUFSIZE,
			"%s,%s", 
			PGR_RETURN_TUPLES_MSG,
			response_value);
	}
	else if (sync_command_flg == true)
	{
		snprintf(result,PGR_MESSAGE_BUFSIZE,
			"%d,%u,%u,%u,%d,%u", 
			PGR_SET_CURRENT_TIME_FUNC_NO,
			(unsigned int)ntohl(header->tv.tv_sec),
			(unsigned int)ntohl(header->tv.tv_usec),
			(unsigned int)ntohl(PGR_Log_Header->replicate_id),
			PGR_Response_Inf->response_mode,
			*PGR_ReplicateSerializationID);
	}
	else
	{
		snprintf(result,PGR_MESSAGE_BUFSIZE,
			"%d,%u,%u,%d", 
			PGR_SET_CURRENT_REPLICATION_QUERY_ID_NO,
			*PGR_ReplicateSerializationID,
			0,
			PGR_Response_Inf->response_mode);
	}
	/* execute query in the exceptional host */
	/* it is not use replication */
	if (is_need_wait_answer(header) == true)
	{
		status = PGRreturn_result(dest,result, PGR_WAIT_ANSWER);
	}
	else
	{
		status = PGRreturn_result(dest, result, PGR_NOWAIT_ANSWER);
	}

	/*
	if (status == STATUS_ERROR )
	{
		show_error("%s: %s[%d] should be down ",func,host_ptr->hostName,host_ptr->port);
		PGRset_host_status(host_ptr,DB_TBL_ERROR);
	}
	*/

	/* delete server table when query use template db */
	if (PGR_Response_Inf->response_mode != PGR_RELIABLE_MODE)
	{
		delete_template(host_ptr,header);
	}
#ifdef PRINT_DEBUG
	show_debug("end thread_send_source()");
#endif
	return (void *)0;
}

static void *
thread_send_cluster(void * arg)
{
	char * func = "thread_send_cluster()";
	ThreadArgInf * thread_arg = NULL;
	ReplicateHeader * header = (ReplicateHeader*)NULL;
	char * query = NULL;
	int dest = 0;
	HostTbl * host_ptr = (HostTbl*)NULL;
	uintptr_t rtn = 0;
	uintptr_t status = STATUS_OK;
	TransactionTbl * transaction_tbl = (TransactionTbl *)NULL;
	int current_cluster = 0;
	char result[PGR_MESSAGE_BUFSIZE];

#ifdef PRINT_DEBUG
	show_debug("start thread_send_cluster()");
#endif
	if (arg == NULL)
	{
		show_error("%s:arg is NULL",func);
		status = (uintptr_t)STATUS_ERROR;
		pthread_exit((void *) status);
	}

	thread_arg = (ThreadArgInf *)arg;
	header = thread_arg->header;
	query = thread_arg->query;
	dest = thread_arg->dest;
	host_ptr = thread_arg->host_ptr;
	transaction_tbl = thread_arg->transaction_tbl;
	current_cluster = thread_arg->current_cluster;

	
	if(header->cmdSts==CMD_STS_OTHER &&
	   header->cmdType==CMD_TYPE_CONNECTION_CLOSE) 
	{
		check_delete_transaction(host_ptr, header);
		return (void *)0;
	}

	rtn = (uintptr_t)send_replicate_packet_to_server( transaction_tbl, current_cluster, host_ptr, header, query ,  result,*PGR_ReplicateSerializationID, false, arg);

	show_debug("%s:return value from send_replicate_packet_to_server() is %d",func,rtn);
	if (rtn == (uintptr_t)STATUS_ABORTED)
	{
		show_debug("PGGRID: status aborted");
		snprintf(result,PGR_MESSAGE_BUFSIZE,"%d", PGR_NOTICE_ABORT_FUNC_NO);
		status = PGRreturn_result(dest, result, PGR_NOWAIT_ANSWER);
		status = (uintptr_t)STATUS_ABORTED;
		pthread_exit((void *) status);
	}
	/* delete server table when query use template db */
	delete_template(host_ptr,header);
	show_debug("%s:pthread_exit[%d]",func,current_cluster );

	pthread_exit((void *) rtn);
}

/*--------------------------------------------------
 * SYMBOL
 *     PGRreplicate_packet_send_each_server()
 * NOTES
 *     Send query to a cluster DB server and return result.
 * ARGS 
 *     HostTbl * ptr : cluster server info table (I)
 *     bool return_response : flag for return result(I)
 *     ReplicateHeader * header: header data (I)
 *     char * query : query data (I)
 *     int dest : socket of destination server(I)
 * RETURN
 *     OK : STATUS_OK
 *     NG : STATUS_ERROR
 *---------------------------------------------------
 */
int
PGRreplicate_packet_send_each_server( HostTbl * ptr, bool return_response, ReplicateHeader * header, char * query,int dest)
{
	char * func = "PGRreplicate_packet_send_each_server()";
	char * host;
	int rtn;

	host = ptr->hostName;
	/*
	 * send query to cluster DB
	 */
	if (PGR_Result == NULL)
	{
		show_error("%s:PGR_Result is not initialize",func);
		return STATUS_ERROR;
	}

	rtn = PGRsend_replicate_packet_to_server( ptr, header,query,PGR_Result, dest, false);

	return rtn;
}

/*--------------------------------------------------
 * SYMBOL
 *     PGRread_packet()
 * NOTES
 *     Read packet data and send the query to each cluster DB.
 *     The packet data has header data and query data.
 * ARGS 
 *     int sock : socket (I)
 *     ReplicateHeader *header : header data (O)
 * RETURN
 *     OK: pointer of read query
 *     NG: NULL
 *---------------------------------------------------
 */
char *
PGRread_packet(int sock, ReplicateHeader *header)
{
	char * func = "PGRread_packet()";
	int r =0;
	int cnt = 0;
	char * read_ptr = NULL;
	int read_size = 0;
	int header_size = 0;
	char * query = NULL;
	fd_set      rmask;
	struct timeval timeout;
	int rtn;

	if (header == NULL)
	{
		return NULL;
	}
	memset(header,0,sizeof(ReplicateHeader));
	read_ptr = (char*)header;
	header_size = sizeof(ReplicateHeader);
	cnt = 0;

	for (;;){
		/*
		 * read header data
		 */

		timeout.tv_sec = 1;
		timeout.tv_usec = 0;

				/*
		 * Wait for something to happen.
		 */
		FD_ZERO(&rmask);
		FD_SET(sock,&rmask);
		rtn = select(sock+1,  &rmask, (fd_set *)NULL,(fd_set *)NULL, &timeout);

		if (rtn < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;

			show_error("%s:select failed ,errno is %s",func , strerror(errno));
			return NULL;
		}

		if (rtn && FD_ISSET(sock, &rmask))
		{
			r = recv(sock,read_ptr + read_size ,header_size - read_size, MSG_WAITALL);
			/*
			  r = recv(sock,read_ptr + read_size ,header_size - read_size, 0);
			*/
			if (r < 0)
			{
				show_error("%s:recv failed: (%s)",func,strerror(errno));
				if (errno == EINTR || errno == EAGAIN)
					continue;
				else
				{
					show_error("%s:recv failed: (%s)",func,strerror(errno));
					return NULL;
				}
			}
			else if (r > 0)
			{
				read_size += r;
				if ( read_size == header_size)
				{
					query = PGRread_query(sock,header);
					return query;
				}
			}
			else if (r == 0)
			{
				return NULL;
			}
		}
	}
	return NULL;
}

char *
PGRread_query(int sock, ReplicateHeader *header)
{
	char * func = "PGRread_query()";
	int r =0;
	int cnt = 0;
	char * read_ptr;
	int read_size = 0;
	int query_size = 0;
	char * query = NULL;

	query_size = ntohl(header->query_size);
	if (query_size < 0)
	{
		show_error("%s:receive size less than 0",func);
		return NULL;
	}
	query = malloc(query_size+4);
	if (query == NULL)
	{
		/*
		 * buffer allocation failed
		 */
		show_error("%s:malloc failed: (%s)",func,strerror(errno));
		return NULL;
	}
	memset(query,0,query_size+4);
	if (query_size == 0)
	{
		return query;
	}
	read_size = 0;
	cnt = 0;
	read_ptr = (char *)query;
	for (;;)
	{
		/*
		 * read query data
		 */

		/*r = recv(sock,read_ptr + read_size ,query_size - read_size, MSG_WAITALL); */
		r = recv(sock,read_ptr + read_size ,query_size - read_size, 0); 
		if (r < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;
			else
			{
				show_error("%s:recv failed: (%s)",func,strerror(errno));
				free(query);
				query = NULL;
				return NULL;
			}
		}
		else if (r > 0)
		{
			read_size += r;
			if ( read_size == query_size)
			{
				return query;
			}
		}
		else /* r == 0 */
		{
			show_error("%s:unexpected EOF", func);
			free(query);
			query = NULL;
			return NULL;
		}
	}
	free(query);
	query = NULL;
	return NULL;
}

static bool
is_autocommit_off(char * query)
{
	int i;
	char buf[256];
	char * p = NULL;

	if (query == NULL)
		return false;
	memset(buf,0,sizeof(buf));
	p = query;
	i = 0;
	while ( *p != '\0' )
	{
		buf[i++] = toupper(*p);
		p++;
		if (i >= (sizeof(buf) -2))
			break;
	}
	p = strstr(buf,"AUTOCOMMIT");
	if ( p == NULL)
	{
		return false;
	}
	p = strstr(buf,"OFF");
	if ( p == NULL )
	{
		return false;
	}
	return true;
}

static bool
is_autocommit_on(char * query)
{
	int i;
	char buf[256];
	char * p = NULL;

	if (query == NULL)
		return false;
	memset(buf,0,sizeof(buf));
	p = query;
	i = 0;
	while ( *p != '\0' )
	{
		buf[i++] = toupper(*p);
		p++;
		if (i >= (sizeof(buf) -2))
			break;
	}
	p = strstr(buf,"AUTOCOMMIT");
	if ( p == NULL)
	{
		return false;
	}
	p = strstr(buf,"ON");
	if ( p == NULL )
	{
		return false;
	}
	return true;
}

static unsigned int 
get_host_ip_from_tbl(char * host)
{
	Dlelem * ptr = NULL;

	pthread_mutex_lock(&transaction_table_mutex);
	if (Transaction_Tbl_Begin == NULL)
	{
		pthread_mutex_unlock(&transaction_table_mutex);
		return 0;
	}
	ptr = DLGetHead(Transaction_Tbl_Begin);
	while (ptr)
	{
		TransactionTbl *transaction = DLE_VAL(ptr);
		if (!strncasecmp(transaction->host,host,sizeof(transaction->host)))
		{
			pthread_mutex_unlock(&transaction_table_mutex);
			return transaction->hostIP;
		}
		ptr = DLGetSucc(ptr);
	}
	pthread_mutex_unlock(&transaction_table_mutex);

	return 0;
}

static unsigned int 
get_srcHost_ip_from_tbl(char * srcHost)
{
	Dlelem * ptr = NULL;

	pthread_mutex_lock(&transaction_table_mutex);

	if (Transaction_Tbl_Begin == NULL)
	{
		pthread_mutex_unlock(&transaction_table_mutex);

		return 0;
	}
	ptr = DLGetHead(Transaction_Tbl_Begin);
	while (ptr)
	{
		TransactionTbl *transaction = DLE_VAL(ptr);
		if (!strncasecmp(transaction->srcHost,srcHost,sizeof(transaction->srcHost)))
		{
			pthread_mutex_unlock(&transaction_table_mutex);

			return transaction->srcHostIP;
		}
		ptr = DLGetSucc(ptr);
	}
	pthread_mutex_unlock(&transaction_table_mutex);

	return 0;
}

unsigned int
PGRget_next_query_id(void)
{
	if (PGR_Query_ID >= PGR_MAX_QUERY_ID)
	{
		PGR_Query_ID = 0;
	}
	PGR_Query_ID ++;
	return PGR_Query_ID;
}


void
PGRnotice_replication_server(char * hostName, unsigned short portNumber,unsigned short recoveryPortNumber, unsigned short lifecheckPortNumber, char * userName)
{
	char * func ="PGRnotice_replication_server()";
	ReplicateHeader  header;
	char query[PGR_MESSAGE_BUFSIZE];

	if (((hostName == NULL) || (*hostName == 0)) ||
		((userName == NULL) || (*userName == 0)) ||
		((portNumber == 0) || (recoveryPortNumber == 0)))
	{
#ifdef PRINT_DEBUG
		show_debug("%s: can not connect server[%s][%s][%d][%d]",func,hostName,userName,portNumber,recoveryPortNumber);
#endif			
		return;
	}
	memset(&header,0,sizeof(ReplicateHeader));
	memset(query,0,sizeof(query));
	snprintf(query,sizeof(query)-1,"SELECT %s(%d,'%s',%d,%d,%d)",
			PGR_SYSTEM_COMMAND_FUNC,
			PGR_STARTUP_REPLICATION_SERVER_FUNC_NO,
			hostName,
			portNumber,
			recoveryPortNumber,
			lifecheckPortNumber);
	header.cmdSys = CMD_SYS_CALL;
	header.cmdSts = CMD_STS_NOTICE;
	header.query_size = htonl(strlen(query));
	header.query_id = htonl(PGRget_next_query_id());
	strncpy(header.from_host,hostName,sizeof(header.from_host));
	strncpy(header.userName,userName,sizeof(header.userName));
	strcpy(header.dbName,"template1");
	PGRreplicate_packet_send( &header, query, NOTICE_SYSTEM_CALL_TYPE ,RECOVERY_INIT);
}

static bool
is_need_use_rlog(ReplicateHeader * header)
{
	bool rtn = false;
	if ((Cascade_Inf->useFlag != DB_TBL_USE) ||
		(PGR_Use_Replication_Log != true)  ||
		(header->rlog > 0))
	{
		rtn=false;
	}
	else if ((header->cmdSts == CMD_STS_QUERY ) &&
		((header->cmdType == CMD_TYPE_INSERT) || 
		 (header->cmdType == CMD_TYPE_UPDATE) || 
		 (header->cmdType == CMD_TYPE_DELETE) || 
		 (header->cmdType == CMD_TYPE_EXECUTE)))
	{
		rtn = true;	
	}
	else 
	{
		if ((header->cmdSts == CMD_STS_TRANSACTION ) &&
			(header->cmdType == CMD_TYPE_COMMIT))
		{
			rtn = true;
		}
	}
	return rtn;
}

int
PGRinit_transaction_table(void)
{
	if (Transaction_Tbl_Begin != NULL)
	{
		DLFreeList(Transaction_Tbl_Begin);
	}

	Transaction_Tbl_Begin = DLNewList();

	return STATUS_OK;
} 

static bool
is_need_queue_jump( ReplicateHeader * header,char *query)
{
	if (header == NULL)
	{
		return true;
	}

	if (header->cmdSts == CMD_STS_QUERY)
	{
		if ((header->cmdType  == CMD_TYPE_VACUUM ) ||
			(header->cmdType  == CMD_TYPE_ANALYZE ))
		{
			if ((strstr(query,"full") == NULL) &&
				(strstr(query,"FULL") == NULL))
			{
				return true;
			}
		}
	}
	return false;
}


static bool
is_executed_query_in_origin( ReplicateHeader *header )
{
	char *database = NULL;
	char port[8];
	char *userName = NULL;
	char *password = NULL;
	char * md5Salt = NULL;
	char * cryptSalt = NULL;
	char * host = NULL;
	HostTbl * host_ptr = (HostTbl*)NULL;
	TransactionTbl * transaction_tbl = (TransactionTbl*)NULL;
	PGconn * conn = (PGconn *)NULL;
	bool result = false;

	if (Host_Tbl_Begin == NULL)
	{
		return STATUS_ERROR;
	}
	host_ptr = Host_Tbl_Begin;
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
		if (PGRis_same_host(header->from_host,ntohs(header->port),host_ptr->hostName, host_ptr->port) == true)
		{
			break;
		}
		host_ptr ++;
	}
	if (host_ptr->useFlag == DB_TBL_END)
	{
		return false;
	}
	/*
	 * set up the connection
	 */
	transaction_tbl = getTransactionTbl(host_ptr,header);
	if (transaction_tbl == (TransactionTbl *)NULL)
	{
		transaction_tbl = setTransactionTbl(host_ptr, header);
		if (transaction_tbl == (TransactionTbl *)NULL)
		{
			return false;
		}
	}
	else
	{
		if ((transaction_tbl->conn == (PGconn *)NULL) ||
			(transaction_tbl->conn->sock <= 0))
		{
			database = (char *)(header->dbName);
			snprintf(port,sizeof(port),"%d", host_ptr->port);
			userName = (char *)(header->userName);
			password = (char *)(header->password);
			md5Salt = (char *)(header->md5Salt);
			cryptSalt = (char *)(header->cryptSalt);
			host = (char *)(host_ptr->hostName);
		 	transaction_tbl->conn = PGRcreateConn(host,port,database,userName,password,md5Salt,cryptSalt, PGR_LOGIN_TIMEOUT);
		}
	}
	conn = transaction_tbl->conn;
	if (conn == NULL)
	{
		return false;
	}

	result = is_executed_query( conn, header);
	deleteTransactionTbl(host_ptr,header);
	return result;
}

static bool
is_executed_query( PGconn *conn, ReplicateHeader * header)
{
	static PGresult * res = (PGresult *)NULL;
	char sync_command[PGR_MESSAGE_BUFSIZE];
	char * str = NULL;
	
	snprintf(sync_command,sizeof(sync_command),
		"SELECT %s(%d,%u,%u,%u,%d) ",
		PGR_SYSTEM_COMMAND_FUNC,
		PGR_QUERY_CONFIRM_ANSWER_FUNC_NO,
		(unsigned int)ntohl(header->tv.tv_sec),
		(unsigned int)ntohl(header->tv.tv_usec),
		(unsigned int)ntohl(header->replicate_id),
		PGR_Response_Inf->response_mode);
	
	res = PQexec(conn, sync_command);
	if (res != NULL)
	{
		str = PQcmdStatus(res);
		if ((str != NULL) &&
			(!strncasecmp(str,PGR_ALREADY_REPLICATED_NOTICE_CMD,strlen(PGR_ALREADY_REPLICATED_NOTICE_CMD))))
		{
			PQclear(res);
			return true;
		}
		PQclear(res);

	}
	return false;
}

static int
replicate_lo( PGconn * conn, ReplicateHeader * header, LOArgs * query)
{
	int status = STATUS_OK;
	int mode = 0;
	Oid lobjId = 0;
	int fd = 0;
	char * buf = NULL;
	char * filename = NULL;
	size_t len = 0;
	int offset = 0;
	int whence = 0;

	if ((conn == (PGconn *)NULL) || (query == (LOArgs *)NULL) || (header == (ReplicateHeader *)NULL))
	{
		return STATUS_ERROR;
	}
	switch (header->cmdType)
	{
		case CMD_TYPE_LO_IMPORT :
			filename = query->buf;
			if (lo_import(conn, filename) > 0 )
			{
				status = STATUS_OK;
			}
			else
			{
				status = STATUS_ERROR;
			}
			break;
		case CMD_TYPE_LO_CREATE :
			mode = (int)ntohl(query->arg1);
			if (lo_creat(conn, mode) > 0)
			{
				status = STATUS_OK;
			}
			else
			{
				status = STATUS_ERROR;
			}
			break;
		case CMD_TYPE_LO_OPEN :
			lobjId = (Oid)ntohl(query->arg1);
			mode = (int)ntohl(query->arg2);
			if (lo_open(conn, lobjId, mode) > 0)
			{
				status = STATUS_OK;
			}
			else
			{
				status = STATUS_ERROR;
			}
			break;
		case CMD_TYPE_LO_WRITE :
			fd = (int)ntohl(query->arg1);
			len = (int)ntohl(query->arg2);
			buf = query->buf;
			if (lo_write(conn, fd, buf, len) == len )
			{
				status = STATUS_OK;
			}
			else
			{
				status = STATUS_ERROR;
			}
			break;
		case CMD_TYPE_LO_LSEEK :
			fd = (int)ntohl(query->arg1);
			offset = (int)ntohl(query->arg2);
			whence = (int)ntohl(query->arg3);
			if (lo_lseek(conn, fd, offset, whence) >= 0)
			{
				status = STATUS_OK;
			}
			else
			{
				status = STATUS_ERROR;
			}
			break;
		case CMD_TYPE_LO_CLOSE :
			fd = (int)ntohl(query->arg1);
			if (lo_close(conn, fd) == 0)
			{
				status = STATUS_OK;
			}
			else
			{
				status = STATUS_ERROR;
			}
			break;
		case CMD_TYPE_LO_UNLINK :
			lobjId = (Oid)ntohl(query->arg1);
			if (lo_unlink(conn,lobjId) >= 0)
			{
				status = STATUS_OK;
			}
			else
			{
				status = STATUS_ERROR;
			}
			break;
		default :
			break;
	}
	return status;
}

static int 
send_func(HostTbl * host_ptr,ReplicateHeader * header, char * func,char * result)
{
	char * f ="send_func()";
	char	   *database = NULL;
	char	   port[8];
	char	   *userName = NULL;
	char	   *password = NULL;
	char * md5Salt = NULL;
	char * cryptSalt = NULL;
	char * host = NULL;
	char * str = NULL;
	TransactionTbl * transaction_tbl = (TransactionTbl *)NULL;
	PGresult * res = (PGresult *)NULL;
	PGconn * conn = (PGconn *)NULL;
	int rtn = 0;
	int current_cluster = 0;

	if ((host_ptr == (HostTbl *)NULL)		||
		(header == (ReplicateHeader *)NULL)	||
		(func == NULL)						||
		(result == NULL))
	{
		return STATUS_ERROR;
	}
	/*
	 * set up the connection
	 */
	database = (char *)header->dbName;
	snprintf(port,sizeof(port),"%d", host_ptr->port);
	userName = (char *)(header->userName);
	password = (char *)(header->password);
	md5Salt = (char *)(header->md5Salt);
	cryptSalt = (char *)(header->cryptSalt);
	host = (char *)(host_ptr->hostName);
	if (PGR_Response_Inf != NULL)
	{
		current_cluster = PGR_Response_Inf->current_cluster;
	}

	/*
	 * get the transaction table data
	 * it has the connection data with each cluster DB
	 */
	transaction_tbl = getTransactionTbl(host_ptr,header);
	/*
	 * if the transaction process is new one, 
	 * create connection data and add the transaction table
	 */
	if (transaction_tbl == (TransactionTbl *)NULL)
	{
		transaction_tbl = setTransactionTbl(host_ptr, header);
		if (transaction_tbl == (TransactionTbl *)NULL)
		{
			StartReplication[current_cluster] = true;
			show_error("%s:setTransactionTbl failed",f);
			if ( header->cmdSts != CMD_STS_NOTICE )
			{
				PGRset_host_status(host_ptr,DB_TBL_ERROR);
			}
			return STATUS_ERROR;
		}
	}
	else
	{
		/*
		 * re-use the connection data
		 */
		if ((transaction_tbl->conn != (PGconn *)NULL) &&
			(transaction_tbl->conn->sock > 0))
		{
			StartReplication[current_cluster] = false;
		}
		else
		{
			if (transaction_tbl->conn != (PGconn *)NULL)
			{
				PQfinish(transaction_tbl->conn);
			}
		 	transaction_tbl->conn = PGRcreateConn(host,port,database,userName,password,md5Salt,cryptSalt, PGR_LOGIN_TIMEOUT);
			StartReplication[current_cluster] = true;
		}
	}
	conn = transaction_tbl->conn;

	if (conn == NULL)
	{
		show_error("%s:[%d@%s] may be down",f,host_ptr->port,host_ptr->hostName);
		if ( header->cmdSts != CMD_STS_NOTICE )
		{
			StartReplication[current_cluster] = true;
			PGRset_host_status(host_ptr,DB_TBL_ERROR);
		}
		return STATUS_ERROR;
	}
	res = PQexec(conn, func);
	if (res == NULL)
	{
		StartReplication[current_cluster] = true;
		return STATUS_ERROR;
	}
	str = PQcmdStatus(res);
	if ((str == NULL) || (*str == '\0'))
	{
		rtn = STATUS_ERROR;
	}
	else
	{
		snprintf(result, PGR_MESSAGE_BUFSIZE, "%s",str);
		rtn = STATUS_OK;
	}
	if (res != NULL)
		PQclear(res);
	return rtn;	
}

static uint32_t
get_oid(HostTbl * host_ptr,ReplicateHeader * header)
{
	char sync_command[PGR_MESSAGE_BUFSIZE];
	char result[PGR_MESSAGE_BUFSIZE];

	memset(result,0,sizeof(result));
	snprintf(sync_command,sizeof(sync_command),
		"SELECT %s(%d)",
		PGR_SYSTEM_COMMAND_FUNC, PGR_GET_OID_FUNC_NO);
	if (send_func(host_ptr, header, sync_command, result) == STATUS_OK)
	{
		return (strtoul(result, NULL, 10));
	}
	return 0;
}

static int
set_oid(HostTbl * host_ptr,ReplicateHeader * header, uint32_t oid)
{
	char sync_command[PGR_MESSAGE_BUFSIZE];
	char result[PGR_MESSAGE_BUFSIZE];

	memset(result,0,sizeof(result));
	snprintf(sync_command,sizeof(sync_command),
		"SELECT %s(%d,%u)",
		PGR_SYSTEM_COMMAND_FUNC, 
		PGR_SET_OID_FUNC_NO,
		oid);
	return ( send_func(host_ptr, header, sync_command, result) );
}

/*
 * sync oid during cluster DB's 
 */
int
PGRsync_oid(ReplicateHeader *header)
{
	HostTbl * host_ptr = (HostTbl*)NULL;
	uint32_t max_oid = 0;
	uint32_t oid = 0;
	int recovery_status = 0;

	/* get current oid of all cluster db's */
	host_ptr = Host_Tbl_Begin;
	if (host_ptr == (HostTbl *)NULL)
	{
		return STATUS_ERROR;
	}
	recovery_status = PGRget_recovery_status();
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
		/*
		 * skip loop during recover and the host name is master DB
		 */
		if (is_master_in_recovery(host_ptr->hostName, host_ptr->port,recovery_status) == true)
		{
			host_ptr ++;
			continue;
		}
		oid = get_oid(host_ptr,header);
		if (max_oid < oid )
		{
			max_oid = oid;
		}
		host_ptr ++;
	}
	if (max_oid <= 0)
		return STATUS_ERROR;
	
	/* set oid in cluster db */
	host_ptr = Host_Tbl_Begin;
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
		/*
		 * skip loop during recover and the host name is master DB
		 */
		if (is_master_in_recovery(host_ptr->hostName, host_ptr->port,recovery_status) == true)
		{
			host_ptr ++;
			continue;
		}
		set_oid(host_ptr,header,max_oid);
		host_ptr ++;
	}

	return STATUS_OK;
}

int
PGRload_replication_id(void)
{
	char * func = "PGRload_replication_id()";
	char buf[256];
	char *p;

	if (Recovery_Status_Inf == (RecoveryStatusInf *)NULL)
	{
		show_error("%s: Recovery_Status_Inf is NULL",func);
		return STATUS_ERROR;
	}
	if (RidFp == (FILE *)NULL)
	{
		show_error("%s: replication id file is not open",func);
		return STATUS_ERROR;
	}
	rewind(RidFp);
	if (fgets(buf,sizeof(buf),RidFp) == NULL)
	{
		Recovery_Status_Inf->replication_id = 0;
	}
	else
	{
		p = strrchr(buf,' ');
		if (p != NULL)
		{
			p++;
			Recovery_Status_Inf->replication_id = (uint32_t) atol(p);
		}
		else
		{
			Recovery_Status_Inf->replication_id = 0;
		}
	}
	return STATUS_OK;
}

static int
notice_abort(HostTbl * host_ptr,ReplicateHeader * header)
{
	char sync_command[PGR_MESSAGE_BUFSIZE];
	char result[PGR_MESSAGE_BUFSIZE];

	memset(result,0,sizeof(result));
	snprintf(sync_command,sizeof(sync_command),
		"SELECT %s(%d)",
		PGR_SYSTEM_COMMAND_FUNC, 
		PGR_NOTICE_ABORT_FUNC_NO);
	return ( send_func(host_ptr, header, sync_command, result) );
}

static int
send_p_parse (PGconn * conn, StringInfo input_message)
{
	const char *stmt_name;
	const char *query_string;
	int			numParams;
	Oid			paramTypes;

	/* get name,query */
	stmt_name = pq_getmsgstring(input_message);
	query_string = pq_getmsgstring(input_message);
	/* send name,query */
	if (pqPutMsgStart('P', false, conn) < 0 ||
		pqPuts(stmt_name, conn) < 0 ||
		pqPuts(query_string, conn) < 0)
	{
		return STATUS_ERROR;
	}
	/* get number of parameter */
	numParams = pq_getmsgint(input_message, 2);
	/* send number of parameter */
	if (pqPutInt(numParams, 2, conn) < 0)
	{
		return STATUS_ERROR;
	}
	if (numParams > 0)
	{
		int			i;
		for (i = 0; i < numParams; i++)
		{
			paramTypes = pq_getmsgint(input_message, 4);
			if (pqPutInt(paramTypes, 4, conn) < 0)
			{
				return STATUS_ERROR;
			}
		}
	}
	if (pqPutMsgEnd(conn) < 0)
	{
		return STATUS_ERROR;
	}
	return STATUS_OK;
}

static int
send_p_bind (PGconn * conn, StringInfo input_message)
{
	const char *portal_name;
	const char *stmt_name;
	int			numPFormats;
	int16		pformats;
	int			numParams;
	int			numRFormats;
	int16		rformats;
	int			i;

	/* Get&Send the fixed part of the message */
	portal_name = pq_getmsgstring(input_message);
	stmt_name = pq_getmsgstring(input_message);
	if (pqPutMsgStart('B', false, conn) < 0 ||
		pqPuts(portal_name, conn) < 0 ||
		pqPuts(stmt_name, conn) < 0)
	{
		return STATUS_ERROR;
	}

	/* Get&Send the parameter format codes */
	numPFormats = pq_getmsgint(input_message, 2);
	if (pqPutInt(numPFormats, 2, conn) < 0)
	{
		return STATUS_ERROR;
	}
	if (numPFormats > 0)
	{
		for (i = 0; i < numPFormats; i++)
		{
			pformats = pq_getmsgint(input_message, 2);
			if (pqPutInt(pformats, 2, conn) < 0)
			{
				return STATUS_ERROR;
			}
		}
	}

	/* Get&Send the parameter value count */
	numParams = pq_getmsgint(input_message, 2);
	if (pqPutInt(numParams, 2, conn) < 0)
	{
		return STATUS_ERROR;
	}
	if (numParams > 0)
	{
		int32       plength;
		for (i = 0 ; i < numParams ; i ++)
		{
			plength = pq_getmsgint(input_message, 4);
			if (plength != -1)
			{
				const char *pvalue = pq_getmsgbytes(input_message, plength);
				if (pqPutInt(plength, 4, conn) < 0 ||
					pqPutnchar(pvalue, plength, conn) < 0)
				{
					return STATUS_ERROR;
				}
			}
			else
			{
				if (pqPutInt(plength, 4, conn) < 0)
				{
					return STATUS_ERROR;
				}
			}
		}
	}

	/* Get&Send the result format codes */
	numRFormats = pq_getmsgint(input_message, 2);
	if (pqPutInt(numRFormats, 2, conn) < 0 )
	{
		return STATUS_ERROR;
	}
	if (numRFormats > 0)
	{
		for (i = 0; i < numRFormats; i++)
		{
			rformats = pq_getmsgint(input_message, 2);
			if (pqPutInt(rformats, 2, conn) < 0)
			{
				return STATUS_ERROR;
			}
		}
	}
	if (pqPutMsgEnd(conn) < 0)
	{
		return STATUS_ERROR;
	}
	return STATUS_OK;
}

static int
send_p_describe (PGconn * conn, StringInfo input_message)
{

	int			describe_type;
	const char *describe_target;

	describe_type = pq_getmsgbyte(input_message);
	describe_target = pq_getmsgstring(input_message);

	/* construct the Describe Portal message */
	if (pqPutMsgStart('D', false, conn) < 0 ||
		pqPutc(describe_type, conn) < 0 ||
		pqPuts(describe_target, conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
	{
		return STATUS_ERROR;
	}
	return STATUS_OK;
}

static int
send_p_execute (PGconn * conn, StringInfo input_message)
{
	const char *portal_name;
	int			max_rows;

	portal_name = pq_getmsgstring(input_message);
	max_rows = pq_getmsgint(input_message, 4);
	/* construct the Execute message */
	if (pqPutMsgStart('E', false, conn) < 0 ||
		pqPuts(portal_name, conn) < 0 ||
		pqPutInt(max_rows, 4, conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
	{
		return STATUS_ERROR;
	}
	return STATUS_OK;
}

static int
send_p_sync (PGconn * conn, StringInfo input_message)
{
	PGresult   *result;
	PGresult   *lastResult;

	/* construct the Sync message */
	if (pqPutMsgStart('S', false, conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
	{
		return STATUS_ERROR;
	}
	/* remember we are using extended query protocol */
	conn->queryclass = PGQUERY_EXTENDED;

	/*
	 * Give the data a push.  In nonblock mode, don't complain if we're unable
	 * to send it all; PQgetResult() will do any additional flushing needed.
	 */
	if (pqFlush(conn) < 0)
	{
		return STATUS_ERROR;
	}

	/* OK, it's launched! */
	conn->asyncStatus = PGASYNC_BUSY;

	lastResult = NULL;
	while ((result = PQgetResult(conn)) != NULL)
	{
		if (lastResult)
		{
			if (lastResult->resultStatus == PGRES_FATAL_ERROR &&
				result->resultStatus == PGRES_FATAL_ERROR)
			{
				PQclear(result);
				result = lastResult;
			}
			else
				PQclear(lastResult);
		}
		lastResult = result;
		if (result->resultStatus == PGRES_COPY_IN ||
			result->resultStatus == PGRES_COPY_OUT ||
			conn->status == CONNECTION_BAD)
			break;
	}
	if (lastResult != NULL)
	{
		PQclear(lastResult);
	}
	return STATUS_OK;
}

static int
send_p_close (PGconn * conn, StringInfo input_message)
{

	int			close_type;
	const char *close_target;

	close_type = pq_getmsgbyte(input_message);
	close_target = pq_getmsgstring(input_message);
	if (pqPutMsgStart('C', false, conn) < 0 ||
		pqPutc(close_type, conn) < 0 ||
		pqPuts(close_target, conn) < 0 ||
		pqPutMsgEnd(conn) < 0)
	{
		return STATUS_ERROR;
	}
	return STATUS_OK;
}
static void
set_string_info(StringInfo input_message, ReplicateHeader * header, char * query)
{
	int len;
	len = ntohl(header->query_size);
	input_message->data = query;
	input_message->maxlen = len;
	input_message->len = len -1;
	input_message->cursor = 0;
}
