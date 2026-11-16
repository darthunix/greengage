/*-------------------------------------------------------------------------
 *
 * test_explain_hooks.c
 *		An extension of EXPLAIN for tests: an option of its own, a line on
 *		each plan node, and data that each qExec sends to the qDisp.
 *
 * EXPLAIN (ANALYZE, TEST_HOOKS) prints "Hooked: true" on each plan node
 * and, after the plan, the segments whose qExecs reported for each slice.
 * At the end of every EXPLAIN ANALYZE each qExec sends two pieces of data:
 * one with the slice it ran, its segment and its qe_identifier, which the
 * qDisp checks against those of the message, and an empty one of another
 * name.  The
 * qDisp counts apart the reports that name another statement than the
 * one it explains.  test_explain_hooks.bad_piece makes each qExec send a
 * malformed piece besides.
 *
 * IDENTIFICATION
 *	  src/test/modules/test_explain_hooks/test_explain_hooks.c
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "cdb/cdbexplain.h"
#include "cdb/cdbgang.h"
#include "cdb/cdbvars.h"
#include "commands/defrem.h"
#include "commands/explain.h"
#include "commands/explain_state.h"
#include "executor/execdesc.h"
#include "lib/stringinfo.h"
#include "nodes/execnodes.h"
#include "utils/guc.h"

PG_MODULE_MAGIC;

void		_PG_init(void);

#define PIECE		"test_explain_hooks"
#define OTHER_PIECE "test_explain_hooks other"
#define MAX_REPORTS 256

/* Malformed pieces a qExec can be told to send. */
typedef enum BadPiece
{
	BAD_PIECE_NONE,
	BAD_PIECE_SHORT,			/* less than the length of a name */
	BAD_PIECE_UNTERMINATED,		/* a name without its zero */
	BAD_PIECE_TRUNCATED			/* a length of data but no data */
} BadPiece;

static const struct config_enum_entry bad_piece_options[] = {
	{"none", BAD_PIECE_NONE, false},
	{"short", BAD_PIECE_SHORT, false},
	{"unterminated", BAD_PIECE_UNTERMINATED, false},
	{"truncated", BAD_PIECE_TRUNCATED, false},
	{NULL, 0, false}
};

static int	bad_piece = BAD_PIECE_NONE;

/* What a qExec says of itself. */
typedef struct SliceSegment
{
	int32		slice;
	int32		segment;
	int32		qe;				/* its qe_identifier */
} SliceSegment;

/* What the qDisp received during the current EXPLAIN. */
typedef struct Report
{
	void	   *statement;		/* the showstatctx the piece came with */
	SliceSegment who;
} Report;

static Report reports[MAX_REPORTS];
static int	nreports = 0;
static int	nother = 0;
static int	nmessages = 0;		/* of the explained statement or another */

static int	extension_id;

static explain_validate_options_hook_type prev_validate_options_hook = NULL;
static explain_per_plan_hook_type prev_explain_per_plan_hook = NULL;
static explain_per_node_hook_type prev_explain_per_node_hook = NULL;
static cdbexplain_send_hook_type prev_cdbexplain_send_hook = NULL;
static cdbexplain_recv_hook_type prev_cdbexplain_recv_hook = NULL;

/* Whether the EXPLAIN asked for TEST_HOOKS. */
static bool
hooked(ExplainState *es)
{
	bool	   *on = GetExplainExtensionState(es, extension_id);

	return on != NULL && *on;
}

static void
test_hooks_option(ExplainState *es, DefElem *opt, ParseState *pstate)
{
	bool	   *on = GetExplainExtensionState(es, extension_id);

	if (on == NULL)
	{
		on = palloc(sizeof(bool));
		SetExplainExtensionState(es, extension_id, on);
	}
	*on = defGetBoolean(opt);
}

static void
test_validate_options(ExplainState *es, List *options, ParseState *pstate)
{
	if (prev_validate_options_hook)
		prev_validate_options_hook(es, options, pstate);

	/* Every EXPLAIN starts with no reports. */
	nreports = 0;
	nother = 0;
	nmessages = 0;

	if (hooked(es) && !es->analyze)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("EXPLAIN option TEST_HOOKS requires ANALYZE")));
}

static void
test_explain_per_node(PlanState *planstate, List *ancestors,
					  const char *relationship, const char *plan_name,
					  ExplainState *es)
{
	if (prev_explain_per_node_hook)
		prev_explain_per_node_hook(planstate, ancestors, relationship,
								   plan_name, es);

	if (hooked(es))
		ExplainPropertyBool("Hooked", true, es);
}

static int
report_cmp(const void *a, const void *b)
{
	const SliceSegment *x = &((const Report *) a)->who;
	const SliceSegment *y = &((const Report *) b)->who;

	if (x->slice != y->slice)
		return x->slice < y->slice ? -1 : 1;
	if (x->segment != y->segment)
		return x->segment < y->segment ? -1 : 1;
	return 0;
}

static void
test_explain_per_plan(PlannedStmt *plannedstmt, IntoClause *into,
					  ExplainState *es, const char *queryString,
					  ParamListInfo params, QueryEnvironment *queryEnv)
{
	StringInfoData text;
	int			nforeign = 0;
	int			last_slice = -1;

	if (prev_explain_per_plan_hook)
		prev_explain_per_plan_hook(plannedstmt, into, es, queryString,
								   params, queryEnv);

	if (!hooked(es))
		return;

	qsort(reports, nreports, sizeof(Report), report_cmp);
	initStringInfo(&text);
	for (int i = 0; i < nreports; i++)
	{
		const SliceSegment *who = &reports[i].who;

		/* The pieces name the statement whose statistics EXPLAIN shows. */
		if (reports[i].statement != es->showstatctx)
		{
			nforeign++;
			continue;
		}
		if (who->slice != last_slice)
			appendStringInfo(&text, "%sslice%d:", text.len == 0 ? "" : " ",
							 who->slice);
		appendStringInfo(&text, " seg%d", who->segment);
		last_slice = who->slice;
	}
	ExplainPropertyText("QE Reports", text.data, es);
	ExplainPropertyInteger("QE Messages", NULL, nmessages, es);
	ExplainPropertyInteger("Other Pieces", NULL, nother, es);
	ExplainPropertyInteger("Reports of Other Statements", NULL, nforeign, es);
	pfree(text.data);
}

/* On a qExec: say which slice it ran and on which segment. */
static void
test_cdbexplain_send(QueryDesc *queryDesc, StringInfo buf)
{
	SliceSegment piece;

	if (prev_cdbexplain_send_hook)
		prev_cdbexplain_send_hook(queryDesc, buf);

	piece.slice = LocallyExecutingSliceIndex(queryDesc->estate);
	piece.segment = GpIdentity.segindex;
	piece.qe = qe_identifier;
	cdbexplain_appendExtensionData(buf, PIECE, &piece, sizeof(piece));
	cdbexplain_appendExtensionData(buf, OTHER_PIECE, "", 0);

	switch ((BadPiece) bad_piece)
	{
		case BAD_PIECE_NONE:
			break;
		case BAD_PIECE_SHORT:
			appendBinaryStringInfo(buf, "\1\0", 2);
			break;
		case BAD_PIECE_UNTERMINATED:
			{
				int32		namelen = 4;

				appendBinaryStringInfo(buf, (char *) &namelen, sizeof(namelen));
				appendBinaryStringInfo(buf, "abcd", 4);
				break;
			}
		case BAD_PIECE_TRUNCATED:
			{
				int32		namelen = 2;
				int32		len = 8;

				appendBinaryStringInfo(buf, (char *) &namelen, sizeof(namelen));
				appendBinaryStringInfo(buf, "x", 2);
				appendBinaryStringInfo(buf, (char *) &len, sizeof(len));
				break;
			}
	}
}

/* On the qDisp: check what a qExec said against its message. */
static void
test_cdbexplain_recv(struct CdbExplain_ShowStatCtx *showstatctx,
					 int sliceIndex, int segindex, int qe,
					 const char *name, const char *data, int len)
{
	SliceSegment piece;

	if (prev_cdbexplain_recv_hook)
		prev_cdbexplain_recv_hook(showstatctx, sliceIndex, segindex,
								  qe, name, data, len);

	/* A message comes first, with no name, then its pieces. */
	if (name == NULL)
	{
		nmessages++;
		return;
	}

	if (strcmp(name, OTHER_PIECE) == 0)
	{
		if (len != 0)
			elog(ERROR, "the other piece of seg%d has %d bytes", segindex, len);
		nother++;
		return;
	}
	if (strcmp(name, PIECE) != 0)
		return;

	if (len != sizeof(piece))
		elog(ERROR, "the piece of seg%d has %d bytes", segindex, len);
	memcpy(&piece, data, sizeof(piece));
	if (piece.slice != sliceIndex || piece.segment != segindex ||
		piece.qe != qe)
		elog(ERROR, "qExec %d of slice %d on seg%d says qExec %d of slice %d on seg%d",
			 qe, sliceIndex, segindex,
			 piece.qe, piece.slice, piece.segment);
	if (nreports < MAX_REPORTS)
	{
		reports[nreports].statement = showstatctx;
		reports[nreports].who = piece;
		nreports++;
	}
}

void
_PG_init(void)
{
	DefineCustomEnumVariable("test_explain_hooks.bad_piece",
							 "Makes each qExec send a malformed piece of data.",
							 NULL,
							 &bad_piece,
							 BAD_PIECE_NONE,
							 bad_piece_options,
							 PGC_USERSET,
							 GUC_GPDB_NEED_SYNC,
							 NULL, NULL, NULL);

	extension_id = GetExplainExtensionId("test_explain_hooks");
	RegisterExtensionExplainOption("test_hooks", test_hooks_option);

	prev_validate_options_hook = explain_validate_options_hook;
	explain_validate_options_hook = test_validate_options;
	prev_explain_per_plan_hook = explain_per_plan_hook;
	explain_per_plan_hook = test_explain_per_plan;
	prev_explain_per_node_hook = explain_per_node_hook;
	explain_per_node_hook = test_explain_per_node;
	prev_cdbexplain_send_hook = cdbexplain_send_hook;
	cdbexplain_send_hook = test_cdbexplain_send;
	prev_cdbexplain_recv_hook = cdbexplain_recv_hook;
	cdbexplain_recv_hook = test_cdbexplain_recv;
}
