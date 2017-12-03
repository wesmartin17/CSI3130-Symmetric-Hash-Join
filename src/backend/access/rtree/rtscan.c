/*-------------------------------------------------------------------------
 *
 * rtscan.c
 *	  routines to manage scans on index relations
 *
 * Portions Copyright (c) 1996-2005, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  $PostgreSQL: pgsql/src/backend/access/rtree/rtscan.c,v 1.60 2005/10/15 02:49:09 momjian Exp $
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "access/genam.h"
#include "access/rtree.h"
#include "utils/lsyscache.h"
#include "utils/resowner.h"


/* routines defined and used here */
static void rtregscan(IndexScanDesc s);
static void rtdropscan(IndexScanDesc s);
static void rtadjone(IndexScanDesc s, int op, BlockNumber blkno,
		 OffsetNumber offnum);
static void adjuststack(RTSTACK *stk, BlockNumber blkno);
static void adjustiptr(IndexScanDesc s, ItemPointer iptr,
		   int op, BlockNumber blkno, OffsetNumber offnum);

/*
 *	Whenever we start an rtree scan in a backend, we register it in private
 *	space.	Then if the rtree index gets updated, we check all registered
 *	scans and adjust them if the tuple they point at got moved by the
 *	update.  We only need to do this in private space, because when we update
 *	an rtree we have a write lock on the tree, so no other process can have
 *	any locks at all on it.  A single transaction can have write and read
 *	locks on the same object, so that's why we need to handle this case.
 */

typedef struct RTScanListData
{
	IndexScanDesc rtsl_scan;
	ResourceOwner rtsl_owner;
	struct RTScanListData *rtsl_next;
} RTScanListData;

typedef RTScanListData *RTScanList;

/* pointer to list of local scans on rtrees */
static RTScanList RTScans = NULL;

Datum
rtbeginscan(PG_FUNCTION_ARGS)
{
	Relation	r = (Relation) PG_GETARG_POINTER(0);
	int			nkeys = PG_GETARG_INT32(1);
	ScanKey		key = (ScanKey) PG_GETARG_POINTER(2);
	IndexScanDesc s;

	s = RelationGetIndexScan(r, nkeys, key);

	rtregscan(s);

	PG_RETURN_POINTER(s);
}

Datum
rtrescan(PG_FUNCTION_ARGS)
{
	IndexScanDesc s = (IndexScanDesc) PG_GETARG_POINTER(0);
	ScanKey		key = (ScanKey) PG_GETARG_POINTER(1);
	RTreeScanOpaque p;
	int			i;

	/*
	 * Clear all the pointers.
	 */
	ItemPointerSetInvalid(&s->currentItemData);
	ItemPointerSetInvalid(&s->currentMarkData);

	p = (RTreeScanOpaque) s->opaque;
	if (p != NULL)
	{
		/* rescan an existing indexscan --- reset state */
		freestack(p->s_stack);
		freestack(p->s_markstk);
		p->s_stack = p->s_markstk = NULL;
		p->s_flags = 0x0;
		/* drop pins on buffers -- no locks held */
		if (BufferIsValid(p->curbuf))
		{
			ReleaseBuffer(p->curbuf);
			p->curbuf = InvalidBuffer;
		}
		if (BufferIsValid(p->markbuf))
		{
			ReleaseBuffer(p->markbuf);
			p->markbuf = InvalidBuffer;
		}
	}
	else
	{
		/* initialize opaque data */
		p = (RTreeScanOpaque) palloc(sizeof(RTreeScanOpaqueData));
		p->s_stack = p->s_markstk = NULL;
		p->curbuf = p->markbuf = InvalidBuffer;
		p->s_internalNKey = s->numberOfKeys;
		p->s_flags = 0x0;
		s->opaque = p;
		if (s->numberOfKeys > 0)
			p->s_internalKey = (ScanKey) palloc(sizeof(ScanKeyData) * s->numberOfKeys);
	}

	/* Update scan key, if a new one is given */
	if (key && s->numberOfKeys > 0)
	{
		memmove(s->keyData,
				key,
				s->numberOfKeys * sizeof(ScanKeyData));

		/*
		 * Scans on internal pages use different operators than they do on
		 * leaf pages.	For example, if the user wants all boxes that exactly
		 * match (x1,y1,x2,y2), then on internal pages we need to find all
		 * boxes that contain (x1,y1,x2,y2).  rtstrat.c knows how to pick the
		 * opclass member to use for internal pages. In some cases we need to
		 * negate the result of the opclass member.
		 */
		for (i = 0; i < s->numberOfKeys; i++)
		{
			AttrNumber	attno = s->keyData[i].sk_attno;
			Oid			opclass;
			Oid			subtype;
			StrategyNumber orig_strategy;
			StrategyNumber int_strategy;
			Oid			int_oper;
			RegProcedure int_proc;
			int			int_flags;

			opclass = s->indexRelation->rd_indclass->values[attno - 1];
			subtype = s->keyData[i].sk_subtype;
			orig_strategy = s->keyData[i].sk_strategy;
			int_strategy = RTMapToInternalOperator(orig_strategy);
			int_oper = get_opclass_member(opclass, subtype, int_strategy);
			Assert(OidIsValid(int_oper));
			int_proc = get_opcode(int_oper);
			int_flags = s->keyData[i].sk_flags;
			if (RTMapToInternalNegate(orig_strategy))
				int_flags |= SK_NEGATE;
			ScanKeyEntryInitialize(&(p->s_internalKey[i]),
								   int_flags,
								   attno,
								   int_strategy,
								   subtype,
								   int_proc,
								   s->keyData[i].sk_argument);
		}
	}

	PG_RETURN_VOID();
}

Datum
rtmarkpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc s = (IndexScanDesc) PG_GETARG_POINTER(0);
	RTreeScanOpaque p;
	RTSTACK    *o,
			   *n,
			   *tmp;

	s->currentMarkData = s->currentItemData;
	p = (RTreeScanOpaque) s->opaque;
	if (p->s_flags & RTS_CURBEFORE)
		p->s_flags |= RTS_MRKBEFORE;
	else
		p->s_flags &= ~RTS_MRKBEFORE;

	o = NULL;
	n = p->s_stack;

	/* copy the parent stack from the current item data */
	while (n != NULL)
	{
		tmp = (RTSTACK *) palloc(sizeof(RTSTACK));
		tmp->rts_child = n->rts_child;
		tmp->rts_blk = n->rts_blk;
		tmp->rts_parent = o;
		o = tmp;
		n = n->rts_parent;
	}

	freestack(p->s_markstk);
	p->s_markstk = o;

	/* Update markbuf: make sure to bump ref count on curbuf */
	if (BufferIsValid(p->markbuf))
	{
		ReleaseBuffer(p->markbuf);
		p->markbuf = InvalidBuffer;
	}
	if (BufferIsValid(p->curbuf))
	{
		IncrBufferRefCount(p->curbuf);
		p->markbuf = p->curbuf;
	}

	PG_RETURN_VOID();
}

Datum
rtrestrpos(PG_FUNCTION_ARGS)
{
	IndexScanDesc s = (IndexScanDesc) PG_GETARG_POINTER(0);
	RTreeScanOpaque p;
	RTSTACK    *o,
			   *n,
			   *tmp;

	s->currentItemData = s->currentMarkData;
	p = (RTreeScanOpaque) s->opaque;
	if (p->s_flags & RTS_MRKBEFORE)
		p->s_flags |= RTS_CURBEFORE;
	else
		p->s_flags &= ~RTS_CURBEFORE;

	o = NULL;
	n = p->s_markstk;

	/* copy the parent stack from the current item data */
	while (n != NULL)
	{
		tmp = (RTSTACK *) palloc(sizeof(RTSTACK));
		tmp->rts_child = n->rts_child;
		tmp->rts_blk = n->rts_blk;
		tmp->rts_parent = o;
		o = tmp;
		n = n->rts_parent;
	}

	freestack(p->s_stack);
	p->s_stack = o;

	/* Update curbuf; be sure to bump ref count on markbuf */
	if (BufferIsValid(p->curbuf))
	{
		ReleaseBuffer(p->curbuf);
		p->curbuf = InvalidBuffer;
	}
	if (BufferIsValid(p->markbuf))
	{
		IncrBufferRefCount(p->markbuf);
		p->curbuf = p->markbuf;
	}

	PG_RETURN_VOID();
}

Datum
rtendscan(PG_FUNCTION_ARGS)
{
	IndexScanDesc s = (IndexScanDesc) PG_GETARG_POINTER(0);
	RTreeScanOpaque p;

	p = (RTreeScanOpaque) s->opaque;

	if (p != NULL)
	{
		freestack(p->s_stack);
		freestack(p->s_markstk);
		if (BufferIsValid(p->curbuf))
			ReleaseBuffer(p->curbuf);
		if (BufferIsValid(p->markbuf))
			ReleaseBuffer(p->markbuf);
		pfree(s->opaque);
	}

	rtdropscan(s);

	PG_RETURN_VOID();
}

static void
rtregscan(IndexScanDesc s)
{
	RTScanList	l;

	l = (RTScanList) palloc(sizeof(RTScanListData));
	l->rtsl_scan = s;
	l->rtsl_owner = CurrentResourceOwner;
	l->rtsl_next = RTScans;
	RTScans = l;
}

static void
rtdropscan(IndexScanDesc s)
{
	RTScanList	l;
	RTScanList	prev;

	prev = NULL;

	for (l = RTScans;
		 l != NULL && l->rtsl_scan != s;
		 l = l->rtsl_next)
		prev = l;

	if (l == NULL)
		elog(ERROR, "rtree scan list corrupted -- could not find 0x%p",
			 (void *) s);

	if (prev == NULL)
		RTScans = l->rtsl_next;
	else
		prev->rtsl_next = l->rtsl_next;

	pfree(l);
}

/*
 * ReleaseResources_rtree() --- clean up rtree subsystem resources.
 *
 * This is here because it needs to touch this module's static var RTScans.
 */
void
ReleaseResources_rtree(void)
{
	RTScanList	l;
	RTScanList	prev;
	RTScanList	next;

	/*
	 * Note: this should be a no-op during normal query shutdown. However, in
	 * an abort situation ExecutorEnd is not called and so there may be open
	 * index scans to clean up.
	 */
	prev = NULL;

	for (l = RTScans; l != NULL; l = next)
	{
		next = l->rtsl_next;
		if (l->rtsl_owner == CurrentResourceOwner)
		{
			if (prev == NULL)
				RTScans = next;
			else
				prev->rtsl_next = next;

			pfree(l);
			/* prev does not change */
		}
		else
			prev = l;
	}
}

void
rtadjscans(Relation r, int op, BlockNumber blkno, OffsetNumber offnum)
{
	RTScanList	l;
	Oid			relid;

	relid = RelationGetRelid(r);
	for (l = RTScans; l != NULL; l = l->rtsl_next)
	{
		if (RelationGetRelid(l->rtsl_scan->indexRelation) == relid)
			rtadjone(l->rtsl_scan, op, blkno, offnum);
	}
}

/*
 *	rtadjone() -- adjust one scan for update.
 *
 *		By here, the scan passed in is on a modified relation.	Op tells
 *		us what the modification is, and blkno and offind tell us what
 *		block and offset index were affected.  This routine checks the
 *		current and marked positions, and the current and marked stacks,
 *		to see if any stored location needs to be changed because of the
 *		update.  If so, we make the change here.
 */
static void
rtadjone(IndexScanDesc s,
		 int op,
		 BlockNumber blkno,
		 OffsetNumber offnum)
{
	RTreeScanOpaque so;

	adjustiptr(s, &(s->currentItemData), op, blkno, offnum);
	adjustiptr(s, &(s->currentMarkData), op, blkno, offnum);

	so = (RTreeScanOpaque) s->opaque;

	if (op == RTOP_SPLIT)
	{
		adjuststack(so->s_stack, blkno);
		adjuststack(so->s_markstk, blkno);
	}
}

/*
 *	adjustiptr() -- adjust current and marked item pointers in the scan
 *
 *		Depending on the type of update and the place it happened, we
 *		need to do nothing, to back up one record, or to start over on
 *		the same page.
 */
static void
adjustiptr(IndexScanDesc s,
		   ItemPointer iptr,
		   int op,
		   BlockNumber blkno,
		   OffsetNumber offnum)
{
	OffsetNumber curoff;
	RTreeScanOpaque so;

	if (ItemPointerIsValid(iptr))
	{
		if (ItemPointerGetBlockNumber(iptr) == blkno)
		{
			curoff = ItemPointerGetOffsetNumber(iptr);
			so = (RTreeScanOpaque) s->opaque;

			switch (op)
			{
				case RTOP_DEL:
					/* back up one if we need to */
					if (curoff >= offnum)
					{

						if (curoff > FirstOffsetNumber)
						{
							/* just adjust the item pointer */
							ItemPointerSet(iptr, blkno, OffsetNumberPrev(curoff));
						}
						else
						{
							/*
							 * remember that we're before the current tuple
							 */
							ItemPointerSet(iptr, blkno, FirstOffsetNumber);
							if (iptr == &(s->currentItemData))
								so->s_flags |= RTS_CURBEFORE;
							else
								so->s_flags |= RTS_MRKBEFORE;
						}
					}
					break;

				case RTOP_SPLIT:
					/* back to start of page on split */
					ItemPointerSet(iptr, blkno, FirstOffsetNumber);
					if (iptr == &(s->currentItemData))
						so->s_flags &= ~RTS_CURBEFORE;
					else
						so->s_flags &= ~RTS_MRKBEFORE;
					break;

				default:
					elog(ERROR, "unrecognized operation in rtree scan adjust: %d", op);
			}
		}
	}
}

/*
 *	adjuststack() -- adjust the supplied stack for a split on a page in
 *					 the index we're scanning.
 *
 *		If a page on our parent stack has split, we need to back up to the
 *		beginning of the page and rescan it.  The reason for this is that
 *		the split algorithm for rtrees doesn't order tuples in any useful
 *		way on a single page.  This means on that a split, we may wind up
 *		looking at some heap tuples more than once.  This is handled in the
 *		access method update code for heaps; if we've modified the tuple we
 *		are looking at already in this transaction, we ignore the update
 *		request.
 */
static void
adjuststack(RTSTACK *stk, BlockNumber blkno)
{
	while (stk != NULL)
	{
		if (stk->rts_blk == blkno)
			stk->rts_child = FirstOffsetNumber;

		stk = stk->rts_parent;
	}
}
