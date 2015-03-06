/*--------------------------------------------------------------------
 * FILE:
 *     recovery.c
 *
 * NOTE:
 *     This file is composed of the functions to call with the source
 *     at pgreplicate for the recovery.
 *
 * Portions Copyright (c) 2003-2008, Atsushi Mitani
 *--------------------------------------------------------------------
 */
#include "postgres.h"

#include <stdio.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/msg.h>
#include <netdb.h>
#include <netinet/in.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <arpa/inet.h>
#include <sys/param.h>
#include <sys/file.h>

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_CRYPT_H
#include <crypt.h>
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
#else
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#endif

#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif
#include "pgreplicate.h"


/*--------------------------------------
 * GLOBAL VARIABLE DECLARATION
 *--------------------------------------
 */
RecoveryPacket MasterPacketData;
RecoveryTbl Master;
RecoveryTbl Target;


/*--------------------------------------
 * PROTOTYPE DECLARATION
 *--------------------------------------
 */
static int read_packet(int sock,RecoveryPacket * packet);
static int read_packet_from_master( RecoveryTbl * host, RecoveryPacket * packet );
static int send_recovery_packet(int  sock, RecoveryPacket * packet);
static int send_packet(RecoveryTbl * host, RecoveryPacket * packet );
static void start_recovery_prepare(void);
static void reset_recovery_prepare(void);
static void finish_recovery(void);
static bool first_setup_recovery(int * sock, RecoveryPacket * packet);
static int wait_transaction_count_clear(void);
static bool second_setup_recovery (RecoveryPacket * packet);
static void pgrecovery_loop(int fd);
static int PGRsend_queue(RecoveryTbl * master, RecoveryTbl * target);
static int send_vacuum(HostTbl *host, char * userName, int stage);
static char * read_queue_file(FILE * fp, ReplicateHeader * header, char * query);

#ifdef PRINT_DEBUG
static void show_recovery_packet(RecoveryPacket * packet);
#endif			

int PGRsend_load_balance_packet(RecoveryPacket * packet);
void PGRrecovery_main(int fork_wait_time);

/*-----------------------------------------------------------
 * SYMBOL
 *    read_packet()
 * NOTES
 *    Read recovery packet data 
 * ARGS
 *    int sock : socket
 *    RecoveryPacket * packet : read packet buffer
 * RETURN
 *    -1 : error
 *    >0 : read size
 *-----------------------------------------------------------
 */
static int
read_packet(int sock,RecoveryPacket * packet)
{
#ifdef PRINT_DEBUG
	char * func = "read_packet()";
#endif			
	int r = 0;
	char * read_ptr = NULL;
	int read_size = 0;
	int packet_size = 0;

	if (packet == NULL)
	{
		return -1;
	}
	read_ptr = (char*)packet;
	packet_size = sizeof(RecoveryPacket);
	for (;;)
	{
		r = recv(sock,read_ptr + read_size ,packet_size - read_size, MSG_WAITALL);
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
			if (read_size == packet_size)
			{
#ifdef PRINT_DEBUG
				show_debug("%s:receive packet",func);
				show_recovery_packet(packet);
#endif			
				return read_size;
			}
		}
		else /* r == 0 */
		{
			show_error("%s:unexpected EOF", func);
			return -1;
		}
	}
	return -1;
}

static int
read_packet_from_master( RecoveryTbl * host, RecoveryPacket * packet )
{
	int read_size = 0;
	int rtn;
	fd_set	  rmask;
	struct timeval timeout;

	for(;;)
	{
		timeout.tv_sec = RECOVERY_TIMEOUT;
		timeout.tv_usec = 0;

		/*
		 * Wait for something to happen.
		 */
		FD_ZERO(&rmask);
		FD_SET(host->recovery_sock,&rmask);
		rtn = select(host->recovery_sock+1, &rmask, (fd_set *)NULL, (fd_set *)NULL, &timeout);
		
		if (rtn == 0) /* timeout */
		{
			return -1;
		}

		if (rtn && FD_ISSET(host->recovery_sock, &rmask))
		{
			read_size = read_packet(host->recovery_sock, packet);
			return read_size;
		}
	}
}

static int
send_recovery_packet(int  sock, RecoveryPacket * packet)
{
	char *func = "send_recovery_packet";
	char * send_ptr;
	int send_size= 0;
	int buf_size = 0;
	int s;
	
	send_ptr = (char *)packet;
	buf_size = sizeof(RecoveryPacket);

	for (;;)
	{
		s = send(sock, send_ptr + send_size,buf_size - send_size ,0);
		if (s < 0)
		{
			if (errno == EINTR || errno == EAGAIN)
				continue;

			show_error("%s:send error: %d(%s)", func, errno, strerror(errno));
			return STATUS_ERROR;
		}
		else if (s == 0)
		{
			show_error("%s:unexpected EOF", func);
			return STATUS_ERROR;
		}

		send_size += s;
		if (send_size == buf_size)
			return STATUS_OK;
	}
}

static int
send_packet(RecoveryTbl * host, RecoveryPacket * packet )
{
	char * func = "send_packet()";
	int count = 0;

	if (host->recovery_sock == -1)
	{
		while(PGR_Create_Socket_Connect(&(host->recovery_sock), host->hostName , host->recoveryPort) != STATUS_OK )
		{
			if (count > MAX_RETRY_TIMES )
			{
				show_error("%s:host[%s] port[%d]PGR_Create_Socket_Connect failed",func,host->hostName, host->recoveryPort);
				return STATUS_ERROR;
			}
			count ++;
		}
	}
	count = 0;
	while (send_recovery_packet(host->recovery_sock,packet) != STATUS_OK)
	{
		close(host->recovery_sock);
		host->recovery_sock = -1;
		PGR_Create_Socket_Connect(&(host->recovery_sock), host->hostName , host->recoveryPort);
#ifdef PRINT_DEBUG
		show_debug("%s:PGR_Create_Socket_Connectsock[%d] host[%s] port[%d]",
			func,host->recovery_sock,host->hostName,host->recoveryPort);
#endif
		if (count > PGR_CONNECT_RETRY_TIME )
		{

			show_error("%s:send failed and PGR_Create_Socket_Connect failed",func);
			return STATUS_ERROR;
		}
		count ++;
	}
	return STATUS_OK;
}

static void
start_recovery_prepare(void)
{
	PGRset_recovery_status (RECOVERY_PREPARE_START);	
}

static void
reset_recovery_prepare(void)
{
	PGRset_recovery_status (RECOVERY_INIT);
}

static void
finish_recovery(void)
{
	PGRset_recovery_status (RECOVERY_INIT);
}

int
PGRsend_load_balance_packet(RecoveryPacket * packet)
{
	char * func = "PGRsend_load_balance_packet()";
	RecoveryTbl * lbp;
	int status;

	lbp = LoadBalanceTbl;
	if (lbp == (RecoveryTbl *)NULL)
	{
		show_error("%s:recovery table is NULL",func);
		return STATUS_ERROR;
	}
	while (lbp->hostName[0] != 0)
	{
		if (lbp->recovery_sock != -1)
		{
			close(lbp->recovery_sock);
			lbp->recovery_sock = -1;
		}
#ifdef PRINT_DEBUG
	show_debug("%s:host[%s] port[%d]",func,lbp->hostName,lbp->recoveryPort);
#endif
		status = send_packet(lbp,packet);
		if (lbp->recovery_sock != -1)
		{
			close(lbp->recovery_sock);
			lbp->recovery_sock = -1;
		}
		lbp ++;
	}
	return STATUS_OK;
}

static int
send_vacuum(HostTbl *host, char * userName, int stage)
{
	int rtn = STATUS_OK;
	ReplicateHeader header;
	char * query = NULL;

	if (stage == PGR_RECOVERY_1ST_STAGE)
	{
		query = strdup("VACUUM");
	}
	else
	{
		query = strdup("VACUUM FULL");
	}
	memset(&header,0,sizeof(header));
	header.query_size = strlen(query) + 1;
	strncpy(header.dbName,"template1",sizeof(header.dbName));
	strncpy(header.userName,userName,sizeof(header.userName));
	header.cmdSys = CMD_SYS_REPLICATE;
	header.cmdSts = CMD_STS_QUERY;
	header.cmdType = CMD_TYPE_VACUUM;
	header.pid = getpid();
	header.query_id = getpid();
	header.isAutoCommit=1;
	rtn = PGRsend_replicate_packet_to_server(host,&header,query,PGR_Result,0, true);
	if (query !=NULL)
		free(query);
	return rtn;	
}

static bool
first_setup_recovery(int * sock, RecoveryPacket * packet)
{
	char * func = "first_setup_recovery()";
	int status;
	HostTbl * master = (HostTbl *)NULL;
	bool loop_end = false;
	HostTbl host_tbl;
	char * userName = NULL;
	int ip;

	memset(Target.hostName,0,sizeof(Target.hostName));
	strncpy(Target.hostName,packet->hostName,sizeof(Target.hostName));
	ip = PGRget_ip_by_name(Target.hostName);
	sprintf(Target.resolvedName,
		 "%d.%d.%d.%d",
		 (ip      ) & 0xff ,
		 (ip >>  8) & 0xff ,
		 (ip >> 16) & 0xff ,
		 (ip >> 24) & 0xff );
	Target.port = ntohs(packet->port);
	Target.recoveryPort = ntohs(packet->recoveryPort);
	Target.sock = *sock;
	Target.recovery_sock = *sock;
#ifdef PRINT_DEBUG
	show_debug("%s:1st setup target %s",func,Target.hostName);
	show_debug("%s:1st setup port %d",func,Target.port);
#endif			
	/*
	 * check another recovery process 
	 */
	if (PGRget_recovery_status() != RECOVERY_INIT)
	{
		/*
		 * recovery process is already running
		 */
#ifdef PRINT_DEBUG
		show_debug("%s:already recovery job runing",func);
#endif			
		memset(packet,0,sizeof(packet));
		PGRset_recovery_packet_no(packet, RECOVERY_ERROR_OCCUPIED) ;
		status = send_packet(&Target,packet);
		loop_end = true;
		return loop_end;
	}
	/*
	 * add recovery target to host table
	 */
#ifdef PRINT_DEBUG
	show_debug("%s:add recovery target to host table",func);
#endif			
	memcpy(host_tbl.hostName,Target.hostName,sizeof(host_tbl.hostName));
	memcpy(host_tbl.resolvedName,Target.resolvedName,sizeof(host_tbl.resolvedName));
	host_tbl.port = Target.port;
	host_tbl.recoveryPort = Target.recoveryPort;
	PGRset_recovered_host(&host_tbl,DB_TBL_INIT);
	PGRadd_HostTbl(&host_tbl,DB_TBL_INIT);
	/*
	 * send prepare recovery to load balancer
	 */
	PGRsend_load_balance_packet(packet);
	userName = strdup(packet->userName);

	/*
	 * set RECOVERY_PGDATA_REQ packet data
	 */
#ifdef PRINT_DEBUG
	show_debug("%s:set RECOVERY_PGDATA_REQ packet data",func);
#endif			
	memset(packet,0,sizeof(RecoveryPacket));
	PGRset_recovery_packet_no(packet, RECOVERY_PGDATA_REQ );

retry_connect_master:
	master = PGRget_master();
	if (master == (HostTbl *)NULL)
	{
		/*
		 * connection error , master may be down
		 */
		show_error("%s:get master info error , master may be down",func);
		PGRset_recovery_packet_no(packet, RECOVERY_ERROR_TARGET_ONLY);
		status = send_packet(&Target, packet);
		reset_recovery_prepare();
		loop_end = true;
		if (userName != NULL)
			free(userName);
		return loop_end;
	}
	/* send vauum command to master server */
	status = send_vacuum(master, userName, PGR_RECOVERY_1ST_STAGE );
	if (status != STATUS_OK)
	{
		PGRset_host_status(master, DB_TBL_ERROR);
		goto retry_connect_master;
	}

	memcpy(Master.hostName,master->hostName,sizeof(Master.hostName));
	memcpy(Master.resolvedName,master->resolvedName,sizeof(Master.resolvedName));
	Master.sock = -1;
	Master.recovery_sock = -1;
	Master.port = master->port;
	Master.recoveryPort = master->recoveryPort;

#ifdef PRINT_DEBUG
	show_debug("%s:send packet to master %s recoveryPort %d",func, Master.hostName, Master.recoveryPort);
#endif			
	status = send_packet(&Master, packet);
	if (status != STATUS_OK)
	{
		/*
		 * connection error , master may be down
		 */
		show_error("%s:connection error , master may be down",func);
		PGRset_host_status(master,DB_TBL_ERROR);
		goto retry_connect_master ;
	}
	
	/*
	 * start prepare of recovery
	 *     set recovery status to "prepare start"
	 *     start transaction count up
	 */
	start_recovery_prepare();
	/*
	 * wait answer from master server 
	 */
#ifdef PRINT_DEBUG
	show_debug("%s:wait answer from master server",func);
#endif			
	memset(packet,0,sizeof(RecoveryPacket));
	read_packet_from_master(&Master, packet);
#ifdef PRINT_DEBUG
	show_debug("%s:get answer from master:no[%d]",func,ntohs(packet->packet_no));
#endif			
	if (ntohs(packet->packet_no) == RECOVERY_PGDATA_ANS)
	{
		/*
		 * send a packet to load balancer that is stopped master's 
		 * load balancing until all recovery process is finished
		 */
		PGRsend_load_balance_packet(packet);
		memcpy((char *)&MasterPacketData,packet,sizeof(RecoveryPacket));

		/*
		 * prepare answer from master DB
		 */
		PGRset_recovery_packet_no(packet, RECOVERY_PREPARE_ANS );
		memcpy(packet->hostName,Master.hostName,sizeof(packet->hostName));
		status = send_packet(&Target, packet);
		if (status != STATUS_OK)
		{
			show_error("%s:no[%d] send_packet to target error",func,ntohs(packet->packet_no));
			PGRset_recovery_packet_no(packet, RECOVERY_ERROR_TARGET_ONLY);
			status = send_packet(&Master,packet);
			reset_recovery_prepare();
			loop_end = true;
		}
	}
	if (userName != NULL)
		free(userName);


	return loop_end;
}

static int
wait_transaction_count_clear(void)
{
	char * func ="wait_transaction_count_clear()";
	HostTbl * master = (HostTbl *)NULL;
	int cnt = 0;
	int recovery_status = PGRget_recovery_status();

	while (recovery_status != RECOVERY_CLEARED)
	{
		master = PGRget_master();
		if (master == (HostTbl *)NULL)
		{
			show_error("%s:get master info error , master may be down",func);
			continue;
		}
		if ((recovery_status == RECOVERY_PREPARE_START) &&
			(master->transaction_count==0))
		{
			PGRset_recovery_status(RECOVERY_CLEARED);
			break;
		}

		sleep(1);
#ifdef PRINT_DEBUG
		show_debug("now, waiting clear every transaction for recovery");
#endif
		cnt ++;
		if (cnt > RECOVERY_TIMEOUT * 60 )
		{
			show_error("sorry, it is  timeout for waiting clear transaction");
			return STATUS_ERROR;
		}
		recovery_status = PGRget_recovery_status();
	}
	return STATUS_OK;
}

static bool
second_setup_recovery (RecoveryPacket * packet)
{
	char * func = "second_setup_recovery()";
	HostTbl * master = (HostTbl *)NULL;
	int status;
	bool loop_end = false;
	char * userName = NULL;
	int recovery_status = 0;

	/* send vauum command to master server */
	while ((master = PGRget_master()) != NULL)
	{
		/*
		 * wait until all started transactions are going to finish
		 */
		status = wait_transaction_count_clear();
		if (status != STATUS_OK)
		{
			show_error("%s:transaction is too busy, please try again after",func);
			PGRset_recovery_packet_no(packet, RECOVERY_ERROR_TARGET_ONLY);
			status = send_packet(&Target,packet);
			status = send_packet(&Master,packet);
			reset_recovery_prepare();
			return true;
		}
		userName = strdup(packet->userName);
		status = send_vacuum(master, userName, PGR_RECOVERY_2ND_STAGE );
		if (status != STATUS_OK)
		{
			PGRset_host_status(master, DB_TBL_ERROR);
			if (userName != NULL)
			{
				free(userName);
				userName = NULL;
			}
			continue;
		}
		break;
	}

	if (master == NULL)
	{
		show_error("%s:vacuum error , master may be down",func);
		PGRset_recovery_packet_no(packet, RECOVERY_ERROR_TARGET_ONLY);
		status = send_packet(&Target,packet);
		status = send_packet(&Master,packet);
		reset_recovery_prepare();

		return true;		
	}

	recovery_status = PGRget_recovery_status();
	if ((recovery_status != RECOVERY_PREPARE_START) &&
		(recovery_status != RECOVERY_WAIT_CLEAN) &&
		(recovery_status != RECOVERY_CLEARED))
	{
		show_error("%s:queue set failed. stop to recovery",func);
		PGRset_recovery_packet_no(packet, RECOVERY_ERROR_CONNECTION);
		status = send_packet(&Target,packet);
		status = send_packet(&Master,packet);
		reset_recovery_prepare();
		if (userName != NULL)
			free(userName);
		return true;
	}

	/*
	 * then, send fsync request to master DB
	 */
	PGRset_recovery_packet_no(packet, RECOVERY_FSYNC_REQ );
	status = send_packet(&Master,packet);
	if (status != STATUS_OK)
	{
		/*
		 * connection error , master may be down
		 */
		show_error("%s:connection error , master may be down",func);
		PGRset_recovery_packet_no(packet, RECOVERY_ERROR_CONNECTION);
		status = send_packet(&Target,packet);
		status = send_packet(&Master,packet);
		reset_recovery_prepare();
		if (userName != NULL)
			free(userName);
		return true;
	}

	recovery_status = PGRget_recovery_status();
	if ((recovery_status != RECOVERY_PREPARE_START) &&
		(recovery_status != RECOVERY_WAIT_CLEAN) &&
		(recovery_status != RECOVERY_CLEARED))
	{
		show_error("%s:queue set failed. stop to recovery",func);
		PGRset_recovery_packet_no(packet, RECOVERY_ERROR_CONNECTION);
		status = send_packet(&Target,packet);
		status = send_packet(&Master,packet);
		reset_recovery_prepare();
		if (userName != NULL)
			free(userName);
		return true;
	}

	/*
	 * wait answer from master server 
	 */
	memset(packet,0,sizeof(RecoveryPacket));
	read_packet_from_master(&Master,packet);
	if (ntohs(packet->packet_no) == RECOVERY_FSYNC_ANS )
	{
		/*
		 * master DB finished fsync
		 */
		PGRset_recovery_packet_no(packet, RECOVERY_START_ANS );
		memcpy(packet->hostName,Master.hostName,sizeof(packet->hostName));
		status = send_packet(&Target,packet);
		if (status != STATUS_OK)
		{
			finish_recovery();
			loop_end = true;
		}
	}
	else
	{
		show_error("%s:failure answer returned",func);
		PGRset_recovery_packet_no(packet, RECOVERY_ERROR_CONNECTION);
		status = send_packet(&Target,packet);
		status = send_packet(&Master,packet);
		reset_recovery_prepare();
		loop_end = true;
	}
	if (userName != NULL)
		free(userName);
	return loop_end;
}

static char *
read_queue_file(FILE * fp, ReplicateHeader * header, char *query)
{
	char * func = "read_queue_file()";
	int size = 0;

	if (fp == NULL)
	{
		return NULL;
	}
	if (fread((char*)header,sizeof(ReplicateHeader),1,fp) < 1)
	{
		return NULL;
	}
	size = ntohl(header->query_size);
	if (size >= 0)
	{
		query = malloc(size+4);
		if (query == NULL)
		{
			show_error("%s:malloc failed:(%s)",func,strerror(errno));
		}
		memset(query,0,size+4);
		if (size > 0)
		{
			if (fread(query,size,1,fp) < 1)
			{
				return NULL;
			}
		}
		return query;
	}
	return NULL;
}

/**
 * send queries from queue.
 *
 * return
 *   STATUS_OK - success both
 *   STATUS_ERROR - fail both
 */
static int
PGRsend_queue(RecoveryTbl * master, RecoveryTbl * target)
{
	char * func = "PGRsend_queue()";
	HostTbl * master_ptr = NULL;
	HostTbl * target_ptr = NULL;
	RecoveryQueueFile * msg = NULL;
	FILE * rfp = NULL;
	ReplicateHeader header;
	char * query = NULL;
	int size = 0;
	int status = 0;
	int query_size = 0;
	int rtn=0;

	if (master == (RecoveryTbl *)NULL)
	{
		show_error("%s:there is no master ",func);
		return STATUS_ERROR;
	}
#ifdef PRINT_DEBUG
	show_debug("%s:master %s - %d",func,master->hostName,master->port);
#endif			
	master_ptr = PGRget_HostTbl(master->resolvedName,master->port);
	if (master_ptr == (HostTbl *)NULL)
	{
		show_error("%s:master table is null",func);
		return STATUS_ERROR;
	}
	if (target != (RecoveryTbl *)NULL)
	{
#ifdef PRINT_DEBUG
		show_debug("%s:target %s - %d",func,target->hostName,target->port);
#endif			
		target_ptr = PGRget_HostTbl(target->resolvedName,target->port);
		if (target_ptr == (HostTbl *)NULL)
		{
			show_error("%s:target table is null",func);
			return STATUS_ERROR;
		}
	}

	size = sizeof(RecoveryQueueFile) + FILENAME_MAX_LENGTH;
	msg = (RecoveryQueueFile *)malloc(size+4);
	if (msg == NULL)
	{
#ifdef PRINT_DEBUG
		show_debug("%s:malloc() failed. reason: %s",func, strerror(errno));
#endif
		return STATUS_ERROR;
	}
	memset(msg,0,size+4);
	status = STATUS_OK;
	while (msgrcv(*RecoveryMsgid , msg, FILENAME_MAX_LENGTH, 0, IPC_NOWAIT) > 0 )
	{
		strncpy(Recovery_Status_Inf->read_file,(char *)(msg->mdata),FILENAME_MAX_LENGTH);
		PGRsem_lock(RecoverySemID, SEM_NUM_OF_RECOVERY_QUEUE);
		if (!strncmp(Recovery_Status_Inf->write_file,Recovery_Status_Inf->read_file,sizeof(Recovery_Status_Inf->write_file)))
		{
			memset(Recovery_Status_Inf->write_file,0,sizeof(Recovery_Status_Inf->write_file));
		}
		PGRsem_unlock(RecoverySemID, SEM_NUM_OF_RECOVERY_QUEUE);
		rfp = fopen(Recovery_Status_Inf->read_file,"r");
		if (rfp == NULL)
		{
			show_error("%s:queue file [%s] can not be opened:(%s)",func,Recovery_Status_Inf->read_file,strerror(errno));
			return STATUS_ERROR;
		}
		while ((query = read_queue_file(rfp, &header,query)) != NULL)
		{
			query_size = ntohl(header.query_size);
			if (query_size < 0)
			{
				if (query != NULL)
				{
					free(query);
					query = NULL;
				}
				break;
			}
			PGR_Response_Inf->current_cluster = 0;
			rtn=PGRsend_replicate_packet_to_server(master_ptr,&header,query,PGR_Result,ntohl(header.replicate_id), true);
			if (target_ptr != NULL)
			{
				PGR_Response_Inf->current_cluster = 1;
				rtn=PGRsend_replicate_packet_to_server(target_ptr,&header,query,PGR_Result,ntohl(header.replicate_id), true);
			}
		}
		if (query != NULL)
		{
			free(query);
			query = NULL;
		}
		if (rfp != NULL)
		{
			fclose(rfp);
			rfp = NULL;
			unlink(Recovery_Status_Inf->read_file);
			memset(Recovery_Status_Inf->read_file,0,sizeof(Recovery_Status_Inf->read_file));
		}
	}
#ifdef PRINT_DEBUG
	show_debug("%s:send_queue return status %d",func,status);
#endif			
	return status;
}

static void
pgrecovery_loop(int fd)
{
	char * func = "pgrecovery_loop()";
	int count;
	int sock;
	int status;
	bool loop_end = false;
	RecoveryPacket packet;
	HostTbl new_host;
	RecoveryTbl * lbp;

	lbp = LoadBalanceTbl;
	if (lbp == (RecoveryTbl *)NULL)
	{
		show_error("%s:recovery table is NULL",func);
		return ;
	}
#ifdef PRINT_DEBUG
	show_debug("%s:recovery accept port %d",func, Recovery_Port_Number);
#endif			
	count = 0;
	while ((status = PGR_Create_Acception(fd,&sock,"",Recovery_Port_Number)) != STATUS_OK)
	{
		show_error("%s:PGR_Create_Acception failed",func);
		PGR_Close_Sock(&sock);
		sock = -1;
		if ( count > PGR_CONNECT_RETRY_TIME)
		{
			return;
		}
		count ++;
	}
	if(sock==-1) {
			show_error("can't create recovery socket.exit.");
			PGRreplicate_exit(1);
	}
	for(;;)
	{
		int read_size = 0;
		int rtn;
		fd_set	  rmask;
		struct timeval timeout;

		timeout.tv_sec = RECOVERY_TIMEOUT;
		timeout.tv_usec = 0;

		/*
		 * Wait for something to happen.
		 */
		FD_ZERO(&rmask);
		FD_SET(sock,&rmask);
		/*
		 * read packet from target cluster server
		 */
		rtn = select(sock+1, &rmask, (fd_set *)NULL, (fd_set *)NULL, &timeout);

		if (rtn == 0) /* timeout */
		{
			return;
		}

		if (rtn && FD_ISSET(sock, &rmask))
		{
			read_size = read_packet(sock, &packet);
		}
		else
		{
			continue;
		}

#ifdef PRINT_DEBUG
		show_debug("%s:receive packet no:%d",func,ntohs(packet.packet_no));
#endif			

		switch (ntohs(packet.packet_no))
		{
			case RECOVERY_PREPARE_REQ :
				/*
				 * start prepare of recovery
				 */

#ifdef PRINT_DEBUG
				show_debug("%s:1st master %s - %d",
					func,Master.hostName,Master.port);
				show_debug("%s:1st target %s - %d",
					func,Target.hostName,Target.port);
#endif			

				loop_end = first_setup_recovery(&sock, &packet);
#ifdef PRINT_DEBUG
				show_debug("%s:first_setup_recovery end:%d ",func,loop_end);
#endif
				break;
			case RECOVERY_START_REQ : 
				/*
				 * now, recovery process will start
				 *    stop the transaction count up
				 *    start queueing and stop send all queries for master DB
				 */
#ifdef PRINT_DEBUG
				show_debug("%s:2nd master %s - %d",
					func, Master.hostName,Master.port);
				show_debug("%s:2nd target %s - %d",
					func, Target.hostName,Target.port);
#endif			
				loop_end = second_setup_recovery (&packet);
#ifdef PRINT_DEBUG
				show_debug("%s:second_setup_recovery end :%d ",
					func,loop_end);
#endif			
				break;
			case RECOVERY_QUEUE_DATA_REQ : 
				/*
				 * send all queries in queue
				 */

#ifdef PRINT_DEBUG
				show_debug("%s:last master %s - %d",
					func, Master.hostName,Master.port);
				show_debug("%s:last target %s - %d",
					func, Target.hostName,Target.port);
#endif			
				status = PGRsend_queue(&Master,&Target);
				if (status == STATUS_OK)
				{
					memcpy(new_host.hostName,Target.hostName,sizeof(new_host.hostName));
					memcpy(new_host.resolvedName,Target.resolvedName,sizeof(new_host.resolvedName));
					new_host.port = Target.port;
					new_host.recoveryPort = Target.recoveryPort;
					PGRset_recovered_host(&new_host,DB_TBL_USE);
					PGRadd_HostTbl(&new_host,DB_TBL_USE);
					PGRset_recovery_packet_no(&packet, RECOVERY_QUEUE_DATA_ANS );
					status = send_packet(&Target, &packet);
					if (status != STATUS_OK)
					{
						finish_recovery();
					}
				}
				else
				{
					/* connection error , master or target may be down */
					show_error("%s:PGRsend_queue failed",func);
					PGRset_recovery_packet_no(&packet, RECOVERY_ERROR_CONNECTION);
					status = send_packet(&Target,&packet);
					finish_recovery();
				}
				loop_end = true;
				break;
			case RECOVERY_FINISH : 
				/*
				 * finished rsync DB datas from master to target 
				 */
				/*
				 * stop queueing, and re-initialize recovery status
				 */
				finish_recovery();
				loop_end = true;
				/*
				 * send finish recovery to load balancer
				 */
				if (Master.recovery_sock != -1)
				{
					close(Master.recovery_sock);
					Master.recovery_sock = -1;
				}
				if (Target.recovery_sock != -1)
				{
					close(Target.recovery_sock);
					Target.recovery_sock = -1;
				}
				send_packet(&Master, &packet);
				MasterPacketData.packet_no = packet.packet_no;
				PGRsend_load_balance_packet(&MasterPacketData);
				PGRsend_load_balance_packet(&packet);
				memset((char *)&MasterPacketData,0,sizeof(RecoveryPacket));
				break;
			case RECOVERY_ERROR_ANS : 
#ifdef PRINT_DEBUG
				show_debug("%s:recovery error accept. top queueing and initiarse recovery status",func);
#endif			
				status = PGRsend_queue(&Master,NULL);
				memset(&packet,0,sizeof(RecoveryPacket));
				PGRset_recovery_packet_no(&packet, RECOVERY_ERROR_ANS);
				send_packet(&Master, &packet);
				finish_recovery();
				loop_end = true;
				PGRset_recovery_packet_no(&MasterPacketData, RECOVERY_FINISH );
				PGRsend_load_balance_packet(&MasterPacketData);
				memset((char *)&MasterPacketData,0,sizeof(RecoveryPacket));
				break;
		default:
		  show_error("%s:unknown packet. abort to parse");
			     loop_end=true;
			     break;
		}
		if (loop_end)
		{
			if (Master.sock != -1)
			{
				close (Master.sock);
			}
			if (Master.recovery_sock != -1)
			{
				close (Master.recovery_sock);
			}
			PGR_Close_Sock(&sock);
			return;
		}
	}
}

void
PGRrecovery_main(int fork_wait_time)
{
	char * func = "PGRrecovery_main()";
	int status;
	int fd = -1;
	int rtn;
	pid_t pgid = 0;
	pid_t pid = 0;

	pgid = getpgid(0);
	pid = fork();
	if (pid != 0)
	{
		return;
	}

	PGRsignal(SIGCHLD, SIG_DFL);
	PGRsignal(SIGHUP, PGRexit_subprocess);	
	PGRsignal(SIGINT, PGRexit_subprocess);	
	PGRsignal(SIGQUIT, PGRexit_subprocess);	
	PGRsignal(SIGTERM, PGRexit_subprocess);	
	PGRsignal(SIGPIPE, SIG_IGN);	
	/*
	 * in child process,
	 * call recovery module
	 */
	setpgid(0,pgid);

	if (fork_wait_time > 0) {
#ifdef PRINT_DEBUG
		show_debug("recovery process: wait fork(): pid = %d", getpid());
#endif		
		sleep(fork_wait_time);
	}

#ifdef PRINT_DEBUG
	show_debug("%s:PGRrecovery_main bind port %d",func,Recovery_Port_Number);
#endif			
	status = PGR_Create_Socket_Bind(&fd, "", Recovery_Port_Number);
	if (status != STATUS_OK)
	{
		show_error("%s:PGR_Create_Socket_Bind failed",func);
		exit(1);
	}
	memset(&MasterPacketData,0,sizeof(RecoveryPacket));
	memset(&Master,0,sizeof(RecoveryTbl));
	memset(&Target,0,sizeof(RecoveryTbl));
	for (;;)
	{
		fd_set	  rmask;
		struct timeval timeout;

		timeout.tv_sec = RECOVERY_TIMEOUT;
		timeout.tv_usec = 0;

		/*
		 * Wait for something to happen.
		 */
		FD_ZERO(&rmask);
		FD_SET(fd,&rmask);
		rtn = select(fd+1, &rmask, (fd_set *)NULL, (fd_set *)NULL, &timeout);
		if (rtn && FD_ISSET(fd, &rmask))
		{
			pgrecovery_loop(fd);
		}
	}
}

#ifdef PRINT_DEBUG
static void
show_recovery_packet(RecoveryPacket * packet)
{
	show_debug("no = %d",ntohs(packet->packet_no));
	show_debug("max_connect = %d",ntohs(packet->max_connect));
	show_debug("port = %d",ntohs(packet->port));
	show_debug("recoveryPort = %d",ntohs(packet->recoveryPort));
	if (packet->hostName != NULL)
		show_debug("hostName = %s",packet->hostName);
	if (packet->pg_data != NULL)
		show_debug("pg_data = %s",packet->pg_data);
}
#endif			
