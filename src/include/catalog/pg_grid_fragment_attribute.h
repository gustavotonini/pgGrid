/*-------------------------------------------------------------------------
 *
 * pg_grid_fragment_attribute.h
 *	  definition of the relation fragment attribute
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
#ifndef PG_GRID_FRAGMENT_ATT_H
#define PG_GRID_FRAGMENT_ATT_H


#define FragmentAttRelationId	1272

CATALOG(pg_grid_fragment_attribute,1272) BKI_WITHOUT_OIDS
{
	Oid			fragmentid;		 /* fragment ID*/
	int2		attnum;		     /* attribute number */
} FormData_pg_grid_fragment_attribute;

typedef FormData_pg_grid_fragment_attribute *Form_pg_grid_fragment_attribute;

/* ----------------
 *		compiler constants for pg_grid_fragment_attribute
 * ----------------
 */
#define Natts_pg_grid_attribute						2
#define Anum_pg_grid_attribute_fragment_oid   		1
#define Anum_pg_grid_attribute_attnum   			2


#endif /* PG_GRID_FRAGMENT_ATT_H */
