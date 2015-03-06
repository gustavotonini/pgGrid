/*-------------------------------------------------------------------------
 *
 * place.c
 *	  Commands for placing fragments into servers
 *
 * Portions Copyright (c) 1996-2008, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * $PostgreSQL: pgsql/src/backend/commands/user.c,v 1.178 2008/01/01 19:45:49 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/catalog.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_grid_site.h"
#include "catalog/pg_grid_fragment.h"
#include "catalog/pg_grid_allocation.h"
#include "commands/place.h"
#include "libpq/md5.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/flatfiles.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"

/*
 * PLACE
 */
void
Place(PlaceStmt *stmt)
{
	Relation	pg_allocation_rel;
	TupleDesc	pg_allocation_dsc;
	HeapTuple	tuple, fragmenttuple, sitetuple;
	Datum		new_record[Natts_pg_grid_allocation];
	char		new_record_nulls[Natts_pg_grid_allocation];
	Oid fragmentid, siteid;
	Form_pg_grid_fragment fragment;
	Form_pg_grid_site site;
	
	/* Check some permissions first */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to allocate data fragments")));
	
	fragmenttuple = SearchSysCache(FRAGMENTNAME,
						   CStringGetDatum(stmt->fragment),
						   0, 0, 0);
	if (!HeapTupleIsValid(fragmenttuple))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("fragment \"%s\" does not exists",
						stmt->fragment)));
	
	sitetuple = SearchSysCache(SITENAME,
						   CStringGetDatum(stmt->server),
						   0, 0, 0);
	if (!HeapTupleIsValid(sitetuple))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("server \"%s\" does not exists",
						stmt->server)));	

	pg_allocation_rel = heap_open(AllocationRelationId, RowExclusiveLock);
	pg_allocation_dsc = RelationGetDescr(pg_allocation_rel);

	
	/*Build a tuple to insert*/
	 
	MemSet(new_record, 0, sizeof(new_record));
	
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));
	
	site = (Form_pg_grid_site) GETSTRUCT(sitetuple);
	fragment = (Form_pg_grid_fragment) GETSTRUCT(fragmenttuple);
	
	new_record[Anum_pg_grid_allocation_site_id - 1] =
		ObjectIdGetDatum(site->siteid);
	
	new_record[Anum_pg_grid_allocation_frag_id - 1] =
		ObjectIdGetDatum(fragment->fragmentid);
	
	tuple = heap_formtuple(pg_allocation_dsc, new_record, new_record_nulls);

	//Insert new record in the pg_grid_allocation table
	simple_heap_insert(pg_allocation_rel, tuple);
	CatalogUpdateIndexes(pg_allocation_rel, tuple);

	// Advance command counter so we can see new record;
	 
	CommandCounterIncrement();
	
	/*
	 * Close pg_grid_fragment, but keep lock till commit (this is important to
	 * prevent any risk of deadlock failure while updating flat file)
	 */
	heap_close(pg_allocation_rel, NoLock);
	
	ReleaseSysCache(fragmenttuple);
	ReleaseSysCache(sitetuple);
}

/*TODO: REMOVE*/
