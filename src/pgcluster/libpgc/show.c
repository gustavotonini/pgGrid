/*--------------------------------------------------------------------
 * FILE:
 *     show.c
 *
 * NOTE:
 *     This file is composed of the logging and debug functions
 *
 * Portions Copyright (c) 2003-2008, Atsushi Mitani
 *--------------------------------------------------------------------
 */
#include <sys/time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <errno.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include "libpgc.h"

#define TIMESTAMP_SIZE 19		/* format `YYYY-MM-DD HH:MM:SS' */

/*--------------------------------------
 * PROTOTYPE DECLARATION
 *--------------------------------------
 */
static char* get_current_timestamp(void);
static int file_rotation(char * fname, int max_rotation);

FILE * PGRopen_log_file(char * fname, int max_size, int rotation);
void PGRclose_log_file(FILE * fp);
void show_debug(const char * fmt,...);
void show_error(const char * fmt,...);
void PGRwrite_log_file(FILE * fp, const char * fmt,...);

extern int Debug_Print;
extern int Log_Print;

LogFileInf * LogFileData = NULL;

static char*
get_current_timestamp(void)
{
	time_t now;
	static char buf[TIMESTAMP_SIZE + 1];

	now = time(NULL);
	strftime(buf, sizeof(buf),
		 "%Y-%m-%d %H:%M:%S", localtime(&now));
	return buf;
}

void
show_debug(const char * fmt,...)
{
	va_list ap;
	char *timestamp;
	char buf[256];

	if (Debug_Print)
	{
		timestamp = get_current_timestamp();
		fprintf(stdout,"%s [%d] DEBUG:",timestamp, getpid());
		va_start(ap,fmt);
		vfprintf(stdout,fmt,ap);
		va_end(ap);
		fprintf(stdout,"\n");
		fflush(stdout);
		if ((Log_Print) && (LogFileData != NULL))
		{
			FILE * fp = NULL;
			fp = PGRopen_log_file(LogFileData->file_name, LogFileData->max_size, LogFileData->rotation);
			va_start(ap,fmt);
			vsnprintf(buf,sizeof(buf),fmt,ap);
			va_end(ap);
			PGRwrite_log_file(fp, buf);
			PGRclose_log_file(fp);
		}
	}
}

void
show_error(const char * fmt,...)
{
	va_list ap;
	char buf[256], *timestamp;

	if (Debug_Print)
	{
		timestamp = get_current_timestamp();
		fprintf(stderr,"%s [%d] ERROR:",timestamp, getpid());
		va_start(ap,fmt);
		vfprintf(stderr,fmt,ap);
		va_end(ap);
		fprintf(stderr,"\n");
		fflush(stderr);
	}
	if ((Log_Print) && (LogFileData != NULL))
	{
		FILE * fp = NULL;
		fp = PGRopen_log_file(LogFileData->file_name, LogFileData->max_size, LogFileData->rotation);
		va_start(ap,fmt);
		vsnprintf(buf,sizeof(buf),fmt,ap);
		va_end(ap);
		PGRwrite_log_file(fp, buf);
		PGRclose_log_file(fp);
	}
}

void
PGRwrite_log_file(FILE * fp, const char * fmt,...)
{
	char buf[256];
	char log[288];
	char * p;
	va_list ap;
	time_t t;

	if (fp == NULL)
	{
		return;
	}
	if (time(&t) < 0)
	{
		return;
	}
	snprintf(log,sizeof(log),"%s ",ctime(&t));
	p = strchr(log,'\n');
	if (p != NULL)
	{
		*p = ' ';
	}
	va_start(ap,fmt);
	vsnprintf(buf,sizeof(buf),fmt,ap);
	va_end(ap);
	strcat(log,buf);
	strcat(log,"\n");
	if (fputs(log,fp) >= 0)
	{
		fflush(fp);
	}
}

FILE *
PGRopen_log_file(char * fname, int max_size, int rotation)
{
	int rtn;
	struct stat st;

	if (fname == NULL)
	{
		return (FILE *)NULL;
	}

	if (max_size > 0)
	{
		rtn = stat(fname,&st);
		if (rtn == 0)
		{
			if (st.st_size > max_size)
			{
				if (file_rotation(fname, rotation) < 0)
				{
					return (FILE *)NULL;
				}
			}
		}
	}
	return (fopen(fname,"a"));
}

void
PGRclose_log_file(FILE * fp)
{
	if (fp != NULL)
	{
		fflush(fp);
		fclose(fp);
	}
}

static int
file_rotation(char * fname, int max_rotation)
{
	char * func = "file_rotation()";
	int i;
	int rtn;
	struct stat st;
	char old_fname[256];
	char new_fname[256];

	if ((fname == NULL) || (max_rotation < 0))
	{
		return -1;
	}

	for ( i = max_rotation ; i > 1 ; i -- )
	{
		sprintf(old_fname,"%s.%d",fname,i-1);
		rtn = stat(old_fname,&st);
		if (rtn == 0)
		{
			sprintf(new_fname,"%s.%d",fname,i);
			rtn = rename(old_fname, new_fname);
			if (rtn < 0)
			{
				show_error("%s:rotate failed: (%s)",func,strerror(errno));
				return rtn;
			}
		}
	}
	if (max_rotation > 0)
	{
		sprintf(new_fname,"%s.1",fname);
		rtn = rename(fname, new_fname);
	}
	else
	{
		rtn = unlink(fname);
	}

	return rtn;
}

