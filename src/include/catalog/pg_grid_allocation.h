/*-------------------------------------------------------------------------
 *
 * pg_grid_allocation.h
 *	  definition of the fragment allocation rules
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------*/
#ifndef PG_GRID_ALLOCATION_H
#define PG_GRID_ALLOCATION_H

#define AllocationRelationId	1273

CATALOG(pg_grid_allocation,1273) BKI_WITHOUT_OIDS
{
	Oid			siteid; 		 /* site ID*/
	Oid			fragmentid;		 /* fragment id */
} FormData_pg_grid_allocation;

typedef FormData_pg_grid_allocation *Form_pg_grid_allocation;

/* ----------------
 *		compiler constants for pg_grid_allocation
 * ----------------
 */
#define Natts_pg_grid_allocation		2
#define Anum_pg_grid_allocation_site_id	1
#define Anum_pg_grid_allocation_frag_id 2

#endif /* PG_GRID_ALLOCATION_H */
