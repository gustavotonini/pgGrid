#include "postgres.h"

#include "access/heapam.h"
#include "executor/execdebug.h"
#include "executor/nodeSeqscan.h"
#include "executor/nodeClusterScan.h"
#include "pggrid/dquery.h"
#include "replicate.h"
#include "parser/parsetree.h"

static TupleTableSlot *ClusterNext(ClusterScanState *node);
static void InitClusterScanRelation(ClusterScanState *node, EState *estate);
static void ReInitClusterScanRelation(ClusterScanState *node, EState *estate);
static void RemoveTempRel();

static bool secondStep=false;

/* ----------------------------------------------------------------
 *		ClusterNext
 *
 *		This is a workhorse for ExecClusterScan
 * ----------------------------------------------------------------
 */
static TupleTableSlot *
ClusterNext(ClusterScanState *node){
	
	return NULL;
}

/* ----------------------------------------------------------------
 *		ExecClusterScan(node)
 *
 *		Scans the relation sequentially on all servers and returns the next qualifying
 *		tuple.
 *		It calls the ExecScan() routine and passes it the access method
 *		which retrieve tuples sequentially from cluster
 *
 */

TupleTableSlot *
ExecClusterScan(ClusterScanState *node)
{
	TupleTableSlot *rv;
	ClusterScan *scan;
	Index scanrelid=node->ss_currentScanDesc->rs_rd->rd_id;
	elog(NOTICE, "PGGRID: exec cluster scan first step");
	rv=ExecSeqScan(node);
	
	if (TupIsNull(rv) && !secondStep){
		elog(NOTICE, "PGGRID: exec cluster scan second step");
		secondStep=true;
		//RemoveTempRel();
		scan= (ClusterScan *) node->ps.plan;
		//como parar a recursividade?
		scan->scanrelid=ExecClusterQuery( scanrelid );
		(node->ps.state)->es_snapshot=SnapshotNow;
		/*
		* reinitialize scan relation
		*/
		//elog(NOTICE, "PGGRID: reinitializing seqscan with relid %d", scan->scanrelid);
		ReInitClusterScanRelation(node, node->ps.state);
		//elog(NOTICE, "PGGRID: doing seqscan in temp rel");
		rv=ExecSeqScan(node);
	}
	return rv;
}

/* ----------------------------------------------------------------
 *		ExecInitClusterScan
 * ----------------------------------------------------------------
 */
ClusterScanState *
ExecInitClusterScan(ClusterScan *node, EState *estate, int eflags){
	ClusterScanState *scanstate;

	Assert(outerPlan(node) == NULL);
	Assert(innerPlan(node) == NULL);

	/*
	 * create state structure
	 */
	scanstate = makeNode(ClusterScanState);
	scanstate->ps.plan = (Plan *) node;
	scanstate->ps.state = estate;
	secondStep=false;

	/*
	 * Miscellaneous initialization
	 *
	 * create expression context for node
	 */
	ExecAssignExprContext(estate, &scanstate->ps);

	/*
	 * initialize child expressions
	 */
	scanstate->ps.targetlist = (List *)
		ExecInitExpr((Expr *) node->plan.targetlist,
					 (PlanState *) scanstate);
	scanstate->ps.qual = (List *)
		ExecInitExpr((Expr *) node->plan.qual,
					 (PlanState *) scanstate);

#define SEQSCAN_NSLOTS 2
	/*
	 * tuple table initialization
	 */
	ExecInitResultTupleSlot(estate, &scanstate->ps);
	ExecInitScanTupleSlot(estate, scanstate);

	/*
	 * initialize scan relation
	 */
	InitClusterScanRelation(scanstate, estate);

	scanstate->ps.ps_TupFromTlist = false;

	/*
	 * Initialize result tuple type and projection info.
	 */
	ExecAssignResultTypeFromTL(&scanstate->ps);
	ExecAssignScanProjectionInfo(scanstate);

	return scanstate;
}

int
ExecCountSlotsClusterScan(ClusterScan *node)
{
	//local count;
	int count = ExecCountSlotsSeqScan((SeqScan *)node);
	count+=10;
	Index relid=node->scanrelid;
	//if (!PGR_Is_Replicated_Query){
	if (IsRelationFragmented(relid)){
		count+=ExecCountForeignTuples(relid);
	}
	return count;
}


void 
ExecEndClusterScan(ClusterScanState *node){
	Relation	relation;
	HeapScanDesc scanDesc;	
	//elog(NOTICE, "terminando scan");
	/*
	 * get information from node
	 */
	relation = node->ss_currentRelation;
	scanDesc = node->ss_currentScanDesc;

	/*
	 * Free the exprcontext
	 */
	ExecFreeExprContext(&node->ps);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(node->ps.ps_ResultTupleSlot);
	ExecClearTuple(node->ss_ScanTupleSlot);

	/*
	 * close heap scan
	 */
	heap_endscan(scanDesc);

	/*
	 * close the heap relation.
	 */
	ExecCloseScanRelation(relation);

	//remove temporary relation
	if (secondStep)
		RemoveTempRel();
}

static void RemoveTempRel(){
	RangeVar   *rel;
	List *list=list_make1(makeString("pg_temp_dquery"));
	{
		rel=makeRangeVarFromNameList(list);
		RemoveRelation(rel, DROP_RESTRICT);
	}
}

void 
InitClusterScanRelation(ClusterScanState *node, EState *estate){
	Relation	currentRelation;
	HeapScanDesc currentScanDesc;

	/*
	 * get the relation object id from the relid'th entry in the range table,
	 * open that relation and acquire appropriate lock on it.
	 */
	Index scanrelid=((ClusterScan *) node->ps.plan)->scanrelid;

	currentRelation = ExecOpenScanRelation(estate, scanrelid);
	currentScanDesc = heap_beginscan(currentRelation,
									 estate->es_snapshot,
									 0,
									 NULL);

	node->ss_currentRelation = currentRelation;
	node->ss_currentScanDesc = currentScanDesc;

	ExecAssignScanType(node, RelationGetDescr(currentRelation));
}

void 
ReInitClusterScanRelation(ClusterScanState *node, EState *estate){
	Relation	currentRelation;
	HeapScanDesc currentScanDesc;

	/*
	 * get the relation object id from the relid'th entry in the range table,
	 * open that relation and acquire appropriate lock on it.
	 */
	Index scanrelid=((ClusterScan *) node->ps.plan)->scanrelid;

	/*close first step's scan descriptors*/
	heap_endscan(node->ss_currentScanDesc);
	ExecCloseScanRelation(node->ss_currentRelation);

	//elog(NOTICE, "PGGRID: opening scan relation %d", scanrelid);
	currentRelation = heap_open(scanrelid, AccessShareLock);

	//currentRelation = ExecOpenScanRelation(estate, scanrelid);

	//elog(NOTICE, "PGGRID: getting scan relation descriptor");
	currentScanDesc = heap_beginscan(currentRelation,
									 estate->es_snapshot,
									 0,
									 NULL);

	node->ss_currentRelation = currentRelation;
	node->ss_currentScanDesc = currentScanDesc;
	
	/*elog(NOTICE, "PGGRID: testando tuplas");
	while(heap_getnext(node->ss_currentScanDesc, ForwardScanDirection)){
		elog(NOTICE, "PGGRID: achei tupla");
	}*/

	ExecAssignScanType(node, RelationGetDescr(currentRelation));
}
