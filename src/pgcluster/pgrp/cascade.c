/*--------------------------------------------------------------------
 * FILE:
 *     cascade.c
 *
 * NOTE:
 *     This file is composed of the functions to call with the source
 *     at pgreplicate for backup and cascade .
 *
 * Portions Copyright (c) 2003-2008, Atsushi Mitani
 *--------------------------------------------------------------------
 */
#ifdef USE_REPLICATION

#include "postgres.h"
#include "postgres_fe.h"

#include <stdio.h>
#include <unistd.h>
#ifdef HAVE_SYS_TYPES_H
#include <sys/types.h>
#endif
#ifdef HAVE_FCNTL_H
#include <fcntl.h>
#endif
#include <errno.h>
#include <ctype.h>
#include <time.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/sem.h>
#include <signal.h>
#include <sys/socket.h>
#ifdef HAVE_UNISTD_H
#include <unistd.h>
#endif
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

#if 0
static int count_cascade(int flag);
static void PGRinit_cascade_child(void); 
#endif

static int fixup_socket_for_cascades(int *sock ,ReplicateServerInfo * target);
static ReplicateServerInfo * get_cascade_data(int * cnt, int flag);
static int add_cascade_data(ReplicateHeader * header, ReplicateServerInfo * add_data);
static int update_cascade_data(ReplicateHeader * header, ReplicateServerInfo * update_data);
static void write_cascade_status_file(ReplicateServerInfo * cascade);
static int notice_cascade_data(int sock);
static int notice_cascade_data_to_cluster_db(void);

/**
 * socket variables, moved from  Cascade_Inf->(lower|upper)->sock.
 * Cascade->Inf is in shared memory, so sometimes cascades returns EBADF due to not initialized socket in specified process.
 * 05/10/05 tanida@sraoss.co.jp
 */

static int lsock=-1; /* socket for lower-cascade. */
static int usock=-1; /* socket for upper-cascade. */

/*--------------------------------------
 * PROTOTYPE DECLARATION
 *--------------------------------------
 */

#if 0
static int
count_cascade(int flag)
{
	int cnt = 0;
	int cascade_cnt = 0;
	ReplicateServerInfo * cascade = NULL;

	if ((Cascade_Tbl == NULL) || (Cascade_Inf == NULL))
	{
		return 0;
	}

	/* count cascadeing replication server */
	switch (flag)
	{
		case UPPER_CASCADE:
		case ALL_CASCADE:
			cascade = Cascade_Tbl;
			break;
		case LOWER_CASCADE:
			cascade = Cascade_Inf->myself;
			break;
	}

	if (cascade == NULL)
	{
		return 0;
	}
	while (cascade->useFlag != DB_TBL_END)
	{
		if (cascade->useFlag == DB_TBL_USE)
		{
			cascade_cnt ++;
		}
		if ((flag == UPPER_CASCADE) &&
			(cascade == Cascade_Inf->myself))
		{
			break;
		}
		cnt ++;
		if (cnt >= MAX_DB_SERVER -1 )
		{
			break;
		}
		cascade ++;
	}
	return cascade_cnt;
}

static void 
PGRinit_cascade_child(void) {
      fixup_socket_for_cascades(&usock,NULL);
      fixup_socket_for_cascades(&lsock,NULL);
}
#endif /* if 0 */

static ReplicateServerInfo * 
get_cascade_data(int * cnt, int flag)
{
	char * func = "get_cascade_data()";
	int i = 0;
	int loop_cnt = 0;
	int size = 0;
	ReplicateServerInfo * buf = NULL;
	ReplicateServerInfo * cascade = NULL;

	size = sizeof(ReplicateServerInfo) * MAX_DB_SERVER;
	buf = (ReplicateServerInfo *)malloc(size);
	if (buf == (ReplicateServerInfo *)NULL)
	{
		show_error("%s:malloc failed: (%s)",func,strerror(errno));
		*cnt = 0;
		return NULL;
	}
	memset(buf,0,size);

	switch (flag)
	{
		case UPPER_CASCADE:
		case ALL_CASCADE:
			cascade = Cascade_Tbl;
			break;
		case LOWER_CASCADE:
			cascade = Cascade_Inf->myself;
			break;
			default:
		free(buf);
		*cnt = 0;
		return NULL;
					
	}

	if (cascade == NULL)
	{
		free(buf);
		*cnt = 0;
		return NULL;
	}
	PGRsem_lock(CascadeSemID,1);
	i = 0;
	loop_cnt = 0;
	while (cascade->useFlag != DB_TBL_END)
	{
		if (cascade->useFlag == DB_TBL_USE) 
		{
			(buf + i)->useFlag = htonl(cascade->useFlag);
			strncpy((buf + i)->hostName,cascade->hostName,sizeof(cascade->hostName));
			(buf + i)->portNumber = htons(cascade->portNumber);
			(buf + i)->recoveryPortNumber = htons(cascade->recoveryPortNumber);
			(buf + i)->lifecheckPortNumber = htons(cascade->lifecheckPortNumber);
			i++;
		}
		if ((flag == UPPER_CASCADE) &&
			(cascade == Cascade_Inf->myself))
		{
			break;
		}
		loop_cnt ++;
		if (loop_cnt >= MAX_DB_SERVER -1 )
		{
			break;
		}
		if (Cascade_Inf->end == cascade)
		{
			break;
		}
		cascade ++;
	}
	*cnt = i;
	PGRsem_unlock(CascadeSemID,1);

	return buf;
}

static int
update_cascade_data(ReplicateHeader * header, ReplicateServerInfo * update_data)
{
	char * func = "update_cascade_data()";
	int size = 0;
	int cnt = 0;
	ReplicateServerInfo * ptr = NULL;
	ReplicateServerInfo * cascade = NULL;
	char hostName[HOSTNAME_MAX_LENGTH];


	show_debug("executing %s",func);
	if ((header == NULL ) || ( update_data == NULL))
	{
		show_error("%s:receive data is wrong",func);
		return STATUS_ERROR;
	}
	if ((Cascade_Tbl == NULL) || (Cascade_Inf == NULL))
	{
		show_error("%s:config data read error",func);
		return STATUS_ERROR;
	}


	size = ntohl(header->query_size);
	cnt = size / sizeof(ReplicateServerInfo);
	if (cnt >= MAX_DB_SERVER)
	{
		show_error("%s:update cascade data is too large. it's more than %d", func,MAX_DB_SERVER);
		return STATUS_ERROR;
	}

	Cascade_Inf->useFlag = DB_TBL_INIT;
	fixup_socket_for_cascades(&usock,NULL);
	fixup_socket_for_cascades(&lsock,NULL);

	Cascade_Inf->upper = NULL;
	Cascade_Inf->lower = NULL;

	gethostname(hostName,sizeof(hostName));
	ptr = update_data;
	cascade = Cascade_Tbl;
	memset(cascade,0,(sizeof(ReplicateServerInfo)*MAX_DB_SERVER));
	Cascade_Inf->top = cascade;
	while (cnt > 0)
	{

		cascade->useFlag = ntohl(ptr->useFlag);
		strncpy(cascade->hostName,ptr->hostName,sizeof(cascade->hostName));
		cascade->portNumber = ntohs(ptr->portNumber);
		cascade->recoveryPortNumber = ntohs(ptr->recoveryPortNumber);
		cascade->lifecheckPortNumber = ntohs(ptr->lifecheckPortNumber);

		if ((!strncmp(cascade->hostName,hostName,sizeof(cascade->hostName)))  &&
			(cascade->portNumber == Port_Number) &&
			(cascade->recoveryPortNumber == Recovery_Port_Number))
		{
			Cascade_Inf->myself = cascade;
		}

		Cascade_Inf->end = cascade;
		cascade ++;
		ptr ++;
		cnt --;
		cascade->useFlag = DB_TBL_END;
	}
	Cascade_Inf->useFlag = DB_TBL_USE;

	return STATUS_OK;
}

static int
add_cascade_data(ReplicateHeader * header, ReplicateServerInfo * add_data)
{
	char *func = "add_cascade_data()";
	int size = 0;
	int cnt = 0;
	ReplicateServerInfo * ptr = NULL;
	ReplicateServerInfo * cascade = NULL;
	char hostName[HOSTNAME_MAX_LENGTH];

	if ((header == NULL ) || ( add_data == NULL))
	{
		show_error("%s:receive data is wrong",func);
		return STATUS_ERROR;
	}
	if ((Cascade_Tbl == NULL) || (Cascade_Inf == NULL))
	{
		show_error("%s:config data read error",func);
		return STATUS_ERROR;
	}
	size = ntohl(header->query_size);
	cnt = size / sizeof(ReplicateServerInfo);
	if (cnt >= MAX_DB_SERVER)
	{
		show_error("%s:addtional cascade data is too large. it's more than %d", func,MAX_DB_SERVER);
		return STATUS_ERROR;
	}

	Cascade_Inf->useFlag = DB_TBL_INIT;
	fixup_socket_for_cascades(&lsock,NULL);
	Cascade_Inf->lower = NULL;

	gethostname(hostName,sizeof(hostName));
	ptr = add_data;
	cascade = Cascade_Inf->myself;
	cascade ++;
	while (cnt > 0)
	{
		cascade->useFlag = ntohl(ptr->useFlag);
		strncpy(cascade->hostName,ptr->hostName,sizeof(cascade->hostName));
		cascade->portNumber = ntohs(ptr->portNumber);
		cascade->recoveryPortNumber = ntohs(ptr->recoveryPortNumber);
		cascade->lifecheckPortNumber = ntohs(ptr->lifecheckPortNumber);
                cascade->replicate_id=-1;
	        cascade->response_mode=-1;

		Cascade_Inf->end = cascade;

		if ((!strncmp(cascade->hostName,hostName,sizeof(cascade->hostName)))  &&
			(cascade->portNumber == Port_Number) &&
			(cascade->recoveryPortNumber == Recovery_Port_Number))
		{
			ptr ++;
			cnt --;
			continue;
		}
		cascade ++;
		cascade->useFlag = DB_TBL_END;
		ptr ++;
		cnt --;
	}
	Cascade_Inf->useFlag = DB_TBL_USE;
	return STATUS_OK;
}

int
PGRstartup_cascade(void)
{
	char * func = "PGRstartup_cascade()";
	int cnt = 0;
	int status = STATUS_OK;
	ReplicateHeader header;
	ReplicateServerInfo * cascade = NULL;
	ReplicateServerInfo * buf = NULL;

	if ((Cascade_Tbl == NULL) || (Cascade_Inf == NULL))
	{
		show_error("%s:config data read error",func);
		return STATUS_ERROR;
	}

	/* count lower server */
	cascade = Cascade_Inf->myself;
	if (cascade == NULL)
	{
		show_error("%s:cascade data initialize error",func);
		return STATUS_ERROR;
	}
	buf = get_cascade_data(&cnt,LOWER_CASCADE);
	if (cnt <= 0)
	{
		show_error("%s:cascade data get error",func);
		return STATUS_ERROR;
	}

	memset(&header,0,sizeof(ReplicateHeader));
	header.cmdSys = CMD_SYS_CASCADE;
	header.cmdSts = CMD_STS_TO_UPPER;
	header.cmdType = CMD_TYPE_ADD;
	header.query_size = htonl(sizeof(ReplicateServerInfo) * cnt);

	status = PGRsend_upper_cascade(&header, (char *)buf);
	if (buf != NULL)
	{
		free(buf);
	}
	if (status == STATUS_OK)
	{
		memset(&header,0,sizeof(ReplicateHeader));
		buf = PGRrecv_cascade_answer( Cascade_Inf->upper, &header);
		if (buf == NULL)
		{
				status=STATUS_ERROR;
		}
		else if((header.cmdSys == CMD_SYS_CASCADE) &&
		    (header.cmdSts == CMD_STS_TO_LOWER) &&
		    (header.cmdType == CMD_TYPE_UPDATE_ALL))
		{
				status = update_cascade_data(&header,buf);
				free(buf);
		}
		
	}
	show_debug("%s:startup packet result is %d",func,status);
	return status;
}

int
PGRsend_lower_cascade(ReplicateHeader * header, char * query)
{


		char * func = "PGRsend_lower_cascade()";
		ReplicateServerInfo *lower = PGRget_lower_cascade();


		while(lower!=NULL) 
		{
				/**
				 * check lower_cascade validaty.
				 *
				 */	       		
				if(lsock!=-1 &&			   
				   PGRsend_cascade(lsock,header,query)==STATUS_OK) 
				{
						return STATUS_OK;
				}
				else
				{
						/**
						 * current lower cascade is missing.
						 * fix socket , or go to next one. 
						 *
						 */
						while(  lower!=NULL &&
								fixup_socket_for_cascades(&lsock,lower)!=STATUS_OK)
						{
								show_error("%s:lower cascade maybe down,challenge new one.",func);
								PGRset_cascade_server_status(lower,DB_TBL_ERROR);
								lower =PGRget_lower_cascade();
						}
				}
				Cascade_Inf->lower=lower;
        }


		return STATUS_ERROR;
}


int
PGRsend_upper_cascade(ReplicateHeader * header, char * query)
{
	char * func = "PGRsend_upper_cascade()";
	ReplicateServerInfo *upper = PGRget_upper_cascade();


	while(upper!=NULL)
	{				
			/**
			 * check upper_cascade validaty.
			 *
			 */	       		
			if(usock!=-1 && 
			   PGRsend_cascade(usock,header,query)==STATUS_OK) 
			{
					return STATUS_OK;
			}
			else
			{
					/**
					 * current upper cascade is missing.
					 * fix socket , or go to next one. 
					 *
					 */
					while(  upper!=NULL &&
							fixup_socket_for_cascades(&usock,upper)!=STATUS_OK) 
					{
							show_error("%s:upper cascade maybe down,challenge new one.",func);
							PGRset_cascade_server_status(upper,DB_TBL_ERROR);
							upper =PGRget_upper_cascade();
					}
			}
			Cascade_Inf->upper=upper;
	}

	return STATUS_ERROR;
}

ReplicateServerInfo *
PGRget_lower_cascade(void)
{
	char * func = "PGRget_lower_cascade()";
	ReplicateServerInfo * cascade = NULL;

	if ((Cascade_Tbl == NULL) || (Cascade_Inf == NULL))
	{
		show_error("%s:config data read error",func);
		return NULL;
	}

	/* count lower server */

	cascade = Cascade_Inf->myself;
	if (cascade == NULL)
	{
		show_error("%s:cascade data initialize error",func);
		return NULL;
	}
	if (cascade->useFlag != DB_TBL_END)
	{
		cascade ++;
	}
	while (cascade->useFlag != DB_TBL_END)
	{
/*
#ifdef PRINT_DEBUG
		show_debug("%s:lower cascade search[%d]@[%s] use[%d]",
			func,
			cascade->portNumber,
			cascade->hostName,
			cascade->useFlag);
#endif			
*/
		if (cascade->useFlag == DB_TBL_USE)
		{
/*
#ifdef PRINT_DEBUG
			show_debug("%s:find lower cascade",func);
#endif			
*/
			return cascade;
		}
		cascade ++;
	}
	return NULL;
}

ReplicateServerInfo *
PGRget_upper_cascade(void)
{
	char * func = "PGRget_upper_cascade()";
	ReplicateServerInfo * cascade = NULL;

	if ((Cascade_Tbl == NULL) || (Cascade_Inf == NULL))
	{
		show_error("%s:config data read error",func);
		return NULL;
	}


	/* count lower server */
	cascade = Cascade_Inf->myself;
	if ((cascade == NULL) || (Cascade_Inf->top == cascade))
	{
		return NULL;
	}
	cascade --;
	while (cascade != NULL)
	{
		if (cascade->useFlag == DB_TBL_USE)
		{
			return cascade;
		}
		if (Cascade_Inf->top == cascade)
		{
			break;
		}
		cascade --;
	}
	return NULL;
}

static void
write_cascade_status_file(ReplicateServerInfo * cascade)
{
	switch( cascade->useFlag)
	{
		case DB_TBL_FREE:
			PGRwrite_log_file(StatusFp,"cascade(%s) port(%d) free",
					cascade->hostName,
					cascade->portNumber);
			break;
		case DB_TBL_INIT:
			PGRwrite_log_file(StatusFp,"cascade(%s) port(%d) initialize",
					cascade->hostName,
					cascade->portNumber);
			break;
		case DB_TBL_USE:
			PGRwrite_log_file(StatusFp,"cascade(%s) port(%d) start use",
					cascade->hostName,
					cascade->portNumber);
			break;
		case DB_TBL_ERROR:
			PGRwrite_log_file(StatusFp,"cascade(%s) port(%d) error",
					cascade->hostName,
					cascade->portNumber);
			break;
		case DB_TBL_TOP:
			PGRwrite_log_file(StatusFp,"cascade(%s) port(%d) become top",
					cascade->hostName,
					cascade->portNumber);
			break;
	}
}

void
PGRset_cascade_server_status(ReplicateServerInfo * cascade, int status)
{
	if (cascade == NULL)
	{
		return;
	}
	if (cascade->useFlag != status)
	{
		cascade->useFlag = status;
		write_cascade_status_file(cascade);
	}
}

ReplicateServerInfo *
PGRrecv_cascade_answer(ReplicateServerInfo * cascade,ReplicateHeader * header)
{
	ReplicateServerInfo * answer = NULL;
        int sock;

	if ((cascade == NULL) || (header == NULL))
	{
		return NULL;
	}

	/* FIXME: ReplicateServerInfo->sock must be removed in cascading. */
	if(cascade == Cascade_Inf->upper ) 
	{
	  sock=usock;
	}
	else if (cascade == Cascade_Inf->lower )
	{
	  sock=lsock;
	}
	else 
	{
	  show_debug("PGRrecv_cascade_answer:receiving packet from sock not belogs to cascade->upper / lower. maybe missing .");
	  sock=cascade->sock;
	}
	answer = (ReplicateServerInfo*)PGRread_packet(sock,header);
	return answer;
}

int
PGRsend_cascade(int sock , ReplicateHeader * header, char * query)
{
	char * func ="PGRsend_cascade()";
	int s;
	char * send_ptr;
	char * buf;
	int send_size = 0;
	int buf_size;
	int header_size;
	int rtn;
	fd_set      wmask;
	struct timeval timeout;
	int query_size = 0;

	/* check parameter */
	if ((header == NULL) || (sock == -1))
	{
		return STATUS_ERROR;
	}

	query_size = ntohl(header->query_size);
	header_size = sizeof(ReplicateHeader);
	buf_size = header_size + query_size + 4;
	buf = malloc(buf_size);
	memset(buf,0,buf_size);
	buf_size -= 4;
	memcpy(buf,header,header_size);
	if (query_size > 0)
	{
		memcpy((char *)(buf+header_size),query,query_size+1);
	}
	send_ptr = buf;

	for (;;)
	{
		timeout.tv_sec = 10;
		timeout.tv_usec = 0;

		/*
		 * Wait for something to happen.
		 */
		FD_ZERO(&wmask);
		FD_SET(sock,&wmask);
		rtn = select(sock+1, (fd_set *)NULL, &wmask, (fd_set *)NULL, &timeout);

		if (rtn < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;

			show_error("%s:select failed ,errno is %s",func , strerror(errno));
			free(buf);
			return STATUS_ERROR;
		}

		if (rtn && FD_ISSET(sock, &wmask))
		{
			s = send(sock,send_ptr + send_size,buf_size - send_size ,0);
			if (s < 0)
			{
				if (errno == EINTR || errno == EAGAIN)
					continue;
				else
				{
					show_error("%s:send failed: %d(%s)",func, errno, strerror(errno));
					free(buf);
					return STATUS_ERROR;
				}
			}
			else if (s == 0)
			{
				show_error("%s:unexpected EOF", func);
				free(buf);
				return STATUS_ERROR;
			}
			send_size += s;
			if (send_size == buf_size)
			{
/*
#ifdef PRINT_DEBUG
				show_debug("%s:send[%s] size[%d]",func,query,send_size);
#endif			
*/
				free(buf);
				return STATUS_OK;
			}
		}
	}
	return STATUS_OK;
}

int
PGRwait_answer_cascade(int  sock)
{
	ReplicateHeader header;
	char * answer = NULL;

	answer = PGRread_packet(sock,&header);
	if (answer != NULL)
	{
		free(answer);
		return STATUS_OK;
	}
	return STATUS_ERROR;
}
/**
 * fixup_socket_for_cascades checks socket's validaty.
 * returns STATUS_OK if succeeded , or STATUS_ERROR if some error occured.
 * if target is null , only close socket.
 *
 * originally written by tanida@sraoss.co.jp
 */
static int
fixup_socket_for_cascades(int *sock, ReplicateServerInfo *target) 
{
	if (*sock > 0)
	{
		close(*sock);	
		*sock=-1;
	}
        if(target!=NULL) {
	       return PGR_Create_Socket_Connect(sock,target->hostName,target->portNumber);
        }
	return STATUS_OK;
}


static int
notice_cascade_data(int sock)
{
	char * func = "notice_cascade_data";
	ReplicateServerInfo *cascade_data = NULL;
	ReplicateHeader header;
	int cnt = 0;
	int size = 0;

	if (sock <= 0)
	{
		return STATUS_ERROR;
	}

	cascade_data = get_cascade_data(&cnt, ALL_CASCADE );
	if (cnt <= 0)
	{
		show_error("%s:cascade data is wrong",func);
		return STATUS_ERROR;
	}
	size = sizeof (ReplicateServerInfo) * cnt ;

	memset(&header,0,sizeof(ReplicateHeader));
	header.cmdSys = CMD_SYS_CASCADE ;
	header.cmdSts = CMD_STS_TO_LOWER ;
	header.cmdType = CMD_TYPE_UPDATE_ALL;
	header.query_size = htonl(size);
	PGRsend_cascade(sock, &header, (char *)cascade_data );
	if (cascade_data != NULL)
	{
		free(cascade_data);
	}
	return STATUS_OK;
}

int
PGRcascade_main(int sock, ReplicateHeader * header, char * query)
{
	switch (header->cmdSts)
	{
		case CMD_STS_TO_UPPER:
			if (header->cmdType == CMD_TYPE_ADD)
			{
				/* add lower cascade data to myself */
				add_cascade_data(header,(ReplicateServerInfo*)query);
				/* send cascade data to upper */
				/* and receive new cascade data from upper */
				PGRstartup_cascade();
				/* return to lower with new cascade data */
				notice_cascade_data(sock);
				/* notifies a cascade server's information to Cluster DBs */
				notice_cascade_data_to_cluster_db();
			}
			break;
		case CMD_STS_TO_LOWER:
			/*
			 * use for cascading replication 
			 */
			break;
	}
	return STATUS_OK;
}

static int
notice_cascade_data_to_cluster_db(void)
{
	char userName[USERNAME_MAX_LENGTH];
	ReplicateServerInfo *s=NULL;

	if (Cascade_Inf->lower == NULL)
	{
		Cascade_Inf->lower = PGRget_lower_cascade();	
	}
	if (Cascade_Inf->lower == NULL)
	{
		return STATUS_ERROR;
	}
	s=Cascade_Inf->lower;
	memset(userName,0,sizeof(userName));
	strncpy(userName ,getenv("LOGNAME"),sizeof(userName)-1);

	PGRnotice_replication_server(s->hostName,
								 s->portNumber,
								 s->recoveryPortNumber,
								 s->lifecheckPortNumber,
								 userName);

	return STATUS_OK;
}

int
PGRwait_notice_rlog_done(void)
{
        ReplicateHeader header;
		if (lsock != -1)
		{
				PGRread_packet(lsock,&header);
				return STATUS_OK;
		}
		return STATUS_ERROR;

}


int
PGRsend_notice_quit(void )
{
		ReplicateHeader header;
		int size = 0;

		size = strlen("QUIT_SAFELY");
		memset(&header,0,sizeof(ReplicateHeader));
		header.cmdSys = CMD_SYS_CALL ;
		header.cmdSts = CMD_STS_RESPONSE ;
		header.cmdType = CMD_TYPE_FRONTEND_CLOSED;
		header.query_size = htonl(size);
		PGRsend_lower_cascade(&header, "QUIT_SAFELY");
		PGRwait_notice_rlog_done();
		return STATUS_OK;
}

int
PGRsend_notice_rlog_done(int sock)
{
	ReplicateHeader header;
	int size = 0;

	if (sock <= 0)
	{
		return STATUS_ERROR;
	}

	size = strlen(PGR_QUERY_DONE_NOTICE_CMD);
	memset(&header,0,sizeof(ReplicateHeader));
	header.cmdSys = CMD_SYS_CASCADE ;
	header.cmdSts = CMD_STS_RESPONSE ;
	header.cmdType = 0;
	header.query_size = htonl(size);
	PGRsend_cascade(sock, &header, PGR_QUERY_DONE_NOTICE_CMD);
	return STATUS_OK;

}
#endif /* USE_REPLICATION */
