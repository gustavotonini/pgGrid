/*-------------------------------------------------------------------------
 *
 * assert.c
 *	  Assert code.
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/utils/error/assert.c,v 1.35 2008/01/01 19:45:53 momjian Exp $
 *
 * NOTE
 *	  This should eventually work with elog()
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include <unistd.h>

#ifdef USE_REPLICATION
#include "replicate.h"
#endif /* USE_REPLICATION */

/*
 * ExceptionalCondition - Handles the failure of an Assert()
 *
 * Note: this can't actually return, but we declare it as returning int
 * because the TrapMacro() macro might get wonky otherwise.
 */
int
ExceptionalCondition(const char *conditionName,
					 const char *errorType,
					 const char *fileName,
					 int lineNumber)
{
	if (!PointerIsValid(conditionName)
		|| !PointerIsValid(fileName)
		|| !PointerIsValid(errorType))
		write_stderr("TRAP: ExceptionalCondition: bad arguments\n");
	else
	{
		write_stderr("TRAP: %s(\"%s\", File: \"%s\", Line: %d)\n",
					 errorType, conditionName,
					 fileName, lineNumber);
	}

	/* Usually this shouldn't be needed, but make sure the msg went out */
	fflush(stderr);

#ifdef USE_REPLICATION
	if ((PGR_Check_Lock.dest == TO_REPLICATION_SERVER ) &&
		(PGR_Need_Notice == true))
	{
		PGR_Notice_Transaction_Query_Aborted();
	}
	if (PGR_Copy_Data_Need_Replicate)
	{
		PGR_Set_Copy_Data(PGRCopyData,NULL,0,1);
	}
#endif /* USE_REPLICATION */

#ifdef SLEEP_ON_ASSERT

	/*
	 * It would be nice to use pg_usleep() here, but only does 2000 sec or 33
	 * minutes, which seems too short.
	 */
	sleep(1000000);
#endif

	abort();

	return 0;
}
