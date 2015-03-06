#include "postgres.h"
#include "access/heapam.h"
#include "pggrid.h"
#include "nodes/execnodes.h"

int ExecCountForeignTuples(Index relid);

Oid ExecClusterQuery(Index relid);