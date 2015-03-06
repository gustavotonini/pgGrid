/*-------------------------------------------------------------------------
 *
 * fragment.c
 *	  Commands for manipulating fragments
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
#include "catalog/pg_auth_members.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_grid_fragment.h"
#include "catalog/pg_grid_fragment_attribute.h"
#include "commands/comment.h"
#include "commands/fragment.h"
#include "libpq/md5.h"
#include "miscadmin.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/flatfiles.h"
#include "utils/fmgroids.h"
#include "utils/guc.h"
#include "utils/rel.h"
#include "utils/lsyscache.h"
#include "utils/syscache.h"
#include "parser/parser.h"
#include "parser/parse_node.h"
#include <string.h>

//find fragment where clause inside user command
static
Datum GetWhereClause(SelectStmt *stmt, Relation relation);



static
Datum GetWhereClause(SelectStmt *stmt, Relation relation){
	if (stmt->whereClause){
		ParseState *pstate = make_parsestate(NULL);
		
		RangeTblEntry *rte = addRangeTableEntryForRelation(pstate,
											relation,
											NULL,
											false,
											true);
		addRTEtoQuery(pstate, rte, false, true, true);
	
		Node	   *expr = transformExpr(pstate, stmt->whereClause);	
		
		expr = coerce_to_boolean(pstate, expr, "CHECK");
	
		return CStringGetDatum(nodeToString(expr));
	}else{
		return CStringGetDatum("");
	}
}


void
InsertFragmentAttribute(Oid fragmentid, Oid relid, const char *attname){
	HeapTuple tuple, atttuple;
	Relation	pg_fragment_att_rel;
	TupleDesc	pg_fragment_att_dsc;
	Datum		new_record[Natts_pg_grid_attribute];
	char		new_record_nulls[Natts_pg_grid_attribute];	
	Form_pg_attribute attribute;
	
	atttuple = SearchSysCacheAttName(relid, attname);
	
	if (!atttuple){
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("attribute \"%s\" does not exists",
						attname)));
	}else{			
		pg_fragment_att_rel = heap_open(FragmentAttRelationId, RowExclusiveLock);
		pg_fragment_att_dsc = RelationGetDescr(pg_fragment_att_rel);
		
		
		attribute = (Form_pg_attribute) GETSTRUCT(atttuple);

		/*
		 * Build a tuple to insert
		 */
		MemSet(new_record, 0, sizeof(new_record));
		
		MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));
		
		new_record[Anum_pg_grid_attribute_fragment_oid - 1] =
			ObjectIdGetDatum(fragmentid);
		
		new_record[Anum_pg_grid_attribute_attnum - 1] =
			Int32GetDatum(attribute->attnum);	

		tuple = heap_formtuple(pg_fragment_att_dsc, new_record, new_record_nulls);

		/*
		 * Insert new record in the pg_grid_fragment_attribute table
		 */
		simple_heap_insert(pg_fragment_att_rel, tuple);
		CatalogUpdateIndexes(pg_fragment_att_rel, tuple);

		/*
		 * Advance command counter so we can see new record;
		 */
		CommandCounterIncrement();
		
		/*
		 * Close pg_grid_fragment_atttribute, but keep lock still commit (this is important to
		 * prevent any risk of deadlock failure while updating flat file)
		 */
		heap_close(pg_fragment_att_rel, NoLock);
		
	}
	ReleaseSysCache(atttuple);	
}

/*
 * CREATE FRAGMENT
 */
void
CreateFragment(CreateFragmentStmt *stmt, const char *queryString)
{
	Relation	pg_fragment_rel;
	Relation target;
	TupleDesc	pg_fragment_dsc;
	HeapTuple	tuple;
	Datum		new_record[Natts_pg_grid_fragment];
	char		new_record_nulls[Natts_pg_grid_fragment];
	Oid fragmentid, fragmentrelid;
	ListCell   *targetList;
	
	/* Check some permissions first */
	if (!superuser())
		ereport(ERROR,
				(errcode(ERRCODE_INSUFFICIENT_PRIVILEGE),
				 errmsg("must be superuser to create data fragments")));

	pg_fragment_rel = heap_open(FragmentRelationId, RowExclusiveLock);
	pg_fragment_dsc = RelationGetDescr(pg_fragment_rel);

	tuple = SearchSysCache(FRAGMENTNAME,
						   CStringGetDatum(stmt->fragment),
						   0, 0, 0);
	if (HeapTupleIsValid(tuple)){
		ReleaseSysCache(tuple);
		ereport(ERROR,
				(errcode(ERRCODE_DUPLICATE_OBJECT),
				 errmsg("fragment \"%s\" already exists",
						stmt->fragment)));
	}

	/*
	 * Build a tuple to insert
	 */
	MemSet(new_record, 0, sizeof(new_record));
	
	MemSet(new_record_nulls, ' ', sizeof(new_record_nulls));

	new_record[Anum_pg_grid_fragment_name - 1] =
		DirectFunctionCall1(namein, CStringGetDatum(stmt->fragment));
	
	new_record[Anum_pg_grid_fragment_owner - 1] =
		ObjectIdGetDatum(GetUserId());
	
	new_record[Anum_pg_grid_fragment_oid - 1] =
		ObjectIdGetDatum(fragmentid=GetNewOid(pg_fragment_rel));	

	//Get target relation ID
	fragmentrelid=RelnameGetRelid(stmt->select_clause->fromClause);
	if (!fragmentrelid){
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_NAME),
				 errmsg("relation \"%s\" does not exists",
						stmt->select_clause->fromClause)));
	}
	/*Open target relation*/
	target = heap_open(fragmentrelid, NoLock);
	new_record[Anum_pg_grid_fragment_relid - 1] = 		
		ObjectIdGetDatum(fragmentrelid);
	
	//Link where clause to fragment	
	new_record[Anum_pg_grid_fragment_where - 1] = 
		DirectFunctionCall1(textin, GetWhereClause(stmt->select_clause, target));
	
	tuple = heap_formtuple(pg_fragment_dsc, new_record, new_record_nulls);
	/*
	 * Insert new record in the pg_grid_site table
	 */
	simple_heap_insert(pg_fragment_rel, tuple);
	CatalogUpdateIndexes(pg_fragment_rel, tuple);

	/*
	 * Advance command counter so we can see new record;
	 */
	CommandCounterIncrement();
	
	/*
	 * Close pg_grid_fragment, but keep lock till commit (this is important to
	 * prevent any risk of deadlock failure while updating flat file)
	 */
	heap_close(pg_fragment_rel, NoLock);

	/*Close target relation*/
	heap_close(target, NoLock);
	
	/*Insert fragment attributes...*/
	
	if (stmt->select_clause && stmt->select_clause->targetList){		
		foreach(targetList, stmt->select_clause->targetList)
		{
			ResTarget *res= (ResTarget *) lfirst(targetList);
			if (IsA(res->val, ColumnRef) &&
				list_length(((ColumnRef *) res->val)->fields) == 1){
					

				//it may enter here only one time					
				ColumnRef *column = res->val;
				char *attname = strVal(linitial((column)->fields));				
				InsertFragmentAttribute(fragmentid, fragmentrelid,
						attname);
			}else{
				ereport(WARNING,
						(errmsg("no name from attribute statement",
								NULL)));
			}
		}	
	}else{
		ereport(NOTICE,
				(errmsg("All attributes will be fragmented",
						NULL)));							
	}

}

void 
DropFragment(DropFragmentStmt *stmt, const char *queryString){
	char	   *fragmentname = stmt->fragmentname;
	HeapScanDesc scandesc;
	Relation	rel;
	HeapTuple	tuple;
	ScanKeyData entry[1];
	Oid			fragmentoid;

	/*
	 * Find the target tuple
	 */
	rel = heap_open(FragmentRelationId, RowExclusiveLock);

	ScanKeyInit(&entry[0],
				Anum_pg_grid_fragment_name,
				BTEqualStrategyNumber, F_NAMEEQ,
				CStringGetDatum(fragmentname));
	scandesc = heap_beginscan(rel, SnapshotNow, 1, entry);
	tuple = heap_getnext(scandesc, ForwardScanDirection);

	if (!HeapTupleIsValid(tuple))
	{
		ereport(ERROR,
				(errcode(ERRCODE_UNDEFINED_OBJECT),
				 errmsg("fragment \"%s\" does not exist",
						fragmentname)));
		return;
	}

	fragmentoid = HeapTupleGetOid(tuple);

	/* TODO: Must be fragment owner 
	if (!pg_tablespace_ownercheck(tablespaceoid, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, ACL_KIND_TABLESPACE,
					   tablespacename);*/

	/*
	 * Remove the pg_fragment tuple (this will roll back if we fail below)
	 */
	simple_heap_delete(rel, &tuple->t_self);

	heap_endscan(scandesc);

	/*
	 * Remove any comments on this fragment.
	 */
	DeleteSharedComments(fragmentoid, FragmentRelationId);

	/*
	 * Remove dependency on owner.
	 */
	deleteSharedDependencyRecordsFor(FragmentRelationId, fragmentoid);

	/* We keep the lock on pg_fragment until commit */
	heap_close(rel, NoLock);
}
