
#include "postgres.h"
#include "access/heapam.h"
#include "pggrid.h"
#include "nodes/execnodes.h"
#include "catalog/pg_grid_site.h"

#define PGGRID 5

/*
	Search catalogs and applies operation to the fragments/sites that rules matches
*/
bool
ReplicateOperation(ExprContext *econtext, HeapTuple tuple, Relation relation, int operation);

/*
	Last command needs to be replicated in current host (receiver)
*/
bool
ReplicateLastCommandLocally();

/*
	Check if relation is fragmented between sites
*/
bool
IsRelationFragmented(Oid relid);

/*
	Verifies if the specied site is current backend
*/
extern
bool IsLocalSite(HeapTuple sitetup, Form_pg_grid_site site);