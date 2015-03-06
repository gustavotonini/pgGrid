#include "pggrid/allocation.h"

#include "utils/syscache.h"
#include "utils/catcache.h"
#include "utils/rel.h"
#include "utils/palloc.h"
#include "utils/memutils.h"
#include "utils/builtins.h"

#include "catalog/pg_grid_fragment.h"
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

bool localReplicateLastCommand;

static 
List *GetFragmentQuals(HeapTuple fragmenttup, Form_pg_grid_fragment fragment);

/*
	Send each operation to the envolved site 
*/
static 
bool SendOperation(HeapTuple sitetup, Form_pg_grid_site site, int operation, HeapTuple tuple);

static 
List *GetFragmentQuals(HeapTuple fragmenttup, Form_pg_grid_fragment fragment){
	Datum wheredata=SysCacheGetAttr(FRAGMENTNAME, fragmenttup, Anum_pg_grid_fragment_where, false);
	char *where=DatumGetCString(DirectFunctionCall1(textout, wheredata));
	if (strcmp(where, "")){
		return make_ands_implicit(stringToNode(where));
	}
	return NIL;
}

bool
ReplicateOperation(ExprContext *econtext, HeapTuple tuple, Relation relation, int operation){
	int i, j;
	EState *estate;
	bool rv=true;
	int fragment_count;
	bool replicated=false, fragments=false;
	CatCList *fragmentList;
	if (PGR_Is_Replicated_Query){
		//this is a replicated query. Just execute locally
		localReplicateLastCommand=true;
		return true;
	}
	fragmentList = SearchSysCacheList(FRAGMENTRELID, 1, relation->rd_id,  0, 0, 0);
	localReplicateLastCommand=false;
	fragment_count=fragmentList->n_members;
	
    for (i = 0; i < fragmentList->n_members; i++)
    {
        HeapTuple	fragmenttup = &fragmentList->members[i]->tuple;
        Form_pg_grid_fragment fragment = (Form_pg_grid_fragment) GETSTRUCT(fragmenttup);
		
        //for each fragment, test if the fragment rule matches.
        //and if true, send to the sites assigned for
		List *quals= GetFragmentQuals(fragmenttup, fragment);
		
		
		//horizontal fragmentation is assembled here
		bool match=true;
		if (quals){
			estate = CreateExecutorState();
			econtext = GetPerTupleExprContext(estate);
			TupleTableSlot *slot = MakeSingleTupleTableSlot(RelationGetDescr(relation));
			ExecStoreTuple(tuple, slot, InvalidBuffer, false);
			econtext->ecxt_scantuple = slot;

			List *predicate = (List *)
				ExecPrepareExpr((Expr *) quals,	estate);
			
			match=ExecQual(predicate, econtext, false);
			
			ExecDropSingleTupleTableSlot(slot);
			ResetExprContext(econtext);
			FreeExecutorState(estate);
			//free(quals);
		}
		elog(NOTICE, "Fragment name");
		elog(NOTICE, (fragment->fragmentname).data);

		if (match)
			elog(NOTICE, "Fragment criteria matches");
		else
			elog(NOTICE, "Fragment criteria does not match");
		if (match){
			//distribute fragment
			CatCList *siteList = SearchSysCacheList(ALLOCFRAGMENTID, 1, fragment->fragmentid,  0, 0, 0);	
			if (siteList){
				for (j = 0; j < siteList->n_members; j++)
				{
				HeapTuple	alloctup = &(siteList->members[j]->tuple);
				Form_pg_grid_allocation allocation = (Form_pg_grid_allocation) GETSTRUCT(alloctup);			
					
					HeapTuple sitetup = SearchSysCache(SITEID, allocation->siteid, 0, 0, 0);
					Form_pg_grid_site site = (Form_pg_grid_site) GETSTRUCT(sitetup);			
					if (IsLocalSite(sitetup, site)){
						//elog(NOTICE, "Just a local site");
						localReplicateLastCommand=true;
					}
					else{
						//elog(NOTICE, "Sending operation to site");
						if (!SendOperation(sitetup, site, operation, tuple)){
							rv=false;
							ReleaseSysCache(sitetup);
							break;
						}
					}
					ReleaseSysCache(sitetup);
					replicated=true;
				}
				ReleaseSysCacheList(siteList);
			}				
		}
    }
	ReleaseSysCacheList(fragmentList);
	
	if (fragment_count == 0){
		//table has no fragments. Store locally
		localReplicateLastCommand=true;
	}else if (!replicated){
		elog(ERROR, "Record does not match to any fragment. Cancelled");
	}
	
	return rv;
}

static 
bool SendOperation(HeapTuple sitetup, Form_pg_grid_site site, int operation, HeapTuple tuple){	
	int status=0;
	Datum sitehostdata;
	char *commandTag;
	ListCell *cell;
	Node *parsenode=NULL;
	foreach(cell, parsetree)
	{
		parsenode=(Node *)lfirst(cell);
		break;
	}
	commandTag = CreateCommandTag(parsenode);
	
	sitehostdata=SysCacheGetAttr(SITENAME, sitetup, Anum_pg_grid_site_host, false);
	char *sitehostname=DatumGetCString(DirectFunctionCall1(textout, sitehostdata));
	int siteport=DatumGetInt32(SysCacheGetAttr(SITENAME, sitetup, Anum_pg_grid_site_port, false));
	
	status = PGR_replication(parseCommand,DestDebug,parsenode,commandTag, sitehostname, siteport);
	if (status == STATUS_REPLICATED || status == STATUS_CONTINUE)
	{
		return true;
	}
	else
		return false;
}

bool 
IsLocalSite(HeapTuple sitetup, Form_pg_grid_site site){

	Datum sitehostdata=SysCacheGetAttr(SITENAME, sitetup, Anum_pg_grid_site_host, false);
	char *sitehostname=DatumGetCString(DirectFunctionCall1(textout, sitehostdata));
	int siteport=DatumGetInt32(SysCacheGetAttr(SITENAME, sitetup, Anum_pg_grid_site_port, false));
	
	return PGRis_same_host(PGRSelfHostName,ntohs(PostPortNumber),sitehostname, ntohs(siteport));
}

bool
ReplicateLastCommandLocally(){
	return localReplicateLastCommand;
}


bool
IsRelationFragmented(Oid relid){
	CatCList *fragmentList = SearchSysCacheList(FRAGMENTRELID, 1, relid,  0, 0, 0);
	int fragment_count=fragmentList->n_members;
	ReleaseSysCacheList(fragmentList);
	return (fragment_count>0);
}

