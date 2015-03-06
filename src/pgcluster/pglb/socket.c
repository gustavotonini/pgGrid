/*--------------------------------------------------------------------
 * FILE:
 *     socket.c
 *
 * NOTE:
 *     This file is composed of the communication modules
 *
 * Portions Copyright (c) 2003-2008, Atsushi Mitani
 * Portions Copyright (c) 2003-2007	PgPool Global Development Group
 *--------------------------------------------------------------------
 * pglb is based on pgpool.
 * pgpool: a language independent connection pool server for PostgreSQL 
 * written by Tatsuo Ishii
 *
 * Permission to use, copy, modify, and distribute this software and
 * its documentation for any purpose and without fee is hereby
 * granted, provided that the above copyright notice appear in all
 * copies and that both that copyright notice and this permission
 * notice appear in supporting documentation, and that the name of the
 * author not be used in advertising or publicity pertaining to
 * distribution of the software without specific, written prior
 * permission. The author makes no representations about the
 * suitability of this software for any purpose.  It is provided "as
 * is" without express or implied warranty.
 *
*/
#include "postgres.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/wait.h>
#include <ctype.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/ipc.h>
#include <netdb.h>
#include <errno.h>
#include <fcntl.h>
#include <time.h>
#include <sys/param.h>
#include <sys/file.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#ifdef HAVE_SYS_SELECT_H
#include <sys/select.h>
#endif

#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

#include "replicate_com.h"
#include "pglb.h"


/*--------------------------------------
 * PROTOTYPE DECLARATION
 *--------------------------------------
 */
int PGRcreate_unix_domain_socket(char * sock_dir, unsigned short port);
int PGRcreate_recv_socket(char * hostName , unsigned short portNumber);
int PGRcreate_acception(int fd, char * hostName , unsigned short portNumber);
void PGRclose_sock(int * sock);
int PGRread_byte(int sock,char * buf,int len, int flag);
int PGRcreate_cluster_socket( int * sock, ClusterTbl * ptr );

static int create_send_socket(int * fdP, char * hostName , unsigned short portNumber);


/*
* create UNIX domain socket
*/
int 
PGRcreate_unix_domain_socket(char * sock_dir, unsigned short port)
{
	char * func = "PGRcreate_unix_domain_socket()";
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
	snprintf(addr.sun_path, sizeof(addr.sun_path), "%s/.s.PGSQL.%d",sock_dir,port);
	len = sizeof(struct sockaddr_un);
	status = bind(fd, (struct sockaddr *)&addr, len);
	if (status == -1)
	{
		show_error("%s: bind() failed. reason: %s", func, strerror(errno));
		return -1;
	}

	if (chmod(addr.sun_path, 0777) == -1)
	{
		show_error("%s: chmod() failed. reason: %s", func, strerror(errno));
		return -1;
	}

	status = listen(fd, PGLB_MAX_SOCKET_QUEUE);
	if (status < 0)
	{
		show_error("%s: listen() failed. reason: %s", func, strerror(errno));
		return -1;
	}
	return fd;
}

int
PGRcreate_recv_socket(char * hostName , unsigned short portNumber)
{
	char * func = "PGRcreate_recv_socket()";
	int fd,err;
	size_t	len = 0;
	struct sockaddr_in addr;
	int one = 1;

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		show_error("%s: socket() failed. (%s)", func, strerror(errno));
		return -1;
	}
	if ((setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one))) == -1)
	{
		PGRclose_sock(&fd);
		show_error("%s: setsockopt() failed. (%s)",func, strerror(errno));
		return -1;
	}
	addr.sin_family = AF_INET;
	if ((hostName == NULL) || (hostName[0] == '\0'))
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	else
	{
		struct hostent *hp;

		hp = gethostbyname(hostName);
		if ((hp == NULL) || (hp->h_addrtype != AF_INET))
		{
			PGRclose_sock(&fd);
			return -1;
		}
		memmove((char *) &(addr.sin_addr), (char *) hp->h_addr, hp->h_length);
	}

	addr.sin_port = htons(portNumber);
	len = sizeof(struct sockaddr_in);
	
	err = bind(fd, (struct sockaddr *) & addr, len);
	if (err < 0)
	{
		PGRclose_sock(&fd);
		show_error("%s: bind() failed. (%s)",func, strerror(errno));
		return -1;
	}
	err = listen(fd, PGLB_MAX_SOCKET_QUEUE);
	if (err < 0)
	{
		PGRclose_sock(&fd);
		show_error("%s: listen() failed. (%s)", func, strerror(errno));
		return -1;
	}
	return	fd;
}

int
PGRcreate_acception(int fd, char * hostName , unsigned short portNumber)
{
	char * func = "PGRcreate_acception()";
	int sock;
	struct sockaddr  addr;
	size_t	len = 0;
	int one = 1;
	int count;

	len = sizeof(struct sockaddr);
	count = 0;
	while ((sock = accept(fd,&addr,&len)) < 0)
	{
		show_error("%s:accept error",func);
		PGRclose_sock(&fd);
		if ( count > PGLB_CONNECT_RETRY_TIME)
		{
			return -1;
		}
		fd = PGRcreate_recv_socket(hostName , portNumber);
		count ++;
	}
	
	count = 0;
	while (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof(one)) < 0)
	{
		show_error("%s: setsockopt TCP_NODELAY error (%s)",func, strerror(errno));
		if ( count > PGLB_CONNECT_RETRY_TIME)
		{
			return -1;
		}
		count ++;
	}
	count = 0;
	while (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *) &one, sizeof(one)) < 0)
	{
		show_error("%s:setsockopt SO_KEEPALIVE error (%s)",func,strerror(errno));
		if ( count > PGLB_CONNECT_RETRY_TIME)
		{
			return -1;
		}
		count ++;
	}

	return	sock;
}

void
PGRclose_sock(int * sock)
{
	close(*sock);
	*sock = -1;
}

int
PGRread_byte(int sock,char * buf,int len, int flag)
{
	char * func = "PGRread_byte()";
	int r;
	char * read_ptr;
	int read_size = 0;
	int max_buf_size ;
	int pid;

	pid = getpid();
	max_buf_size = len;
	read_ptr = (char*)buf;
	for (;;)
	{
		r = recv(sock,read_ptr + read_size ,max_buf_size - read_size, flag);
		if (r < 0)
		{
			if (errno == EINTR)
			{
				continue;
			}
#ifdef EAGAIN
			if (errno == EAGAIN)
			{
				return read_size;
			}
#endif
#if defined(EWOULDBLOCK) && (!defined(EAGAIN) || (EWOULDBLOCK != EAGAIN))
			if (errno == EWOULDBLOCK)
			{
				show_error("%s:no data (%s)",func,strerror(errno));
				return read_size;
			}
#endif
#ifdef ECONNRESET
			if (errno == ECONNRESET)
			{
				PGRclose_sock(&sock);
				show_error("%s:connection reset (%s)",func, strerror(errno));
				return -1;
			}
#endif
			show_error("%s:recv() failed. (%s)",func,strerror(errno));
			read_size = -1;
			break;
		}
		if (r > 0)
		{
			read_size += r;
			if (max_buf_size == read_size)
			{
				break;
			}
			break;
		}
		if (read_size)
		{
			return read_size;
		}
		else
		{
			return -1;
		}
	}

	return read_size;
}

int
PGRcreate_cluster_socket( int * sock, ClusterTbl * ptr )
{
	char * func = "PGRcreate_cluster_socket()";
	int status = STATUS_ERROR;

	/*
	if (PGRis_connection_full(ptr) == 1)
	{
		return STATUS_ERROR;
	}
	*/
	if (ptr != (ClusterTbl *) NULL)
	{
		status = create_send_socket(sock, ptr->hostName, ptr->port)	;
	}
	else
	{
		show_error("%s:ClusterTbl is not initialize",func);
	}
	return status;
}

static int
create_send_socket(int * fdP, char * hostName , unsigned short portNumber)
{
	char * func = "create_send_socket()";
	int sock;
	size_t	len = 0;
	struct sockaddr_in addr;
	int fd;
	int one = 1;

#ifdef PRINT_DEBUG
	show_debug("%s: host:%s port:%d",func, hostName,portNumber);
#endif			

	memset((char *)&addr,0,sizeof(addr));

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		* fdP = -1;
		
		show_error("%s:socket() failed. (%s)",func, strerror(errno));
		return STATUS_ERROR;
	}
	if ((setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one))) == -1)
	{
		PGRclose_sock(&fd);
		* fdP = -1;
		show_error("%s:setsockopt() failed. (%s)", func, strerror(errno));
		return STATUS_ERROR;
		return STATUS_ERROR;
	}
	if ((setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, (char *) &one, sizeof(one))) == -1)
	{
		PGRclose_sock(&fd);
		* fdP = -1;
		show_error("%s:setsockopt() failed. (%s)", func, strerror(errno));
		return STATUS_ERROR;
	}
	if (setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof(one)) < 0)
	{
		PGRclose_sock(&fd);
		* fdP = -1;
		show_error("%s:setsockopt() failed. (%s)",func, strerror(errno));
		return STATUS_ERROR;
	}
	
	addr.sin_family = AF_INET;
	if ((hostName == NULL) || (hostName[0] == '\0'))
   		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	else
	{
		struct hostent *hp;

		hp = gethostbyname(hostName);
		if ((hp == NULL) || (hp->h_addrtype != AF_INET))
		{
			PGRclose_sock(&fd);
			* fdP = -1;
			return STATUS_ERROR;
		}
		memmove((char *) &(addr.sin_addr), (char *) hp->h_addr, hp->h_length);
	}

	addr.sin_port = htons(portNumber);
	len = sizeof(struct sockaddr_in);
	
	if ((sock = connect(fd,(struct sockaddr*)&addr,len)) < 0)
	{
		PGRclose_sock(&fd);
		* fdP = -1;
		return STATUS_ERROR;
	}
	
	* fdP = fd;
	return	STATUS_OK;
}

