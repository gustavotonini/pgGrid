#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <errno.h>

#define STATUS_OK (0)
#define STATUS_ERROR (-1)
int measure_bandwidth(char * target);
char * ping_to_target(char * target);
double get_result (char * ping_data);
double calculate_band(double msec);

static char Ping_Result[512];

int
main(int argc,char *argv[])
{

	int bw = 0;
	if (argc != 2)
	{
		fprintf(stderr,"Usage:%s target-host\n",argv[0]);
		exit(1);
	}
	bw = measure_bandwidth(argv[1]);
	printf("bandwidth is %d kbytes between %s\n",bw,argv[1]);
	return 1;
}

int
measure_bandwidth(char * target)
{
	double msec;
	double kbytes;
	char * ptr;
	ptr = ping_to_target(target);
	if (ptr != NULL)
	{
		msec = get_result(ptr);
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

char *
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
		status = execv("/bin/ping",args);
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

double
get_result (char * ping_data)
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

double
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
