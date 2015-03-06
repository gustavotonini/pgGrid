/*--------------------------------------------------------------------
 * FILE:
 *     recovery.c
 *
 * NOTE:
 *     This file is composed of the functions to call with the source
 *     at backend for the recovery.
 *     Low level I/O functions that called by in these functions are 
 *     contained in 'replicate_com.c'.
 *
 *--------------------------------------------------------------------
 */

/*--------------------------------------
 * INTERFACE ROUTINES
 *
 * I/O call:
 *      PGR_recovery_finish_send
 * master module:
 *      PGR_Master_Main(void);
 * recovery module:
 *      PGR_Recovery_Main
 *-------------------------------------
 */
#ifdef USE_REPLICATION

#include "postgres.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/wait.h>
#include <ctype.h>
#include <time.h>
#include <pwd.h>
#include <sys/time.h>
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
#include <sys/select.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <sys/file.h>
#include <dirent.h>

#include "libpq/pqsignal.h"
#include "utils/guc.h"
#include "miscadmin.h"
#include "nodes/nodes.h"
#include "nodes/parsenodes.h"
#include "access/xact.h"
#include "access/xlog.h"
#include "tcop/tcopprot.h"
#include "postmaster/postmaster.h"

#include "../interfaces/libpq/libpq-fe.h"
#include "../interfaces/libpq/libpq-int.h"
#include "../interfaces/libpq/fe-auth.h"

#include "replicate.h"

#ifdef WIN32
#include "win32.h"
#else
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#endif

#ifndef HAVE_STRDUP
#include "strdup.h"
#endif
#ifdef HAVE_CRYPT_H
#include <crypt.h>
#endif

#ifdef MULTIBYTE
#include "mb/pg_wchar.h"
#endif

#define RECOVERY_LOOP_END	(0)
#define RECOVERY_LOOP_CONTINUE	(1)
#define RECOVERY_LOOP_FAIL	(2)
char Local_Host_Name[HOSTNAME_MAX_LENGTH];
int PGR_Recovery_Mode = 0;
static int Rsync_Retry_times = 0;
static char Ping_Result[512];

static int read_packet(int sock,RecoveryPacket * packet);
static int send_recovery_packet(int  sock, RecoveryPacket * packet);
static int send_packet(int * sock, RecoveryPacket * packet );
static void master_loop(int fd);
static int start_recovery_send(int * sock, ReplicateServerInfo * host);
static int stop_recovery_send(int * sock, ReplicateServerInfo * host);
static int rsync_pg_data(char * src , char * dest);
static int flexible_rsync(char * src, char * dest, int retry);
static int remove_dir(char * dir_name);
static int clear_bkup_dir(char * dir_name);
static int bkup_dir(char * dir_name);
static int restore_dir(char * dir_name);
static int rsync_global_dir(char * src, char * dest, int stage);
static int first_recovery(char * src, char * dest, char * dir);
static int second_recovery(char * src, char * dest, char * dir);
static int recovery_rsync(char * src , char * dest, int stage);
static int recovery_loop(int fd, int mode);
static void show_recovery_packet(RecoveryPacket * packet);
static int direct_send_packet(int packet_no);
static void set_recovery_packet(RecoveryPacket * packet, int packet_no);
static int cold_recovery(char * src, RecoveryPacket *packet, bool need_sync_table_space, int stage);
static int hot_recovery(RecoveryPacket *packet, int stage);
static int restore_from_dumpall( char * hostName, uint16_t portNum, char * userName);
static int restore_from_dump( char * hostName, uint16_t portNum, char * userName, char * dbName);
static int restore_from_each_dump( char * hostName, uint16_t portNum, char * userName);
static PGresult * get_dbName(char * hostName, uint16_t portNum, char * userName);

static int sync_table_space(char * hostName, uint16_t portNum, char * userName, int stage);
static PGresult * get_table_space_location(char * hostName, uint16_t portNum, char * userName);
static int rsync_table_space(char * hostName, char * location, int stage);
static char * ping_to_target(char * target);
static double get_ping_result (char * ping_data);
static double calculate_band(double msec);
static char * get_target_hostname(char * src);

int PGR_recovery_error_send(void);
int PGR_recovery_finish_send(void);
int PGR_recovery_queue_data_req(void);
int PGR_Master_Main(void);
int PGR_Recovery_Main(int mode);
int PGR_measure_bandwidth(char * target);

static int
read_packet(int sock,RecoveryPacket * packet)
{
	int r;
	char * read_ptr;
	int read_size = 0;
	int packet_size = 0;

	read_ptr = (char*)packet;
	packet_size = sizeof(RecoveryPacket);

	for (;;){
		r = recv(sock,read_ptr + read_size ,packet_size, MSG_WAITALL);
		if (r < 0) {
			if (errno == EINTR || errno == EAGAIN) {
				continue;
			} else {
				elog(DEBUG1, "read_packet():recv failed");
				return -1;
			}
		} else if (r == 0) {
			elog(DEBUG1, "read_packet():unexpected EOF");
			return -1;
		} else /*if (r > 0)*/ {
			read_size += r;
			if (read_size == packet_size) {
				show_recovery_packet(packet);
				return read_size;
			}
		}
	}
	return -1;
}

static int
send_recovery_packet(int  sock, RecoveryPacket * packet)
{
	char * send_ptr;
	int send_size= 0;
	int buf_size = 0;
	int s;
	int rtn;	
	fd_set	  wmask;
	struct timeval timeout;

	timeout.tv_sec = RECOVERY_TIMEOUT;
	timeout.tv_usec = 0;

	/*
	 * Wait for something to happen.
	 */
	rtn = 1;
	while (rtn)
	{
		for (;;)
		{
			timeout.tv_sec = RECOVERY_TIMEOUT;
			timeout.tv_usec = 0;

			FD_ZERO(&wmask);
			FD_SET(sock,&wmask);
			rtn = select(sock+1, (fd_set *)NULL, &wmask, (fd_set *)NULL, &timeout);

			if (rtn < 0)
			{
				if (errno == EINTR || errno == EAGAIN)
				{
					continue;
				}
				else
				{
					rtn = 0;
					break;
				}
			}
			else if (rtn && FD_ISSET(sock, &wmask))
			{
				send_ptr = (char *)packet;
				buf_size = sizeof(RecoveryPacket);

				s = send(sock,send_ptr + send_size,buf_size - send_size ,0);
				if (s < 0) {
					if (errno == EINTR || errno == EAGAIN) {
						continue;
					}
					elog(DEBUG1, "send_recovery_packet():send error");

					/* EPIPE || ENCONNREFUSED || ENSOCK || EHOSTUNREACH */
					return STATUS_ERROR;
				} else if (s == 0) {
					elog(DEBUG1, "send_recovery_packet():unexpected EOF");
					return STATUS_ERROR;
				} else /*if (s > 0)*/ {
					send_size += s;
					if (send_size == buf_size)
					{
						return STATUS_OK;
					}
				}
			}
		}
	}
	return STATUS_ERROR;
}

static int
send_packet(int * sock, RecoveryPacket * packet )
{
	int count = 0;
	ReplicateServerInfo * host = NULL;

	host = PGR_get_replicate_server_info();
	if (host == (ReplicateServerInfo*)NULL)
	{
		return STATUS_ERROR;
	}
	count = 0;
	while (send_recovery_packet(*sock,packet) != STATUS_OK)
	{
		if (count < MAX_RETRY_TIMES )
		{
			count ++;
			continue;
		}
		count = 0;
		close(*sock);
		PGR_Set_Replication_Server_Status(host,DATA_ERR);
		host = PGR_get_replicate_server_info();
		if (host == (ReplicateServerInfo*)NULL)
		{
			return STATUS_ERROR;
		}
		PGR_Set_Replication_Server_Status(host,DATA_USE);
		PGR_Create_Socket_Connect(sock, host->hostName , host->recoveryPortNumber);
	}
	return STATUS_OK;
}

static void
master_loop(int fd)
{
	int count;
	int sock;
	int status = STATUS_OK;
	RecoveryPacket packet;
	int r_size = 0;
	bool loop_end = false;

	count = 0;
	while ((status = PGR_Create_Acception(fd,&sock,"",RecoveryPortNumber)) != STATUS_OK)
	{
		PGR_Close_Sock(&sock);
		sock = -1;
		if ( count > MAX_RETRY_TIMES)
		{
			return;
		}
		count ++;
	}
	for(;;)
	{
		int	rtn;
		fd_set	  rmask;
		struct timeval timeout;

		timeout.tv_sec = RECOVERY_TIMEOUT;
		timeout.tv_usec = 0;

		/*
		 * Wait for something to happen.
		 */
		FD_ZERO(&rmask);
		FD_SET(sock,&rmask);
		memset(&packet,0,sizeof(RecoveryPacket));
		rtn = select(sock+1, &rmask, (fd_set *)NULL, (fd_set *)NULL, &timeout);
		if (rtn && FD_ISSET(sock, &rmask))
		{
			r_size = read_packet(sock,&packet);
			if (r_size == 0)
			{
				continue;
			}
			else if (r_size < 0)
			{
			    loop_end=true;
			    break;
		        }
		}
		else
		{
			continue;
		}
		switch (ntohs(packet.packet_no))
		{
			case RECOVERY_PGDATA_REQ :
				/*
				 * PGDATA information request
				 */
				/*
				 * get master server information
				 */
				memset(&packet,0,sizeof(packet));
				set_recovery_packet(&packet, RECOVERY_PGDATA_ANS) ;
				status = send_packet(&sock,&packet);
				PGR_Set_Cluster_Status(STATUS_RECOVERY);
				break;
			case RECOVERY_FSYNC_REQ : 
				/*
				 * get master server information
				 */
				memset(&packet,0,sizeof(packet));
				set_recovery_packet(&packet, RECOVERY_FSYNC_ANS );
				status = send_packet(&sock,&packet);
				PGR_Set_Cluster_Status(STATUS_RECOVERY);
				loop_end = true;
				break;
			case RECOVERY_ERROR_TARGET_ONLY:	
				memset(&packet,0,sizeof(packet));
				set_recovery_packet(&packet, RECOVERY_ERROR_ANS );
				status = send_packet(&sock,&packet);
				PGR_Set_Cluster_Status(STATUS_REPLICATED);
				break;
			case RECOVERY_ERROR_CONNECTION:
				memset(&packet,0,sizeof(packet));
				set_recovery_packet(&packet, RECOVERY_ERROR_ANS );
				status = send_packet(&sock,&packet);
				PGR_Set_Cluster_Status(STATUS_REPLICATED);
				/**
				 * kill broken cluster db.
				 * FIXME: missing MyProcPid here. It must be postmaster's pid.
				 * but here's a bug MyProcPid doesn't initialized properly , so MyProcPid = postmaster's pid.				
				 * To fix this, define variable to set posmaster's pid.
				 */
				kill(MyProcPid,SIGQUIT);
				loop_end = true;
				break;
			case RECOVERY_ERROR_ANS:
			  /* TODO: recovery failed. close this postmaster */
			        loop_end = true;
			        break;
			case RECOVERY_FINISH:
				PGR_Set_Cluster_Status(STATUS_REPLICATED);
				loop_end = true;
				break;
             		default:
		                loop_end = true;
				break;
		}
		if (loop_end)
		{
			break;
		}
	}
	PGR_Close_Sock(&sock);
}

int
PGR_Master_Main(void)
{
	int status;
	int fd = -1;
	int rtn;
	int pid;

	if ((pid = fork()) != 0 )
	{
		return pid;
	}
	
	memset(Local_Host_Name,0,sizeof(Local_Host_Name));
	gethostname(Local_Host_Name,sizeof(Local_Host_Name));
	pqsignal(SIGHUP, authdie);
	pqsignal(SIGTERM, authdie);
	pqsignal(SIGINT, authdie);
	pqsignal(SIGQUIT, authdie);
	pqsignal(SIGALRM, authdie);
	PG_SETMASK(&UnBlockSig);

	status = STATUS_ERROR;
	while ((status = PGR_Create_Socket_Bind(&fd, "", RecoveryPortNumber)) != STATUS_OK)
	{
		fprintf(stderr,"PGR_Master_Main:PGR_Create_Socket_Bind failed. Port [%d] can not be used\n",RecoveryPortNumber);
		sleep(RECOVERY_WAIT_CLEAN);
	}
	for (;;)
	{
		fd_set	  rmask;
		struct timeval timeout;

		timeout.tv_sec = 60;
		timeout.tv_usec = 0;

		/*
		 * Wait for something to happen.
		 */
		FD_ZERO(&rmask);
		FD_SET(fd,&rmask);
		rtn = select(fd+1, &rmask, (fd_set *)NULL, (fd_set *)NULL, &timeout);
		if (rtn && FD_ISSET(fd, &rmask))
		{
			master_loop(fd);
		}
	}
	return pid;
}

static int
start_recovery_send(int * sock, ReplicateServerInfo * host)
{
	int status;
	RecoveryPacket packet;
	status = PGR_Create_Socket_Connect(sock, host->hostName, host->recoveryPortNumber);
	if (status != STATUS_OK)
	{
		if (Debug_pretty_print)
		{
			elog(DEBUG1,"connection error to replication server");
		}
		return STATUS_ERROR;
	}

	memset(&packet,0,sizeof(packet));
	set_recovery_packet(&packet, RECOVERY_PREPARE_REQ );
	status = send_packet(sock,&packet);

	return status;
}

static int
stop_recovery_send(int * sock, ReplicateServerInfo * host)
{
	int status;
	RecoveryPacket packet;

	memset(&packet,0,sizeof(packet));
	set_recovery_packet(&packet, RECOVERY_ERROR_ANS );
	status = send_packet(sock,&packet);
	return status;
}

static int
direct_send_packet(int packet_no)
{

	int status;
	int fd = -1;
	ReplicateServerInfo * host;
	RecoveryPacket packet;

	host = PGR_get_replicate_server_info();
	if (host == NULL)
	{
		return STATUS_ERROR;
	}
	status = PGR_Create_Socket_Connect(&fd, host->hostName, host->recoveryPortNumber);
	if (status != STATUS_OK)
	{
		PGR_Set_Replication_Server_Status(host,DATA_ERR);
		return STATUS_ERROR;
	}

	memset(&packet,0,sizeof(packet));
	set_recovery_packet(&packet, packet_no );
	status = send_packet(&fd,&packet);

	close(fd);

	return status;
}

int
PGR_recovery_error_send(void)
{
	return direct_send_packet(RECOVERY_ERROR_ANS);
}

int
PGR_recovery_finish_send(void)
{
	return direct_send_packet(RECOVERY_FINISH);
}

int
PGR_recovery_queue_data_req(void)
{
	int status = STATUS_OK;
	int r_size = 0;
	int rtn = STATUS_OK;
	int fd = -1;
	ReplicateServerInfo * host = NULL;
	RecoveryPacket packet;

	host = PGR_get_replicate_server_info();
	if (host == NULL)
	{
		return STATUS_ERROR;
	}
	status = PGR_Create_Socket_Connect(&fd, host->hostName, host->recoveryPortNumber);
	if (status != STATUS_OK)
	{
		PGR_Set_Replication_Server_Status(host,DATA_ERR);
		PGR_Set_Cluster_Status(STATUS_REPLICATED);
		close(fd);
		return STATUS_ERROR;
	}

	memset(&packet,0,sizeof(packet));
	PGRset_recovery_packet_no(&packet, RECOVERY_QUEUE_DATA_REQ );
	status = send_packet(&fd,&packet);
	if (status != STATUS_OK)
	{
		status = stop_recovery_send(&fd,host);
		PGR_Set_Cluster_Status(STATUS_REPLICATED);
		close(fd);
		return STATUS_ERROR;
	}
	memset(&packet,0,sizeof(RecoveryPacket));
	r_size = read_packet(fd,&packet);
	if (r_size <= 0)
	{
		rtn =  STATUS_ERROR;
	}
	switch (ntohs(packet.packet_no))
	{
		case RECOVERY_QUEUE_DATA_ANS:
			rtn =  STATUS_OK;
			break;
		default:
			rtn =  STATUS_ERROR;
			break;
	}
	PGR_Set_Cluster_Status(STATUS_REPLICATED);
	close(fd);
	return rtn;
}

static int
rsync_pg_data(char * src, char * dest)
{
	int status;

	while ((status = flexible_rsync(src,dest, Rsync_Retry_times)) != STATUS_OK)
	{
		Rsync_Retry_times ++;
		if (Rsync_Retry_times > MAX_RETRY_TIMES)
		{
			break;
		}
	}
	return status;
}

static int
flexible_rsync(char * src, char * dest, int retry)
{
	int status;
	char *args[16];
	int pid, i = 0;
	char timeout[24];
	char bwlimit[24];
	char * target = NULL;

	args[i++] = "rsync";
	args[i++] = "-a";
	args[i++] = "-r";
	if (RsyncCompress)
		args[i++] = "-z";
	if (PGR_Rsync_Timeout > 0)
	{
		memset(timeout,0,sizeof(timeout));
		sprintf(timeout,"--timeout=%d",PGR_Rsync_Timeout);
		args[i++] = timeout;
	}

	if (retry == 0)
	{
		if (PGR_Rsync_Bwlimit > 0)
		{
			memset(bwlimit,0,sizeof(bwlimit));
			sprintf(bwlimit,"--bwlimit=%d", PGR_Rsync_Bwlimit);
			args[i++] = bwlimit;
fprintf(stderr,"specified bwlimit[%s]\n",bwlimit);
		}
	}
	else
	{
		int kbytes = 0;
		target = get_target_hostname(src);
		if (target != NULL)
		{
			kbytes = PGR_measure_bandwidth(target);
			fprintf(stderr,"\n  measured bandwidth is [%d] KBytes/sec,",kbytes);
			memset(bwlimit,0,sizeof(bwlimit));
			if (kbytes/retry > 0)
			{
				sprintf(bwlimit,"--bwlimit=%d",kbytes/retry);
				args[i++] = bwlimit;
				fprintf(stderr,"so set [%d] as a bwlimit.\n",kbytes/retry);
			}
			else
			{
				free(target);
				fprintf(stderr,"rsync might be failed, since current bandwidth is less than 1Kbyte/sec. please try again after a while.\n");
				return STATUS_ERROR;
			}
		}
	}
	args[i++] = "--delete";
	args[i++] = "-e";
	args[i++] = RsyncOption;
	args[i++] = src;
	args[i++] = dest;
	args[i++] = NULL;

	pid = fork();
	if (pid == 0)
	{
		status = execv(RsyncPath,args);
	}
	else
	{
		if (target != NULL)
		{
			free(target);
			target = NULL;
		}
		for (;;)
		{
			int result;
			result = wait(&status);
			if (result < 0)
			{
				if (errno == EINTR)
					continue;
				return STATUS_ERROR;
			}

			if (WIFEXITED(status) == 0 || WEXITSTATUS(status) != 0)
				return STATUS_ERROR;
			else
				break;
		}
	}
	if (target != NULL)
	{
		free(target);
		target = NULL;
	}
	return STATUS_OK;
}

static int
remove_dir(char * dir_name)
{
	DIR * dp = NULL;
	struct dirent *dirp = NULL;
	char fname[256];
	int status = 0;

	if ((dp = opendir(dir_name)) == NULL)
	{
		return STATUS_ERROR;
	}
	while ((dirp = readdir(dp)) != NULL)
	{
		if ((!strcmp(dirp->d_name,".")) ||
			(!strcmp(dirp->d_name,"..")))
		{
			continue;
		}
		sprintf(fname,"%s/%s",dir_name,dirp->d_name);
		status = remove(fname);
		if (status < 0)
		{
			remove_dir(fname);
		}
	}
	closedir(dp);
	if (remove(dir_name) < 0)
	{
		return STATUS_ERROR;
	}
	return STATUS_OK;
}

static int
clear_bkup_dir(char * dir_name)
{
	char bkp_dir[256];
	pid_t pid = getpid();

	sprintf(bkp_dir,"%s_%d",dir_name,pid);
	return (remove_dir(bkp_dir));
}

static int
bkup_dir(char * dir_name)
{
	int status;
	char org_dir[256];
	char bkp_dir[256];
	pid_t pid = getpid();

	sprintf(org_dir,"%s",dir_name);
	sprintf(bkp_dir,"%s_%d",dir_name,pid);
	status = rename(org_dir,bkp_dir);
	if (status < 0)
	{
		return STATUS_ERROR;
	}
	return STATUS_OK;
}

static int
restore_dir(char * dir_name)
{
	int status;
	char org_dir[256];
	char bkp_dir[256];
	pid_t pid = getpid();

	if (PGR_Recovery_Mode == PGR_WITHOUT_BACKUP)
	{
		return STATUS_OK;
	}
	sprintf(org_dir,"%s",dir_name);
	sprintf(bkp_dir,"%s_%d",dir_name,pid);
	status = rename(bkp_dir,org_dir);
	if (status < 0)
	{
		remove_dir(org_dir);
		status = rename(bkp_dir,org_dir);
		if (status < 0)
		{
			return STATUS_ERROR;
		}
	}
	return STATUS_OK;
}

static int
rsync_global_dir(char * src, char * dest, int stage)
{
	int status;
	char control_file[256];
	char org_dir[256];
	char src_dir[256];
	struct stat fstat;
	int cnt;

	sprintf(org_dir,"%s/global",dest);
	sprintf(control_file,"%s/global/pg_control",dest);
	if ((stage == PGR_1ST_RECOVERY) && (PGR_Recovery_Mode != PGR_WITHOUT_BACKUP))
	{
		if (bkup_dir(org_dir) != STATUS_OK)
		{
			return STATUS_ERROR;
		}
	}
	sprintf(src_dir,"%s/global",src);
	status = rsync_pg_data(src_dir, dest);
	if (status != STATUS_OK )
	{
		restore_dir(org_dir);
		return STATUS_ERROR;
	}
	/* check pg_control file */
	cnt = 0;
	while (stat(control_file, &fstat) < 0)
	{
		if (cnt > MAX_RETRY_TIMES )
		{
			restore_dir(org_dir);
			return STATUS_ERROR;
		}
		cnt ++;
		sleep(1);
	}
	if ((stage == PGR_2ND_RECOVERY) && (PGR_Recovery_Mode != PGR_WITHOUT_BACKUP))
	{
		clear_bkup_dir(org_dir);
	}
	return STATUS_OK;
}

static int
first_recovery(char * src, char * dest, char * dir)
{
	int status = STATUS_OK;
	char src_dir[256];
	char dest_dir[256];

	memset(src_dir,0,sizeof(src_dir));
	memset(dest_dir,0,sizeof(dest_dir));
	sprintf(src_dir,"%s/%s",src,dir);
	sprintf(dest_dir,"%s/%s",dest,dir);
	if (PGR_Recovery_Mode != PGR_WITHOUT_BACKUP)
	{
		status = bkup_dir(dest_dir);
		if (status < 0)
		{
			return STATUS_ERROR;
		}
	}
	status = rsync_pg_data(src_dir, dest);
	if (status != STATUS_OK )
	{
		restore_dir(dest_dir);
		return STATUS_ERROR;
	}
	return STATUS_OK;
}

static int
second_recovery(char * src, char * dest, char * dir)
{
	int status = STATUS_OK;
	char src_dir[256];
	char dest_dir[256];

	memset(src_dir,0,sizeof(src_dir));
	memset(dest_dir,0,sizeof(dest_dir));
	sprintf(src_dir,"%s/%s",src,dir);
	sprintf(dest_dir,"%s/%s",dest,dir);

	status = rsync_pg_data(src_dir, dest);
	if (status != STATUS_OK )
	{
		restore_dir(dest_dir);
		return STATUS_ERROR;
	}
	if (PGR_Recovery_Mode != PGR_WITHOUT_BACKUP)
	{
		clear_bkup_dir(dest_dir);
	}

	return STATUS_OK;
}

static int
recovery_rsync(char * src , char * dest, int stage)
{
	if ((src== NULL) || ( dest == NULL))
	{
		return STATUS_ERROR;
	}

	/* recovery step of "global" directory */
	fprintf(stderr,"%s recovery step of [global] directory...",
			((stage == 1)?"1st":"2nd"));
	if (rsync_global_dir(src, dest, stage) != STATUS_OK)
	{
		fprintf(stderr,"NG\n");
		return STATUS_ERROR;
	}
	fprintf(stderr,"OK\n");

	if (stage == PGR_1ST_RECOVERY)
	{
		/* 1st recovery step of "base" directory */
		fprintf(stderr,"1st recovery step of [base] directory...");
		if (first_recovery(src,dest,"base") != STATUS_OK)
		{
			fprintf(stderr,"NG\n");
			return STATUS_ERROR;
		}
		fprintf(stderr,"OK\n");

		fprintf(stderr,"1st recovery step of [pg_clog] directory...");
		/* 1st recovery step of "pg_clog" directory */
		if (first_recovery(src,dest,"pg_clog") != STATUS_OK)
		{
			fprintf(stderr,"NG\n");
			return STATUS_ERROR;
		}
		fprintf(stderr,"OK\n");

		/* 1st recovery step of "pg_xlog" directory */
		fprintf(stderr,"1st recovery step of [pg_xlog] directory...");
		if (first_recovery(src,dest,"pg_xlog") != STATUS_OK)
		{
			fprintf(stderr,"NG\n");
			return STATUS_ERROR;
		}
		fprintf(stderr,"OK\n");
	}
	else
	{
		/* 2nd recovery step of "base" directory */
		fprintf(stderr,"2nd recovery step of [base] directory...");
		if (second_recovery(src,dest,"base") != STATUS_OK)
		{
			fprintf(stderr,"NG\n");
			return STATUS_ERROR;
		}
		fprintf(stderr,"OK\n");

		/* 2nd recovery step of "pg_clog" directory */
		fprintf(stderr,"2nd recovery step of [pg_clog] directory...");
		if (second_recovery(src,dest,"pg_clog") != STATUS_OK)
		{
			fprintf(stderr,"NG\n");
			return STATUS_ERROR;
		}
		fprintf(stderr,"OK\n");

		/* 2nd recovery step of "pg_xlog" directory */
		fprintf(stderr,"2nd recovery step of [pg_xlog] directory...");
		if (second_recovery(src,dest,"pg_xlog") != STATUS_OK)
		{
			fprintf(stderr,"NG\n");
			return STATUS_ERROR;
		}
		fprintf(stderr,"OK\n");
	}

	return STATUS_OK;
}

static int
recovery_loop(int fd, int mode)
{

	int status = STATUS_OK;
	RecoveryPacket packet;
	int r_size = 0;
	int rtn = RECOVERY_LOOP_END;
	char src[256];
	bool need_sync_table_space = false;

	memset(&packet,0,sizeof(RecoveryPacket));
	r_size = read_packet(fd,&packet);
	if (r_size <= 0)
	{
		rtn = RECOVERY_LOOP_FAIL;
	}
	switch (ntohs(packet.packet_no))
	{
		case RECOVERY_PREPARE_ANS :
			/*
			 * get master information
			 */
			/*
			 * sync master data before recovery
			 */
			if (Debug_pretty_print)
			{
				elog(DEBUG1,"local host : %s  master:%s",Local_Host_Name,packet.hostName);
			}
			if (!strncmp(Local_Host_Name,packet.hostName,strlen(Local_Host_Name)))
			{
				strcpy(src,packet.pg_data);
				need_sync_table_space = false;
			}
			else
			{
				sprintf(src,"%s:%s",packet.hostName,packet.pg_data);
				need_sync_table_space = true;
			}
			if (PGR_Recovery_Mode == PGR_COLD_RECOVERY)
			{
				rtn = cold_recovery(src,&packet,need_sync_table_space,PGR_1ST_RECOVERY);
			}
			else
			{
				rtn = hot_recovery(&packet,PGR_1ST_RECOVERY);
			}
			if (rtn != STATUS_OK)
			{
				rtn = RECOVERY_LOOP_FAIL;
				break;
			}

			/*
			 * send recovery start request
			 */
			PGRset_recovery_packet_no(&packet, RECOVERY_START_REQ );
			status = send_packet(&fd,&packet);
			if (status != STATUS_OK)
			{
				fprintf(stderr,"RECOVERY_START_REQ send error\n");
				rtn = RECOVERY_LOOP_FAIL;
				break;
			}
			rtn = RECOVERY_LOOP_CONTINUE;
			break;
		case RECOVERY_START_ANS : 
			/*
			 * sync master data for recovery
			 */
			if (!strncmp(Local_Host_Name,packet.hostName,strlen(Local_Host_Name)))
			{
				strcpy(src,packet.pg_data);
				need_sync_table_space = false;
			}
			else
			{
				sprintf(src,"%s:%s",packet.hostName,packet.pg_data);
				need_sync_table_space = true;
			}
			if (PGR_Recovery_Mode == PGR_COLD_RECOVERY)
			{
				rtn = cold_recovery(src,&packet,need_sync_table_space,PGR_2ND_RECOVERY);
			}
			else
			{
				rtn = hot_recovery(&packet,PGR_2ND_RECOVERY);
			}

			if (rtn == STATUS_OK)
			{
				fprintf(stderr,"2nd recovery successed\n");
				Rsync_Retry_times = 0;
				if (mode == PGR_HOT_RECOVERY)
				{
					rtn = RECOVERY_LOOP_CONTINUE;
					/*
					 * send recovery queued data request
					 */
					PGRset_recovery_packet_no(&packet, RECOVERY_QUEUE_DATA_REQ );
					status = send_packet(&fd,&packet);
					if (status != STATUS_OK)
					{
						fprintf(stderr,"RECOVERY_QUEUE_DATA_REQ send error\n");
						rtn = RECOVERY_LOOP_FAIL;
						break;
					}
				}
				else
				{
					rtn = RECOVERY_LOOP_END;
				}
			}
			else
			{
				fprintf(stderr,"2nd hot recovery failed\n");
				rtn = RECOVERY_LOOP_FAIL;
			}
			break;
		case RECOVERY_QUEUE_DATA_ANS:
			rtn = RECOVERY_LOOP_END;
			break;
		case RECOVERY_ERROR_OCCUPIED:
			fprintf(stderr,"already in use for another recovery\n");
			rtn = RECOVERY_LOOP_FAIL;
			break;
		case RECOVERY_ERROR_CONNECTION:
			fprintf(stderr,"connection failed\n");
			rtn = RECOVERY_LOOP_FAIL;
			break;
		default:
			fprintf(stderr,"unknown packet received\n");
			rtn = RECOVERY_LOOP_FAIL;
			break;
	}

	return rtn;
}

int
PGR_Recovery_Main(int mode)
{
	int status;
	int fd = -1;
	int rtn;
	ReplicateServerInfo * host;

	memset(Local_Host_Name,0,sizeof(Local_Host_Name));
	gethostname(Local_Host_Name,sizeof(Local_Host_Name));
	PGR_Recovery_Mode = mode;

	status = STATUS_ERROR;

Retry_Start_Recovery:
	host = PGR_get_replicate_server_info();
	if (host == NULL)
	{
		if (Debug_pretty_print)
		{
			elog(DEBUG1,"not found replication server");
		}
		PGR_Set_Cluster_Status(STATUS_REPLICATED);
		return STATUS_ERROR;
	}

	PGR_Set_Cluster_Status(STATUS_RECOVERY);
	status = start_recovery_send(&fd,host);
	if (status != STATUS_OK)
	{
		PGR_Set_Replication_Server_Status(host,DATA_ERR);
		close(fd);
		if (Debug_pretty_print)
		{
			elog(DEBUG1,"start recovery packet send error");
		}
		goto Retry_Start_Recovery;
	}

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
			status = recovery_loop(fd, mode);
			if (status == RECOVERY_LOOP_CONTINUE)
			{
				continue;
			}
			else if (status == RECOVERY_LOOP_END)
			{
				close(fd);
				break;
			}
			else if (status == RECOVERY_LOOP_FAIL)
			{
				status = stop_recovery_send(&fd,host);
				PGR_Set_Cluster_Status(STATUS_REPLICATED);
				if (status != STATUS_OK)
				{
					close(fd);
					return STATUS_ERROR;
				}
				close(fd);
				return STATUS_ERROR;
			}
			else 
			{
			    close(fd);
				PGR_Set_Cluster_Status(STATUS_REPLICATED);
			    return STATUS_ERROR;
			}
		}
	}
	PGR_Set_Cluster_Status(STATUS_REPLICATED);
	return STATUS_OK;
}

static void
show_recovery_packet(RecoveryPacket * packet)
{

	if (Debug_pretty_print)
	{
		elog(DEBUG1,"no = %d",ntohs(packet->packet_no));
		elog(DEBUG1,"max_connect = %d",ntohs(packet->max_connect));
		elog(DEBUG1,"port = %d",ntohs(packet->port));
		elog(DEBUG1,"recoveryPort = %d",ntohs(packet->recoveryPort));
		if (packet->hostName != NULL)
			elog(DEBUG1,"hostName = %s",packet->hostName);
		if (packet->pg_data != NULL)
			elog(DEBUG1,"pg_data = %s",packet->pg_data);
	}
}

static void
set_recovery_packet(RecoveryPacket * packet, int packet_no)
{
	struct passwd * pw = NULL;

	if (packet == NULL)
	{
		return;
	}
	PGRset_recovery_packet_no(packet, packet_no );
	packet->max_connect = htons(MaxBackends);
	packet->port = htons(PostPortNumber);
	packet->recoveryPort = htons(RecoveryPortNumber);
	gethostname(packet->hostName,sizeof(packet->hostName));
	memcpy(packet->pg_data,DataDir,sizeof(packet->pg_data));
	memset(packet->userName,0,sizeof(packet->userName));
	if ((pw = getpwuid(geteuid())) != NULL)
	{
		strncpy(packet->userName,pw->pw_name,sizeof(packet->userName));
	}
	else
	{
		cuserid(packet->userName);
	}
}

static int
sync_table_space(char * hostName, uint16_t portNum, char * userName, int stage)
{
	PGresult * res = (PGresult *)NULL;
	int i = 0;
	int row_num = 0;
	char * location = NULL;
	int rtn = STATUS_OK;

	res = get_table_space_location(hostName, portNum, userName);
	if (res == (PGresult *)NULL)
	{
		return STATUS_ERROR;
	}
	row_num = PQntuples(res);
	for ( i = 0 ; i < row_num ; i ++)
	{
		location = PQgetvalue(res,i,0);
		if (strlen(location) > 0 )
		{
			fprintf(stderr,"sync tablespace[%s]...",location);
			rtn = rsync_table_space(hostName, location, stage);
			fprintf(stderr,"%s\n", (rtn == STATUS_OK)?"OK":"NG");
		}
	}
	if (res != (PGresult *)NULL)
	{
		PQclear(res);
	}

	return STATUS_OK;
}

static PGresult *
get_table_space_location(char * hostName, uint16_t portNum, char * userName)
{
	PGresult * res = (PGresult *)NULL;
	int cnt = 0;
	PGconn * conn = (PGconn *)NULL;
	char port[8];
	char *database = "template1";
	char * query = "select spclocation from pg_tablespace where spcname not like 'pg_%'";

	if ( (hostName == NULL) ||
		(portNum <= 0)      ||
		(userName == NULL))
	{
		return (PGresult *)NULL;
	}
	snprintf(port,sizeof(port),"%d", portNum);

	/* create connection to master */
	conn = PQsetdbLogin(hostName, port, NULL, NULL, database, userName, NULL);
	if (conn == NULL)
	{
		return (PGresult *)NULL;
	}
	/* check to see that the backend Connection was successfully made */
	cnt = 0;
	while (PQstatus(conn) == CONNECTION_BAD)
	{
		if (conn != NULL)
		{
			PQfinish(conn);
		}
		if (cnt > MAX_RETRY_TIMES )
		{
			return (PGresult *)NULL;
		}
		conn = PQsetdbLogin(hostName, port, NULL, NULL, database, userName, NULL);
		cnt ++;
	}
	res = PQexec(conn , query);
	if ((res == NULL) ||
		(PQresultStatus(res) != PGRES_TUPLES_OK))
	{
		PQclear(res);
		res = (PGresult *)NULL;
	}
	if (conn != NULL)
	{
		PQfinish(conn);
	}

	return res;
}

static int
rsync_table_space(char * hostName, char * location, int stage)
{
	int status = STATUS_OK;
	char src_dir[256];
	char dest_dir[256];
	struct stat fstat;
	int cnt = 0;

	sprintf(src_dir,"%s:%s",hostName,location);
	strncpy(dest_dir,location,sizeof(dest_dir));

	if ((stage == PGR_1ST_RECOVERY) && (PGR_Recovery_Mode != PGR_WITHOUT_BACKUP))
	{
		status = bkup_dir(location);
	}
	status = rsync_pg_data(src_dir, dest_dir);
	if (status != STATUS_OK )
	{
		restore_dir(location);
		return STATUS_ERROR;
	}
	/* check file status */
	cnt = 0;
	while (stat(location,&fstat) < 0)
	{
		if (cnt > MAX_RETRY_TIMES )
		{
			restore_dir(location);
			return STATUS_ERROR;
		}
		cnt ++;
		sleep(1);
	}
	if ((stage == PGR_2ND_RECOVERY) && (PGR_Recovery_Mode != PGR_WITHOUT_BACKUP))
	{
		clear_bkup_dir(location);
	}
	return STATUS_OK;
}

static int
cold_recovery(char * src, RecoveryPacket *packet, bool need_sync_table_space, int stage)
{
	int status = STATUS_OK;

	status = recovery_rsync(src,DataDir,stage);
	if (status != STATUS_OK)
	{
		if (Debug_pretty_print)
		{
			elog(DEBUG1,"%s rsync error",
				((stage == PGR_1ST_RECOVERY)?"1st":"2nd"));
		}
		return STATUS_ERROR;
	}
	if (need_sync_table_space == true)
	{
		status = sync_table_space(packet->hostName, ntohs(packet->port), packet->userName, stage);
		fprintf(stderr,"%s sync_table_space ",
			((stage == PGR_1ST_RECOVERY)?"1st":"2nd"));
		if (status != STATUS_OK)
		{
			if (Debug_pretty_print)
			{
				elog(DEBUG1,"%s sync table space error",
					((stage == PGR_1ST_RECOVERY)?"1st":"2nd"));
			}
			fprintf(stderr,"NG\n");
			return STATUS_ERROR;
		}
		fprintf(stderr,"OK\n");
	}
	return STATUS_OK;
}

static int
hot_recovery(RecoveryPacket *packet, int stage)
{
	int status = STATUS_OK;

	fprintf(stderr,"%s restore from pg_dump ",
		((stage == PGR_1ST_RECOVERY)?"1st":"2nd"));
	if (stage == PGR_1ST_RECOVERY)
	{
		status = restore_from_dumpall(packet->hostName, ntohs(packet->port), packet->userName );
	}
	else
	{
		status = restore_from_each_dump(packet->hostName, ntohs(packet->port), packet->userName );
	}
	if (status != STATUS_OK)
	{
		if (Debug_pretty_print)
		{
			elog(DEBUG1,"%s sync table space error",
				((stage == PGR_1ST_RECOVERY)?"1st":"2nd"));
		}
		fprintf(stderr,"->NG\n");
		return STATUS_ERROR;
	}
	fprintf(stderr,"->OK\n");
	return STATUS_OK;
}

static int
restore_from_dumpall( char * hostName, uint16_t portNum, char * userName)
{
	int status;
	char exec_command[512];
	int pid;
	char pg_dumpall[256];
	char psql[256];
	char *p=NULL;

	/* set pg_dumpall path */
	memset(pg_dumpall, 0, sizeof(pg_dumpall));
	strncpy(pg_dumpall, PgDumpPath, sizeof(pg_dumpall));
	p = strrchr(pg_dumpall,'/');
	if (p == NULL)
	{
		return STATUS_ERROR;
	}
	p++;
	strcpy(p,"pg_dumpall");

	/* set psql path */
	p = NULL;
	memset(psql, 0, sizeof(psql));
	strncpy(psql, PgDumpPath, sizeof(psql));
	p = strrchr(psql,'/');
	if (p == NULL)
	{
		return STATUS_ERROR;
	}
	p++;
	strcpy(p,"psql");
	p+=4;
	*p = '\0';

	snprintf(exec_command,sizeof(exec_command),"%s -i -o -c -h %s -p %d -U %s | %s -p %d template1",
		pg_dumpall,
		hostName,
		portNum,
		userName,
		psql,
		PostPortNumber
	);
	fprintf(stderr,"1st exec:[%s]\n",exec_command);

	pid = fork();
	if (pid == 0)
	{
		system(exec_command);
		exit(0);
	}
	else
	{
		for (;;)
		{
			int result;
			result = wait(&status);
			if (result < 0)
			{
				if (errno == EINTR)
					continue;
				return STATUS_ERROR;
			}

			if (WIFEXITED(status) == 0 || WEXITSTATUS(status) != 0)
				return STATUS_ERROR;
			else
				break;
		}
	}
	return STATUS_OK;
}

static int
restore_from_dump( char * hostName, uint16_t portNum, char * userName, char * dbName)
{
	int status;
	char exec_command[512];
	int pid= 0;
	char pg_restore[256];
	char *p=NULL;

	/* set pq_restore path */
	p = NULL;
	memset(pg_restore, 0, sizeof(pg_restore));
	strncpy(pg_restore, PgDumpPath, sizeof(pg_restore));
	p = strrchr(pg_restore,'/');
	if (p == NULL)
	{
		return STATUS_ERROR;
	}
	p++;
	strcpy(p,"pg_restore");

	snprintf(exec_command,sizeof(exec_command),"%s -i -Fc -o -b -h %s -p %d -U %s %s | %s -i -c -p %d -d %s",
		PgDumpPath,
		hostName,
		portNum,
		userName,
		dbName,
		pg_restore,
		PostPortNumber,
		dbName
	);

	fprintf(stderr,"2nd exec:[%s]\n",exec_command);
	pid = fork();
	if (pid == 0)
	{
		system(exec_command);
		exit(0);
	}
	else
	{
		for (;;)
		{
			int result;
			result = wait(&status);
			if (result < 0)
			{
				if (errno == EINTR)
					continue;
				return STATUS_ERROR;
			}

			if (WIFEXITED(status) == 0 || WEXITSTATUS(status) != 0)
				return STATUS_ERROR;
			else
				break;
		}
	}
	return STATUS_OK;
}

static int
restore_from_each_dump( char * hostName, uint16_t portNum, char * userName)
{
	PGresult * res = (PGresult *)NULL;
	int i = 0;
	int row_num = 0;
	char * dbName = NULL;
	int rtn = STATUS_OK;

	res = get_dbName(hostName, portNum, userName);
	if (res == (PGresult *)NULL)
	{
		return STATUS_ERROR;
	}
	row_num = PQntuples(res);
	for ( i = 0 ; i < row_num ; i ++)
	{
		dbName = PQgetvalue(res,i,0);
		if (strlen(dbName) > 0 )
		{
			if ((strcmp("template0",dbName)) &&
				(strcmp("template1",dbName)))
			{
				rtn = restore_from_dump(hostName, portNum, userName, dbName);
				fprintf(stderr,".");
			}
		}
	}
	if (res != (PGresult *)NULL)
	{
		PQclear(res);
	}

	return STATUS_OK;
}

static PGresult *
get_dbName(char * hostName, uint16_t portNum, char * userName)
{
	PGresult * res = (PGresult *)NULL;
	int cnt = 0;
	PGconn * conn = (PGconn *)NULL;
	char port[8];
	char *database = "template1";
	char * query = "SELECT datname FROM pg_database";

	if ( (hostName == NULL) ||
		(portNum <= 0)      ||
		(userName == NULL))
	{
		return (PGresult *)NULL;
	}
	snprintf(port,sizeof(port),"%d", portNum);

	/* create connection to master */
	conn = PQsetdbLogin(hostName, port, NULL, NULL, database, userName, NULL);
	if (conn == NULL)
	{
		return (PGresult *)NULL;
	}
	/* check to see that the backend Connection was successfully made */
	cnt = 0;
	while (PQstatus(conn) == CONNECTION_BAD)
	{
		if (conn != NULL)
		{
			PQfinish(conn);
		}
		if (cnt > MAX_RETRY_TIMES )
		{
			return (PGresult *)NULL;
		}
		conn = PQsetdbLogin(hostName, port, NULL, NULL, database, userName, NULL);
		cnt ++;
	}
	res = PQexec(conn , query);
	if ((res == NULL) ||
		(PQresultStatus(res) != PGRES_TUPLES_OK))
	{
		PQclear(res);
		res = (PGresult *)NULL;
	}
	if (conn != NULL)
	{
		PQfinish(conn);
	}

	return res;
}

int
PGR_measure_bandwidth(char * target)
{
	double msec;
	double kbytes;
	char * ptr;

	ptr = ping_to_target(target);
	if (ptr != NULL)
	{
		msec = get_ping_result(ptr);
		if (msec <= 0)
		{
			fprintf(stderr,"band measurement failed between %s\n",target);
			return STATUS_ERROR;
		}
		kbytes = calculate_band(msec);
	}
	else
	{
		fprintf(stderr,"band measurement failed between %s\n",target);
		return STATUS_ERROR;
	}
	return (int)kbytes;
}

static char *
ping_to_target(char * target)
{
	int pfd[2];
	int status;
	char * args[8];
	int pid, i = 0;
	int r_size = 0;

	memset(Ping_Result,0,sizeof(Ping_Result));

	if (pipe(pfd) == -1)
	{
		fprintf(stderr,"pipe open error:%s\n",strerror(errno));
		return NULL;
	}

	args[i++] = "ping";
	args[i++] = "-q";
	args[i++] = "-c3";
	args[i++] = target;
	args[i++] = NULL;

	pid = fork();
	if (pid == 0)
	{
		close(STDOUT_FILENO);
		dup2(pfd[1], STDOUT_FILENO);
		close(pfd[0]);
		status = execv(PingPath,args);
		exit(0);
	}
	else
	{
		close(pfd[1]);
		for (;;)
		{
			int result;
			result = wait(&status);
			if (result < 0)
			{
				if (errno == EINTR)
					continue;
				return NULL;
			}

			if (WIFEXITED(status) == 0 || WEXITSTATUS(status) != 0)
				return NULL;
			else
				break;
		}
		i = 0;
		while  (( (r_size = read (pfd[0], &Ping_Result[i], sizeof(Ping_Result)-i)) > 0) && (errno == EINTR))
		{
			i += r_size;
		}
		close(pfd[0]);
	}
	return Ping_Result;
}

static double
get_ping_result (char * ping_data)
{
	char * sp = NULL;
	char * ep = NULL;
	int i;
	double msec = 0;

	if (ping_data == NULL)
	{
		return STATUS_ERROR;
	}
	/*
	 skip result until average data
	 tipical result of ping is as follows,
	 "rtt min/avg/max/mdev = 0.045/0.045/0.046/0.006 ms"
	 we can find the average data beyond the 4th '/'.
	 */
	sp = ping_data;
	for ( i = 0 ; i < 4 ; i ++)
	{
		sp = strchr(sp,'/');	
		if (sp == NULL)
		{
			return STATUS_ERROR;
		}
		sp ++;
	}
	ep = strchr (sp,'/');
	if (ep == NULL)
	{
		return STATUS_ERROR;
	}
	*ep = '\0';
	errno = 0;
	/* convert to numeric data from text */
	msec = strtod(sp,(char **)NULL);
	if (errno != 0)
	{
		return STATUS_ERROR;
	}
	return msec;
}

static double
calculate_band(double msec)
{
	double bw;

	if (msec <= 0)
	{
		return STATUS_ERROR;
	}
	/* default ping packet size is 64 byte */
	bw = 64 * 2  / msec / 1.024 ;
	return bw;
}

static char *
get_target_hostname(char * src)
{
	char * p = NULL;
	char * target = NULL;
	if (src == NULL)
	{
		return NULL;
	}
	p = strchr(src,':');
	if ( p == NULL)
	{
		target = strdup(Local_Host_Name);
	}
	else
	{
		target = strdup(src);
		p = strchr(target,':');
		*p = '\0';
	}
	return target;	
}

#endif /* USE_REPLICATION */
