/*--------------------------------------------------------------------
 * FILE:
 *     sem.c
 *
 * NOTE:
 *     This file is composed of the functions to call with the source
 *     at pgreplicate for the semapho control.
 *
 * Portions Copyright (c) 2003-2008, Atsushi Mitani
 *--------------------------------------------------------------------
 */
#include <stdio.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <sys/ipc.h>
#include <sys/sem.h>
#include <signal.h>

extern void show_debug(const char * fmt,...);

void PGRsem_unlock( int semid, short sem_num );
void PGRsem_lock( int semid, short sem_num );

#define PGR_SEM_UNLOCK_WAIT_MSEC (100)
#define PGR_SEM_LOCK_WAIT_MSEC (500)

void
PGRsem_unlock( int semid, short sem_num )
{
	int	status = 0;
	struct sembuf sops;

	sops.sem_num = sem_num;
	sops.sem_op = 1;
	/*sops.sem_flg = IPC_NOWAIT;*/
	sops.sem_flg = 0;
	do
	{
		status = semop(semid, &sops, 1);
		if ((status == -1) && (errno != EINTR))
		{
			usleep(PGR_SEM_UNLOCK_WAIT_MSEC);
		}
	} while (status == -1);
}

void
PGRsem_lock( int semid, short sem_num )
{
	int	status = 0;
	struct sembuf sops;

	sops.sem_num = sem_num;
	sops.sem_op = -1;
	/*sops.sem_flg = IPC_NOWAIT;*/
	sops.sem_flg = 0;
	do
	{
		status = semop(semid, &sops, 1);
		if ((status == -1) && (errno != EINTR))
		{
			usleep(PGR_SEM_LOCK_WAIT_MSEC);
		}
	} while (status == -1);
}

