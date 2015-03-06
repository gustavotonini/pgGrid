/*--------------------------------------------------------------------
 * FILE:
 *     pglb.h
 *
 * Portions Copyright (c) 2003-2008  Atsushi Mitani
 *--------------------------------------------------------------------
 */
#ifndef PGLB_H
#define PGLB_H

#define PGLB_VERSION	"1.9.0rc5"

#include <signal.h>
#include <time.h>
#include <netinet/in.h>

#include "../libpgc/libpgc.h"
#include "pool_type.h"
#include "pool_ip.h"

#define REPLICATION (0)
#define IN_LOAD_BALANCE (0)
#define MASTER_SLAVE (0)

/*
 * from pool.h
 */ 

/* undef this if you have problems with non blocking accept() */
#define NONE_BLOCK

#define POOLMAXPATHLEN 8192

/* configuration file name */
#define POOL_CONF_FILE_NAME "pgpool.conf"
#define HBA_CONF_FILE_NAME "pool_hba.conf"

/* pid file directory */
#define DEFAULT_LOGDIR "/tmp"

/* Unix domain socket directory */
#define DEFAULT_SOCKET_DIR "/tmp"

/* pid file name */
#define PID_FILE_NAME "pgpool.pid"

/* strict mode comment in SQL */
#define STRICT_MODE_STR "/*STRICT*/"
#define STRICT_MODE(s) (strncasecmp((s), STRICT_MODE_STR, strlen(STRICT_MODE_STR)) == 0)
#define NO_STRICT_MODE_STR "/*NO STRICT*/"
#define NO_STRICT_MODE(s) (strncasecmp((s), NO_STRICT_MODE_STR, strlen(NO_STRICT_MODE_STR)) == 0)

typedef enum {
	POOL_CONTINUE = 0,
	POOL_IDLE,
	POOL_END,
	POOL_ERROR,
	POOL_FATAL,
	POOL_DEADLOCK
} POOL_STATUS;

/* protocol major version numbers */
#define PROTO_MAJOR_V2	2
#define PROTO_MAJOR_V3	3

/*
 * startup packet definitions (v2) stolen from PostgreSQL
 */
#define SM_DATABASE		64
#define SM_USER			32
#define SM_OPTIONS		64
#define SM_UNUSED		64
#define SM_TTY			64

typedef struct StartupPacket_v2
{
	int			protoVersion;		/* Protocol version */
	char		database[SM_DATABASE];	/* Database name */
	char		user[SM_USER];	/* User name */
	char		options[SM_OPTIONS];	/* Optional additional args */
	char		unused[SM_UNUSED];		/* Unused */
	char		tty[SM_TTY];	/* Tty for debug output */
} StartupPacket_v2;

#ifndef PQCOMM_H
/* startup packet info */
typedef struct
{
	char *startup_packet;		/* raw startup packet without packet length (malloced area) */
	int len;					/* raw startup packet length */
	int major;	/* protocol major version */
	int minor;	/* protocol minor version */
	char *database;	/* database name in startup_packet (malloced area) */
	char *user;	/* user name in startup_packet (malloced area) */
} StartupPacket;
#endif

typedef struct CancelPacket
{
	int			protoVersion;		/* Protocol version */
	int			pid;	/* bcckend process id */
	int			key;	/* cancel key */
} CancelPacket;

#define MAX_CONNECTION_SLOTS 1

/*
 * configuration paramters
 */
typedef struct {
	char *listen_addresses; /* hostnames/IP addresses to listen on */
    int	port;	/* port # to bind */
	char *socket_dir;		/* pgpool socket directory */
    char	*backend_host_name;	/* backend host name */
    int	backend_port;	/* backend port # */
    char	*secondary_backend_host_name;	/* secondary backend host name */
    int	secondary_backend_port;	/* secondary backend port # */
    int	num_init_children;	/* # of children initially pre-forked */
    int	child_life_time;	/* if idle for this seconds, child exits */
    int	connection_life_time;	/* if idle for this seconds, connection closes */
    int	child_max_connections;	/* if max_connections received, child exits */
    int	max_pool;	/* max # of connection pool per child */
    char *logdir;		/* logging directory */
    char *backend_socket_dir;	/* Unix domain socket directory for the PostgreSQL server */
	int replication_mode;		/* replication mode */
	int replication_strict;	/* if non 0, wait for completion of the
                               query sent to master to avoid deadlock */
	double weight_master;		/* master weight for load balancing */
	double weight_secondary;		/* secondary weight for load balancing */
	/*
	 * if secondary does not respond in this milli seconds, abort this session.
	 * this is not compatible with replication_strict = 1. 0 means no timeout.
	 */
	int replication_timeout;

	int load_balance_mode;		/* load balance mode */

	int replication_stop_on_mismatch;		/* if there's a data mismatch between master and secondary
											 * start degenration to stop replication mode
											 */
	int replicate_select; /* if non 0, replicate SELECT statement when load balancing is disabled. */
	char **reset_query_list;		/* comma separated list of quries to be issued at the end of session */

	int print_timestamp;		/* if non 0, print time stamp to each log line */
	int master_slave_mode;		/* if non 0, operate in master/slave mode */
	int connection_cache;		/* if non 0, cache connection pool */
	int health_check_timeout;	/* health check timeout */
	int health_check_period;	/* health check period */
	char *health_check_user;		/* PostgreSQL user name for health check */
	int insert_lock;	/* if non 0, automatically lock table with INSERT to keep SERIAL
						   data consistency */
	int ignore_leading_white_space;		/* ignore leading white spaces of each query */
	/* followings do not exist in the configuration file */
    char *current_backend_host_name;	/* current backend host name */
    int	current_backend_port;	/* current backend port # */
	int replication_enabled;		/* replication mode enabled */
	int master_slave_enabled;		/* master/slave mode enabled */
	int num_reset_queries;		/* number of queries in reset_query_list */
	int num_servers;			/* number of PostgreSQL servers */
	int server_status[MAX_CONNECTION_SLOTS];	/* server status 0:unused, 1:up, 2:down */
	int log_statement; /* 0:false, 1: true - logs all SQL statements */
	int log_connections;		/* 0:false, 1:true - logs incoming connections */
	int log_hostname;		/* 0:false, 1:true - resolve hostname */
	int enable_pool_hba;		/* 0:false, 1:true - enables pool_hba.conf file authentication */
} POOL_CONFIG;

#define MAX_PASSWORD_SIZE		1024

typedef struct {
	int num;	/* number of entries */
	char **names;		/* parameter names */
	char **values;		/* values */
} ParamStatus;

/*
 * stream connection structure
 */
typedef struct {
	int fd;		/* fd for connection */

	char *wbuf;	/* write buffer for the connection */
	int wbufsz;	/* write buffer size */
	int wbufpo;	/* buffer offset */

	char *hp;	/* pending data buffer head address */
	int po;		/* pending data offset */
	int bufsz;	/* pending data buffer size */
	int len;	/* pending data length */

	char *sbuf;	/* buffer for pool_read_string */
	int sbufsz;	/* its size in bytes */

	char *buf2;	/* buffer for pool_read2 */
	int bufsz2;	/* its size in bytes */

	int isbackend;		/* this connection is for backend if non 0 */
	int issecondary_backend;		/* this connection is for secondary backend if non 0 */

	char tstate;		/* transaction state (V3 only) */

	/*
	 * following are used to remember when re-use the authenticated connection
	 */
	int auth_kind;		/* 3: clear text password, 4: crypt password, 5: md5 password */
	int pwd_size;		/* password (sent back from frontend) size in host order */
	char password[MAX_PASSWORD_SIZE];		/* password (sent back from frontend) */
	char salt[4];		/* password salt */

	/*
	 * following are used to remember current session paramter status.
	 * re-used connection will need them (V3 only)
	 */
	ParamStatus params;

	int no_forward;		/* if non 0, do not write to frontend */

	/*
	 * frontend info needed for hba
	 */
	int protoVersion;
	SockAddr raddr;
	UserAuth auth_method;
	char *auth_arg;
	char *database;
	char *username;
#ifdef USE_SSL
	bool ssl;
#endif

} POOL_CONNECTION;

/*
 * connection pool structure
 */
typedef struct {
	StartupPacket *sp;	/* startup packet info */
    int pid;	/* backend pid */
    int key;	/* cancel key */
    POOL_CONNECTION	*con;
	time_t closetime;	/* absolute time in second when the connection closed
						 * if 0, that means the connection is under use.
						 */
} POOL_CONNECTION_POOL_SLOT;

typedef struct {
    int num;	/* number of slots */
    POOL_CONNECTION_POOL_SLOT	*slots[MAX_CONNECTION_SLOTS];
} POOL_CONNECTION_POOL;

#define MASTER_CONNECTION(p) ((p)->slots[0])
#define SECONDARY_CONNECTION(p) ((p)->slots[1])
#define DUAL_MODE (REPLICATION || MASTER_SLAVE)
#define MASTER(p) MASTER_CONNECTION(p)->con
#define SECONDARY(p) SECONDARY_CONNECTION(p)->con
#define MAJOR(p) MASTER_CONNECTION(p)->sp->major
#define TSTATE(p) MASTER(p)->tstate

#define Max(x, y)		((x) > (y) ? (x) : (y))
#define Min(x, y)		((x) < (y) ? (x) : (y))

#define LOCK_COMMENT "/*INSERT LOCK*/"
#define LOCK_COMMENT_SZ (sizeof(LOCK_COMMENT)-1)
#define NO_LOCK_COMMENT "/*NO INSERT LOCK*/"
#define NO_LOCK_COMMENT_SZ (sizeof(NO_LOCK_COMMENT)-1)

/*
 * pool_signal.h
 */
#ifdef HAVE_SIGPROCMASK
extern sigset_t UnBlockSig,
			BlockSig,
			AuthBlockSig;

#define POOL_SETMASK(mask)	sigprocmask(SIG_SETMASK, mask, NULL)
#define POOL_SETMASK2(mask, oldmask)	sigprocmask(SIG_SETMASK, mask, oldmask)
#else
extern int	UnBlockSig,
			BlockSig,
			AuthBlockSig;

#ifndef WIN32
#define POOL_SETMASK(mask)	sigsetmask(*((int*)(mask)))
#define POOL_SETMASK2(mask, oldmask)	do {oldmask = POOL_SETMASK(mask)} while (0)
#else
#define POOL_SETMASK(mask)		pqsigsetmask(*((int*)(mask)))
#define POOL_SETMASK2(mask, oldmask)	do {oldmask = POOL_SETMASK(mask)} while (0)
int			pqsigsetmask(int mask);
#endif
#endif

/*
 * pglb
 */

typedef struct {
	int useFlag;
	int sock;
}SocketTbl;

typedef struct {
	int useFlag;
	char hostName[HOSTNAME_MAX_LENGTH];
	unsigned short port;
	short max_connect;
	int use_num;
	int rate;
	int rec_no;
	int retry_count;
}ClusterTbl;

typedef struct {
	long mtype;
	char mdata[1];
}MsgData;

typedef struct {
	int useFlag;
	int rec_no;
	pid_t pid;
}ChildTbl;

#define UNIX_DOMAIN_FD	(0)
#define INET_DOMAIN_FD	(1)
typedef struct {
	int unix_fd;
	int inet_fd;
}FrontSocket;

#define pool_config_listen_addresses	("*")
#define pool_config_child_life_time	(Connection_Life_Time)
#define pool_config_child_max_connections (CurrentCluster->max_connect)
#define pool_config_connection_life_time	(Connection_Life_Time)
#define pool_config_connection_cache	(0)
#define pool_config_current_backend_host_name	(CurrentCluster->hostName)
#define pool_config_current_backend_port	(CurrentCluster->port)
#define pool_config_secondary_backend_host_name (CurrentCluster->hostName)
#define pool_config_secondary_backend_port	(CurrentCluster->port)
#define pool_config_max_pool	(Max_Pool)
#define pool_config_master_slave_enabled (0)
#define pool_config_master_slave_mode (0)
#define pool_config_health_check_user (PGRuserName)
#define pool_config_health_check_timeout (15)
#define pool_config_health_check_period (15)
#define pool_config_backend_host_name	(CurrentCluster->hostName)
#define pool_config_backend_socket_dir	(Backend_Socket_Dir)
#define pool_config_log_connections (0)
#define pool_config_log_hostname (0)
#define pool_config_log_statement (0)
#define pool_config_logdir	"./"
#define pool_config_replication_enabled	(0)
#define pool_config_replication_mode	(0)
#define pool_config_replication_strict	(0)
#define pool_config_replication_timeout	(0)
#define pool_config_load_balance_mode	(0)
#define pool_config_replication_stop_on_mismatch	(0)
#define pool_config_replicate_select (0)
#define pool_config_port	(Recv_Port_Number)
#define pool_config_socket_dir	(Backend_Socket_Dir)
#define pool_config_backend_port	(CurrentCluster->port)
#define pool_config_num_init_children	(CurrentCluster->max_connect)
#define	pool_config_weight_master (1.0)
#define pool_config_weight_secondary (0.0)
#define pool_config_num_reset_queries (3)
#define pool_config_print_timestamp (0)
#define pool_config_insert_lock (0)
#define pool_config_ignore_leading_white_space (0)
#define pool_config_enable_pool_hba (0)

extern char * Reset_Query_List[];
/*
 * for pglb
 */
#ifndef MAX_DB_SERVER
#define MAX_DB_SERVER	(32)
#endif

#define PGLB_MAX_SOCKET_QUEUE (10000)
#define	CLUSTER_TBL_SHM_KEY	(1010)
#define PGLB_CONNECT_RETRY_TIME  (3)
#define DEFAULT_CONNECT_NUM	(32)
#define DEFAULT_PORT	(5432)
#define BUF_SIZE	(16384)
#define TBL_FREE	(0)
#define TBL_INIT	(1)
#define TBL_USE		(2)
#define TBL_STOP	(3)
#define TBL_ACCEPT	(10)
#define TBL_ERROR_NOTICE	(98)
#define TBL_ERROR	(99)
#define TBL_END	(-1)
#define STATUS_OK	(0)
#define STATUS_ERROR	(-1)
#ifdef	RECOVERY_PREPARE_REQ
#define ADD_DB		RECOVERY_PREPARE_REQ
#else
#define ADD_DB		(1)
#endif
#ifdef	RECOVERY_PGDATA_ANS
#define STOP_DB		RECOVERY_PGDATA_ANS
#else
#define STOP_DB		(3)
#endif
#ifdef	RECOVERY_FINISH
#define START_DB	RECOVERY_FINISH
#else
#define START_DB	(9)
#endif
#define DELETE_DB	(99)
#define QUERY_TERMINATE	(0x00)
#define RESPONSE_TERMINATE	(0x5a)
#define PGLB_CONF_FILE	"pglb.conf"
#define PGLB_PID_FILE	"pglb.pid"
#define PGLB_STATUS_FILE "pglb.sts"
#define PGLB_LOG_FILE "pglb.log"
#define CLUSTER_SERVER_TAG	"Cluster_Server_Info"
#define MAX_CONNECT_TAG	"Max_Connect"
#define RECOVERY_PORT_TAG	"Recovery_Port"
#define RECV_PORT_TAG	"Receive_Port"
#define MAX_CLUSTER_TAG	"Max_Cluster_Num"
#define USE_CONNECTION_POOL_TAG "Use_Connection_Pooling"
#define MAX_POOL_TAG	"Max_Pool_Each_Server"
#define BACKEND_SOCKET_DIR_TAG	"Backend_Socket_Dir"
#define CONNECTION_LIFE_TIME	"Connection_Life_Time"
#define NOT_USE_CONNECTION_POOL	(0)
#define USE_CONNECTION_POOL	(1)

#define PGR_SEND_RETRY_CNT (100)
#define PGR_SEND_WAIT_MSEC (500)
#define PGR_RECV_RETRY_CNT (100)
#define PGR_RECV_WAIT_MSEC (500)

extern int Recv_Port_Number;
extern int Recovery_Port_Number;
extern uint16_t LifeCheck_Port_Number;
extern int Use_Connection_Pool;
extern int Max_Pool;
extern int Connection_Life_Time;
extern int Msg_Id;
extern ClusterTbl * Cluster_Tbl;
extern int Max_DB_Server;
extern int MaxBackends;
extern char * Backend_Socket_Dir;
extern int ClusterShmid;
extern int ClusterSemid;
extern int ChildShmid;
extern int ClusterNum;
extern ChildTbl * Child_Tbl;
extern char * PGR_Data_Path;
extern char * PGR_Write_Path;
extern char * Backend_Socket_Dir;
extern FrontSocket Frontend_FD;
extern FILE * StatusFp;
extern char * ResolvedName;
extern char * PGRuserName;

/* for child.c */
extern POOL_CONNECTION * Frontend;
extern ClusterTbl * CurrentCluster;

extern char * Function;

extern POOL_CONNECTION_POOL *pool_connection_pool;	/* connection pool */
extern volatile sig_atomic_t backend_timer_expired;
extern char remote_ps_data[]; 


/* extern of main.c */
extern void PGRrecreate_child(int signal_args);
extern void PGRexit_subprocess(int sig);
extern void PGRsend_signal(int sig);

/* extern of child.c */
extern int PGRdo_child( int use_pool);
extern void pool_free_startup_packet(StartupPacket *sp);
extern int health_check(void);
extern int PGRpre_fork_children(ClusterTbl * ptr);
extern int PGRpre_fork_child(ClusterTbl * ptr);
extern int PGRcreate_child(ClusterTbl * cluster_p);
extern pid_t PGRscan_child_tbl(ClusterTbl * cluster_p);
extern void pool_free_startup_packet(StartupPacket *sp);
extern int health_check(void);
extern int  PGRpre_fork_children(ClusterTbl * ptr);
extern int PGRpre_fork_child(ClusterTbl * ptr);
extern int PGRcreate_child(ClusterTbl * cluster_p);
extern pid_t PGRscan_child_tbl(ClusterTbl * cluster_p);
extern int PGRset_status_to_child_tbl(pid_t pid, int status);
extern int PGRadd_child_tbl(ClusterTbl * cluster_p, pid_t pid, int status);
extern int PGRget_child_status(pid_t pid);
extern void PGRreturn_connection_full_error (void);
extern void PGRreturn_no_connection_error(void);
extern void PGRquit_children_on_cluster(int rec_no);
extern void notice_backend_error(int master);

/* extern of cluster_table.c */
extern int PGRis_cluster_alive(void) ;
extern ClusterTbl * PGRscan_cluster(void);
extern void PGRset_key_of_cluster(ClusterTbl * ptr, RecoveryPacket * packet);
extern ClusterTbl * PGRadd_cluster_tbl (ClusterTbl * conf_data);
extern ClusterTbl * PGRset_status_on_cluster_tbl (int status, ClusterTbl * ptr);
extern ClusterTbl * PGRsearch_cluster_tbl(ClusterTbl * conf_data);

/* extern of load_balance.c */
extern int PGRload_balance(void);
extern int PGRload_balance_with_pool(void);
extern char PGRis_connection_full(ClusterTbl * ptr);
extern void PGRuse_connection(ClusterTbl * ptr);
extern void PGRrelease_connection(ClusterTbl * ptr);
extern void PGRchild_wait(int sig);
//extern char PGRis_same_host(char * host1, char * host2);

/* extern of recovery.c */
extern void PGRrecovery_main(int fork_wait_fime);

/* extern of socket.c */
extern int PGRcreate_unix_domain_socket(char * sock_dir, unsigned short port);
extern int PGRcreate_recv_socket(char * hostName , unsigned short portNumber);
extern int PGRcreate_acception(int fd, char * hostName , unsigned short portNumber);
extern void PGRclose_sock(int * sock);
extern int PGRread_byte(int sock,char * buf,int len, int flag);
extern int PGRcreate_cluster_socket( int * sock, ClusterTbl * ptr );

/* extern of pool_auth.c */
extern int pool_do_auth(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *cp);
extern int pool_do_reauth(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *cp);
extern int pool_read_message_length(POOL_CONNECTION_POOL *cp);
extern int *pool_read_message_length2(POOL_CONNECTION_POOL *cp);
extern signed char pool_read_kind(POOL_CONNECTION_POOL *cp);
extern signed char pool_read_kind2(POOL_CONNECTION_POOL *cp);

/* extern of pool_connection_pool.c */
extern int pool_init_cp(void);
extern POOL_CONNECTION_POOL *pool_get_cp(char *user, char *database, int protoMajor, int check_socket);
extern void pool_discard_cp(char *user, char *database, int protoMajor);
extern POOL_CONNECTION_POOL *pool_create_cp(void);
extern void pool_connection_pool_timer(POOL_CONNECTION_POOL *backend);
extern void pool_backend_timer_handler(int sig);
extern void pool_backend_timer(void);
extern int connect_inet_domain_socket(int secondary_backend);
extern int connect_unix_domain_socket(int secondary_backend);

/* extern of pool_process_query.c */
extern POOL_STATUS pool_process_query(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend, int connection_reuse, int first_ready_for_query_received);
extern POOL_STATUS ErrorResponse(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend);
extern POOL_STATUS ErrorResponse2(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend);
extern POOL_STATUS NoticeResponse(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend);
extern void pool_enable_timeout(void); 
extern void pool_disable_timeout(void);
extern int pool_check_fd(POOL_CONNECTION *cp, int notimeout);
extern void pool_send_frontend_exits(POOL_CONNECTION_POOL *backend);
extern POOL_STATUS SimpleForwardToFrontend(char kind, POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend);
extern POOL_STATUS SimpleForwardToBackend(char kind, POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend);
extern POOL_STATUS ParameterStatus(POOL_CONNECTION *frontend, POOL_CONNECTION_POOL *backend);
extern void pool_send_error_message(POOL_CONNECTION *frontend, int protoMajor, char *code, char *message, char *detail, char *hint, char *file, int line);
extern void init_prepared_list(void);

/* extern of pool_params.c */
extern int pool_init_params(ParamStatus *params);
extern void pool_discard_params(ParamStatus *params);
extern char *pool_find_name(ParamStatus *params, char *name, int *pos);
extern int pool_get_param(ParamStatus *params, int index, char **name, char **value);
extern int pool_add_param(ParamStatus *params, char *name, char *value);
extern void pool_param_debug_print(ParamStatus *params);

/* extern of pool_stream.c */
extern POOL_CONNECTION *pool_open(int fd);
extern void pool_close(POOL_CONNECTION *cp);
extern int pool_read(POOL_CONNECTION *cp, void *buf, int len);
extern char *pool_read2(POOL_CONNECTION *cp, int len);
extern int pool_write(POOL_CONNECTION *cp, void *buf, int len);
extern int pool_flush_it(POOL_CONNECTION *cp);
extern int pool_flush(POOL_CONNECTION *cp);
extern int pool_write_and_flush(POOL_CONNECTION *cp, void *buf, int len);
extern char *pool_read_string(POOL_CONNECTION *cp, int *len, int line);
extern int pool_unread(POOL_CONNECTION *cp, void *data, int len);

/* extern of pool_ip.c */
extern void pool_getnameinfo_all(SockAddr *saddr, char *remote_host, char *remote_port);
extern int getaddrinfo_all(const char *hostname, const char *servname, const struct addrinfo * hintp, struct addrinfo ** result);
extern void freeaddrinfo_all(int hint_ai_family, struct addrinfo * ai);
extern int getnameinfo_all(const struct sockaddr_storage * addr, int salen, char *node, int nodelen, char *service, int servicelen, int flags);
#ifndef HAVE_GAI_STRERROR
extern const char * gai_strerror(int errcode);
#endif
extern int rangeSockAddr(const struct sockaddr_storage * addr, const struct sockaddr_storage * netaddr, const struct sockaddr_storage * netmask);
extern int SockAddr_cidr_mask(struct sockaddr_storage * mask, char *numbits, int family);

/*
 * external prototype in show.c
 */
extern void show_error(const char * fmt,...);
extern void show_debug(const char * fmt,...);
extern void PGRwrite_log_file(FILE * fp, const char * fmt,...);

/*
 * external prototype in lifecheck.c
 */
extern int PGRlifecheck_main(int fork_wait_time);

/*
 * external prototype in ps_status.c
 */
 extern char ** save_ps_display_args(int argc, char **argv);
 extern void init_ps_display(const char *username, const char *dbname,const char *host_info, const char *initial_str);
 extern void set_ps_display(const char *activity, bool force);
 extern const char * get_ps_display(int *displen);



#endif /* PGLB_H */
