#include "nodes/execnodes.h"

extern int ExecCountSlotsClusterScan(ClusterScan *node);
extern ClusterScanState *ExecInitClusterScan(ClusterScan *node, EState *estate, int eflags);
extern TupleTableSlot *ExecClusterScan(ClusterScanState *node);
extern void ExecEndClusterScan(ClusterScanState *node);