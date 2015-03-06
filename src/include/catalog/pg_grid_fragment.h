/*-------------------------------------------------------------------------
 *
 * pg_grid_fragment.h
 *	  definition of the table fragment
 *
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/include/catalog/pg_class.h,v 1.104 2008/01/01 19:45:56 momjian Exp $
 *
 * NOTES
 *	  the genbki.sh script reads this file and generates .bki
 *	  information from the DATA() statements.
 *
 *-------------------------------------------------------------------------*/
#ifndef PG_GRID_FRAGMENT_H
#define PG_GRID_FRAGMENT_H

#define FragmentRelationId	1271

CATALOG(pg_grid_fragment,1271) BKI_WITHOUT_OIDS
{
	Oid			fragmentid;		     /* fragment ID*/
	NameData	fragmentname;	     /* fragment name */
	Oid			fragmentowner;	     /* fragment owner */
	Oid			relid;	 		     /* fragment relation (from pg_class) */
	text        fragmentWhereClause; /* where clause which generates fragmented data*/
} FormData_pg_grid_fragment;

/* ----------------
 *		compiler constants for pg_grid_site
 * ----------------
 */
#define Natts_pg_grid_fragment			5
#define Anum_pg_grid_fragment_oid  		1
#define Anum_pg_grid_fragment_name		2
#define Anum_pg_grid_fragment_owner		3
#define Anum_pg_grid_fragment_relid		4
#define Anum_pg_grid_fragment_where		5

typedef FormData_pg_grid_fragment *Form_pg_grid_fragment;


#endif /* PG_GRID_FRAGMENT_H */
