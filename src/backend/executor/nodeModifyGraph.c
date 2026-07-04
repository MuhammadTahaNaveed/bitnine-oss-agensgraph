/*
 * nodeModifyGraph.c
 *	  routines to handle ModifyGraph nodes.
 *
 * Copyright (c) 2022 by Bitnine Global, Inc.
 *
 * IDENTIFICATION
 *	  src/backend/executor/nodeModifyGraph.c
 */

#include "postgres.h"

#include "access/htup_details.h"
#include "access/xact.h"
#include "catalog/ag_graph_fn.h"
#include "catalog/namespace.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_type.h"
#include "executor/executor.h"
#include "executor/nodeModifyGraph.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_relation.h"
#include "pgstat.h"
#include "utils/acl.h"
#include "utils/arrayaccess.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "access/heapam.h"
#include "executor/execCypherCreate.h"
#include "executor/execCypherSet.h"
#include "executor/execCypherDelete.h"
#include "executor/execCypherMerge.h"
#include "catalog/ag_label_fn.h"
#include "catalog/ag_label.h"
#include "catalog/ag_edge_d.h"
#include "executor/execGraphMeta.h"
#include "tcop/tcopprot.h"
#include "utils/graph.h"
#include "utils/syscache.h"

bool		enable_multiple_update = true;
bool		auto_gather_graphmeta = false;

#define DatumGetItemPointer(X)	 ((ItemPointer) DatumGetPointer(X))

static TupleTableSlot *ExecModifyGraph(PlanState *pstate);
static void initGraphWRStats(ModifyGraphState *mgstate, GraphWriteOp op);
static List *ExecInitGraphPattern(List *pattern, ModifyGraphState *mgstate);
static List *ExecInitGraphSets(List *sets, ModifyGraphState *mgstate);
static List *ExecInitGraphDelExprs(List *exprs, ModifyGraphState *mgstate);

/* eager */
static void reflectModifiedProp(ModifyGraphState *mgstate);
static TupleTableSlot *execModifyGraphReadSubplan(ModifyGraphState *mgstate);
static void publishModifiedCid(ModifyGraphState *mgstate);
static void execModifyGraphChild(ModifyGraphState *mgstate);
static bool predrainEagerWriterWalker(PlanState *node, void *context);
static void predrainEagerWriters(PlanState *node);

/* common */
static bool isEdgeArrayOfPath(List *exprs, char *variable);

static void openResultRelInfosIndices(ModifyGraphState *mgstate);

ModifyGraphState *
ExecInitModifyGraph(ModifyGraph *mgplan, EState *estate, int eflags)
{
	TupleTableSlot *slot;
	ModifyGraphState *mgstate;
	ResultRelInfo *resultRelInfo;
	ListCell   *l;

	Assert(!(eflags & (EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));

	mgstate = makeNode(ModifyGraphState);
	mgstate->ps.plan = (Plan *) mgplan;
	mgstate->ps.state = estate;
	mgstate->ps.ExecProcNode = ExecModifyGraph;

	/* Tuple desc for result is the same as the subplan. */
	slot = ExecAllocTableSlot(&estate->es_tupleTable, NULL, &TTSOpsMinimalTuple);
	mgstate->ps.ps_ResultTupleSlot = slot;

	/*
	 * We don't use ExecInitResultTypeTL because we need to get the
	 * information of the subplan, not the current plan.
	 */
	mgstate->ps.ps_ResultTupleDesc = ExecTypeFromTL(mgplan->subplan->targetlist);
	ExecSetSlotDescriptor(slot, mgstate->ps.ps_ResultTupleDesc);
	ExecAssignExprContext(estate, &mgstate->ps);

	mgstate->elemTupleSlot = ExecInitExtraTupleSlot(estate, NULL,
													&TTSOpsMinimalTuple);

	mgstate->done = false;
	mgstate->child_done = false;
	mgstate->predrained = false;
	mgstate->iter_fresh = false;
	mgstate->iter_wrote = false;
	mgstate->eagerness = mgplan->eagerness;

	/*
	 * A per-row CALL body clause accumulates long-lived per-iteration
	 * allocations (the modified-element table's element copies); give them
	 * a context the iteration reset can reclaim wholesale.
	 */
	mgstate->iterCxt = (mgplan->iterStride > 0) ?
		AllocSetContextCreate(CurrentMemoryContext,
							  "ModifyGraph iteration context",
							  ALLOCSET_SMALL_SIZES) : NULL;
	mgstate->modify_cid = GetCurrentCommandId(false) +
		(mgplan->nr_modify * MODIFY_CID_MAX);

	mgstate->subplan = ExecInitNode(mgplan->subplan, estate, eflags);
	Assert(mgplan->operation != GWROP_MERGE ||
		   IsA(mgstate->subplan, NestLoopState) ||

	/*
	 * The subplan may be a Result node instead of a NestLoop if a one-time
	 * filter is applied (e.g. in case of MATCH with non-existent labels in
	 * previous clause).
	 */
		   IsA(mgstate->subplan, ResultState));

	mgstate->graphid = get_graph_path_oid();
	mgstate->pattern = ExecInitGraphPattern(mgplan->pattern, mgstate);
	mgstate->exprs = ExecInitGraphDelExprs(mgplan->exprs, mgstate);

	mgstate->numResultRelInfo = list_length(mgplan->resultRelations);
	mgstate->resultRelInfo = (ResultRelInfo *)
		palloc(mgstate->numResultRelInfo * sizeof(ResultRelInfo));

	resultRelInfo = mgstate->resultRelInfo;
	foreach(l, mgplan->resultRelations)
	{
		Index		resultRelation = lfirst_int(l);

		ExecInitResultRelation(estate, resultRelInfo, resultRelation);
		resultRelInfo++;
	}

	/*
	 * Initialize any WITH CHECK OPTION constraints if needed.
	 */
	resultRelInfo = mgstate->resultRelInfo;
	foreach(l, mgplan->withCheckOptionLists)
	{
		List	   *wcoList = (List *) lfirst(l);
		List	   *wcoExprs = NIL;
		ListCell   *ll;

		foreach(ll, wcoList)
		{
			WithCheckOption *wco = (WithCheckOption *) lfirst(ll);
			ExprState  *wcoExpr = ExecInitQual((List *) wco->qual,
											   &mgstate->ps);

			wcoExprs = lappend(wcoExprs, wcoExpr);
		}

		resultRelInfo->ri_WithCheckOptions = wcoList;
		resultRelInfo->ri_WithCheckOptionExprs = wcoExprs;
		resultRelInfo++;
	}

	openResultRelInfosIndices(mgstate);

	/* For Set Operation. */
	mgstate->sets = ExecInitGraphSets(mgplan->sets, mgstate);
	mgstate->update_cols = NULL;

	/* Initialize for EPQ. */
	EvalPlanQualInit(&mgstate->mt_epqstate, estate, NULL, NIL,
					 mgplan->epqParam, mgplan->resultRelations);
	mgstate->mt_arowmarks = (List **) palloc0(sizeof(List *) * 1);
	EvalPlanQualSetPlan(&mgstate->mt_epqstate, mgplan->subplan,
						mgstate->mt_arowmarks[0]);

	/*
	 * Fill eager action information.
	 *
	 * A SET clause always needs the modified-element table: it is how a
	 * repeated update of the same element within one clause is detected, both
	 * to apply the update only once (enable_multiple_update) and to warn
	 * about the repetition (!enable_multiple_update) -- the update paths
	 * probe it unconditionally.  Likewise DELETE uses it to skip an element
	 * that was already deleted.
	 */
	if (mgstate->eagerness ||
		mgstate->sets != NIL ||
		mgstate->exprs != NIL)
	{
		HASHCTL		ctl;

		memset(&ctl, 0, sizeof(ctl));
		ctl.keysize = sizeof(Graphid);
		ctl.entrysize = sizeof(ModifiedElemEntry);
		ctl.hcxt = CurrentMemoryContext;

		mgstate->elemTable =
			hash_create("modified object table", 128, &ctl,
						HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
	}
	else
	{
		/* We will not use eager action */
		mgstate->elemTable = NULL;
	}
	/*
	 * A per-row CALL body clause may be asked to replay its buffered output
	 * within one iteration (see ExecReScanModifyGraph), which needs a
	 * rewindable store.
	 */
	mgstate->tuplestorestate = tuplestore_begin_heap(mgplan->iterStride > 0,
													 false, eager_mem);

	switch (mgplan->operation)
	{
		case GWROP_CREATE:
			mgstate->execProc = ExecCreateGraph;
			break;
		case GWROP_DELETE:
			mgstate->execProc = ExecDeleteGraph;
			break;
		case GWROP_SET:
			mgstate->execProc = mgplan->accumulate ? ExecSetGraphAccum :
				ExecSetGraph;
			break;
		case GWROP_MERGE:
			mgstate->execProc = ExecMergeGraph;
			break;
		default:
			elog(ERROR, "unknown operation");
	}

	initGraphWRStats(mgstate, mgplan->operation);
	return mgstate;
}

static void
reflectTupleChanges(PlanState *pstate, TupleTableSlot *result)
{
	ModifyGraphState *mgstate = castNode(ModifyGraphState, pstate);
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;
	TupleDesc	tupDesc = result->tts_tupleDescriptor;
	int			natts = tupDesc->natts;
	int			i;

	for (i = 0; i < natts; i++)
	{
		Oid			type;
		Datum		orig_elem;
		Datum		elem;

		if (result->tts_isnull[i])
			continue;

		orig_elem = result->tts_values[i];
		type = TupleDescAttr(tupDesc, i)->atttypid;

		/*
		 * A node or relationship whose id is NULL was bound by an OPTIONAL
		 * MATCH that did not match; it carries no modification to reflect.
		 */
		if ((type == VERTEXOID || type == EDGEOID) &&
			graphElementIdIsNull(orig_elem, type))
			continue;

		if (type == VERTEXOID)
		{
			Datum		graphid;
			bool		found;

			graphid = getVertexIdDatum(orig_elem);
			elem = getElementFromEleTable(mgstate, type, orig_elem, graphid,
										  &found);
			if (!found)
			{
				continue;
			}
		}
		else if (type == EDGEOID)
		{
			Datum		graphid;
			bool		found;

			graphid = getEdgeIdDatum(orig_elem);
			elem = getElementFromEleTable(mgstate, type, orig_elem, graphid,
										  &found);
			if (!found)
			{
				continue;
			}
		}
		else if (type == GRAPHPATHOID)
		{
			/*
			 * When deleting the graphpath, edge array of graphpath is deleted
			 * first and vertex array is deleted in the next plan. So, the
			 * graphpath must be passed to the next plan for deleting vertex
			 * array of the graphpath.
			 */
			if (isEdgeArrayOfPath(mgstate->exprs,
								  NameStr(TupleDescAttr(tupDesc, i)->attname)))
				continue;

			elem = getPathFinal(mgstate, orig_elem);
		}
		else if (type == EDGEARRAYOID && plan->operation == GWROP_DELETE)
		{
			/*
			 * The edges are used only for removal, not for result output.
			 *
			 * This assumes that there are only variable references in the
			 * target list.
			 */
			continue;
		}
		else
		{
			continue;
		}

		setSlotValueByAttnum(result, elem, i + 1);
	}
}

/*
 * reflectTupleTids
 *
 * Refresh the tuple id inside each element of an output row from the
 * modified-element table, keeping the row's own property values.  Used by
 * accumulating clauses that surface per-row values: the flush at clause end
 * rewrote each element's heap tuple, so only the id needs updating for a
 * later write clause to find the current version.
 */
static void
reflectTupleTids(PlanState *pstate, TupleTableSlot *result)
{
	ModifyGraphState *mgstate = castNode(ModifyGraphState, pstate);
	TupleDesc	tupDesc = result->tts_tupleDescriptor;
	int			natts = tupDesc->natts;
	int			i;

	for (i = 0; i < natts; i++)
	{
		Oid			type;
		Datum		elem;
		Datum		gid;
		Datum		final_elem;
		bool		found;

		if (result->tts_isnull[i])
			continue;

		type = TupleDescAttr(tupDesc, i)->atttypid;
		if (type != VERTEXOID && type != EDGEOID)
			continue;

		elem = result->tts_values[i];
		if (graphElementIdIsNull(elem, type))
			continue;

		gid = (type == VERTEXOID) ? getVertexIdDatum(elem) :
			getEdgeIdDatum(elem);

		final_elem = getElementFromEleTable(mgstate, type, elem, gid, &found);
		if (!found || final_elem == (Datum) 0)
			continue;

		if (type == VERTEXOID)
			elem = makeGraphVertexDatum(getVertexIdDatum(elem),
										getVertexPropDatum(elem),
										getVertexTidDatum(final_elem));
		else
			elem = makeGraphEdgeDatum(getEdgeIdDatum(elem),
									  getEdgeStartDatum(elem),
									  getEdgeEndDatum(elem),
									  getEdgePropDatum(elem),
									  getEdgeTidDatum(final_elem));

		setSlotValueByAttnum(result, elem, i + 1);
	}
}

/*
 * execModifyGraphReadSubplan
 *
 * Pull one tuple from this clause's input, reading at the clause's command-id
 * window so the scan observes earlier clauses' writes but not its own.  Returns
 * the input slot, or NULL when the input is exhausted.  Shared by the eager
 * (execModifyGraphChild) and streaming (ExecModifyGraph) read paths so the
 * command-id window arithmetic lives in exactly one place.
 */
static TupleTableSlot *
execModifyGraphReadSubplan(ModifyGraphState *mgstate)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;
	EState	   *estate = mgstate->ps.state;
	CommandId	svCid;
	TupleTableSlot *slot;

	/* ExecInsertIndexTuples() uses per-tuple context. Reset it here. */
	ResetPerTupleExprContext(estate);

	svCid = estate->es_snapshot->curcid;

	switch (plan->operation)
	{
		case GWROP_MERGE:
		case GWROP_DELETE:
			estate->es_snapshot->curcid =
				mgstate->modify_cid + MODIFY_CID_NLJOIN_MATCH;
			break;
		default:
			estate->es_snapshot->curcid =
				mgstate->modify_cid + MODIFY_CID_LOWER_BOUND;
			break;
	}

	slot = ExecProcNode(mgstate->subplan);

	estate->es_snapshot->curcid = svCid;

	return slot;
}

/*
 * publishModifiedCid
 *
 * Make this clause's writes visible to a later *reading* clause in the same
 * statement.
 *
 * Cross-clause visibility is approximated with per-clause command-id windows
 * (modify_cid = base_cid + nr_modify * MODIFY_CID_MAX).  A write clause stamps
 * the tuple versions it produces with a command id inside its own window
 * (modify_cid + MODIFY_CID_OUTPUT for CREATE/MERGE/DELETE, modify_cid +
 * MODIFY_CID_SET for SET).  Another *write* clause that follows reads through
 * execModifyGraphReadSubplan(), which raises curcid to its own (higher) window
 * for the duration of that read, so it observes those versions.
 *
 * A trailing read clause -- MATCH ... RETURN, or any non-write clause after a
 * write, e.g. "... SET p.x = 1 WITH p MATCH (p) RETURN p.x" -- is not a
 * ModifyGraph node and never sets a window: it scans the heap at the ambient
 * snapshot command id, which was captured before this statement's writes ran
 * and is therefore below the command id those writes were stamped with.  The
 * read then misses them (stale property, or a freshly CREATEd element matching
 * nothing).
 *
 * Once this clause's modifications are physically applied, advance the ambient
 * snapshot command id past the top of this clause's window so that any later
 * heap read in the statement observes them.  This mirrors the command-counter
 * bump ExecEndModifyGraph() performs for the *next* statement, brought forward
 * to the moment the writes land so it benefits a reader in *this* statement.
 *
 * The bump is monotonic (never lowers curcid) and is invisible to other write
 * clauses, which always override curcid for their own reads regardless of the
 * ambient value.  It changes no plan, adds no scan or join, and runs once per
 * clause (eager) or once per produced row (streaming), so it preserves the
 * set-based plans and is EXPLAIN-identical.
 */
static void
publishModifiedCid(ModifyGraphState *mgstate)
{
	EState	   *estate = mgstate->ps.state;
	CommandId	visible_cid = mgstate->modify_cid + MODIFY_CID_MAX;

	if (estate->es_snapshot->curcid < visible_cid)
		estate->es_snapshot->curcid = visible_cid;
}

/*
 * execModifyGraphChild
 *
 * Pull every tuple from the subplan and apply this clause's graph
 * modifications, leaving the results buffered in the tuplestore for an eager
 * node.  This is the "child" (read+write) phase of ExecModifyGraph, factored
 * out so that a later clause can force an earlier eager clause to run before
 * the later clause reads the heap (see predrainEagerWriters).
 *
 * It is safe to call more than once: the second call is a no-op because
 * child_done is set.  A non-eager node streams its rows out one at a time and
 * therefore must not be drained ahead of time; this routine is only ever
 * applied to eager nodes.
 */
static void
execModifyGraphChild(ModifyGraphState *mgstate)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;

	if (mgstate->child_done)
		return;

	for (;;)
	{
		TupleTableSlot *slot = execModifyGraphReadSubplan(mgstate);

		if (TupIsNull(slot))
			break;

		/*
		 * A gated row (no match on the write-gate column) is passed through
		 * untouched: the write, its triggers, and its statistics all skip.
		 */
		if (plan->writeGateAttno != InvalidAttrNumber)
		{
			bool		gatenull;

			(void) slot_getattr(slot, plan->writeGateAttno, &gatenull);
			if (gatenull)
			{
				tuplestore_puttupleslot(mgstate->tuplestorestate, slot);
				continue;
			}
		}

		mgstate->iter_wrote = true;
		slot = mgstate->execProc(mgstate, slot);

		Assert(mgstate->eagerness);
		Assert(slot != NULL);

		tuplestore_puttupleslot(mgstate->tuplestorestate, slot);
	}

	mgstate->child_done = true;

	publishModifiedCid(mgstate);

	if (mgstate->elemTable != NULL
		&& plan->operation != GWROP_DELETE
		&& (plan->operation != GWROP_SET || plan->accumulate))
		reflectModifiedProp(mgstate);
}

/*
 * predrainEagerWriters
 *
 * Before a ModifyGraph clause reads the graph, run any eager write clause that
 * precedes it in the same statement so that the earlier clause's heap changes
 * physically exist by the time this clause scans the target labels.
 *
 * AgensGraph chains write clauses by nesting the earlier clause as a subquery
 * on one side of a join in the later clause's plan.  Cross-clause visibility
 * relies on per-clause command-id windows (modify_cid): the later clause reads
 * at a command id that already sees the earlier clause's writes.  That works
 * only if the earlier write has actually happened when the later scan runs.
 * Under a streaming join (e.g. nested loop) it has, because the inner side is
 * re-read after the outer (earlier) clause produces a row.  But a hash or merge
 * join materializes one input -- which may be the later clause's scan of a
 * label the earlier clause writes -- before pulling the side that drives the
 * earlier clause.  The materialized side then captures pre-write tuple versions
 * (stale ctids and stale properties), and the subsequent update/delete targets
 * a superseded tuple, which the heap reports as "attempted to update/delete
 * invisible tuple".
 *
 * Draining the earlier eager clause up front is the same barrier openCypher
 * implementations insert between a write and a following read (Neo4j's "Eager"
 * operator).  It changes nothing about the chosen join methods, so set-based
 * hash/merge plans are preserved; it only fixes the order in which the earlier
 * writes and the later reads happen.
 */
static bool
predrainEagerWriterWalker(PlanState *node, void *context)
{
	if (node == NULL)
		return false;

	if (IsA(node, ModifyGraphState))
	{
		ModifyGraphState *child = (ModifyGraphState *) node;

		/*
		 * This walk drains the clause directly, without the ExecProcNode
		 * wrapper that fires a pending (changed-parameter) rescan first.
		 * Fire it here instead: draining a per-row CALL body clause still
		 * carrying its iteration mark would run it in the previous
		 * iteration's state, and its later deferred reset would then run it
		 * again -- writing twice.  Only the clause itself needs this; the
		 * nodes below it are pulled through ExecProcNode during the drain,
		 * which fires their pending rescans in pull order, after the plan
		 * above them has set their parameters.
		 */
		if (child->ps.chgParam != NULL)
			ExecReScan((PlanState *) child);

		/*
		 * A ModifyGraphState keeps its input in its own "subplan" field rather
		 * than as an ordinary left child, so planstate_tree_walker() does not
		 * descend into it.  Recurse explicitly, and do so before draining this
		 * node, so that in a chain of three or more write clauses the innermost
		 * (earliest) eager clause is drained first.
		 */
		predrainEagerWriters(child->subplan);

		if (child->eagerness && !child->child_done)
		{
			execModifyGraphChild(child);

			/*
			 * Mark the child drained so that when the parent later pulls it via
			 * ExecProcNode() its own ExecModifyGraph() does not repeat the
			 * now-redundant subplan walk.
			 */
			child->predrained = true;
		}

		return false;
	}

	if (IsA(node, GraphVLEState))
	{
		/*
		 * A GraphVLE (variable-length path expansion) likewise holds its input
		 * in a private "subplan" field that planstate_tree_walker() does not
		 * descend, so recurse into it explicitly; otherwise a write clause
		 * nested under a [*] expansion would escape the barrier.
		 */
		predrainEagerWriters(((GraphVLEState *) node)->subplan);
		return false;
	}

	if (IsA(node, NestLoopState) &&
		((NestLoopState *) node)->js.jointype == JOIN_CYPHER_CALL)
	{
		/*
		 * The inner side is a per-row CALL subquery body: the join drives it
		 * once per input row with fresh parameters, so its write clauses must
		 * not run early (their parameters are not even set yet).  Keep
		 * walking the input side only.
		 */
		predrainEagerWriters(outerPlanState(node));
		return false;
	}

	/*
	 * Ordinary node: descend through its execution inputs.  The context
	 * argument is unused -- the walk threads no state -- but is required by the
	 * planstate_tree_walker() callback signature.  planstate_tree_walker() also
	 * visits initPlan/subPlan expression subqueries; those can never contain a
	 * graph write (the parser rejects writes inside expression subqueries), so
	 * the walk drains exactly the writing clauses on this statement's input
	 * spine and nothing extraneous.
	 */
	return planstate_tree_walker(node, predrainEagerWriterWalker, NULL);
}

static void
predrainEagerWriters(PlanState *node)
{
	if (node == NULL)
		return;

	/*
	 * The ModifyGraph/GraphVLE arms of the walker recurse back into this
	 * function directly rather than through planstate_tree_walker(), so guard
	 * the recursion depth here the way planstate_tree_walker() does internally.
	 */
	check_stack_depth();

	/*
	 * planstate_tree_walker() invokes the walker on the children of "node",
	 * not on "node" itself, so handle the case where "node" is directly a
	 * write clause before descending.
	 */
	(void) predrainEagerWriterWalker(node, NULL);
}

/*
 * ExecPredrainGraphWriters
 *
 * Run every eager graph-write clause in the plan tree before the first read
 * of the plan's output.  A ModifyGraph node applies this barrier itself
 * before it reads the heap (see predrainEagerWriters above), but when the
 * statement's LAST reader is not a graph-write clause -- a plain SELECT over
 * chained write clauses, e.g. "MATCH ... SET ... WITH x MATCH ... RETURN ..."
 * -- nothing above the writes enforces it, and the top-level plan is free to
 * order its work so that the write side is read late or never:
 *
 * - A hash join whose other input turns out empty returns before building
 *   (or ever pulling) the side that contains the write clauses, silently
 *   skipping every write.
 * - A hash join that prefetches/builds the label-scan side first captures
 *   pre-write tuple versions, so the trailing read observes stale data even
 *   though the writes do land.
 *
 * Cypher's clause pipeline semantics require a write clause to apply to
 * every row that reaches it, with later clauses observing the result, no
 * matter what plan shape the reader chose.  ExecutorRun therefore applies
 * the barrier once, before the first pull of any CMD_SELECT plan that
 * contains a graph write.  Draining is idempotent (child_done guards), does
 * not change the chosen plan, and only forces work the pipeline semantics
 * require to happen anyway.
 */
void
ExecPredrainGraphWriters(PlanState *planstate)
{
	predrainEagerWriters(planstate);
}

static TupleTableSlot *
ExecModifyGraph(PlanState *pstate)
{
	ModifyGraphState *mgstate = castNode(ModifyGraphState, pstate);
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;

	if (mgstate->done)
		return NULL;

	/*
	 * Force any earlier eager write clause nested in our subplan to run before
	 * we read the heap, so its modifications are already in place.  This is done
	 * once, before the first read; for a streaming node we re-enter this
	 * function per output row, so guard against repeating the tree walk.
	 */
	if (!mgstate->predrained)
	{
		mgstate->predrained = true;
		predrainEagerWriters(mgstate->subplan);
	}

	if (!mgstate->child_done)
	{
		if (mgstate->eagerness)
		{
			execModifyGraphChild(mgstate);
		}
		else
		{
			for (;;)
			{
				TupleTableSlot *slot = execModifyGraphReadSubplan(mgstate);

				if (TupIsNull(slot))
					break;

				/* see the gate note in execModifyGraphChild */
				if (plan->writeGateAttno != InvalidAttrNumber)
				{
					bool		gatenull;

					(void) slot_getattr(slot, plan->writeGateAttno, &gatenull);
					if (gatenull)
					{
						if (plan->last)
							continue;
						return slot;
					}
				}

				mgstate->iter_wrote = true;
				slot = mgstate->execProc(mgstate, slot);

				/*
				 * The write for this row is now applied; make it visible to a
				 * later reading clause before we hand the row upward (the
				 * consumer may read the heap as soon as it receives it).
				 */
				publishModifiedCid(mgstate);

				if (slot != NULL)
				{
					return slot;
				}
				else
				{
					Assert(plan->last == true);
				}
			}

			mgstate->child_done = true;

			if (mgstate->elemTable != NULL
				&& plan->operation != GWROP_DELETE
				&& (plan->operation != GWROP_SET || plan->accumulate))
				reflectModifiedProp(mgstate);
		}
	}

	if (mgstate->eagerness)
	{
		TupleTableSlot *result;

		/* don't care about scan direction */
		result = mgstate->ps.ps_ResultTupleSlot;
		tuplestore_gettupleslot(mgstate->tuplestorestate, true, false, result);

		if (TupIsNull(result))
			return result;

		slot_getallattrs(result);

		if (mgstate->elemTable == NULL ||
			hash_get_num_entries(mgstate->elemTable) < 1)
			return result;

		/*
		 * A returning CALL body surfaces each row's own accumulated value:
		 * keep the buffered properties, but refresh the tuple id -- the
		 * clause-end flush superseded the version captured at scan time, and
		 * a later write clause updating through a stale id would fail with
		 * "attempted to update invisible tuple".  Everything else (including
		 * a unit CALL body, whose collapsed output must carry the final
		 * state) substitutes the final element.
		 */
		if (plan->accumOwnValues)
		{
			reflectTupleTids(pstate, result);
			return result;
		}

		reflectTupleChanges(pstate, result);

		return result;
	}

	mgstate->done = true;

	return NULL;
}

/*
 * ExecModifyGraphBeginIteration
 *
 * Reset a write clause for the next iteration of the per-row CALL subquery
 * body it belongs to.  The CypherCall join announces each iteration
 * explicitly (ExecCypherCallBeginIteration) before it rescans the body -- a
 * plan-shape-independent signal, where inferring the boundary from changed
 * parameters would fail whenever the planner has elided the body's outer
 * references (e.g. a body whose reads collapsed to a constant-false scan).
 *
 * Cross-iteration visibility is what distinguishes this from an ordinary
 * rescan: each iteration must observe every previous iteration's writes and
 * none of its own clause's.  Writes are stamped with virtual command ids
 * inside this clause's window (modify_cid), so advancing the window by the
 * body's full span -- the iteration stride computed at analysis time --
 * re-creates the ordinary consecutive-clause arrangement between
 * iterations.  The real command counter only catches up once, at
 * ExecEndModifyGraph().
 */
static void
ExecModifyGraphBeginIteration(ModifyGraphState *mgstate)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;

	Assert(plan->iterStride > 0);

	/*
	 * A window is only consumed once something was stamped inside it: an
	 * iteration in which this clause wrote nothing left the window clean,
	 * so the next iteration's reads need no higher vantage point.  This
	 * keeps the command-id space consumed proportional to actual writes,
	 * not to input rows.
	 */
	if (mgstate->iter_wrote)
	{
		/* the next window must fit under the command-id ceiling */
		if (mgstate->modify_cid >
			PG_UINT32_MAX - (plan->iterStride + 1) * MODIFY_CID_MAX)
			ereport(ERROR,
					(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
					 errmsg("too many executions of a CALL subquery body in one statement")));
		mgstate->modify_cid += plan->iterStride * MODIFY_CID_MAX;
		mgstate->iter_wrote = false;
	}

	mgstate->done = false;
	mgstate->child_done = false;
	mgstate->predrained = false;
	mgstate->iter_fresh = true;
	tuplestore_clear(mgstate->tuplestorestate);

	/*
	 * Multiple-update/delete detection restarts with the iteration.  Empty
	 * the table in place (re-creating it here would put it in whatever
	 * short-lived context the call runs in), then reset the iteration
	 * context that owns its long-lived element copies -- without the reset
	 * a long input grows the per-query memory by every iteration's copies.
	 * The datums the reset does not cover live in per-tuple memory the
	 * expression-context resets already reclaim.
	 */
	if (mgstate->elemTable != NULL)
	{
		HASH_SEQ_STATUS seq;
		ModifiedElemEntry *entry;

		hash_seq_init(&seq, mgstate->elemTable);
		while ((entry = (ModifiedElemEntry *) hash_seq_search(&seq)) != NULL)
		{
			if (hash_search(mgstate->elemTable, &entry->key,
							HASH_REMOVE, NULL) == NULL)
				elog(ERROR, "modified object table corrupted");
		}
	}

	if (mgstate->iterCxt != NULL)
		MemoryContextReset(mgstate->iterCxt);
}

static bool
callBeginIterationWalker(PlanState *node, void *context)
{
	if (node == NULL)
		return false;

	/*
	 * Mark every node of the body as having a changed parameter, whether or
	 * not its own subtree references one.  The nestloop parameters only
	 * invalidate plan nodes whose subtrees use them; a caching node on a
	 * parameter-free branch (a Materialize or Hash over a plain label scan
	 * on the inner side of a join inside the body) would otherwise just
	 * rewind at the iteration boundary and replay the previous iteration's
	 * tuples, hiding that iteration's writes.  The mark uses a parameter
	 * number no plan node reads (one past the last real one), so it only
	 * trips the "something changed, recompute" checks; the first rescan of
	 * each node consumes it.
	 */
	node->chgParam = bms_add_member(node->chgParam, *(int *) context);

	if (IsA(node, ModifyGraphState))
	{
		ModifyGraphState *mgstate = (ModifyGraphState *) node;

		ExecModifyGraphBeginIteration(mgstate);
		/* the input lives in a private field the generic walker misses */
		return callBeginIterationWalker(mgstate->subplan, context);
	}

	if (IsA(node, GraphVLEState))
		return callBeginIterationWalker(((GraphVLEState *) node)->subplan,
										context);

	return planstate_tree_walker(node, callBeginIterationWalker, context);
}

/*
 * ExecCypherCallBeginIteration
 *
 * Called by the CypherCall nestloop for every input row, before it rescans
 * the body: reset every write clause of the body for the new iteration.  (A
 * nested per-row CALL cannot occur inside the body -- the parser rejects it
 * -- so every ModifyGraph found belongs to this body.)
 */
void
ExecCypherCallBeginIteration(PlanState *body)
{
	int			iterparam;

	check_stack_depth();

	iterparam = list_length(body->state->es_plannedstmt->paramExecTypes);
	(void) callBeginIterationWalker(body, &iterparam);
}

/*
 * ExecReScanModifyGraph
 *
 * Rescan a write clause of a per-row CALL subquery body (the only
 * graph-writing plans a rescan reaches).  At an iteration boundary the
 * clause was just reset by ExecCypherCallBeginIteration; all that remains is
 * to pass the changed parameters down its input.  Any other rescan is a join
 * inside the body re-reading its input side within one iteration: the
 * clause's writes have happened and its output is buffered (every
 * non-terminal body write is eager), so replay the buffer exactly as a
 * Material node would.
 */
void
ExecReScanModifyGraph(ModifyGraphState *mgstate)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;

	if (plan->iterStride == 0)
		elog(ERROR, "cannot re-scan a graph-writing plan outside a CALL subquery body");

	if (mgstate->iter_fresh)
	{
		mgstate->iter_fresh = false;

		/*
		 * The input is held in a private "subplan" field, so
		 * changed-parameter signaling is on us (as in
		 * ExecReScanSubqueryScan).
		 */
		if (mgstate->ps.chgParam != NULL)
			UpdateChangedParamSet(mgstate->subplan, mgstate->ps.chgParam);
		if (mgstate->subplan->chgParam == NULL)
			ExecReScan(mgstate->subplan);
		return;
	}

	if (!mgstate->eagerness)
		elog(ERROR, "cannot replay a streaming graph-writing plan");
	tuplestore_rescan(mgstate->tuplestorestate);
}

void
ExecEndModifyGraph(ModifyGraphState *mgstate)
{
	CommandId	used_cid;

	if (mgstate->update_cols != NULL)
	{
		pfree(mgstate->update_cols);
	}

	tuplestore_end(mgstate->tuplestorestate);

	if (mgstate->elemTable != NULL)
		hash_destroy(mgstate->elemTable);

	/*
	 * clean out the tuple table
	 */
	ExecClearTuple(mgstate->ps.ps_ResultTupleSlot);

	/*
	 * Terminate EPQ execution if active
	 */
	EvalPlanQualEnd(&mgstate->mt_epqstate);

	ExecEndNode(mgstate->subplan);

	/*
	 * ModifyGraph plan uses multi-level CommandId for supporting visibitliy
	 * between cypher Clauses. Need to raise the cid to see the modifications
	 * made by this ModifyGraph plan in the next command.
	 */
	used_cid = mgstate->modify_cid + MODIFY_CID_MAX;
	if (used_cid > GetCurrentCommandId(true))
		ForwardCommandCounterTo(used_cid);
}

static void
initGraphWRStats(ModifyGraphState *mgstate, GraphWriteOp op)
{
	if (mgstate->pattern != NIL)
	{
		Assert(op == GWROP_CREATE || op == GWROP_MERGE);

		graphWriteStats.insertVertex = 0;
		graphWriteStats.insertEdge = 0;
	}
	if (mgstate->exprs != NIL)
	{
		Assert(op == GWROP_DELETE);

		graphWriteStats.deleteVertex = 0;
		graphWriteStats.deleteEdge = 0;
	}
	if (mgstate->sets != NIL)
	{
		Assert(op == GWROP_SET || op == GWROP_MERGE);

		graphWriteStats.updateProperty = 0;
	}
}

static List *
ExecInitGraphPattern(List *pattern, ModifyGraphState *mgstate)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;
	GraphPath  *gpath;
	ListCell   *le;

	if (plan->operation != GWROP_MERGE)
		return pattern;

	Assert(list_length(pattern) == 1);

	gpath = linitial(pattern);

	foreach(le, gpath->chain)
	{
		Node	   *elem = lfirst(le);

		if (IsA(elem, GraphVertex))
		{
			GraphVertex *gvertex = (GraphVertex *) elem;

			gvertex->es_expr = ExecInitExpr((Expr *) gvertex->expr,
											(PlanState *) mgstate);
		}
		else
		{
			GraphEdge  *gedge = (GraphEdge *) elem;

			Assert(IsA(elem, GraphEdge));

			gedge->es_expr = ExecInitExpr((Expr *) gedge->expr,
										  (PlanState *) mgstate);
		}
	}

	return pattern;
}

static List *
ExecInitGraphSets(List *sets, ModifyGraphState *mgstate)
{
	ListCell   *ls;

	foreach(ls, sets)
	{
		GraphSetProp *gsp = lfirst(ls);

		gsp->es_elem = ExecInitExpr((Expr *) gsp->elem, (PlanState *) mgstate);
		gsp->es_expr = ExecInitExpr((Expr *) gsp->expr, (PlanState *) mgstate);
	}

	return sets;
}

static List *
ExecInitGraphDelExprs(List *exprs, ModifyGraphState *mgstate)
{
	ListCell   *lc;

	foreach(lc, exprs)
	{
		GraphDelElem *gde = lfirst(lc);

		gde->es_elem = ExecInitExpr((Expr *) gde->elem, (PlanState *) mgstate);
	}

	return exprs;
}

Datum
getElementFromEleTable(ModifyGraphState *mgstate, Oid type_oid, Datum orig_elem,
					   Datum gid, bool *found)
{
	ModifyGraph *plan = (ModifyGraph *) mgstate->ps.plan;
	ModifiedElemEntry *entry;

	entry = hash_search(mgstate->elemTable, &gid, HASH_FIND, found);

	/* Unmodified or deleted */
	if (!(*found) || plan->operation == GWROP_DELETE)
		return (Datum) 0;
	else
		return entry->elem;
}

Datum
getPathFinal(ModifyGraphState *mgstate, Datum origin)
{
	Datum		vertices_datum;
	Datum		edges_datum;
	AnyArrayType *arrVertices;
	AnyArrayType *arrEdges;
	int			nvertices;
	int			nedges;
	Datum	   *vertices;
	Datum	   *edges;
	int16		typlen;
	bool		typbyval;
	char		typalign;
	array_iter	it;
	int			i;
	Datum		value;
	bool		isnull;
	bool		modified = false;
	bool		isdeleted = false;
	Datum		result;

	getGraphpathArrays(origin, &vertices_datum, &edges_datum);

	arrVertices = DatumGetAnyArrayP(vertices_datum);
	arrEdges = DatumGetAnyArrayP(edges_datum);

	nvertices = ArrayGetNItems(AARR_NDIM(arrVertices), AARR_DIMS(arrVertices));
	nedges = ArrayGetNItems(AARR_NDIM(arrEdges), AARR_DIMS(arrEdges));
	Assert(nvertices == nedges + 1);

	vertices = palloc(nvertices * sizeof(Datum));
	edges = palloc(nedges * sizeof(Datum));

	get_typlenbyvalalign(AARR_ELEMTYPE(arrVertices), &typlen,
						 &typbyval, &typalign);
	array_iter_setup(&it, arrVertices);
	for (i = 0; i < nvertices; i++)
	{
		Datum		vertex;
		Datum		graphid;
		bool		found;

		value = array_iter_next(&it, &isnull, i, typlen, typbyval, typalign);
		Assert(!isnull);

		graphid = getVertexIdDatum(value);
		vertex = getElementFromEleTable(mgstate, VERTEXOID, value, graphid,
										&found);

		if (!found)
		{
			vertex = value;
		}

		if (vertex == (Datum) 0)
		{
			if (i == 0)
				isdeleted = true;

			if (isdeleted)
				continue;
			else
				elog(ERROR, "cannot delete a vertex in graphpath");
		}

		if (found)
		{
			modified = true;
		}

		vertices[i] = vertex;
	}

	get_typlenbyvalalign(AARR_ELEMTYPE(arrEdges), &typlen,
						 &typbyval, &typalign);
	array_iter_setup(&it, arrEdges);
	for (i = 0; i < nedges; i++)
	{
		Datum		edge;
		Datum		graphid;
		bool		found;

		value = array_iter_next(&it, &isnull, i, typlen, typbyval, typalign);
		Assert(!isnull);

		graphid = getEdgeIdDatum(value);
		edge = getElementFromEleTable(mgstate, EDGEOID, value, graphid, &found);

		if (!found)
		{
			edge = value;
		}

		if (edge == (Datum) 0)
		{
			if (isdeleted)
				continue;
			else
				elog(ERROR, "cannot delete a edge in graphpath.");
		}

		if (found)
		{
			modified = true;
		}

		edges[i] = edge;
	}

	if (isdeleted)
		result = (Datum) 0;
	else if (modified)
		result = makeGraphpathDatum(vertices, nvertices, edges, nedges);
	else
		result = origin;

	pfree(vertices);
	pfree(edges);

	return result;
}

static void
reflectModifiedProp(ModifyGraphState *mgstate)
{
	HASH_SEQ_STATUS seq;
	ModifiedElemEntry *entry;

	Assert(mgstate->elemTable != NULL);

	hash_seq_init(&seq, mgstate->elemTable);
	while ((entry = hash_seq_search(&seq)) != NULL)
	{
		ItemPointer ctid;
		Datum		gid = PointerGetDatum((void *) entry->key);
		Oid			type;

		type = get_labid_typeoid(mgstate->graphid,
								 GraphidGetLabid(DatumGetGraphid(gid)));

		ctid = LegacyUpdateElemProp(mgstate, type, gid, entry->elem);

		if (mgstate->eagerness)
		{
			Datum		property;
			Datum		newelem;

			if (type == VERTEXOID)
				property = getVertexPropDatum(entry->elem);
			else if (type == EDGEOID)
				property = getEdgePropDatum(entry->elem);
			else
				elog(ERROR, "unexpected graph type %d", type);

			if (mgstate->iterCxt != NULL)
			{
				MemoryContext oldcxt = MemoryContextSwitchTo(mgstate->iterCxt);

				newelem = makeModifiedElem(entry->elem, type, gid, property,
										   PointerGetDatum(ctid));
				MemoryContextSwitchTo(oldcxt);
			}
			else
				newelem = makeModifiedElem(entry->elem, type, gid, property,
										   PointerGetDatum(ctid));

			pfree(DatumGetPointer(entry->elem));
			entry->elem = newelem;
		}
	}
}

ResultRelInfo *
getResultRelInfo(ModifyGraphState *mgstate, Oid relid)
{
	ResultRelInfo *resultRelInfo = mgstate->resultRelInfo;
	int			i;

	for (i = 0; i < mgstate->numResultRelInfo; i++)
	{
		if (RelationGetRelid(resultRelInfo->ri_RelationDesc) == relid)
			return resultRelInfo;
		resultRelInfo++;
	}

	elog(ERROR, "invalid object ID %u for the target label", relid);
}

/*
 * GraphmetaRecordEdgeInsertFromSlot
 *		Record, in the transaction's ag_graphmeta deltas, that an edge described
 *		by `slot` was created in `rel`.  No-op unless connectivity gathering is
 *		on and `rel` is an edge label.  This is how non-Cypher write paths
 *		(direct SQL DML, COPY, logical-replication apply) keep ag_graphmeta
 *		complete, so graphmeta-based scan pruning stays sound.
 *
 * Only the connectivity-adding direction (inserts, and the new endpoint of a
 * rewiring update) is recorded here, which is all that soundness requires:
 * missing a new triple could make pruning drop rows, whereas a stale leftover
 * triple only causes a harmless extra empty scan.  Deletions on these
 * non-Cypher paths are therefore not decremented and may leave ag_graphmeta
 * overstating connectivity until a regather() compacts it (never wrong, only an
 * extra scan).  The graph and edge label are taken from `rel`, not the session
 * graph_path, so this is correct regardless of the caller's graph_path.
 */
void
GraphmetaRecordEdgeInsertFromSlot(Relation rel, TupleTableSlot *slot)
{
	HeapTuple	tup;
	Form_ag_label lab;
	Oid			graph;
	Labid		edge_labid;
	Datum		startd;
	Datum		endd;
	bool		startnull;
	bool		endnull;

	if (!auto_gather_graphmeta)
		return;

	/*
	 * Resolving (is-edge-label, graph, edge labid) from the relation is
	 * invariant for a given target, so a bulk load repeats it per row.  This is
	 * left per-row deliberately: LABELRELID is syscache-backed, so after the
	 * first row it is a hash probe, and it is dominated anyway by the per-row
	 * dynahash insert in agstat_count_edge_create_ext().  A relation-keyed memo
	 * is not worth the hazard -- a stale entry surviving a DROP/CREATE relid
	 * reuse could mis-resolve an edge label as a non-edge and under-record
	 * connectivity, dropping rows (the one failure the soundness rule forbids).
	 */
	tup = SearchSysCache1(LABELRELID, ObjectIdGetDatum(RelationGetRelid(rel)));
	if (!HeapTupleIsValid(tup))
		return;					/* not a graph label */
	lab = (Form_ag_label) GETSTRUCT(tup);
	if (lab->labkind != LABEL_KIND_EDGE)
	{
		ReleaseSysCache(tup);
		return;
	}
	graph = lab->graphid;
	edge_labid = (Labid) lab->labid;
	ReleaseSysCache(tup);

	startd = slot_getattr(slot, Anum_ag_edge_start, &startnull);
	endd = slot_getattr(slot, Anum_ag_edge_end, &endnull);
	if (startnull || endnull)
		return;

	agstat_count_edge_create_ext(graph, edge_labid,
								 GraphidGetLabid(DatumGetGraphid(startd)),
								 GraphidGetLabid(DatumGetGraphid(endd)));
}

Datum
findVertex(TupleTableSlot *slot, GraphVertex *gvertex, Graphid *vid)
{
	bool		isnull;
	Datum		vertex;

	if (gvertex->resno == InvalidAttrNumber)
		return (Datum) 0;

	vertex = slot_getattr(slot, gvertex->resno, &isnull);
	if (isnull)
		return (Datum) 0;

	if (vid != NULL)
		*vid = DatumGetGraphid(getVertexIdDatum(vertex));

	return vertex;
}

Datum
findEdge(TupleTableSlot *slot, GraphEdge *gedge, Graphid *eid)
{
	bool		isnull;
	Datum		edge;

	if (gedge->resno == InvalidAttrNumber)
		return (Datum) 0;

	edge = slot_getattr(slot, gedge->resno, &isnull);
	if (isnull)
		return (Datum) 0;

	if (eid != NULL)
		*eid = DatumGetGraphid(getEdgeIdDatum(edge));

	return edge;
}

AttrNumber
findAttrInSlotByName(TupleTableSlot *slot, char *name)
{
	TupleDesc	tupDesc = slot->tts_tupleDescriptor;
	int			i;

	for (i = 0; i < tupDesc->natts; i++)
	{
		Form_pg_attribute attr = TupleDescAttr(tupDesc, i);

		if (namestrcmp(&(attr->attname), name) == 0 && !attr->attisdropped)
			return attr->attnum;
	}

	ereport(ERROR,
			(errcode(ERRCODE_INVALID_NAME),
			 errmsg("variable \"%s\" does not exist", name)));
}

void
setSlotValueByName(TupleTableSlot *slot, Datum value, char *name)
{
	AttrNumber	attno;

	if (slot == NULL)
		return;

	attno = findAttrInSlotByName(slot, name);

	setSlotValueByAttnum(slot, value, attno);
}

void
setSlotValueByAttnum(TupleTableSlot *slot, Datum value, int attnum)
{
	if (slot == NULL)
		return;

	Assert(attnum > 0 && attnum <= slot->tts_tupleDescriptor->natts);

	slot->tts_values[attnum - 1] = value;
	slot->tts_isnull[attnum - 1] = (value == (Datum) 0) ? true : false;
}

Datum *
makeDatumArray(int len)
{
	if (len == 0)
		return NULL;

	return palloc(len * sizeof(Datum));
}

static bool
isEdgeArrayOfPath(List *exprs, char *variable)
{
	ListCell   *lc;

	foreach(lc, exprs)
	{
		GraphDelElem *gde = castNode(GraphDelElem, lfirst(lc));

		if (exprType(gde->elem) == EDGEARRAYOID &&
			strcmp(gde->variable, variable) == 0)
			return true;
	}

	return false;
}

/*
 * openResultRelInfosIndices
 */
static void
openResultRelInfosIndices(ModifyGraphState *mgstate)
{
	int			index;
	ResultRelInfo *resultRelInfo = mgstate->resultRelInfo;

	for (index = 0; index < mgstate->numResultRelInfo; index++)
	{
		ExecOpenIndices(resultRelInfo, false);
		resultRelInfo++;
	}
}
