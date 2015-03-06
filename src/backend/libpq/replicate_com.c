/*--------------------------------------------------------------------
 * FILE:
 *     replicate_com.c
 *
 * NOTE:
 *     This file is composed of the functions to call with the source
 *     at backend for the replication.
 *     Low level I/O functions that called by in these functions are 
 *     contained in 'replicate_com.c'.
 *
 *--------------------------------------------------------------------
 */

/*--------------------------------------
 * INTERFACE ROUTINES
 *
 * setup/teardown:
 *      PGR_Close_Sock
 *      PGR_Free_Conf_Data
 * I/O call:
 *      PGR_Create_Socket_Connect
 *      PGR_Create_Socket_Bind
 *      PGR_Create_Acception
 * table handling:
 *      PGR_Get_Conf_Data
 *-------------------------------------
 */
#ifdef USE_REPLICATION

#include "postgres.h"

#include <signal.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <unistd.h>
#include <ctype.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <netdb.h>
#include <netinet/in.h>
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif
#include <arpa/inet.h>
#include <sys/file.h>
#include <netdb.h>

#include "libpq/libpq.h"
#include "miscadmin.h"
#include "nodes/print.h"
#include "utils/guc.h"
#include "parser/parser.h"
#include "access/xact.h"
#include "replicate_com.h"

int PGR_Create_Socket_Connect(int * fdP, char * hostName , unsigned short portNumber);
void PGR_Close_Sock(int * sock);
int PGR_Create_Socket_Bind(int * fdP, char * hostName , unsigned short portNumber);
int PGR_Create_Acception(int fd, int * sockP, char * hostName , unsigned short portNumber);
int PGR_Free_Conf_Data(void);
int PGR_Get_Conf_Data(char * dir , char * fname);
void PGRset_recovery_packet_no(RecoveryPacket * packet, int packet_no);
extern bool PGRis_same_host(char * host1, unsigned short port1 , char * host2, unsigned short port2);
unsigned int PGRget_ip_by_name(char * host);
int PGRget_time_value(char *str);
int PGRget_bw_value(char *str);

static char * get_string(char * buf);
static bool is_start_tag(char * ptr);
static bool is_end_tag(char * ptr);
static void init_conf_data(ConfDataType *conf);
static int get_key(char * key, char * str);
static int get_conf_key_value(char * key, char * value , char * str);
static int add_conf_data(char *table,int rec_no, char *key,char * value);
static int get_table_data(FILE * fp,char * table, int rec_no);
static int get_single_data(char * str);
static int get_conf_file(char * fname);

/*--------------------------------------------------------------------
 * SYMBOL
 *     PGR_Create_Socket_Connect()
 * NOTES
 *     create new socket
 * ARGS
 *    int * fdP:
 *    char * hostName:
 *    unsigned short portNumber:
 * RETURN
 *    OK: STATUS_OK
 *    NG: STATUS_ERROR
 *--------------------------------------------------------------------
 */
int
PGR_Create_Socket_Connect(int * fdP, char * hostName , unsigned short portNumber)
{

	int sock;
	size_t	len = 0;
	struct sockaddr_in addr;
	int one = 1;

	if ((*hostName == '\0') || (portNumber < 1000))
	{
		* fdP = -1;
		return STATUS_ERROR;
	}
	if ((*fdP = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		* fdP = -1;
		return STATUS_ERROR;
	}
	if ((setsockopt(*fdP, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one))) == -1)
	{
		PGR_Close_Sock(fdP);
		return STATUS_ERROR;
	}
	if (setsockopt(*fdP, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof(one)) < 0)
	{
		PGR_Close_Sock(fdP);
		return STATUS_ERROR;
	}
	
	addr.sin_family = AF_INET;
	if ((hostName == NULL ) || (hostName[0] == '\0'))
   		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	else
	{
		struct hostent *hp;

		hp = gethostbyname(hostName);
		if ((hp == NULL) || (hp->h_addrtype != AF_INET))
		{
			PGR_Close_Sock(fdP);
			return STATUS_ERROR;
		}
		memmove((char *) &(addr.sin_addr), (char *) hp->h_addr, hp->h_length);
	}

	addr.sin_port = htons(portNumber);
	len = sizeof(struct sockaddr_in);
	
	if ((sock = connect(*fdP,(struct sockaddr*)&addr,len)) < 0)
	{
		PGR_Close_Sock(fdP);
		return STATUS_ERROR;
	}
	
	return	STATUS_OK;
}

int
PGR_Create_Socket_Bind(int * fdP, char * hostName , unsigned short portNumber)
{

	int err;
	size_t	len = 0;
	struct sockaddr_in addr;
	int one = 1;

	if ((*fdP = socket(AF_INET, SOCK_STREAM, 0)) < 0)
	{
		return STATUS_ERROR;
	}
	if ((setsockopt(*fdP, SOL_SOCKET, SO_REUSEADDR, (char *) &one, sizeof(one))) == -1)
	{
		PGR_Close_Sock(fdP);
		return STATUS_ERROR;
	}
	addr.sin_family = AF_INET;
	if ((hostName == NULL ) || (hostName[0] == '\0'))
		addr.sin_addr.s_addr = htonl(INADDR_ANY);
	else
	{
		struct hostent *hp;

		hp = gethostbyname(hostName);
		if ((hp == NULL) || (hp->h_addrtype != AF_INET))
		{
			PGR_Close_Sock(fdP);
			return STATUS_ERROR;
		}
		memmove((char *) &(addr.sin_addr), (char *) hp->h_addr, hp->h_length);
	}

	addr.sin_port = htons(portNumber);
	len = sizeof(struct sockaddr_in);
	
	err = bind(*fdP, (struct sockaddr *) & addr, len);
	if (err < 0)
	{
		PGR_Close_Sock(fdP);
		return STATUS_ERROR;
	}
	err = listen(*fdP, MAX_SOCKET_QUEUE );
	if (err < 0)
	{
		PGR_Close_Sock(fdP);
		return STATUS_ERROR;
	}
	return	STATUS_OK;
}

int
PGR_Create_Acception(int fd, int * sockP, char * hostName , unsigned short portNumber)
{
	int sock;
	struct sockaddr  addr;
	size_t	len = 0;
	int one = 1;

	len = sizeof(struct sockaddr);
	if ((sock = accept(fd, &addr, &len)) < 0)
	{
		*sockP = -1;
		return STATUS_ERROR;
	}
	
	if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, (char *) &one, sizeof(one)) < 0)
	{
		return STATUS_ERROR;
	}
	if (setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, (char *) &one, sizeof(one)) < 0)
	{
		return STATUS_ERROR;
	}
	*sockP = sock;

	return	STATUS_OK;
}

void
PGR_Close_Sock(int * sock)
{
	close( (int)*sock);
	*sock = -1;
}

static char *
get_string(char * buf)
{
	int i,len1,len2,start_flag;
	char *readp, *writep; 

	writep = readp = buf;
	i = len1 = 0;
	while (*(readp +i) != '\0')
	{
		if (!isspace(*(readp+ i)))
		{
			len1 ++;
		}
		i++;
	}
	start_flag = len2 = 0;
	while (*readp != '\0')
	{
		if (*readp == '#') 
		{
			*writep = '\0';
			break;
		}
		if (isspace(*readp))
		{
			if ((len2 >= len1) || (!start_flag))
			{
				readp++;
				continue;
			}
			*writep = *readp;
		}
		else
		{
			start_flag = 1;
			*writep = *readp;
			len2 ++;
		}
		readp ++;
		writep ++;
	}
	*writep = '\0';
	return buf;
}

static bool
is_start_tag(char * ptr)
{
	if ((*ptr == '<') && (*(ptr+1) != '/'))
	{
		return true;
	}
	return false;
}

static bool
is_end_tag(char * ptr)
{
	if ((*ptr == '<') && (*(ptr+1) == '/'))
	{
		return true;
	}
	return false;
}

static void
init_conf_data(ConfDataType *conf)
{
	memset(conf->table,0,sizeof(conf->table));
	memset(conf->key,0,sizeof(conf->key));
	memset(conf->value,0,sizeof(conf->value));
	conf->rec_no = 0;
	conf->last = NULL;
	conf->next = NULL;
}

static int
get_key(char * key, char * str)
{
	int offset = 1;
	char * ptr_s,*ptr_e;

	ptr_s = strchr(str,'<');
	if (ptr_s == NULL)
	{
		return STATUS_ERROR;
	}
	if (*(ptr_s+1) == '/')
	{
		offset = 2;
	}
	ptr_e = strchr(str,'>');
	if (ptr_e == NULL)
	{
		return STATUS_ERROR;
	}
	*ptr_e = '\0';
	strcpy(key,ptr_s + offset);
	*ptr_e = '>';
	return STATUS_OK;
}

static int
get_conf_key_value(char * key, char * value , char * str)
{
	int i;
	int len1,len2,start_flag;
	char * ptr_s,*ptr_e;

	if(get_key(key,str) == STATUS_ERROR)
	{
		return STATUS_ERROR;
	}
	ptr_e = strchr(str,'>');
	if (ptr_e == NULL)
	{
		return STATUS_ERROR;
	}
	ptr_s = ptr_e + 1;

	len1 = 0;
	while ((*ptr_s != '<') && (*ptr_s != '\0'))
	{
			if (! isspace(*ptr_s))
			{
				len1 ++;
			}
			ptr_s ++;
	}
	ptr_s = ptr_e + 1;
	i = len2 = start_flag = 0;
	while ((*ptr_s != '<') && (*ptr_s != '\0'))
	{
		if (isspace(*ptr_s))
		{
			if ((len2 >= len1) || (!start_flag))
			{
				ptr_s ++;
				continue;
			}
			*(value + i) = *ptr_s;
		}
		else
		{
			start_flag = 1;
			*(value + i) = *ptr_s;
			len2 ++;
		}
		i++;
		ptr_s ++;
	}
	*(value + i) = '\0';
	return STATUS_OK;
}

static int
add_conf_data(char *table,int rec_no, char *key,char * value)
{
	ConfDataType * conf_data;

	conf_data = (ConfDataType *)malloc(sizeof(ConfDataType));
	if (conf_data == NULL)
	{
		return STATUS_ERROR;
	}
	init_conf_data(conf_data);
	if (table != NULL)
	{
		memcpy(conf_data->table,table,sizeof(conf_data->table));
	}
	else
	{
		memset(conf_data->table,0,sizeof(conf_data->table));
	}
	memcpy(conf_data->key,key,sizeof(conf_data->key));
	memcpy(conf_data->value,value,sizeof(conf_data->value));
	conf_data->rec_no = rec_no;
	if (ConfData_Top == (ConfDataType *)NULL)
	{
		ConfData_Top = conf_data;
		conf_data->last = (char *)NULL;
	}
	if (ConfData_End == (ConfDataType *)NULL)
	{
		conf_data->last = (char *)NULL;
	}
	else
	{
		conf_data->last = (char *)ConfData_End;
		ConfData_End->next = (char *)conf_data;
	}
	ConfData_End = conf_data;
	conf_data->next = (char *)NULL;
	return STATUS_OK;
}

static int
get_table_data(FILE * fp,char * table, int rec_no)
{
	char buf[1024];
	char key_buf[1024];
	char value_buf[1024];
	int len = 0;
	char * ptr;

	while (fgets(buf,sizeof(buf),fp) != NULL)
	{
		/*
		 * pic up a data string
		 */
		ptr = get_string(buf);
		len = strlen(ptr);
		if (len == 0)
		{
			continue;
		}
		if (is_end_tag(ptr))
		{
			if(get_key(key_buf,ptr) == STATUS_ERROR)
			{
				return STATUS_ERROR;
			}
			if (!strcmp(key_buf,table))
			{
				return STATUS_OK;
			}
		}
		if (is_start_tag(ptr))
		{
			if(get_conf_key_value(key_buf,value_buf,ptr) == STATUS_ERROR)
			{
				return STATUS_ERROR;
			}
			add_conf_data(table,rec_no,key_buf,value_buf);
		}
	}
	return STATUS_ERROR;
}

static int
get_single_data(char * str)
{
	char key_buf[1024];
	char value_buf[1024];
	if(get_conf_key_value(key_buf,value_buf,str) == STATUS_ERROR)
	{
		return STATUS_ERROR;
	}
	add_conf_data(NULL,0,key_buf,value_buf);
	return STATUS_OK;
}


static int
get_conf_file(char * fname)
{
	FILE * fp = NULL;
	int len;
	char buf[1024];
	char key_buf[1024];
	char last_key_buf[1024];
	char *ptr;
	int rec_no = 0;

	/*
	 * configuration file open
	 */
	if ((fp = fopen(fname,"r")) == NULL)
	{
		return STATUS_ERROR;
	}
	/*
	 * configuration file read
	 */
	memset(last_key_buf,0,sizeof(last_key_buf));
	memset(key_buf,0,sizeof(key_buf));
	while (fgets(buf,sizeof(buf),fp) != NULL)
	{
		/*
		 * pic up a data string
		 */
		ptr = get_string(buf);
		len = strlen(ptr);
		if (len == 0)
		{
			continue;
		}
		if (is_start_tag(ptr))
		{
			if(get_key(key_buf,ptr) == STATUS_ERROR)
			{
				fclose(fp);
				return STATUS_ERROR;
			}
			if (strstr(ptr,"</") == NULL)
			{
				if (strcmp(last_key_buf,key_buf))
				{
					rec_no = 0;
					strcpy(last_key_buf,key_buf);
				}
				get_table_data(fp,key_buf,rec_no);
				rec_no ++;
			}
			else
			{
				get_single_data(ptr);
			}
		}
	}
	fclose(fp);
	return STATUS_OK;
}

int
PGR_Free_Conf_Data(void)
{
	ConfDataType * conf, *nextp;

	if (ConfData_Top == (ConfDataType *)NULL)
	{
		return STATUS_ERROR;
	}
	conf = ConfData_Top;

	while (conf != (ConfDataType *)NULL)
	{
		nextp = (ConfDataType*)conf->next;
		free (conf);
		conf = nextp;
	}
	ConfData_Top = ConfData_End = (ConfDataType *)NULL;
	return STATUS_OK;
}

int
PGR_Get_Conf_Data(char * dir , char * fname)
{

	int status;

	char * conf_file;
	if ((dir == NULL) || ( fname == NULL))
	{
		return STATUS_ERROR;
	}
	conf_file = malloc(strlen(dir) + strlen(fname) + 2);
	if (conf_file == NULL)
	{
		return STATUS_ERROR;
	}
	sprintf(conf_file,"%s/%s",dir,fname);

	ConfData_Top = ConfData_End = (ConfDataType * )NULL;
	status = get_conf_file(conf_file);
	free (conf_file);
	conf_file = NULL;

	return status;
}

void
PGRset_recovery_packet_no(RecoveryPacket * packet, int packet_no)
{
	if (packet == NULL)
	{
		return;
	}
	packet->packet_no = htons(packet_no) ;

}

bool
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
		//show_debug("%s:target host",func);
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
}

unsigned int
PGRget_ip_by_name(char * host)
{
	struct hostent *hp = NULL;
	unsigned int ip = 0;
	unsigned char uc = 0;
	int i;

	if ((host == NULL) || (*host == '\0'))
	{
		return 0;
	}
	hp = gethostbyname( host );
	if (hp == NULL)
	{
		return 0;
	}
	for (i = 3 ; i>= 0 ; i --)
	{
		uc = (unsigned char)hp->h_addr_list[0][i];
		ip = ip | uc;
		if (i > 0)
		ip = ip << 8;
	}
	return ip;
}

int
PGRget_time_value(char *str)
{
	int i,len;
	char * ptr;
	int unit = 1;
	
	if (str == NULL)
		return -1;

	len = strlen(str);
	ptr = str;
	for (i = 0; i < len ; i ++,ptr++)
	{
		if ((! isdigit(*ptr)) && (! isspace(*ptr)))
		{
			switch (*ptr)
			{
				case 'm':
				case 'M':
					unit = 60;
					break;
				case 'h':
				case 'H':
					unit = 60*60;
					break;
			}
			*ptr = '\0';
			break;
		}
	}
	return (atoi(str) * unit);
}

int
PGRget_bw_value(char *str)
{
	int i,len;
	char * ptr;
	int unit = 1;
	
	if (str == NULL)
		return -1;

	len = strlen(str);
	ptr = str;
	for (i = 0; i < len ; i ++,ptr++)
	{
		if ((! isdigit(*ptr)) && (! isspace(*ptr)))
		{
			switch (*ptr)
			{
				/* MByte */
				case 'm':
				case 'M':
					unit = 1024;
					break;
				/* GByte */
				case 'g':
				case 'G':
					unit = 1024*1024;
					break;
			}
			*ptr = '\0';
			break;
		}
	}
	return (atoi(str) * unit);
}

#endif /* USE_REPLICATION */
