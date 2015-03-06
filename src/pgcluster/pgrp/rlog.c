/*--------------------------------------------------------------------
 * FILE:
 *     rlog.c
 *
 * NOTE:
 *     This file is composed of the functions to call with the source
 *     at pgreplicate for replicate ahead log.
 *
 * Portions Copyright (c) 2003-2008, Atsushi Mitani
 *--------------------------------------------------------------------
 */
#ifdef USE_REPLICATION

#include "postgres.h"
#include "postgres_fe.h"

#include <stdio.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <sys/msg.h>
#include <signal.h>
#include <sys/socket.h>
#include <netdb.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <dirent.h>
#include <arpa/inet.h>

#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

#include "libpq-fe.h"
#include "libpq-int.h"
#include "fe-auth.h"
#include "access/xact.h"
#include "replicate_com.h"
#include "pgreplicate.h"

static int RLog_Recv_Sock = -1;
/*--------------------------------------
 * PROTOTYPE DECLARATION
 *--------------------------------------
 */
static int set_query_log(ReplicateHeader * header, char * query);
static QueryLogType * get_query_log_by_header(ReplicateHeader * header);
static QueryLogType * get_query_log(ReplicateHeader * header);
static void delete_query_log(ReplicateHeader * header);
static int set_commit_log(ReplicateHeader * header);
static CommitLogInf * get_commit_log(ReplicateHeader * header);
static void delete_commit_log(ReplicateHeader * header);
static bool was_committed_transaction(ReplicateHeader * header);
static int create_recv_rlog_socket(void);
static int do_rlog(int fd);
static int recv_message(int sock,char * buf, int len);
static int send_message(int sock, char * msg, int len);
static void exit_rlog(int sig);
static int reconfirm_commit(ReplicateHeader * header);


int
PGRwrite_rlog(ReplicateHeader * header, char * query)
{
	char * func = "PGRwrite_rlog()";

 	if (header == NULL)
	{
		show_error("%s:header is null",func);
		return STATUS_ERROR;
	}
	switch (header->cmdSts)
	{
		case CMD_STS_QUERY:
#ifdef PRINT_DEBUG
			show_debug("%s:set_query_log",func);
#endif			
			set_query_log(header,query);
			break;
		case CMD_STS_DELETE_QUERY:
#ifdef PRINT_DEBUG
			show_debug("%s:delete_query_log",func);
#endif			
			delete_query_log(header);
			break;
		case CMD_STS_TRANSACTION:
			if (header->cmdType == CMD_TYPE_COMMIT)
			{
#ifdef PRINT_DEBUG
				show_debug("%s:set_commit_log call",func);
#endif			
				set_commit_log(header);
			}
			break;
		case CMD_STS_DELETE_TRANSACTION:
			if (header->cmdType == CMD_TYPE_COMMIT)
			{
#ifdef PRINT_DEBUG
				show_debug("%s:delete_commit_log call",func);
#endif			
				delete_commit_log(header);
			}
			break;
	default:
	  show_error("%s:unknown status %c",func,header->cmdSts);
	  break;
	}
	return STATUS_OK;
}

ReplicateHeader *
PGRget_requested_query(ReplicateHeader * header)
{
	QueryLogType * query_log = NULL;

	if (Query_Log_Top == NULL)
	{
		return NULL;
	}
	query_log = Query_Log_Top;
	while(query_log != (QueryLogType *)NULL)
	{
		if ((query_log->header->request_id == header->request_id) &&
			(query_log->header->pid == header->pid) &&
			(query_log->header->port == header->port) &&
			(!strncmp(query_log->header->from_host,header->from_host,sizeof(header->from_host))))
		{
			return query_log->header;
		}
		query_log = (QueryLogType *)(query_log->next);
	}
	return (ReplicateHeader *)NULL;
}

static int
set_query_log(ReplicateHeader * header, char * query)
{
	char * func = "set_query_log()";
	int size = 0;
	QueryLogType * query_log = NULL;

	if (Query_Log_Top == NULL)
	{
		Query_Log_Top = (QueryLogType *)malloc(sizeof(QueryLogType));
		if (Query_Log_Top == (QueryLogType *)NULL)
		{
			show_error("%s:malloc failed: (%s)",func,strerror(errno));
			return STATUS_ERROR;
		}
		Query_Log_Top->next = NULL;
		Query_Log_Top->last = NULL;
		Query_Log_End = Query_Log_Top;
		Query_Log_End->next = NULL;
		Query_Log_End->last = NULL;
		query_log = Query_Log_Top;	
	}
	else
	{
		query_log = (QueryLogType *)malloc(sizeof(QueryLogType));
		if (query_log == (QueryLogType *)NULL)
		{
			show_error("%s:malloc failed: (%s)",func,strerror(errno));
			return STATUS_ERROR;
		}
		Query_Log_End->next = (char *)query_log;
		query_log->last = (char *)Query_Log_End;
		query_log->next = NULL;
		Query_Log_End = query_log;	
	}
	query_log->header = (ReplicateHeader *)malloc(sizeof(ReplicateHeader));
	if (query_log->header == (ReplicateHeader *)NULL)
	{
		show_error("%s:malloc failed: (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
	size = ntohl(header->query_size);

	query_log->query = (char *)malloc(size+4);
	if (query_log->query == (char *)NULL)
	{
		show_error("%s:malloc failed: (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
	memset(query_log->query,0,size+4);
	memcpy(query_log->header,header,sizeof(ReplicateHeader));
	query_log->header->rlog = FROM_R_LOG_TYPE ;
	memcpy(query_log->query,query,size);

	return STATUS_OK;
}

static QueryLogType *
get_query_log_by_header(ReplicateHeader * header)
{
	QueryLogType * query_log = NULL;

	if (Query_Log_Top == NULL)
	{
		return (QueryLogType *)NULL;
	}
	query_log = Query_Log_Top;
	    show_debug("get_query_log_by_header:header is %d,%d,%d,%s",
		       header->request_id,
		       header->pid,
		       header->port,
		       header->from_host);

	while(query_log != (QueryLogType *)NULL)
	  {
	    show_debug("get_query_log_by_header:comparing to %d,%d,%d,%s",
		       query_log->header->request_id,
		        query_log->header->pid,
		        query_log->header->port,
		        query_log->header->from_host);

		if ((query_log->header->request_id == header->request_id) &&
			(query_log->header->pid == header->pid) &&
			(query_log->header->port == header->port) &&
			(!strncmp(query_log->header->from_host,header->from_host,sizeof(header->from_host))))
		{
			return query_log;
		}
		query_log = (QueryLogType *)(query_log->next);
	}
	return (QueryLogType *)NULL;
}

static QueryLogType *
get_query_log(ReplicateHeader * header)
{
	QueryLogType * query_log = NULL;

	if (Query_Log_Top == NULL)
	{
		return NULL;
	}
	query_log = Query_Log_Top;
	while(query_log != (QueryLogType *)NULL)
	{
	  show_debug("get_qurey_log: comparing in log is %d,header is %d",query_log->header->replicate_id,header->replicate_id);
		if (query_log->header->replicate_id == header->replicate_id)
		{
			return query_log;
		}
		query_log = (QueryLogType *)(query_log->next);
	}
	return (QueryLogType*)NULL;
}

static void
delete_query_log(ReplicateHeader * header)
{
	QueryLogType * query_log = NULL;
	QueryLogType * last = NULL;
	QueryLogType * next = NULL;

	query_log = get_query_log(header);

	if (query_log == NULL)
	{
		return ;
	}
	last = (QueryLogType *)query_log->last;
	next = (QueryLogType *)query_log->next;

	/* change link */
	if (last != (QueryLogType *)NULL)
	{
		last->next = (char *)next;
	}
	else
	{
		Query_Log_Top = next;
	}
	if (next != (QueryLogType *)NULL)
	{
		next->last = (char *)last;
	}
	else
	{
		Query_Log_End = last;
	}

	/* delete contents */
	if (query_log->header != NULL)
	{
		free(query_log->header);
	}
	if (query_log->query != NULL)
	{
		free(query_log->query);
	}
	free(query_log);
}

static int
set_commit_log(ReplicateHeader * header)
{

	CommitLogInf * commit_log = NULL;
	ReplicateHeader * c_header;

	if (Commit_Log_Tbl == NULL)
	{
		return STATUS_ERROR;
	}
	commit_log = Commit_Log_Tbl + 1;
	while (	commit_log->inf.useFlag != DB_TBL_END )
	{
		if (commit_log->inf.useFlag != DB_TBL_USE)
		{
			commit_log->inf.useFlag = DB_TBL_USE;
			c_header = &(commit_log->header);
			memcpy(c_header,header,sizeof(ReplicateHeader));
			Commit_Log_Tbl->inf.commit_log_num ++;
			break;
		}
		commit_log ++;
	}
	return STATUS_OK;
}

static CommitLogInf *
get_commit_log(ReplicateHeader * header)
{
	CommitLogInf * commit_log = NULL;
	ReplicateHeader * c_header;
	int cnt = 0;

	if (Commit_Log_Tbl == NULL)
	{
		return (CommitLogInf *)NULL;
	}
	commit_log = Commit_Log_Tbl + 1;
	while (	commit_log->inf.useFlag != DB_TBL_END )
	{
		if (commit_log->inf.useFlag == DB_TBL_USE)
		{
			cnt ++;
			c_header = &(commit_log->header);
			if (c_header == NULL)
			{
				commit_log ++;
				continue;
			}
			if (c_header->replicate_id == header->replicate_id)
			{
				return commit_log;	
			}
		}
		else
		{
		}
		if (cnt >= Commit_Log_Tbl->inf.commit_log_num)
		{
			break;
		}
		commit_log ++;
	}
	return (CommitLogInf *)NULL;
}

static void
delete_commit_log(ReplicateHeader * header)
{
	CommitLogInf * commit_log = NULL;

	commit_log = get_commit_log(header);
	if (commit_log != NULL)
	{
		memset(&(commit_log->header),0,sizeof(commit_log->header));
		commit_log->inf.useFlag = DB_TBL_INIT;
		Commit_Log_Tbl->inf.commit_log_num --;
	}
}

static bool
was_committed_transaction(ReplicateHeader * header)
{
	CommitLogInf * commit_log = NULL;

	commit_log = get_commit_log(header);
	if (commit_log != NULL)
	{
		return true;
	}
	return false;
}

void 
PGRreconfirm_commit(int sock, ReplicateHeader * header)
{
	int result = PGR_NOT_YET_COMMIT;

	if (Replicateion_Log == NULL) 
	{
		return ;
	}
	
	if (Replicateion_Log->r_log_sock > 0)
	{
		close(Replicateion_Log->r_log_sock );
		Replicateion_Log->r_log_sock = -1;
	}
	Replicateion_Log->r_log_sock = PGRcreate_send_rlog_socket();
	if (Replicateion_Log->r_log_sock == -1)
		return;
	
	header->query_size = 0;
	PGRsend_rlog_packet(Replicateion_Log->r_log_sock,header,"");
	PGRrecv_rlog_result(Replicateion_Log->r_log_sock,&result, sizeof(result));

	
	close(Replicateion_Log->r_log_sock );
	Replicateion_Log->r_log_sock = -1;
	
	snprintf(PGR_Result,PGR_MESSAGE_BUFSIZE,"%d,%d", PGR_TRANSACTION_CONFIRM_ANSWER_FUNC_NO,result);

	PGRreturn_result(sock, PGR_Result,PGR_NOWAIT_ANSWER);
}

static int 
reconfirm_commit(ReplicateHeader * header)
{
	char * func = "reconfirm_commit()";
	int result = PGR_NOT_YET_COMMIT;

	/* check the transaction was committed */
	if (was_committed_transaction(header) == true)
	{
		result = PGR_ALREADY_COMMITTED;
	}
	return result;
}

void
PGRset_rlog(ReplicateHeader * header, char * query)
{
		char * func = "PGRset_rlog()";
		int status = STATUS_OK;
		bool send_flag = false;

		if (PGR_Log_Header == NULL)
		{
				return;
		}
		switch (header->cmdSts)
		{
				case CMD_STS_QUERY:
						send_flag = true;
						break;
				case CMD_STS_TRANSACTION:
						if (header->cmdType == CMD_TYPE_COMMIT)
						{
								send_flag = true;
								PGR_Log_Header->cmdType = header->cmdType;
								PGR_Log_Header->query_size = htonl(strlen(query));
						}
						break;
		}
		if (send_flag != true)
		{
				show_error("%s:send_flag is false",func);
				return;
		}
		PGR_Log_Header->cmdSys = CMD_SYS_LOG;
		if (Cascade_Inf->useFlag == DB_TBL_USE)
		{
				/* save log data in remote server */
				show_debug("%s:set rlog %s",func,query);
				status = PGRsend_lower_cascade(PGR_Log_Header, query);
		        if (status == STATUS_OK) {
						status=PGRwait_notice_rlog_done();
				}
				if (status != STATUS_OK)
				{
#ifdef PRINT_DEBUG
						show_debug("%s:PGRsend_lower_cascade failed",func);
#endif			
						PGRwrite_rlog(PGR_Log_Header, query);
				}
		}
		else
		{
				/* save log data in local server */
				PGRwrite_rlog(PGR_Log_Header, query);
		}
}

void
PGRunset_rlog(ReplicateHeader * header, char * query)
{
        int status = STATUS_OK;
	bool send_flag = false;

 	if (PGR_Log_Header == NULL)
	{
		return;
	}
	switch (header->cmdSts)
	{
		case CMD_STS_QUERY:
			send_flag = true;
			PGR_Log_Header->cmdSts = CMD_STS_DELETE_QUERY;
			break;
		case CMD_STS_TRANSACTION:
			if (PGR_Log_Header->cmdType == CMD_TYPE_COMMIT)
		{
				PGR_Log_Header->cmdSts = CMD_STS_DELETE_TRANSACTION;
				PGR_Log_Header->query_size = htonl(strlen(query));
				send_flag = true;
			}
			break;
	}
	if (send_flag != true)
	{
		return;
	}
	PGR_Log_Header->cmdSys = CMD_SYS_LOG;
	if (Cascade_Inf->useFlag == DB_TBL_USE)
	{
		/* save log data in remote server */
	  show_debug("unset rlog %s",query);

		status = PGRsend_lower_cascade(PGR_Log_Header, query);	
		if (status == STATUS_OK)
		{
				status=PGRwait_notice_rlog_done();
		}
		if (status != STATUS_OK)
		{
#ifdef PRINT_DEBUG
			show_debug("PGRsend_lower_cascade recv failed");
#endif			
			PGRwrite_rlog(PGR_Log_Header, query);
		}
	}
	else
	{
		/* save log data in local server */
		 PGRwrite_rlog(PGR_Log_Header, query);
	}
}

int
PGRresend_rlog_to_db(void)
{
  char *func="PGRresend_rlog_to_db";
	QueryLogType * query_log = NULL;
	QueryLogType * next = NULL;
	int status = STATUS_OK;
	int dest = 0;

	  show_debug("%s:enter.",func);

	query_log = Query_Log_Top;
	
	memset(query_log->header->sitehostname,0,sizeof(query_log->header->sitehostname));

	while (query_log != NULL)
	{


	  show_debug("%s:processing qlog,query=%s",func,query_log->query);
		if (query_log->header->rlog != FROM_R_LOG_TYPE )
		{
			query_log = (QueryLogType *)query_log->next;
			continue;
		}
	        status = replicate_packet_send_internal(query_log->header,query_log->query, dest,RECOVERY_INIT,false);
		show_debug("%s:status=%d",func,status);
		
		if (status == STATUS_SKIP_REPLICATE )
		{
			Query_Log_Top = query_log;
			query_log = (QueryLogType *)query_log->next;
       		}
		else		 
		{
		        if (query_log->header != NULL)
			  {
			    free(query_log->header );
			  }
			if (query_log->query != NULL)
			  {
			    free(query_log->query );
			  }
			next = (QueryLogType *)query_log->next;
			free(query_log);
			query_log = next;
			Query_Log_Top = query_log;
		}
		if (query_log != NULL)
		{
			Query_Log_End = (QueryLogType *)query_log->next;
		}
		else
		{
			Query_Log_End = (QueryLogType *)NULL;
		}
	}

	  show_debug("%s:exit.",func);

	return STATUS_OK;
}
	
pid_t
PGR_RLog_Main(void)
{
	char * func = "PGR_RLog_Main()";
	int afd = -1;
	int rtn;
	struct sockaddr addr;
	socklen_t addrlen;
	pid_t pid = 0;
	pid_t pgid = 0;

	extern int fork_wait_time;

	if (Replicateion_Log == NULL)
	{
		show_error("%s:Replicateion_Log is NULL",func);
		return -1;
	}
	pgid = getpgid(0);
	if ((pid = fork()) != 0 )
	{
		return pid;
	}
	PGRsignal(SIGTERM,exit_rlog);
	PGRsignal(SIGINT,exit_rlog);
	PGRsignal(SIGQUIT,exit_rlog);
	PGRsignal(SIGPIPE,SIG_IGN);

	if (PGRinit_transaction_table() != STATUS_OK)
	{
		show_error("RLog process transaction table memory allocate failed");
		return -1;
	}

	setpgid(0,pgid);	
	RLog_Recv_Sock = create_recv_rlog_socket();
	if(RLog_Recv_Sock == -1) 
	{
		show_error("rlog socket creation failure.quit all process.");
		kill(pgreplicate_pid, SIGINT);
		exit_rlog(0);
	}

	if (fork_wait_time > 0) {
#ifdef PRINT_DEBUG
		show_debug("rlog process: wait fork(): pid = %d", getpid());
#endif			
		sleep(fork_wait_time);
	}

	for (;;)
	{
		fd_set	  rmask;
		struct timeval timeout;

		timeout.tv_sec = PGR_Replication_Timeout;
		timeout.tv_usec = 0;

		Idle_Flag = IDLE_MODE ;
		if (Exit_Request)
		{
			exit_rlog(0);
		}
		/*
		 * Wait for something to happen.
		 */
		FD_ZERO(&rmask);
		FD_SET(RLog_Recv_Sock,&rmask);
		rtn = select(RLog_Recv_Sock+1, &rmask, (fd_set *)NULL, (fd_set *)NULL, &timeout);
		if (rtn < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;
		}
		else if (rtn && FD_ISSET(RLog_Recv_Sock, &rmask))
		{
			Idle_Flag = BUSY_MODE ;
			addrlen = sizeof(addr);
			afd = accept(RLog_Recv_Sock, &addr, &addrlen);
			if (afd < 0)
			{
				continue;
			}
			else
			{
				do_rlog(afd);
		       	close(afd);
				afd = -1;
			}
		}
	}
	exit(0);
}

static int 
create_recv_rlog_socket(void)
{
	char * func = "create_recv_socket()";
	struct sockaddr_un addr;
	int fd;
	int status;
	int len;

	/* set unix domain socket path */
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1)
	{
		show_error("%s:Failed to create UNIX domain socket. reason: %s",func,  strerror(errno));
		return -1;
	}
	memset((char *) &addr, 0, sizeof(addr));
	((struct sockaddr *)&addr)->sa_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/.s.PGRLOG.%d",
			PGR_Write_Path, 
			Replicateion_Log->RLog_Port_Number);
	if (Replicateion_Log->RLog_Sock_Path == NULL)
	{
		Replicateion_Log->RLog_Sock_Path = strdup(addr.sun_path);
	}
	len = sizeof(struct sockaddr_un);
	status = bind(fd, (struct sockaddr *)&addr, len);
	if (status == -1)
	{
		show_error("%s: bind() failed. reason: %s", func, strerror(errno));
		return -1;
	}

	if (chmod(addr.sun_path, 0770) == -1)
	{
		show_error("%s: chmod() failed. reason: %s", func, strerror(errno));
		return -1;
	}

	status = listen(fd, 1000000);
	if (status < 0)
	{
		show_error("%s: listen() failed. reason: %s", func, strerror(errno));
		return -1;
	}
	return fd;
}

int 
PGRcreate_send_rlog_socket(void)
{
	char * func = "create_recv_socket()";
	struct sockaddr_un addr;
	int fd;
	int len;

	/* set unix domain socket path */
	fd = socket(AF_UNIX, SOCK_STREAM, 0);
	if (fd == -1)
	{
		show_error("%s:Failed to create UNIX domain socket. reason: %s",func,  strerror(errno));
		return -1;
	}
	memset((char *) &addr, 0, sizeof(addr));
	((struct sockaddr *)&addr)->sa_family = AF_UNIX;
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/.s.PGRLOG.%d",
			PGR_Write_Path, 
			Replicateion_Log->RLog_Port_Number);
	if (Replicateion_Log->RLog_Sock_Path == NULL)
	{
		Replicateion_Log->RLog_Sock_Path = strdup(addr.sun_path);
	}
	len = sizeof(struct sockaddr_un);
	if (connect(fd, (struct sockaddr *)&addr, len) < 0)
	{
		close(fd);
		return -1;
	}
	return fd;
}

static int
do_rlog(int fd)
{
	char * func = "do_rlog()";
	QueryLogType * query_log = NULL;
	ReplicateHeader  header;
	char * query = NULL;
	int status = STATUS_OK;

	memset(&header,0,sizeof(header));
	query = PGRread_packet(fd, &header);
	if (header.cmdSys == 0)
	{
		return STATUS_ERROR;
	}
	switch (header.cmdSys)
	{
		case CMD_SYS_REPLICATE:
			if (header.cmdSts != CMD_STS_DELETE_QUERY)
			{
				query_log = get_query_log_by_header(&header);
				if (query_log != (QueryLogType*)NULL)
				{
					memcpy(&header,query_log->header,sizeof(ReplicateHeader));
				}
				send_message(fd,(char *)&header,sizeof(ReplicateHeader));
				header.cmdSts = CMD_STS_DELETE_QUERY;
				PGRwrite_rlog(&header, NULL);
			}
			else
			{
				status = PGRwrite_rlog((ReplicateHeader*)&header,(char *)NULL);
				send_message(fd,(char *)&status,sizeof(status));
			}
			break;
		case CMD_SYS_LOG:
			status = PGRwrite_rlog((ReplicateHeader*)&header, query);
			send_message(fd,(char *)&status,sizeof(status));
			break;
		case  CMD_SYS_CALL:
			if (header.cmdSts == CMD_STS_TRANSACTION_ABORT)
			{
				status = reconfirm_commit(&header);
			}
			else if (header.cmdSts == CMD_STS_QUERY_SUSPEND)
			{
				//			status = PGRresend_rlog_to_db();
			}
			send_message(fd,(char *)&status,sizeof(status));
			break;
	}
	return STATUS_OK;
}

int
PGRsend_rlog_packet(int sock,ReplicateHeader * header, const char * query_string)
{
	char * buf = NULL;
	int buf_size = 0;
	int header_size = 0;
	int query_size = 0;
	int rtn = 0;

	/* check parameter */
	if ((sock <= 0) || (header == NULL))
	{
		return STATUS_ERROR;
	}
	if (query_string != NULL)
	{
		query_size = ntohl(header->query_size);
	}
	header_size = sizeof(ReplicateHeader);
	buf_size = header_size + query_size + 4;
	buf = (char *)malloc(buf_size);
	if (buf == (char *)NULL)
	{
		return STATUS_ERROR;
	}
	memset(buf,0,buf_size);
	buf_size -= 4;
	memcpy(buf,header,header_size);
	if (query_size > 0)
	{
		memcpy((char *)(buf+header_size),query_string,query_size+1);
	}
	rtn = send_message(sock,buf,buf_size);
	free(buf);
	return rtn;
}

int
PGRrecv_rlog_result(int sock,void * result, int size)
{
	char *func = "PGRrecv_rlog_result";
	fd_set      rmask;
	struct timeval timeout;
	int rtn;

	if ((result == (void *)NULL) || (size <= 0))
	{
		return -1;
	}

	/*
	 * Wait for something to happen.
	 */
	rtn = 1;
	for (;;)
	{
		timeout.tv_sec = PGR_Replication_Timeout;
		timeout.tv_usec = 0;

		FD_ZERO(&rmask);
		FD_SET(sock,&rmask);
		rtn = select(sock+1, &rmask, (fd_set *)NULL, (fd_set *)NULL, &timeout);
		if (rtn < 0)
		{
			if (errno != EINTR || errno != EAGAIN)
			{
				show_error("%s: select() failed (%s)",func,strerror(errno));
				return -1;
			}
		}
		else if (rtn && FD_ISSET(sock, &rmask))
		{
			return (recv_message(sock, (char*)result, size));
		}
	}
	return -1;
}


static int
recv_message(int sock,char * buf, int len)
{
	char *func = "recv_message";
	int cnt = 0;
	int r = 0;
	char * read_ptr;
	int read_size = 0;
	cnt = 0;
	read_ptr = buf;

	for (;;)
	{
		r = recv(sock,read_ptr + read_size ,len - read_size, 0); 
		if (r < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;
			else
			{
				show_error("%s:recv failed: %d(%s)",func, errno, strerror(errno));
				return -1;
			}
		}
		else if (r > 0)
		{
			read_size += r;
			if (read_size == len)
				return read_size;
		}
		else /* r == 0 */
		{
			show_error("%s:unexpected EOF", func);
			return -1;
		}
	}
	return -1;
}

int
PGRsend_rlog_to_local(ReplicateHeader * header,char * query)
{
	int status = STATUS_OK;

	if (Replicateion_Log == NULL) 
	{
		return STATUS_ERROR;
	}
	
	if (Replicateion_Log->r_log_sock > 0)
	{
		close(Replicateion_Log->r_log_sock );
		Replicateion_Log->r_log_sock = -1;
	}
	
	Replicateion_Log->r_log_sock = PGRcreate_send_rlog_socket();
	if (Replicateion_Log->r_log_sock == -1)
		return STATUS_ERROR;
	
	show_debug("send_to_local %s",query);
	status = PGRsend_rlog_packet(Replicateion_Log->r_log_sock,header,query);
	show_debug("send_to_local result is %d,errno=%d(%s)",status,errno ,strerror(errno));
        
	if (status != STATUS_ERROR)
	{
		PGRrecv_rlog_result(Replicateion_Log->r_log_sock,&status, sizeof(status));
	}
	
	close(Replicateion_Log->r_log_sock );
	Replicateion_Log->r_log_sock = -1;
	
	return status;
}

int
PGRget_rlog_header(ReplicateHeader * header)
{
	int status = STATUS_OK;
	ReplicateHeader rlog_header;

	if ((Replicateion_Log == NULL) || 
		(header == NULL))
	{
		return STATUS_ERROR;
	}
	
	if (Replicateion_Log->r_log_sock > 0)
	{
		close(Replicateion_Log->r_log_sock );
		Replicateion_Log->r_log_sock = -1;
	}
	Replicateion_Log->r_log_sock = PGRcreate_send_rlog_socket();
	if (Replicateion_Log->r_log_sock == -1)
		return STATUS_ERROR;
	
	memcpy(&rlog_header,header,sizeof(ReplicateHeader));
	rlog_header.cmdSys = CMD_SYS_REPLICATE;
	rlog_header.query_size = 0;
	status =PGRsend_rlog_packet(Replicateion_Log->r_log_sock,&rlog_header,"");
	if (status != STATUS_ERROR)
	{
		status = PGRrecv_rlog_result(Replicateion_Log->r_log_sock,&rlog_header, sizeof(ReplicateHeader));
		if (status > 0)
		{
			memcpy(header,&rlog_header,sizeof(ReplicateHeader));
			status = STATUS_OK;
		}
		else
		{
			status = STATUS_ERROR;
		}
	}
	
	close(Replicateion_Log->r_log_sock );
	Replicateion_Log->r_log_sock = -1;
		
	return status;
}

static int
send_message(int sock, char * msg, int len)
{
	char * func = "send_message()";
	fd_set	  wmask;
	struct timeval timeout;
	int rtn = 0;
	char * send_ptr = NULL;
	int send_size= 0;
	int buf_size = 0;
	int s = 0;
	int flag = 0;
	
	if ((msg == NULL) || (len <= 0) || (sock <= 0))
	{
		return STATUS_ERROR;
	}
	send_ptr = msg;
	buf_size = len;

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
		FD_SET(sock,&wmask);
		rtn = select(sock+1, (fd_set *)NULL, &wmask, (fd_set *)NULL, &timeout);
	  
		if (rtn < 0 )
		{
			if (errno == EAGAIN || errno == EINTR)
				continue;

			show_error("%s:send-select error: %d(%s)",func,errno,strerror(errno));
			return STATUS_ERROR;
		}
		else if (rtn & FD_ISSET(sock, &wmask))
		{
			s = send(sock,send_ptr + send_size,buf_size - send_size ,flag); 
			if (s < 0)
			{
				if (errno == EINTR || errno == EAGAIN)
					continue;
				else
				{
					show_error("%s:send error: %d(%s)",func,errno,strerror(errno));
					memset(send_ptr, 0, len);
					return STATUS_ERROR;
				}
			}
			else if (s == 0)
			{
				show_error("%s:unexpected EOF");
				memset(send_ptr, 0, len);
				return STATUS_ERROR;
			}
			else /* s > 0 */
			{
				send_size += s;
				if (send_size == buf_size)
				{
					return STATUS_OK;
				}
			}
		}
	}
	show_error("%s:send-select unknown error: %d(%s)",
			   func,errno,strerror(errno));
	return STATUS_ERROR;
}

static void
exit_rlog(int sig)
{
	sigset_t mask;

	Exit_Request = true;
	if (sig == SIGTERM)
	{
		if (Idle_Flag == BUSY_MODE)
		{
			return;
		}
	}
	
	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);
	sigaddset(&mask, SIGQUIT);
	sigprocmask(SIG_BLOCK, &mask, NULL);

	if (RLog_Recv_Sock >= 0)
	{
		close(RLog_Recv_Sock);
		RLog_Recv_Sock = -1;
	}
	if (Replicateion_Log->RLog_Sock_Path != NULL)
	{
		unlink(Replicateion_Log->RLog_Sock_Path);
		free(Replicateion_Log->RLog_Sock_Path);
	}
	exit(0);
}
#endif /* USE_REPLICATION */
