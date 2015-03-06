/*--------------------------------------------------------------------
 * FILE:
 *     child.c
 *
 * NOTE:
 *     This file is composed of the functions to call with the source
 *     at child process of pglb.
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
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <sys/un.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/tcp.h>

#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/time.h>
#include <fcntl.h>

#include "replicate_com.h"
#include "pglb.h"

#ifdef NONE_BLOCK
static void set_nonblock(int fd);
static void unset_nonblock(int fd);
#endif

static POOL_CONNECTION *do_accept(int unix_fd, int inet_fd, struct timeval *timeout);
static StartupPacket *read_startup_packet(POOL_CONNECTION *cp);
static int send_startup_packet(POOL_CONNECTION_POOL_SLOT *cp);
static POOL_CONNECTION_POOL *connect_backend(StartupPacket *sp, POOL_CONNECTION *frontend);
static void cancel_request(CancelPacket *sp, int secondary_backend);
static RETSIGTYPE die(int sig);
static RETSIGTYPE close_idle_connection(int sig);
static RETSIGTYPE close_child_connection(int sig);
static int send_params(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend);
static void send_frontend_exits(void);
static void do_pooling_child(int sig);
static void return_with_error (char *msg);

/*
 * non 0 means SIGTERM(smart shutdown) or SIGINT(fast shutdown) has arrived
 */
static int exit_request;

static int idle;		/* non 0 means this child is in idle state */

extern int myargc;
extern char **myargv;

char remote_ps_data[NI_MAXHOST];		/* used for set_ps_display */

/*
* child main loop
*/
int
PGRdo_child(int use_pool)
{
	char * func = "PGRdo_child()";
	pid_t pid = 0;
	POOL_CONNECTION *frontend;
	POOL_CONNECTION_POOL *backend;
	struct timeval now;
	struct timezone tz;
	int child_idle_sec;
	struct timeval timeout;
	static int connected;
	int connections_count = 0;	/* used if child_max_connections > 0 */
	int first_ready_for_query_received;		/* for master/slave mode */
	int found;
	char psbuf[NI_MAXHOST + 128];

	int unix_fd = Frontend_FD.unix_fd;
	int inet_fd = Frontend_FD.inet_fd;
	POOL_STATUS status;

	pid = getpid();
	show_debug("I am %d", pid);

	/* Identify myself via ps */
	init_ps_display("", "", "", "");

	/* set up signal handlers */
	PGRsignal(SIGALRM, SIG_DFL);
	PGRsignal(SIGTERM, die);
	PGRsignal(SIGINT, die);
	PGRsignal(SIGHUP, close_idle_connection);
	PGRsignal(SIGQUIT, die);
	PGRsignal(SIGCHLD, SIG_DFL);
	PGRsignal(SIGUSR1, SIG_IGN);
	PGRsignal(SIGUSR2, close_child_connection);
	PGRsignal(SIGPIPE, SIG_IGN);

#ifdef NONE_BLOCK
	/* set listen fds to none block */
	set_nonblock(unix_fd);
	if (inet_fd)
	{
		set_nonblock(inet_fd);
	}
#endif

	/* initialize random seed */
	gettimeofday(&now, &tz);
	srandom((unsigned int) now.tv_usec);

	/* initialize connection pool */
	if (pool_init_cp())
	{
		PGRset_status_to_child_tbl(pid,TBL_FREE);
		return STATUS_ERROR;
	}

	child_idle_sec = 0;

	timeout.tv_sec = pool_config_child_life_time;
	timeout.tv_usec = 0;

	init_prepared_list();

	for (;;)
	{
		int connection_reuse = 1;
		int ssl_request = 0;
		StartupPacket *sp;

		/* pgpool stop request already sent? */
		if (exit_request)
		{
			PGRset_status_to_child_tbl(pid,TBL_FREE);
			send_frontend_exits();
			/*die(0);*/
			return STATUS_OK;
		}

		idle = 1;

		/* perform accept() */
		frontend = do_accept(unix_fd, inet_fd, &timeout);

		if (frontend == NULL)	/* connection request from frontend timed out */
		{
			/* check select() timeout */
			if (connected && pool_config_child_life_time > 0 &&
				timeout.tv_sec == 0 && timeout.tv_usec == 0)
			{
				show_debug("%s:child life %d seconds expired",func, pool_config_child_life_time);
				PGRset_status_to_child_tbl(pid,TBL_FREE);
				send_frontend_exits();
				return STATUS_ERROR;
			}
			if (use_pool == USE_CONNECTION_POOL )
			{
				continue;
			}
			else
			{
				PGRset_status_to_child_tbl(pid,TBL_FREE);
				return STATUS_ERROR;
			}
		}

		/* set frontend fd to blocking */
		unset_nonblock(frontend->fd);

#ifdef NOT_USED
		set_nonblock(frontend->fd);
#endif

		/* set busy flag and clear child idle timer */
		idle = 0;
		child_idle_sec = 0;

		/* check backend timer is expired */
		if (backend_timer_expired)
		{
			pool_backend_timer();
			backend_timer_expired = 0;
		}

		/* disable timeout */
		pool_disable_timeout();

		/* read the startup packet */
	retry_startup:
		sp = read_startup_packet(frontend);
		if (sp == NULL)
		{
			/* failed to read the startup packet. return to the accept() loop */
			pool_close(frontend);
			if (use_pool == USE_CONNECTION_POOL )
			{
				continue;
			}
			else
			{
				PGRset_status_to_child_tbl(pid,TBL_FREE);
				return STATUS_ERROR;
			}
		}

		PGRset_status_to_child_tbl(pid,TBL_ACCEPT);

		/* cancel request? */
		if (sp->major == 1234 && sp->minor == 5678)
		{
			cancel_request((CancelPacket *)sp->startup_packet, 0);
			if (DUAL_MODE)
				cancel_request((CancelPacket *)sp->startup_packet, 1);
			pool_close(frontend);
			pool_free_startup_packet(sp);
			if (use_pool == USE_CONNECTION_POOL )
			{
				continue;
			}
			else
			{
				PGRset_status_to_child_tbl(pid,TBL_FREE);
				return STATUS_ERROR;
			}
		}

		/* SSL? */
		if (sp->major == 1234 && sp->minor == 5679)
		{
			/* SSL not supported */
			show_debug("%s: SSLRequest: sent N; retry startup",func);
			if (ssl_request)
			{
				pool_close(frontend);
				pool_free_startup_packet(sp);
				PGRset_status_to_child_tbl(pid,TBL_FREE);
				if (use_pool == USE_CONNECTION_POOL )
				{
					continue;
				}
				else
				{
					return STATUS_ERROR;
				}
			}

			/*
			 * say to the frontend "we do not suppport SSL"
			 * note that this is not a NOTICE response despite it's an 'N'!
			 */
			pool_write_and_flush(frontend, "N", 1);
			ssl_request = 1;
			pool_free_startup_packet(sp);
			PGRset_status_to_child_tbl(pid,TBL_FREE);
			if (use_pool == USE_CONNECTION_POOL )
			{
				goto retry_startup;
			}
			else
			{
				return STATUS_ERROR;
			}
		}
#if 0
		if (pool_config_enable_pool_hba)
		{
			/*
			 * do client authentication.
			 * Note that ClientAuthentication does not return if frontend
			 * was rejected; it simply terminates this process.
			 */
			frontend->protoVersion = sp->major;
			frontend->database = strdup(sp->database);
			if (frontend->database == NULL)
			{
				show_error("do_child: strdup failed: %s\n", strerror(errno));
				PGRset_status_to_child_tbl(pid,TBL_FREE);
				return STATUS_ERROR;
			}
			frontend->username = strdup(sp->user);
			if (frontend->username == NULL)
			{
				show_error("do_child: strdup failed: %s\n", strerror(errno));
				PGRset_status_to_child_tbl(pid,TBL_FREE);
				return STATUS_ERROR;
			}
			ClientAuthentication(frontend);
		}
#endif
		/*
		 * Ok, negotiaton with frontend has been done. Let's go to the next step.
		 */

		/*
		 * if there's no connection associated with user and database,
		 * we need to connect to the backend and send the startup packet.
		 */

		first_ready_for_query_received = 0;		/* for master/slave mode */

		/* look for existing connection */
		found = 0;
		backend = pool_get_cp(sp->user, sp->database, sp->major, 1);

		if (backend != NULL)
		{
			found = 1;

			/* existing connection associated with same user/database/major found.
			 * however we should make sure that the startup packet contents identical.
			 * OPTION data and others might be different.
			 */
			if (sp->len != backend->slots[0]->sp->len)
			{
				show_debug("%s:pool_process_query: connection exists but startup packet length is not identical",func);
				found = 0;
			}
			else if(memcmp(sp->startup_packet, backend->slots[0]->sp->startup_packet, sp->len) != 0)
			{
				show_debug("pool_process_query: connection exists but startup packet contents is not identical");
				found = 0;
			}

			if (found == 0)
			{
				/* we need to discard existing connection since startup packet is different */
				pool_discard_cp(sp->user, sp->database, sp->major);
				backend = NULL;
			}
		}

		if (backend == NULL)
		{
			/* create a new connection to backend */
			connection_reuse = 0;

			if ((backend = connect_backend(sp, frontend)) == NULL)
			{
				if (use_pool == USE_CONNECTION_POOL )
				{
					continue;
				}
				else
				{
					PGRset_status_to_child_tbl(pid,TBL_FREE);
					return STATUS_ERROR;
				}
			}

			/* in master/slave mode, the first "ready for query"
			 * packet should be treated as if we were not in the
			 * mode
			 */
			if (MASTER_SLAVE)
				first_ready_for_query_received = 1;
		}

		else
		{
			/*
			 * save startup packet info
			 */
			pool_free_startup_packet(backend->slots[0]->sp);
			backend->slots[0]->sp = sp;

			if (DUAL_MODE)
			{
				backend->slots[1]->sp = sp;
			}

			/* reuse existing connection to backend */

			if (pool_do_reauth(frontend, backend))
			{
				pool_close(frontend);
				if (use_pool == USE_CONNECTION_POOL )
				{
					continue;
				}
				else
				{
					PGRset_status_to_child_tbl(pid,TBL_FREE);
					return STATUS_ERROR;
				}
			}

			if (MAJOR(backend) == 3)
			{
				if (send_params(frontend, backend))
				{
					pool_close(frontend);
					if (use_pool == USE_CONNECTION_POOL )
					{
						continue;
					}
					else
					{
						PGRset_status_to_child_tbl(pid,TBL_FREE);
						return STATUS_ERROR;
					}
				}
			}

			/* send ReadyForQuery to frontend */
			pool_write(frontend, "Z", 1);

			if (MAJOR(backend) == 3)
			{
				int len;
				char tstate;

				len = htonl(5);
				pool_write(frontend, &len, sizeof(len));
				tstate = TSTATE(backend);
				pool_write(frontend, &tstate, 1);
			}

			if (pool_flush(frontend) < 0)
			{
				pool_close(frontend);
				if (use_pool == USE_CONNECTION_POOL )
				{
					continue;
				}
				else
				{
					PGRset_status_to_child_tbl(pid,TBL_FREE);
					return STATUS_ERROR;
				}
			}

		}

		/* enable query result read timeout if non strict mode */
		if (pool_config_replication_strict == 0)
			pool_enable_timeout();

		connected = 1;

		/* show ps status */
		sp = MASTER_CONNECTION(backend)->sp;
		snprintf(psbuf, sizeof(psbuf), "%s %s %s idle",
				 sp->user, sp->database, remote_ps_data);
		set_ps_display(psbuf, false);

		/* query process loop */
		for (;;)
		{

			status = pool_process_query(frontend, backend, 0, first_ready_for_query_received);

			sp = MASTER_CONNECTION(backend)->sp;

			switch (status)
			{
				/* client exits */
				case POOL_END:
					/*
					 * do not cache connection if:
					 * pool_config_connection_cahe == 0 or
					 * datase name is template0, template1, or regression
					 */
					if (pool_config_connection_cache == 0 ||
						!strcmp(sp->database, "template0") ||
						!strcmp(sp->database, "template1") ||
						!strcmp(sp->database, "regression"))
					{
						pool_close(frontend);
						pool_send_frontend_exits(backend);
						pool_discard_cp(sp->user, sp->database, sp->major);
					}
					else
					{
						POOL_STATUS status1;

						/* send reset request to backend */
						status1 = pool_process_query(frontend, backend, 1, 0);
						pool_close(frontend);

						/* if we detect errors on resetting connection, we need to discard
						 * this connection since it might be in unknown status
						 */
						if (status1 != POOL_CONTINUE)
							pool_discard_cp(sp->user, sp->database, sp->major);
						else
							pool_connection_pool_timer(backend);
					}
					break;
				
				/* error occured. discard backend connection pool
                   and disconnect connection to the frontend */
				case POOL_ERROR:
					show_error("do_child: exits with status 1 due to error");
					break;

				/*
				 * kind mismatch fatal error occured.
				 * notice that we need to detach secondary server
				 * and just exit myself...
				 */
				case POOL_FATAL:
					notice_backend_error(0);
					break;

				/* not implemented yet */
				case POOL_IDLE:
					do_accept(unix_fd, inet_fd, &timeout);
					show_debug("accept while idle");
					break;

				default:
					break;
			}

			if (status != POOL_CONTINUE)
				break;
		}

		timeout.tv_sec = pool_config_child_life_time;
		timeout.tv_usec = 0;

		/* increment queries counter if necessary */
		if ( pool_config_child_max_connections > 0 )
			connections_count++;

		/* check if maximum connections count for this child reached */
		if ( ( pool_config_child_max_connections > 0 ) &&
			( connections_count >= pool_config_child_max_connections ) )
		{
			show_error("%s:child exiting, %d connections reached",func, pool_config_child_max_connections);
			send_frontend_exits();
			PGRset_status_to_child_tbl(pid,TBL_INIT);
			return STATUS_OK;
		}
		if (use_pool == NOT_USE_CONNECTION_POOL )
			break;
	}
	if ((status == POOL_ERROR ) ||
		(status == POOL_FATAL ))
	{
		PGRset_status_to_child_tbl(pid,TBL_FREE);
		return STATUS_ERROR;
	}
	else
	{
		PGRset_status_to_child_tbl(pid,TBL_INIT);
		return STATUS_OK;
	}
}

/* -------------------------------------------------------------------
 * private functions
 * -------------------------------------------------------------------
 */

#ifdef NONE_BLOCK
/*
 * set non-block flag
 */
static void set_nonblock(int fd)
{
	int var;

	/* set fd to none blocking */
	var = fcntl(fd, F_GETFL, 0);
	if (var == -1)
	{
		show_error("fcntl failed. %s", strerror(errno));
		exit(1);
	}
	if (fcntl(fd, F_SETFL, var | O_NONBLOCK) == -1)
	{
		show_error("fcntl failed. %s", strerror(errno));
		exit(1);
	}
}
#endif

/*
 * unset non-block flag
 */
static void unset_nonblock(int fd)
{
	int var;

	/* set fd to none blocking */
	var = fcntl(fd, F_GETFL, 0);
	if (var == -1)
	{
		show_error("fcntl failed. %s", strerror(errno));
		exit(1);
	}
	if (fcntl(fd, F_SETFL, var & ~O_NONBLOCK) == -1)
	{
		show_error("fcntl failed. %s", strerror(errno));
		exit(1);
	}
}

/*
* perform accept() and return new fd
*/
static POOL_CONNECTION *do_accept(int unix_fd, int inet_fd, struct timeval *timeout)
{
    fd_set	readmask;
    int fds;
	int save_errno;

	SockAddr saddr;
	int fd = 0;
	int afd;
	int inet = 0;
	POOL_CONNECTION *cp;
#ifdef ACCEPT_PERFORMANCE
	struct timeval now1, now2;
	static long atime;
	static int cnt;
#endif
	struct timeval *timeoutval;
	struct timeval tv1, tv2, tmback = {0, 0};

	char remote_host[NI_MAXHOST];
	char remote_port[NI_MAXSERV];

	set_ps_display("wait for connection request", false);

	FD_ZERO(&readmask);
	FD_SET(unix_fd, &readmask);
	if (inet_fd)
		FD_SET(inet_fd, &readmask);

	if (timeout->tv_sec == 0 && timeout->tv_usec == 0)
		timeoutval = NULL;
	else
	{
		timeoutval = timeout;
		tmback.tv_sec = timeout->tv_sec;
		tmback.tv_usec = timeout->tv_usec;
		gettimeofday(&tv1, NULL);

#ifdef DEBUG
		show_error("before select = {%d, %d}", timeoutval->tv_sec, timeoutval->tv_usec);
		show_error("g:before select = {%d, %d}", tv1.tv_sec, tv1.tv_usec);
#endif
	}

	fds = select(Max(unix_fd, inet_fd)+1, &readmask, NULL, NULL, timeoutval);

	save_errno = errno;
	/* check backend timer is expired */
	if (backend_timer_expired)
	{
		pool_backend_timer();
		backend_timer_expired = 0;
	}

	/*
	 * following code fragment computes remaining timeout val in a
	 * portable way. Linux does this automazically but other platforms do not.
	 */
	if (timeoutval)
	{
		gettimeofday(&tv2, NULL);

		tmback.tv_usec -= tv2.tv_usec - tv1.tv_usec;
		tmback.tv_sec -= tv2.tv_sec - tv1.tv_sec;

		if (tmback.tv_usec < 0)
		{
			tmback.tv_sec--;
			if (tmback.tv_sec < 0)
			{
				timeout->tv_sec = 0;
				timeout->tv_usec = 0;
			}
			else
			{
				tmback.tv_usec += 1000000;
				timeout->tv_sec = tmback.tv_sec;
				timeout->tv_usec = tmback.tv_usec;
			} 
		}
#ifdef DEBUG
		show_error("g:after select = {%d, %d}", tv2.tv_sec, tv2.tv_usec);
		show_error("after select = {%d, %d}", timeout->tv_sec, timeout->tv_usec);
#endif
	}

	errno = save_errno;

	if (fds == -1)
	{
		if (errno == EAGAIN || errno == EINTR)
			return NULL;

		show_error("select() failed. reason %s", strerror(errno));
		return NULL;
	}

	/* timeout */
	if (fds == 0)
	{
		return NULL;
	}

	if (FD_ISSET(unix_fd, &readmask))
	{
		fd = unix_fd;
	}

	if (FD_ISSET(inet_fd, &readmask))
	{
		fd = inet_fd;
		inet++;
	}

	/*
	 * Note that some SysV systems do not work here. For those
	 * systems, we need some locking mechanism for the fd.
	 */
	memset(&saddr, 0, sizeof(saddr));
	saddr.salen = sizeof(saddr.addr);

#ifdef ACCEPT_PERFORMANCE
	gettimeofday(&now1,0);
#endif
	afd = accept(fd, (struct sockaddr *)&saddr.addr, &saddr.salen);

	save_errno = errno;
	/* check backend timer is expired */
	if (backend_timer_expired)
	{
		pool_backend_timer();
		backend_timer_expired = 0;
	}
	errno = save_errno;
	if (afd < 0)
	{
		/*
		 * "Resource temporarily unavailable" (EAGAIN or EWOULDBLOCK)
		 * can be silently ignored. And EINTR can be ignored
		 */
		if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)
			show_error("accept() failed. reason: %s", strerror(errno));
		return NULL;
	}
#ifdef ACCEPT_PERFORMANCE
	gettimeofday(&now2,0);
	atime += (now2.tv_sec - now1.tv_sec)*1000000 + (now2.tv_usec - now1.tv_usec);
	cnt++;
	if (cnt % 100 == 0)
	{
		show_error("cnt: %d atime: %ld", cnt, atime);
	}
#endif

	show_debug("I am %d accept fd %d", getpid(), afd);

	pool_getnameinfo_all(&saddr, remote_host, remote_port);
	snprintf(remote_ps_data, sizeof(remote_ps_data),
			 remote_port[0] == '\0' ? "%s" : "%s(%s)",
			 remote_host, remote_port);

	set_ps_display("accept connection", false);

	/* log who is connecting */
	if (pool_config_log_connections)
	{
		show_error("connection received: host=%s%s%s",
				 remote_host, remote_port[0] ? " port=" : "", remote_port);
	}

	/* set NODELAY and KEEPALIVE options if INET connection */
	if (inet)
	{
		int on = 1;

		if (setsockopt(afd, IPPROTO_TCP, TCP_NODELAY,
					   (char *) &on,
					   sizeof(on)) < 0)
		{
			show_error("do_accept: setsockopt() failed: %s", strerror(errno));
			close(afd);
			return NULL;
		}
		if (setsockopt(afd, SOL_SOCKET, SO_KEEPALIVE,
					   (char *) &on,
					   sizeof(on)) < 0)
		{
			show_error("do_accept: setsockopt() failed: %s", strerror(errno));
			close(afd);
			return NULL;
		}
	}

	if ((cp = pool_open(afd)) == NULL)
	{
		close(afd);
		return NULL;
	}

	/* save ip addres for hba */
	memcpy(&cp->raddr, &saddr, sizeof(SockAddr));
	if (cp->raddr.addr.ss_family == 0)
		cp->raddr.addr.ss_family = AF_UNIX;
	
	return cp;
}

/*
* read startup packet
*/
static StartupPacket *read_startup_packet(POOL_CONNECTION *cp)
{
	StartupPacket *sp;
	StartupPacket_v2 *sp2;
	int protov;
	int len;
	char *p;

	sp = (StartupPacket *)calloc(sizeof(*sp), 1);
	if (!sp)
	{
		show_error("read_startup_packet: out of memory");
		return NULL;
	}

	/* read startup packet length */
	if (pool_read(cp, &len, sizeof(len)))
	{
		return NULL;
	}
	len = ntohl(len);
	len -= sizeof(len);

	if (len <= 0)
	{
		show_error("read_startup_packet: incorrect packet length (%d)", len);
	}

	sp->startup_packet = calloc(len, 1);
	if (!sp->startup_packet)
	{
		show_error("read_startup_packet: out of memory");
		pool_free_startup_packet(sp);
		return NULL;
	}

	/* read startup packet */
	if (pool_read(cp, sp->startup_packet, len))
	{
		pool_free_startup_packet(sp);
		return NULL;
	}

	sp->len = len;
	memcpy(&protov, sp->startup_packet, sizeof(protov));
	sp->major = ntohl(protov)>>16;
	sp->minor = ntohl(protov) & 0x0000ffff;
	p = sp->startup_packet;

	switch(sp->major)
	{
		case PROTO_MAJOR_V2: /* V2 */
			sp2 = (StartupPacket_v2 *)(sp->startup_packet);

			sp->database = calloc(SM_DATABASE+1, 1);
			if (!sp->database)
			{
				show_error("read_startup_packet: out of memory");
				pool_free_startup_packet(sp);
				return NULL;
			}
			strncpy(sp->database, sp2->database, SM_DATABASE);

			sp->user = calloc(SM_USER+1, 1);
			if (!sp->user)
			{
				show_error("read_startup_packet: out of memory");
				pool_free_startup_packet(sp);
				return NULL;
			}
			strncpy(sp->user, sp2->user, SM_USER);

			break;

		case PROTO_MAJOR_V3: /* V3 */
			p += sizeof(int);	/* skip protocol version info */

			while(*p)
			{
				if (!strcmp("user", p))
				{
					p += (strlen(p) + 1);
					sp->user = strdup(p);
					if (!sp->user)
					{
						show_error("read_startup_packet: out of memory");
						pool_free_startup_packet(sp);
						return NULL;
					}
				}
				else if (!strcmp("database", p))
				{
					p += (strlen(p) + 1);
					sp->database = strdup(p);
					if (!sp->database)
					{
						show_error("read_startup_packet: out of memory");
						pool_free_startup_packet(sp);
						return NULL;
					}
				}
				p += (strlen(p) + 1);
			}
			break;

		case 1234:		/* cancel or SSL request */
			/* set dummy database, user info */
			sp->database = calloc(1, 1);
			if (!sp->database)
			{
				show_error("read_startup_packet: out of memory");
				pool_free_startup_packet(sp);
				return NULL;
			}
			sp->user = calloc(1, 1);
			if (!sp->user)
			{
				show_error("read_startup_packet: out of memory");
				pool_free_startup_packet(sp);
				return NULL;
			}
			break;

		default:
			show_error("read_startup_packet: invalid major no: %d", sp->major);
			pool_free_startup_packet(sp);
			return NULL;
	}

	show_debug("Protocol Major: %d Minor: %d database: %s user: %s", 
			   sp->major, sp->minor, sp->database, sp->user);

	return sp;
}

/*
* send startup packet
*/
static int send_startup_packet(POOL_CONNECTION_POOL_SLOT *cp)
{
	int len;

	len = htonl(cp->sp->len + sizeof(len));
	pool_write(cp->con, &len, sizeof(len)); 
	return pool_write_and_flush(cp->con, cp->sp->startup_packet, cp->sp->len);
}

/*
 * process cancel request
 */
static void cancel_request(CancelPacket *sp, int secondary_backend)
{
	int	len;
	int fd;
	POOL_CONNECTION *con;

	show_debug("Cancel request received");

	if (secondary_backend)
	{
		if (*pool_config_secondary_backend_host_name == '\0')
			fd = connect_unix_domain_socket(1);
		else
			fd = connect_inet_domain_socket(1);
	}
	else
	{
		if (*pool_config_current_backend_host_name == '\0')
			fd = connect_unix_domain_socket(0);
		else
			fd = connect_inet_domain_socket(0);
	}

	if (fd < 0)
	{
		show_error("Could not create socket for sending cancel request");
		return;
	}

	con = pool_open(fd);
	if (con == NULL)
		return;

	len = htonl(sizeof(len) + sizeof(CancelPacket));
	pool_write(con, &len, sizeof(len));

	if (pool_write_and_flush(con, sp, sizeof(CancelPacket)) < 0)
		show_error("Could not send cancel request packet");
	pool_close(con);
}

static POOL_CONNECTION_POOL *connect_backend(StartupPacket *sp, POOL_CONNECTION *frontend)
{
	POOL_CONNECTION_POOL *backend;

	/* connect to the backend */
	backend = pool_create_cp();
	if (backend == NULL)
	{
		pool_send_error_message(frontend, sp->major, "XX000", "connection cache is full", "",
								"increace max_pool", __FILE__, __LINE__);
		pool_close(frontend);
		pool_free_startup_packet(sp);
		return NULL;
	}

	/* mark this is a backend connection */
	backend->slots[0]->con->isbackend = 1;

	/*
	 * save startup packet info
	 */
	backend->slots[0]->sp = sp;

	if (DUAL_MODE)
	{
		backend->slots[1]->con->isbackend = 1;
		backend->slots[1]->con->issecondary_backend = 1;
		/*
		 * save startup packet info
		 */
		backend->slots[1]->sp = sp;
	}

	/* send startup packet */
	if (send_startup_packet(backend->slots[0]) < 0)
	{
		show_error("do_child: fails to send startup packet to the backend");
		pool_discard_cp(sp->user, sp->database, sp->major);
		pool_close(frontend);
		return NULL;
	}

	/* send startup packet */
	if (DUAL_MODE)
	{
		if (send_startup_packet(backend->slots[1]) < 0)
		{
			show_error("do_child: fails to send startup packet to the secondary backend");
			pool_discard_cp(sp->user, sp->database, sp->major);
			pool_close(frontend);
			return NULL;
		}
	}

	/*
	 * do authentication stuff
	 */
	if (pool_do_auth(frontend, backend))
	{
		pool_close(frontend);
		pool_discard_cp(sp->user, sp->database, sp->major);
		return NULL;
	}
	return backend;
}

/*
 * signal handler for SIGINT and SIGQUUT
 */
static RETSIGTYPE die(int sig)
{
	exit_request = 1;

	show_debug("child receives shutdown request signal %d", sig);

	switch (sig)
	{
		case SIGTERM:	/* smart shutdown */
			if (idle == 0)
			{
				show_debug("child receives smart shutdown request but it's not in idle state");
				return;
			}
			break;

		case SIGINT:	/* fast shutdown */
		case SIGQUIT:	/* immediate shutdown */
			exit(0);
			break;
		default:
			break;
	}

	send_frontend_exits();

	exit(0);
}

/*
 * signal handler for SIGHUP
 * close all idle connections
 */
static RETSIGTYPE close_idle_connection(int sig)
{
	int i;
	POOL_CONNECTION_POOL *p = pool_connection_pool;

	show_debug("child receives close idle connection request");

	for (i=0;i<pool_config_max_pool;i++, p++)
	{
		if (!MASTER_CONNECTION(p))
			continue;
		if (MASTER_CONNECTION(p)->sp->user == NULL)
			continue;

		if (MASTER_CONNECTION(p)->closetime > 0)		/* idle connection? */
		{
			show_debug("close_idle_connection: close idle connection: user %s database %s", MASTER_CONNECTION(p)->sp->user, MASTER_CONNECTION(p)->sp->database);
			pool_send_frontend_exits(p);
			pool_free_startup_packet(MASTER_CONNECTION(p)->sp);
			pool_close(MASTER_CONNECTION(p)->con);

			if (DUAL_MODE)
			{
				/* do not free memory! we did not allocate them */
				pool_close(SECONDARY_CONNECTION(p)->con);
			}
			memset(p, 0, sizeof(POOL_CONNECTION_POOL));
		}
	}
}

/* notice backend connection error using SIGUSR2 */
static RETSIGTYPE close_child_connection(int sig)
{
	int i;
	POOL_CONNECTION_POOL *p = pool_connection_pool;

	show_debug("child receives close connection request");

	for (i=0;i<pool_config_max_pool;i++, p++)
	{
		if (!MASTER_CONNECTION(p))
			continue;
		if (MASTER_CONNECTION(p)->sp->user == NULL)
			continue;

		pool_send_frontend_exits(p);
		pool_free_startup_packet(MASTER_CONNECTION(p)->sp);
		pool_close(MASTER_CONNECTION(p)->con);

		if (DUAL_MODE)
		{
			/* do not free memory! we did not allocate them */
			pool_close(SECONDARY_CONNECTION(p)->con);
		}
		memset(p, 0, sizeof(POOL_CONNECTION_POOL));
	}
}

/*
 * send frontend exiting messages to all connections.
 * this is called when child life time expires or child max connections expires.
 */
static void send_frontend_exits(void)
{
	int i;
	POOL_CONNECTION_POOL *p = pool_connection_pool;

#ifdef HAVE_SIGPROCMASK
	sigset_t oldmask;
#else
	int	oldmask;
#endif

	POOL_SETMASK2(&BlockSig, &oldmask);

	for (i=0;i<pool_config_max_pool;i++, p++)
	{
		if (!MASTER_CONNECTION(p))
			continue;
		if (MASTER_CONNECTION(p)->sp->user == NULL)
			continue;
		pool_send_frontend_exits(p);
	}

	POOL_SETMASK(&oldmask);
}

static int send_params(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend)
{
	int index;
	char *name, *value;
	int len, sendlen;

	index = 0;
	while (pool_get_param(&MASTER(backend)->params, index++, &name, &value) == 0)
	{
		pool_write(frontend, "S", 1);
		len = sizeof(sendlen) + strlen(name) + 1 + strlen(value) + 1;
		sendlen = htonl(len);
		pool_write(frontend, &sendlen, sizeof(sendlen));
		pool_write(frontend, name, strlen(name) + 1);
		pool_write(frontend, value, strlen(value) + 1);
	}

	if (pool_flush(frontend))
	{
		show_error("pool_send_params: pool_flush() failed");
		return -1;
	}
	return 0;
}

void pool_free_startup_packet(StartupPacket *sp)
{
	if (sp)
	{
		if (sp->startup_packet)
			free(sp->startup_packet);
		if (sp->database)
			free(sp->database);
		if (sp->user)
			free(sp->user);
		free(sp);
	}
}

/*
 * check if we can connect to the backend
 * returns 0 for ok. -1 for master down, -2 for secondary down.
 */
int health_check(void)
{
	int fd;

	/* V2 startup packet */
	typedef struct {
		int len;		/* startup packet length */
		StartupPacket_v2 sp;
	} MySp;
	MySp mysp;
	char kind;

	memset(&mysp, 0, sizeof(mysp));
	mysp.len = htonl(296);
	mysp.sp.protoVersion = htonl(PROTO_MAJOR_V2 << 16);
	strcpy(mysp.sp.database, "template1");
 	strncpy(mysp.sp.user, pool_config_health_check_user, sizeof(mysp.sp.user) - 1);
	*mysp.sp.options = '\0';
	*mysp.sp.unused = '\0';
	*mysp.sp.tty = '\0';

	if (*pool_config_current_backend_host_name == '\0')
		fd = connect_unix_domain_socket(0);
	else
		fd = connect_inet_domain_socket(0);

	if (fd < 0)
	{
		show_error("health check failed. master %s at port %d is down",
				   pool_config_current_backend_host_name,
				   pool_config_current_backend_port);
		return -1;
	}

	if (write(fd, &mysp, sizeof(mysp)) < 0)
	{
		show_error("health check failed during write. master %s at port %d is down",
				   pool_config_current_backend_host_name,
				   pool_config_current_backend_port);
		close(fd);
		return -1;
	}

	read(fd, &kind, 1);

	if (write(fd, "X", 1) < 0)
	{
		show_error("health check failed during write. master %s at port %d is down",
				   pool_config_current_backend_host_name,
				   pool_config_current_backend_port);
		close(fd);
		return -1;
	}

	close(fd);

	if (!DUAL_MODE)
		return 0;

	if (*pool_config_secondary_backend_host_name == '\0')
		fd = connect_unix_domain_socket(1);
	else
		fd = connect_inet_domain_socket(1);

	if (fd < 0)
	{
		show_error("health check failed. secondary %s at port %d is down",
				   pool_config_secondary_backend_host_name,
				   pool_config_secondary_backend_port);
		return -2;
	}

	if (write(fd, &mysp, sizeof(mysp)) < 0)
	{
		show_error("health check failed during write. secondary %s at port %d is down",
				   pool_config_secondary_backend_host_name,
				   pool_config_secondary_backend_port);
		close(fd);
		return -2;
	}

	read(fd, &kind, 1);

	if (write(fd, "X", 1) < 0)
	{
		show_error("health check failed during write. secondary %s at port %d is down",
				   pool_config_secondary_backend_host_name,
				   pool_config_secondary_backend_port);
		close(fd);
		return -2;
	}

	close(fd);

	return 0;
}


/*--------------------------------------------------------------------
 * SYMBOL
 *    PGRpre_fork_children()
 * NOTES
 *    pre forked child precesses
 * ARGS
 *    ClusterTbl * ptr: pointer of cluster server table (I)
 * RETURN
 *    OK: STATUS_OK
 *    NG: STATUS_ERROR
 *--------------------------------------------------------------------
 */
int
PGRpre_fork_children(ClusterTbl * ptr)
{
	int cnt;

	if (ptr == NULL)
	{
		return STATUS_ERROR;
	}
	cnt = 0 ;
	while ((ptr->useFlag != TBL_END) && (cnt < ClusterNum))
	{
		PGRpre_fork_child(ptr);
		cnt ++;
		ptr ++;
	}
	return STATUS_OK;
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    PGRpre_fork_child()
 * NOTES
 *    pre forked child precess
 * ARGS
 *    ClusterTbl * ptr: pointer of cluster server table (I)
 * RETURN
 *    OK: STATUS_OK
 *    NG: STATUS_ERROR
 *--------------------------------------------------------------------
 */
int
PGRpre_fork_child(ClusterTbl * ptr)
{
	pid_t pid = 0;
	int i;

	if (ptr == NULL)
	{
		return STATUS_ERROR;
	}
	if (ptr->useFlag == TBL_END)
	{
		return STATUS_ERROR;
	}
	for ( i = 0 ; i < ptr->max_connect * Max_Pool ; i ++)
	{
		pid = PGRcreate_child(ptr);		
	}
	return STATUS_OK;
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    PGRcreate_child()
 * NOTES
 *    create child process
 * ARGS
 *    ClusterTbl * ptr: pointer of cluster server table (I)
 * RETURN
 *    OK: STATUS_OK
 *    NG: STATUS_ERROR
 *--------------------------------------------------------------------
 */
int 
PGRcreate_child(ClusterTbl * cluster_p)
{
	char * func = "PGRcreate_child()";
	pid_t pid,pgid;

	if (cluster_p == NULL)
		return STATUS_ERROR;

#ifdef PRINT_DEBUG
	show_debug("%s:create child [%d@%s]",func,cluster_p->port,cluster_p->hostName);
#endif			
	PGRsignal(SIGCHLD,PGRrecreate_child);
	pgid = getpgid((pid_t)0);
	pid = fork();
	if (pid < 0)
	{
		show_error("%s:fork() failed. (%s)",func,strerror(errno));
		return STATUS_ERROR;
	}
	if (pid == 0)
	{
		CurrentCluster = cluster_p;
		if (pool_init_cp())
		{
			show_error("%s:pool_init_cp failed",func);
			exit(1);
		}
		PGRsignal(SIGCHLD,PGRchild_wait);
		PGRsignal(SIGTERM, die);
		PGRsignal(SIGHUP, die);
		PGRsignal(SIGINT, die);
		PGRsignal(SIGUSR1,do_pooling_child);
		setpgid((pid_t)0,pgid);
		for (;;)
		{
			pause();
			PGRsignal(SIGUSR1,do_pooling_child);
		}
#ifdef PRINT_DEBUG
		show_debug("%s:create child end [%d@%s]",func,cluster_p->port,cluster_p->hostName);
#endif			
		die(SIGTERM);
	}
	else
	{
		PGRadd_child_tbl(cluster_p,pid,TBL_INIT);
	}
	return pid;
}

/*
 * start up pooling child process
 */
static void
do_pooling_child(int sig)
{
	char * func = "do_pooling_child()";
	int rtn;
	pid_t pid;

	pid = getpid();
	rtn = PGRdo_child(USE_CONNECTION_POOL);
	PGRrelease_connection(CurrentCluster);
	if (rtn != STATUS_OK)
	{
		show_error("%s:PGRdo_child failed",func);
		die(SIGTERM);
	}
	return ;
}

/*--------------------------------------------------------------------
 * SYMBOL
 *    PGRscan_child_tbl()
 * NOTES
 *    get a child process id that is waiting for connection 
 *    with the cluster server
 * ARGS
 *    ClusterTbl * ptr: pointer of cluster server table (I)
 * RETURN
 *    OK: child process id
 *    NG: 0
 *--------------------------------------------------------------------
 */
pid_t
PGRscan_child_tbl(ClusterTbl * cluster_p)
{
	char * func = "PGRscan_child_tbl()";
	ChildTbl * p;

	if ( cluster_p == NULL)
	{
		show_error("%s:Cluster_Tbl is not initialize",func);
		return STATUS_ERROR;
	}
	p = Child_Tbl;
	if ( p == NULL)
	{
		show_error("%s:Child_Tbl is not initialize",func);
		return STATUS_ERROR;
	}
	while(p->useFlag != TBL_END)
	{
		if (p->pid <= 0)
		{
			p++;
			continue;
		}
		if (((p->useFlag == TBL_INIT) ||
			(p->useFlag == TBL_FREE)) &&
			(p->rec_no == cluster_p->rec_no))
		{
			p->useFlag = TBL_USE;
			return (p->pid);
		}
		p++;
	}
	return 0;
}

/*
 * set status in child process table
 */
int
PGRset_status_to_child_tbl(pid_t pid, int status)
{
	char * func = "PGRset_status_to_child_tbl()";
	ChildTbl * p;

	p = Child_Tbl;
	if ( p == NULL)
	{
		show_error("%s:Child_Tbl is not initialize",func);
		return STATUS_ERROR;
	}
	while(p->useFlag != TBL_END)
	{
		if (p->pid == pid)
		{
			p->useFlag = status;
			return STATUS_OK;
		}
		p++;
	}
	return STATUS_ERROR;
}

/*
 * add child process data in child process table
 */
int
PGRadd_child_tbl(ClusterTbl * cluster_p, pid_t pid, int status)
{
	char * func = "PGRadd_child_tbl()";
	ChildTbl * p;

	p = Child_Tbl;
	if ( p == NULL)
	{
		show_error("%s:Child_Tbl is not initialize",func);
		return STATUS_ERROR;
	}
	if ( cluster_p == NULL)
	{
		show_error("%s:Cluster_Tbl is not initialize",func);
		return STATUS_ERROR;
	}
	while(p->useFlag != TBL_END)
	{
		if ((p->useFlag == TBL_FREE) ||
			(p->useFlag == TBL_ERROR))
		{
			p->useFlag = status;
			p->rec_no = cluster_p->rec_no;
			p->pid = pid;
			return STATUS_OK;
		}
		p++;
	}
	return STATUS_ERROR;
}

int
PGRget_child_status(pid_t pid)
{
	char * func = "PGRget_child_status()";
	ChildTbl * p;

	p = Child_Tbl;
	if ( p == NULL)
	{
		show_error("%s:Child_Tbl is not initialize",func);
		return STATUS_ERROR;
	}

	while (p->useFlag != TBL_END)
	{
		if (p->pid == pid)
		{
			return p->useFlag;
		}
		p++;
	}
	return TBL_END;
}

void 
PGRreturn_connection_full_error(void)
{
  return_with_error( "Sorry, backend connection is full\n");
}

void 
PGRreturn_no_connection_error(void) {
  return_with_error("pglb could not connect to server: no cluster available.\n");
}

static void 
return_with_error (char *msg) 
{
	StartupPacket *sp = NULL;
	POOL_CONNECTION *frontend = NULL;
	struct timeval timeout;


	timeout.tv_sec = pool_config_child_life_time;
	timeout.tv_usec = 0;
	/* perform accept() */
	frontend = do_accept(Frontend_FD.unix_fd,Frontend_FD.inet_fd,&timeout);
	if (frontend == NULL)
	{
		/* accept() failed. return to the accept() loop */
		return ;
	}
	sp = read_startup_packet(frontend);
	if (sp == NULL)
	{
		/* failed to read the startup packet. return to the
		   accept() loop */
		pool_close(frontend);
		return ;
	}
	pool_write_and_flush(frontend, "E", 1);
	pool_write_and_flush(frontend, msg, strlen(msg)+1);
	pool_close(frontend);
	return ;
}

void
PGRquit_children_on_cluster(int rec_no)
{
	char * func = "PGRquit_children_on_cluster()";
	ChildTbl * p;

	if (Child_Tbl == NULL)
	{
		return;
	}
	PGRsignal(SIGCHLD,SIG_IGN);
	p = Child_Tbl;
	while(p->useFlag != TBL_END)
	{
		if (p->rec_no == rec_no) 
		{
			if (kill (p->pid,SIGUSR2) == -1)
			{
				show_error("%s:could not stop pid: %d (%s)",func,p->pid,strerror(errno));
				return;
			}
			PGRchild_wait(SIGUSR2);
			p->useFlag = DATA_FREE;
		}
		p++;
	}
	if (Use_Connection_Pool)
	{
		PGRsignal(SIGCHLD,PGRrecreate_child);
	}
	else
	{
		PGRsignal(SIGCHLD,PGRchild_wait);
	}
}


void
notice_backend_error(int master)
{
	pid_t pid = getpid();
	PGRset_status_on_cluster_tbl(TBL_ERROR_NOTICE,CurrentCluster);
	PGRset_status_to_child_tbl(pid,TBL_ERROR);

}

