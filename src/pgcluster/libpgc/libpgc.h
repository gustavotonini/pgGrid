/*-------------------------------------------------------------------------
 *
 * lilbpgc.h
 *	  external definition of the function for pgreplicate and pglb
 *
 * This should be the first file included by replicate modules.
 *
 *-------------------------------------------------------------------------
 */
#ifndef LIBPGC_H
#define	LIBPGC_H

#include <stdio.h>

/* character length of IP address */
#define ADDRESS_LENGTH	(24)

/* logging file data tag in configuration file */
#define	LOG_INFO_TAG	"Log_File_Info"
#define	FILE_NAME_TAG	"File_Name"
#define	FILE_SIZE_TAG	"File_Size"
#define	LOG_ROTATION_TAG	"Rotate"

typedef struct {
	char file_name[256];
	FILE * fp;
	int max_size;
	int rotation;
} LogFileInf;

extern LogFileInf * LogFileData;
/* external definition of the function in sem.c */
extern void PGRsem_unlock( int semid, short sem_num );
extern void PGRsem_lock( int semid, short sem_num );

/* external definition of the function in show.c */
extern FILE * PGRopen_log_file(char * fname, int max_size, int rotation);
extern void PGRclose_log_file(FILE * fp);
extern void show_debug(const char * fmt,...);
extern void show_error(const char * fmt,...);
extern void PGRwrite_log_file(FILE * fp, const char * fmt,...);
/* external definition of the function in signal.c */
typedef void (*PGRsighandler)(int);
extern void PGRinitmask (void);
extern PGRsighandler PGRsignal(int signo, PGRsighandler func);

#endif /* LIBPGC_H */
