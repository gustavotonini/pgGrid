/*-------------------------------------------------------------------------
 *
 * pg_grid_site.h
 *	  definition of the cluster site
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
#ifndef PG_GRID_SITE_H
#define PG_GRID_SITE_H

#define SiteRelationId	1270

CATALOG(pg_grid_site,1270) BKI_WITHOUT_OIDS
{
	Oid			siteid;			 /* site ID*/
	NameData	sitename;		 /* site name */
	Oid			siteowner;		 /* site owner */
	text		sitehostname;    /* site's host name or IP address*/
	int4		siteport;		 /* site's listening port number*/
	int4		siterecoveryport;/* site's recovery daemon listening port number*/
} FormData_pg_grid_site;

typedef FormData_pg_grid_site *Form_pg_grid_site;

/* ----------------
 *		compiler constants for pg_grid_site
 * ----------------
 */
#define Natts_pg_grid_site				6
#define Anum_pg_grid_site_oid   		1
#define Anum_pg_grid_site_name			2
#define Anum_pg_grid_site_owner			3
#define Anum_pg_grid_site_host  		4
#define Anum_pg_grid_site_port  		5
#define Anum_pg_grid_site_recport   	6

/* ----------------
 *		initial contents of pg_grid_site
 */
DATA(insert (  1	localhost	0	localhost	5432  6001 ));

#endif /* PG_GRID_SITE_H_ */
