/*-------------------------------------------------------------------------
 *
 * server.c
 *	  Commands for manipulating servers
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
#include "commands/comment.h"
#include "commands/server.h"
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
 * CREATE SERVER
 */
void
CreateServer(CreateServerStmt *stmt)
{
	Relation	pg_site_rel;
	TupleDesc	pg_site_dsc;
	HeapTuple	tuple;
	Datum		new_record[Natts_pg_grid_site];
	char		new_record_nulls[Natts_pg_grid_site];
	Oid siteid;

	/* Check some permissions first */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create servers")));

	pg_site_rel = heap_open(SiteRelationId, RowExclusiveLock);
	pg_site_dsc = RelationGetDescr(pg_site_rel);

	tuple = SearchSysCache(SITENAME,
						   CStringGetDatum(stmt->server),
						   0, 0, 0);
	if (HeapTupleIsValid(tuple)){
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("server \"%s\" already exists",
						stmt->server)));
	}					

	/*
	 * Build a tuple to insert
	 */
	MemSet(new_record, 0, sizeof(new_record));
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));

	new_record[Anum_pg_grid_site_name - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->server));
	
	
	new_record[Anum_pg_grid_site_host - 1] =
		DirectFunctionCall1(textin, CStringGetDatum(stmt->host));	
	
	new_record[Anum_pg_grid_site_owner - 1] =
		ObjectIdGetDatum(GetUserId());
	
	new_record[Anum_pg_grid_site_oid - 1] =
		ObjectIdGetDatum(GetNewOid(pg_site_rel));	

	new_record[Anum_pg_grid_site_port - 1] = Int32GetDatum(stmt->port);
	new_record[Anum_pg_grid_site_recport - 1] = Int32GetDatum(stmt->recoveryport);
	
	tuple = heap_formtuple(pg_site_dsc, new_record, new_record_nulls);

	/*
	 * Insert new record in the pg_grid_site table
	 */
	siteid = simple_heap_insert(pg_site_rel, tuple);
	CatalogUpdateIndexes(pg_site_rel, tuple);

	/*
	 * Advance command counter so we can see new record;
	 */
	CommandCounterIncrement();
	
	/*
	 * Close pg_grid_site, but keep lock till commit (this is important to
	 * prevent any risk of deadlock failure while updating flat file)
	 */
	heap_close(pg_site_rel, NoLock);
}

void DropServer(DropServerStmt *stmt){
	char	   *servername = stmt->servername;
	HeapScanDesc scandesc;
	Relation	rel;
	HeapTuple	tuple;
	ScanKeyData entry[1];
	Oid			serveroid;

	/*
	 * Find the target tuple
	 */
	rel = heap_open(SiteRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_grid_site_name,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(servername));
	scandesc = heap_beginscan(rel, SnapshotNow, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	if (!HeapTupleIsValid(tuple))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("server \"%s\" does not exist",
						servername)));
		return;
	}

	serveroid = HeapTupleGetOid(tuple);

	/* TODO: Must be server owner 
	if (!pg_tablespace_ownercheck(tablespaceoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TABLESPACE,
					   tablespacename);*/

	/*
	 * Remove the pg_grid_site tuple (this will roll back if we fail below)
	 */
	simple_heap_delete(rel, &tuple->t_self);

	heap_endscan(scandesc);

	/*
	 * Remove any comments on this server.
	 */
	DeleteSharedComments(serveroid, SiteRelationId);

	/*
	 * Remove dependency on owner.
	 */
	deleteSharedDependencyRecordsFor(SiteRelationId, serveroid);

	/* We keep the lock on pg_grid_site until commit */
	heap_close(rel, NoLock);
}
