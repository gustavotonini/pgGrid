#include "pggrid/dquery.h"
#include "pggrid/allocation.h"
#include "utils/syscache.h"
#include "utils/catcache.h"
#include "utils/rel.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "utils/builtins.h"

#include "catalog/pg_grid_fragment.h"
#include "catalog/pg_grid_site.h"
#include "catalog/pg_grid_allocation.h"

#include "executor/executor.h"

#include "nodes/nodes.h"
#include "nodes/parsenodes.h"

#include "parser/parser.h"
#include "parser/analyze.h"

#include "optimizer/clauses.h"

#include "tcop/dest.h"
#include "tcop/utility.h"

#include "replicate.h"

#include "postgres.h"

#include "postmaster/postmaster.h"

#include <string.h>

static 
int SendCountOperation(HeapTuple sitetup, Form_pg_grid_site site, int operation, Oid relationId);
static
bool RetrieveRelationTuplesFromSite(HeapTuple sitetup, Form_pg_grid_site site, Index relid, Relation targetTempRelation);

static
Relation CreateTemporaryRelation(Index relid);


int ExecCountForeignTuples(Index relid){
	int i, j;
	int rv=0, count;
	int fragment_count;
	CatCList *fragmentList;

	elog(NOTICE, "PGGRID: ExecCountForeignTuples relid %u", relid);	
	fragmentList = SearchSysCacheList(FRAGMENTRELID, 1, relid,  0, 0, 0);
	fragment_count=fragmentList->n_members;
	
	for (i = 0; i < fragmentList->n_members; i++)
	{
		
		elog(NOTICE, "PGGRID: Processando fragmento");
		HeapTuple	fragmenttup = &fragmentList->members[i]->tuple;
		Form_pg_grid_fragment fragment = (Form_pg_grid_fragment) GETSTRUCT(fragmenttup);
		
		//distribute fragment
		CatCList *siteList = SearchSysCacheList(ALLOCFRAGMENTID, 1, fragment->fragmentid,  0, 0, 0);	
		if (siteList){
			for (j = 0; j < siteList->n_members; j++)
			{
				HeapTuple	alloctup = &(siteList->members[j]->tuple);
				Form_pg_grid_allocation allocation = (Form_pg_grid_allocation) GETSTRUCT(alloctup);			
					
				HeapTuple sitetup = SearchSysCache(SITEID, allocation->siteid, 0, 0, 0);
				Form_pg_grid_site site = (Form_pg_grid_site) GETSTRUCT(sitetup);			
		
				if (!IsLocalSite(sitetup, site)){
					count=SendCountOperation(sitetup, site, CMD_SELECT, relid);
					if (count>=0)
						rv+=count;
					else{
						rv=-1;
						ReleaseSysCache(sitetup);
						break;
					}
				}
				ReleaseSysCache(sitetup);
			}
			ReleaseSysCacheList(siteList);
		}				
	}
	ReleaseSysCacheList(fragmentList);
	
	return rv;

}


int 
SendCountOperation(HeapTuple sitetup, Form_pg_grid_site site, int operation, Oid relationId){
	int status=0;
	Datum sitehostdata;
	char parseCommand[256];

	Relation onerel;

	onerel = try_relation_open(relationId, NoLock);
	if (!onerel)
		return -1;

	snprintf(parseCommand,sizeof(parseCommand),
				"count %s",RelationGetRelationName(onerel));

	relation_close(onerel, NoLock);

	
	sitehostdata=SysCacheGetAttr(SITENAME, sitetup, Anum_pg_grid_site_host, false);
	char *sitehostname=DatumGetCString(DirectFunctionCall1(textout, sitehostdata));
	int siteport=DatumGetInt32(SysCacheGetAttr(SITENAME, sitetup, Anum_pg_grid_site_port, false));
	
	elog(NOTICE, "PGGRID: Sending count operation");
	status = PGR_replication(parseCommand,DestDebug,NULL,"COUNT", sitehostname, siteport);
	if (status == STATUS_REPLICATED || status == STATUS_CONTINUE)
	{
		return PGR_getcountresult();
	}
	return -1;
}


Oid 
ExecClusterQuery(Index relid){
	
	CatCList *fragmentList;
	int i, j;
	Relation rv;
	Oid ret;
	//insert data in relation
	
	elog(NOTICE, "PGGRID: criando temporaria");
	rv=CreateTemporaryRelation(relid);
	elog(NOTICE, "PGGRID: criada temporaria");
	fragmentList = SearchSysCacheList(FRAGMENTRELID, 1, relid,  0, 0, 0);
	
	//verifies each fragment regarding target relation
	for (i = 0; i < fragmentList->n_members; i++)
	{
		
		elog(NOTICE, "PGGRID: Processando fragmento");
		HeapTuple	fragmenttup = &fragmentList->members[i]->tuple;
		Form_pg_grid_fragment fragment = (Form_pg_grid_fragment) GETSTRUCT(fragmenttup);
		
		//verifies each site that stores target relation
		CatCList *siteList = SearchSysCacheList(ALLOCFRAGMENTID, 1, fragment->fragmentid,  0, 0, 0);	
		if (siteList){
			for (j = 0; j < siteList->n_members; j++)
			{
				HeapTuple	alloctup = &(siteList->members[j]->tuple);
				Form_pg_grid_allocation allocation = (Form_pg_grid_allocation) GETSTRUCT(alloctup);			
					
				HeapTuple sitetup = SearchSysCache(SITEID, allocation->siteid, 0, 0, 0);
				Form_pg_grid_site site = (Form_pg_grid_site) GETSTRUCT(sitetup);		
		
				if (!IsLocalSite(sitetup, site)){
					
					if (!RetrieveRelationTuplesFromSite(sitetup, site, relid, rv)){
						elog(ERROR, "PGGRID: Cannot get tuples from site %s", site);
					}					
				}
				ReleaseSysCache(sitetup);
			}
			ReleaseSysCacheList(siteList);
		}				
	}
	ReleaseSysCacheList(fragmentList);	

	//close temp rel
	elog(NOTICE, "PGGRID: closing temp rel");
	ret=rv->rd_id;
	heap_close(rv, NoLock);	
	elog(NOTICE, "PGGRID: dquery executed");
	return ret;
}


Relation
CreateTemporaryRelation(Index relid){
	//get tablespace Oid
	Oid tablespaceOid;
	Relation	relation=heap_open(relid, NoLock);
	if (relation && relation->rd_rel)
		tablespaceOid=relation->rd_rel->reltablespace;
	else
		elog(ERROR, "PGGRID: failed to open relation");
	heap_close(relation, NoLock);

	Oid tempRelOid=make_new_heap(relid, "pg_temp_dquery", tablespaceOid);
	return heap_open(tempRelOid, AccessExclusiveLock);
}

static
bool RetrieveRelationTuplesFromSite(HeapTuple sitetup, Form_pg_grid_site site, Index relid, Relation targetTempRelation){
	int status=0;
	Datum sitehostdata;
	char parseCommand[256];

	//get target relation name
	Relation onerel;
	onerel = try_relation_open(relid, NoLock);
	if (!onerel)
		return -1;

	snprintf(parseCommand,sizeof(parseCommand),
				"get %s",RelationGetRelationName(onerel));

	relation_close(onerel, NoLock);

	
	sitehostdata=SysCacheGetAttr(SITENAME, sitetup, Anum_pg_grid_site_host, false);
	char *sitehostname=DatumGetCString(DirectFunctionCall1(textout, sitehostdata));
	int siteport=DatumGetInt32(SysCacheGetAttr(SITENAME, sitetup, Anum_pg_grid_site_port, false));
	
	elog(NOTICE, "PGGRID: Sending get operation");
	
	//initialize temporary table variables
	PGR_target_temp_rel=targetTempRelation;
	PGR_target_temp_rel_values=(char **) malloc(RelationGetNumberOfAttributes(PGR_target_temp_rel)*sizeof(char *));
	PGR_target_temp_rel_att_counter=0;
	
	status = PGR_replication(parseCommand,DestDebug,NULL,"LOAD DATA", sitehostname, siteport);
	
	//freeze temporary relation variables
	PGR_target_temp_rel=NULL;
	free(PGR_target_temp_rel_values);
	
	if (status == STATUS_REPLICATED || status == STATUS_CONTINUE)
	{
		return true;
	}else
		elog(NOTICE, "PGGRID: error on PGR_replication");
	return false;
}

