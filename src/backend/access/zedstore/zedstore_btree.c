/*
 * zedstore_btree.c
 *		Routines for handling B-trees structures in ZedStore
 *
 * A Zedstore table consists of multiple B-trees, one for each attribute. The
 * functions in this file deal with one B-tree at a time, it is the caller's
 * responsibility to tie together the scans of each btree.
 *
 * Operations:
 *
 * - Sequential scan in TID order
 *  - must be efficient with scanning multiple trees in sync
 *
 * - random lookups, by TID (for index scan)
 *
 * - range scans by TID (for bitmap index scan)
 *
 * TODO:
 * - compression
 *
 * NOTES:
 * - Locking order: child before parent, left before right
 *
 * Copyright (c) 2019, PostgreSQL Global Development Group
 *
 * IDENTIFICATION
 *	  src/backend/access/zedstore/zedstore_btree.c
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/itup.h"
#include "access/zedstore_compression.h"
#include "access/zedstore_internal.h"
#include "storage/bufmgr.h"
#include "utils/datum.h"
#include "utils/rel.h"

/* This used to pass around context information, when inserting a new tuple */
typedef struct ZSInsertState
{
	Relation	rel;
	AttrNumber	attno;
	Datum		datum;
	HeapTupleHeader tuple_header;
} ZSInsertState;

/* prototypes for local functions */
static Buffer zsbt_descend(Relation rel, BlockNumber rootblk, ItemPointerData key);
static Buffer zsbt_find_downlink(Relation rel, AttrNumber attno, ItemPointerData key, BlockNumber childblk, int level, int *itemno);
static Buffer zsbt_find_insertion_target(ZSInsertState *state, BlockNumber rootblk);
static ItemPointerData zsbt_insert_to_leaf(Buffer buf, ZSInsertState *state);
static bool zsbt_compress_leaf(Buffer buf);
static void zsbt_split_leaf(Buffer buf, OffsetNumber lastleftoff, ZSInsertState *state,
				ZSBtreeItem *newitem, bool newitemonleft, OffsetNumber newitemoff);
static void zsbt_insert_downlink(Relation rel, AttrNumber attno, Buffer leftbuf, ItemPointerData rightlokey, BlockNumber rightblkno);
static void zsbt_split_internal(Relation rel, AttrNumber attno, Buffer leftbuf, Buffer childbuf,
					OffsetNumber newoff, ItemPointerData newkey, BlockNumber childblk);
static void zsbt_newroot(Relation rel, AttrNumber attno, int level,
			 ItemPointerData key1, BlockNumber blk1,
			 ItemPointerData key2, BlockNumber blk2,
			 Buffer leftchildbuf);
static int zsbt_binsrch_internal(ItemPointerData key, ZSBtreeInternalPageItem *arr, int arr_elems);

/*
 * Insert a new datum to the given attribute's btree.
 *
 * Returns the TID of the new tuple.
 *
 * TODO: When inserting the first attribute of a row, this OK. But subsequent
 * attributes need to be inserted with the same TID. This should take an
 * optional TID argument for that.
 */
ItemPointerData
zsbt_insert(Relation rel, AttrNumber attno, Datum datum, HeapTupleHeader tupleheader)
{
	ZSInsertState state;
	Buffer		buf;
	BlockNumber	rootblk;

	/* TODO: deal with oversized datums that don't fit on a page */

	rootblk = zsmeta_get_root_for_attribute(rel, attno, true);

	state.rel = rel;
	state.attno = attno;
	state.datum = datum;
	state.tuple_header = tupleheader;

	buf = zsbt_find_insertion_target(&state, rootblk);

	return zsbt_insert_to_leaf(buf, &state);
}

/*
 * Find the leaf buffer containing the given key TID.
 */
static Buffer
zsbt_descend(Relation rel, BlockNumber rootblk, ItemPointerData key)
{
	BlockNumber next;
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	ZSBtreeInternalPageItem *items;
	int			nitems;
	int			itemno;
	int			nextlevel = -1;

	next = rootblk;
	for (;;)
	{
		buf = ReadBuffer(rel, next);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);		/* TODO: shared */
		page = BufferGetPage(buf);
		opaque = ZSBtreePageGetOpaque(page);

		if (nextlevel == -1)
			nextlevel = opaque->zs_level;
		else if (opaque->zs_level != nextlevel)
			elog(ERROR, "unexpected level encountered when descending tree");

		if (opaque->zs_level == 0)
			return buf;

		/*
		 * Do we need to walk right? This could happen if the page was concurrently split.
		 */
		if (ItemPointerCompare(&key, &opaque->zs_hikey) >= 0)
		{
			/* follow the right-link */
			next = opaque->zs_next;
			if (next == InvalidBlockNumber)
				elog(ERROR, "fell off the end of btree");
		}
		else
		{
			/* follow the downlink */
			items = ZSBtreeInternalPageGetItems(page);
			nitems = ZSBtreeInternalPageGetNumItems(page);

			itemno = zsbt_binsrch_internal(key, items, nitems);
			if (itemno < 0)
				elog(ERROR, "could not descend tree for tid (%u, %u)",
					 ItemPointerGetBlockNumberNoCheck(&key),
					 ItemPointerGetOffsetNumberNoCheck(&key));
			next = BlockIdGetBlockNumber(&items[itemno].childblk);
			nextlevel--;
		}
		UnlockReleaseBuffer(buf);
	}
}

/*
 * Re-find the parent page containing downlink for given block.
 * The returned page is exclusive-locked, and *itemno_p is set to the
 * position of the downlink in the parent.
 *
 * If 'childblk' is the root, returns InvalidBuffer.
 */
static Buffer
zsbt_find_downlink(Relation rel, AttrNumber attno,
				   ItemPointerData key, BlockNumber childblk, int level,
				   int *itemno_p)
{
	BlockNumber rootblk;
	BlockNumber next;
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	ZSBtreeInternalPageItem *items;
	int			nitems;
	int			itemno;
	int			nextlevel = -1;

	/* start from root */
	rootblk = zsmeta_get_root_for_attribute(rel, attno, true);
	if (rootblk == childblk)
		return InvalidBuffer;

	/* XXX: this is mostly the same as zsbt_descend, but we stop at an internal
	 * page instead of descending all the way down to root */
	next = rootblk;
	for (;;)
	{
		buf = ReadBuffer(rel, next);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		opaque = ZSBtreePageGetOpaque(page);

		if (nextlevel == -1)
			nextlevel = opaque->zs_level;
		else if (nextlevel != opaque->zs_level)
			elog(ERROR, "unexpected level encountered when descending tree");

		if (opaque->zs_level <= level)
			elog(ERROR, "unexpected page level encountered");

		/*
		 * Do we need to walk right? This could happen if the page was concurrently split.
		 */
		if (ItemPointerCompare(&key, &opaque->zs_hikey) >= 0)
		{
			next = opaque->zs_next;
			if (next == InvalidBlockNumber)
				elog(ERROR, "fell off the end of btree");
		}
		else
		{
			items = ZSBtreeInternalPageGetItems(page);
			nitems = ZSBtreeInternalPageGetNumItems(page);

			itemno = zsbt_binsrch_internal(key, items, nitems);
			if (itemno < 0)
				elog(ERROR, "could not descend tree for tid (%u, %u)",
					 ItemPointerGetBlockNumberNoCheck(&key),
					 ItemPointerGetOffsetNumberNoCheck(&key));

			if (opaque->zs_level == level + 1)
			{
				if (BlockIdGetBlockNumber(&items[itemno].childblk) != childblk)
					elog(ERROR, "could not re-find downlink for block %u", childblk);
				*itemno_p = itemno;
				return buf;
			}

			next = BlockIdGetBlockNumber(&items[itemno].childblk);
			nextlevel--;
		}
		UnlockReleaseBuffer(buf);
	}
}

/*
 * Find a target leaf page to insert new row to.
 *
 * This is used when we're free to pick any TID for the new tuple.
 *
 * TODO: Currently, we just descend to rightmost leaf. Should use a free space
 * map or something to find a suitable target.
 */
static Buffer
zsbt_find_insertion_target(ZSInsertState *state, BlockNumber rootblk)
{
	ItemPointerData rightmostkey;

	ItemPointerSet(&rightmostkey, MaxBlockNumber, 0xfffe);

	return zsbt_descend(state->rel, rootblk, rightmostkey);
}

/*
 * Insert tuple to given leaf page. Return TID of the new item.
 */
static ItemPointerData
zsbt_insert_to_leaf(Buffer buf, ZSInsertState *state)
{
	/* If there's space, add here */
	TupleDesc	desc = RelationGetDescr(state->rel);
	Form_pg_attribute attr = &desc->attrs[state->attno - 1];
	Page		page = BufferGetPage(buf);
	ZSBtreePageOpaque *opaque = ZSBtreePageGetOpaque(page);
	Size		datumsz;
	Size		itemsz;
	ZSBtreeItem	*item;
	char	   *dataptr;
	ItemPointerData tid;
	OffsetNumber maxoff;

	/*
	 * Look at the last item, for its tid.
	 */
	maxoff = PageGetMaxOffsetNumber(page);
	if (maxoff >= FirstOffsetNumber)
	{
		ItemId		iid = PageGetItemId(page, maxoff);
		ZSBtreeItem	*hitup = (ZSBtreeItem *) PageGetItem(page, iid);

		tid = hitup->t_tid;
		ItemPointerIncrement(&tid);
	}
	else
	{
		tid = opaque->zs_lokey;
	}

	datumsz = datumGetSize(state->datum, attr->attbyval, attr->attlen);
	itemsz = offsetof(ZSBtreeItem, t_payload) + datumsz;

	/* for first column header needs to be stored as well */
	if (state->attno == 1)
	{
		Assert(state->tuple_header);
		itemsz += SizeofHeapTupleHeader;
	}

	/* TODO: should we detoast or deal with "expanded" datums here? */

	/*
	 * Form a ZSBtreeItem to insert.
	 */
	item = palloc(itemsz);
	item->t_tid = tid;
	item->t_flags = 0;
	item->t_size = itemsz;

	dataptr = ((char *) item) + offsetof(ZSBtreeItem, t_payload);

	if (state->attno == 1)
	{
		memcpy(dataptr, state->tuple_header, SizeofHeapTupleHeader);
		dataptr += SizeofHeapTupleHeader;
	}

	if (attr->attbyval)
		store_att_byval(dataptr, state->datum, attr->attlen);
	else
		memcpy(dataptr, DatumGetPointer(state->datum), datumsz);

	/*
	 * If there's enough space on the page, insert. Otherwise, have to
	 * split the page.
	 */
	if (PageGetFreeSpace(page) < MAXALIGN(itemsz))
	{
		(void) zsbt_compress_leaf(buf);
		maxoff = PageGetMaxOffsetNumber(page);
	}

	if (PageGetFreeSpace(page) >= MAXALIGN(itemsz))
	{
		OffsetNumber off;
		off = PageAddItemExtended(page, (Item) item, itemsz, maxoff + 1, PAI_OVERWRITE);
		if (off == InvalidOffsetNumber)
			elog(ERROR, "didn't fit, after all?");
		MarkBufferDirty(buf);
		/* TODO: WAL-log */

		UnlockReleaseBuffer(buf);

		return tid;
	}
	else
	{
		maxoff = PageGetMaxOffsetNumber(page);
		zsbt_split_leaf(buf, maxoff, state, item, false, maxoff + 1);
		return tid;
	}
}

/*
 * Try to compress all tuples on a page, if not compressed already.
 *
 * Returns true on success. Can fail, if the tuples didn't fit on the page after
 * compressing anymore. The page is left unchanged in that case.
 */
static bool
zsbt_compress_leaf(Buffer buf)
{
	Page		origpage = BufferGetPage(buf);
	Page		page;
	OffsetNumber maxoff;
	ZSCompressContext compressor;
	int			compressed_items = 0;
	bool		success = true;
	int			already_compressed = 0;
	int			total_compressed = 0;
	int			total = 0;

	zs_compress_init(&compressor);

	page = PageGetTempPageCopySpecial(origpage);

	maxoff = PageGetMaxOffsetNumber(origpage);
	for (int i = FirstOffsetNumber; i <= maxoff; i++)
	{
		ItemId		iid = PageGetItemId(origpage, i);
		ZSBtreeItem	*item = (ZSBtreeItem *) PageGetItem(origpage, iid);
		ZSBtreeItem *newitem1 = NULL;
		ZSBtreeItem *newitem2 = NULL;

		total++;

		if ((item->t_flags & ZSBT_COMPRESSED) != 0)
		{
			already_compressed++;
			/*
			 * Add compressed items as is. It might be worthwhile to uncompress
			 * and recompress them, together with any new items, but currently
			 * we don't bother.
			 */
			if (compressed_items > 0)
			{
				newitem1 = (ZSBtreeItem *) zs_compress_finish(&compressor);
				compressed_items = 0;
				total_compressed++;
			}
			newitem2 = item;
		}
		else
		{
			/* try to add this item to the compressor */
			if (compressed_items == 0)
				zs_compress_begin(&compressor, PageGetFreeSpace(page));

			if (zs_compress_add(&compressor, item))
			{
				compressed_items++;
			}
			else
			{
				if (compressed_items > 0)
				{
					newitem1 = (ZSBtreeItem *) zs_compress_finish(&compressor);
					compressed_items = 0;
					total_compressed++;
					i--;
				}
				else
				{
					/* could not compress, even on its own. Store it uncompressed, then */
					newitem1 = item;
				}
			}
		}

		if (newitem1)
		{
			if (PageGetFreeSpace(page) < MAXALIGN(newitem1->t_size))
			{
				success = false;
				break;
			}
			if (PageAddItemExtended(page,
									(Item) newitem1, newitem1->t_size,
									PageGetMaxOffsetNumber(page) + 1,
									PAI_OVERWRITE) == InvalidOffsetNumber)
				elog(ERROR, "could not add item to page while repacking");
		}

		if (newitem2)
		{
			if (PageGetFreeSpace(page) < MAXALIGN(newitem2->t_size))
			{
				success = false;
				break;
			}
			if (PageAddItemExtended(page,
									(Item) newitem2, newitem2->t_size,
									PageGetMaxOffsetNumber(page) + 1,
									PAI_OVERWRITE) == InvalidOffsetNumber)
				elog(ERROR, "could not add item to page while repacking");
		}
	}

	if (success && compressed_items > 0)
	{
		ZSBtreeItem *item = zs_compress_finish(&compressor);
		total_compressed++;

		if (PageGetFreeSpace(page) < MAXALIGN(item->t_size))
			success = false;
		else if (PageAddItemExtended(page,
									 (Item) item, item->t_size,
									 PageGetMaxOffsetNumber(page) + 1,
									 PAI_OVERWRITE) == InvalidOffsetNumber)
			elog(ERROR, "could not add item to page while repacking");
	}

	//elog(NOTICE, "compresleaf success: %d already %d total %d, total_comp %d, free after compression %d", success, already_compressed, total, total_compressed, (int) PageGetFreeSpace(page));

	zs_compress_free(&compressor);

	if (success)
	{
		/* TODO: WAL-log */
		PageRestoreTempPage(page, origpage);
		MarkBufferDirty(buf);
	}

	return success;
}

/*
 * Split a leaf page for insertion of 'newitem'.
 */
static void
zsbt_split_leaf(Buffer buf, OffsetNumber lastleftoff, ZSInsertState *state,
				ZSBtreeItem *newitem, bool newitemonleft, OffsetNumber newitemoff)
{
	Buffer		leftbuf = buf;
	Buffer		rightbuf;
	BlockNumber rightblkno;
	Page		origpage = BufferGetPage(buf);
	Page		leftpage;
	Page		rightpage;
	ZSBtreePageOpaque *leftopaque;
	ZSBtreePageOpaque *rightopaque;
	ItemPointerData splittid;
	OffsetNumber i,
				maxoff;

	/*
	 * The original page becomes the left half, but we use a temporary copy of it
	 * to operate on. Allocate a new page for the right half.
	 *
	 * TODO: it'd be good to not hold a lock on the original page while we
	 * allocate a new one.
	 */
	leftpage = PageGetTempPageCopySpecial(origpage);
	leftopaque = ZSBtreePageGetOpaque(leftpage);
	Assert(leftopaque->zs_level == 0);
	/* any previous incomplete split must be finished first */
	Assert((leftopaque->zs_flags & ZS_FOLLOW_RIGHT) == 0);

	rightbuf = zs_getnewbuf(state->rel);
	rightpage = BufferGetPage(rightbuf);
	rightblkno = BufferGetBlockNumber(rightbuf);
	PageInit(rightpage, BLCKSZ, sizeof(ZSBtreePageOpaque));
	rightopaque = ZSBtreePageGetOpaque(rightpage);

	/*
	 * Figure out the split point TID.
	 *
	 * TODO: currently, we only append to end, i.e. we only ever split the rightmost leaf.
	 * that makes it easier to figure out the split tid: just take the old page's lokey,
	 * and increment the blocknumber component of it.
	 */
	ItemPointerSet(&splittid, ItemPointerGetBlockNumber(&leftopaque->zs_lokey) + 1, 1);

	/* Set up the page headers */

	rightopaque->zs_next = leftopaque->zs_next;
	rightopaque->zs_lokey = splittid;
	rightopaque->zs_hikey = leftopaque->zs_hikey;
	rightopaque->zs_level = 0;
	rightopaque->zs_flags = 0;
	rightopaque->zs_page_id = ZS_BTREE_PAGE_ID;

	leftopaque->zs_next = rightblkno;
	leftopaque->zs_hikey = splittid;
	leftopaque->zs_flags |= ZS_FOLLOW_RIGHT;

	//elog(NOTICE, "split leaf %u to %u", BufferGetBlockNumber(leftbuf), rightblkno);

	/*
	 * Copy all the tuples
	 */
	maxoff = PageGetMaxOffsetNumber(origpage);
	for (i = FirstOffsetNumber; i <= maxoff; i++)
	{
		ItemId		iid = PageGetItemId(origpage, i);
		ZSBtreeItem	*item = (ZSBtreeItem *) PageGetItem(origpage, iid);
		Page		targetpage;

		if (i == newitemoff)
		{
			targetpage = newitemonleft ? leftpage : rightpage;
			if (PageAddItemExtended(targetpage,
									(Item) newitem, newitem->t_size,
									PageGetMaxOffsetNumber(targetpage) + 1,
									PAI_OVERWRITE) == InvalidOffsetNumber)
				elog(ERROR, "could not add new item to page on split");
		}

		targetpage = (i <= lastleftoff) ? leftpage : rightpage;
		if (PageAddItemExtended(targetpage,
								(Item) item, item->t_size,
								PageGetMaxOffsetNumber(targetpage) + 1,
								PAI_OVERWRITE) == InvalidOffsetNumber)
			elog(ERROR, "could not add item to page on split");
	}
	if (i == newitemoff)
	{
		Assert(!newitemonleft);
		if (PageAddItemExtended(rightpage,
								(Item) newitem, newitem->t_size,
								FirstOffsetNumber,
								PAI_OVERWRITE) == InvalidOffsetNumber)
			elog(ERROR, "could not add new item to page on split");
	}

	PageRestoreTempPage(leftpage, origpage);

	/* TODO: WAL-log */
	MarkBufferDirty(leftbuf);
	MarkBufferDirty(rightbuf);

	UnlockReleaseBuffer(rightbuf);

	zsbt_insert_downlink(state->rel, state->attno, leftbuf, splittid, rightblkno);
}

/*
 * Create a new btree root page, containing two downlinks.
 *
 * NOTE: the very first root page of a btree, which is also the leaf, is created
 *
 */
static void
zsbt_newroot(Relation rel, AttrNumber attno, int level,
			 ItemPointerData key1, BlockNumber blk1,
			 ItemPointerData key2, BlockNumber blk2,
			 Buffer leftchildbuf)
{
	ZSBtreePageOpaque *opaque;
	ZSBtreePageOpaque *leftchildopaque;
	Buffer		buf;
	Page		page;
	ZSBtreeInternalPageItem *items;
	Buffer		metabuf;

	metabuf = ReadBuffer(rel, ZS_META_BLK);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	Assert(ItemPointerCompare(&key1, &key2) < 0);

	buf = zs_getnewbuf(rel);
	page = BufferGetPage(buf);
	PageInit(page, BLCKSZ, sizeof(ZSBtreePageOpaque));
	opaque = ZSBtreePageGetOpaque(page);
	opaque->zs_next = InvalidBlockNumber;
	ItemPointerSet(&opaque->zs_lokey, 0, 1);
	ItemPointerSet(&opaque->zs_hikey, MaxBlockNumber, 0xFFFF);
	opaque->zs_level = level;
	opaque->zs_flags = 0;
	opaque->zs_page_id = ZS_BTREE_PAGE_ID;

	items = ZSBtreeInternalPageGetItems(page);
	items[0].tid = key1;
	BlockIdSet(&items[0].childblk, blk1);
	items[1].tid = key2;
	BlockIdSet(&items[1].childblk, blk2);
	((PageHeader) page)->pd_lower += 2 * sizeof(ZSBtreeInternalPageItem);
	Assert(ZSBtreeInternalPageGetNumItems(page) == 2);

	/* clear the follow-right flag on left child */
	leftchildopaque = ZSBtreePageGetOpaque(BufferGetPage(leftchildbuf));
	leftchildopaque->zs_flags &= ZS_FOLLOW_RIGHT;

	/* TODO: wal-log all, including metapage */

	MarkBufferDirty(buf);
	MarkBufferDirty(leftchildbuf);

	/* Before exiting, update the metapage */
	zsmeta_update_root_for_attribute(rel, attno, metabuf, BufferGetBlockNumber(buf));

	//elog(NOTICE, "new root %u (%u and %u)", BufferGetBlockNumber(buf), blk1, blk2);

	UnlockReleaseBuffer(leftchildbuf);
	UnlockReleaseBuffer(buf);
	UnlockReleaseBuffer(metabuf);
}

/*
 * After page split, insert the downlink of 'rightbuf' to the parent.
 */
static void
zsbt_insert_downlink(Relation rel, AttrNumber attno, Buffer leftbuf,
					 ItemPointerData rightlokey, BlockNumber rightblkno)
{
	BlockNumber	leftblkno = BufferGetBlockNumber(leftbuf);
	Page		leftpage = BufferGetPage(leftbuf);
	ZSBtreePageOpaque *leftopaque = ZSBtreePageGetOpaque(leftpage);
	ItemPointerData leftlokey = leftopaque->zs_lokey;
	ZSBtreeInternalPageItem *items;
	int			nitems;
	int			itemno;
	Buffer		parentbuf;
	Page		parentpage;

	/*
	 * re-find parent
	 *
	 * TODO: this is a bit inefficient. Usually, we have just descended the
	 * tree, and if we just remembered the path we descended, we could just
	 * walk back up.
	 */
	parentbuf = zsbt_find_downlink(rel, attno, leftlokey, leftblkno, leftopaque->zs_level, &itemno);
	if (parentbuf == InvalidBuffer)
	{
		zsbt_newroot(rel, attno, leftopaque->zs_level + 1,
					 leftlokey, BufferGetBlockNumber(leftbuf),
					 rightlokey, rightblkno, leftbuf);
		return;
	}
	parentpage = BufferGetPage(parentbuf);

	/* Find the position in the parent for the downlink */
	items = ZSBtreeInternalPageGetItems(parentpage);
	nitems = ZSBtreeInternalPageGetNumItems(parentpage);
	itemno = zsbt_binsrch_internal(rightlokey, items, nitems);

	/* sanity checks */
	if (itemno < 1 ||
		!ItemPointerEquals(&items[itemno].tid, &leftlokey) ||
		BlockIdGetBlockNumber(&items[itemno].childblk) != leftblkno)
		elog(ERROR, "could not find downlink");
	itemno++;

	if (ZSBtreeInternalPageIsFull(parentpage))
	{
		/* split internal page */
		zsbt_split_internal(rel, attno, parentbuf, leftbuf, itemno, rightlokey, rightblkno);
	}
	else
	{
		/* insert the new downlink for the right page. */
		memmove(&items[itemno + 1],
				&items[itemno],
				(nitems - itemno) * sizeof(ZSBtreeInternalPageItem));
		items[itemno].tid = rightlokey;
		BlockIdSet(&items[itemno].childblk, rightblkno);
		((PageHeader) parentpage)->pd_lower += sizeof(ZSBtreeInternalPageItem);

		leftopaque->zs_flags &= ~ZS_FOLLOW_RIGHT;

		/* TODO: WAL-log */

		MarkBufferDirty(leftbuf);
		MarkBufferDirty(parentbuf);
		UnlockReleaseBuffer(leftbuf);
		UnlockReleaseBuffer(parentbuf);
	}
}

static void
zsbt_split_internal(Relation rel, AttrNumber attno, Buffer leftbuf, Buffer childbuf,
					OffsetNumber newoff, ItemPointerData newkey, BlockNumber childblk)
{
	Buffer		rightbuf;
	Page		origpage = BufferGetPage(leftbuf);
	Page		leftpage;
	Page		rightpage;
	BlockNumber rightblkno;
	ZSBtreePageOpaque *leftopaque;
	ZSBtreePageOpaque *rightopaque;
	ZSBtreeInternalPageItem *origitems;
	ZSBtreeInternalPageItem *leftitems;
	ZSBtreeInternalPageItem *rightitems;
	int			orignitems;
	int			leftnitems;
	int			rightnitems;
	int			splitpoint;
	ItemPointerData splittid;
	bool		newitemonleft;
	int			i;
	ZSBtreeInternalPageItem newitem;

	leftpage = PageGetTempPageCopySpecial(origpage);
	leftopaque = ZSBtreePageGetOpaque(leftpage);
	Assert(leftopaque->zs_level > 0);
	/* any previous incomplete split must be finished first */
	Assert((leftopaque->zs_flags & ZS_FOLLOW_RIGHT) == 0);

	rightbuf = zs_getnewbuf(rel);
	rightpage = BufferGetPage(rightbuf);
	rightblkno = BufferGetBlockNumber(rightbuf);
	PageInit(rightpage, BLCKSZ, sizeof(ZSBtreePageOpaque));
	rightopaque = ZSBtreePageGetOpaque(rightpage);

	/*
	 * Figure out the split point.
	 *
	 * TODO: currently, always do 90/10 split.
	 */
	origitems = ZSBtreeInternalPageGetItems(origpage);
	orignitems = ZSBtreeInternalPageGetNumItems(origpage);
	splitpoint = orignitems * 0.9;
	splittid = origitems[splitpoint].tid;
	newitemonleft = (ItemPointerCompare(&newkey, &splittid) < 0);

	/* Set up the page headers */
	rightopaque->zs_next = leftopaque->zs_next;
	rightopaque->zs_lokey = splittid;
	rightopaque->zs_hikey = leftopaque->zs_hikey;
	rightopaque->zs_level = leftopaque->zs_level;
	rightopaque->zs_flags = 0;
	rightopaque->zs_page_id = ZS_BTREE_PAGE_ID;

	leftopaque->zs_next = rightblkno;
	leftopaque->zs_hikey = splittid;
	leftopaque->zs_flags |= ZS_FOLLOW_RIGHT;

	/* copy the items */
	leftitems = ZSBtreeInternalPageGetItems(leftpage);
	leftnitems = 0;
	rightitems = ZSBtreeInternalPageGetItems(rightpage);
	rightnitems = 0;

	newitem.tid = newkey;
	BlockIdSet(&newitem.childblk, childblk);

	for (i = 0; i < orignitems; i++)
	{
		if (i == newoff)
		{
			if (newitemonleft)
				leftitems[leftnitems++] = newitem;
			else
				rightitems[rightnitems++] = newitem;
		}

		if (i < splitpoint)
			leftitems[leftnitems++] = origitems[i];
		else
			rightitems[rightnitems++] = origitems[i];
	}
	/* cope with possibility that newitem goes at the end */
	if (i <= newoff)
	{
		Assert(!newitemonleft);
		rightitems[rightnitems++] = newitem;
	}
	((PageHeader) leftpage)->pd_lower += leftnitems * sizeof(ZSBtreeInternalPageItem);
	((PageHeader) rightpage)->pd_lower += rightnitems * sizeof(ZSBtreeInternalPageItem);

	Assert(leftnitems + rightnitems == orignitems + 1);

	PageRestoreTempPage(leftpage, origpage);

	//elog(NOTICE, "split internal %u to %u", BufferGetBlockNumber(leftbuf), rightblkno);

	/* TODO: WAL-logging */
	MarkBufferDirty(leftbuf);
	MarkBufferDirty(rightbuf);

	MarkBufferDirty(childbuf);
	ZSBtreePageGetOpaque(BufferGetPage(childbuf))->zs_flags &= ~ZS_FOLLOW_RIGHT;
	UnlockReleaseBuffer(childbuf);

	UnlockReleaseBuffer(rightbuf);

	/* recurse to insert downlink */
	zsbt_insert_downlink(rel, attno, leftbuf, splittid, rightblkno);

	/* Release buffers */
}

/*
 * Begin a scan of the btree.
 */
void
zsbt_begin_scan(Relation rel, AttrNumber attno, ItemPointerData starttid, Snapshot snapshot, ZSBtreeScan *scan)
{
	BlockNumber	rootblk;
	Buffer		buf;

	rootblk = zsmeta_get_root_for_attribute(rel, attno, false);

	if (rootblk == InvalidBlockNumber)
	{
		/* completely empty tree */
		scan->rel = NULL;
		scan->attno = InvalidAttrNumber;
		scan->active = false;
		scan->lastbuf = InvalidBuffer;
		scan->lastoff = InvalidOffsetNumber;
		scan->snapshot = NULL;
		ItemPointerSetInvalid(&scan->nexttid);
		return;
	}

	buf = zsbt_descend(rel, rootblk, starttid);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	scan->rel = rel;
	scan->attno = attno;
	scan->snapshot = snapshot;

	scan->active = true;
	scan->lastbuf = buf;
	scan->lastoff = InvalidOffsetNumber;
	scan->nexttid = starttid;

	scan->has_decompressed = false;
}

void
zsbt_end_scan(ZSBtreeScan *scan)
{
	if (!scan->active)
		return;

	if (scan->lastbuf)
		ReleaseBuffer(scan->lastbuf);
	scan->active = false;
}

/*
 * Return true if there was another tuple. The datum is returned in *datum,
 * and its TID in *tid. For a pass-by-ref datum, it's a palloc'd copy.
 */
bool
zsbt_scan_next(ZSBtreeScan *scan, Datum *datum, ItemPointerData *tid, bool *visible)
{
	TupleDesc	desc;
	Form_pg_attribute attr;
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	OffsetNumber off;
	OffsetNumber maxoff;
	BlockNumber	next;

	if (!scan->active)
		return false;

	desc = RelationGetDescr(scan->rel);
	attr = &desc->attrs[scan->attno - 1];

	for (;;)
	{
loop:
		while (scan->has_decompressed)
		{
			ZSBtreeItem *item = zs_decompress_read_item(&scan->decompressor);

			if (item == NULL)
			{
				scan->has_decompressed = false;
				break;
			}
			if (ItemPointerCompare(&item->t_tid, &scan->nexttid) >= 0)
			{
				char		*ptr = ((char *) item) + offsetof(ZSBtreeItem, t_payload);

				/* first column stores the MVCC information */
				if (scan->attno == 1)
				{
					/* TODO: How to handle hintbit setting for uncompressed items? */
					*visible = zs_tuple_satisfies_visibility((HeapTupleHeader)ptr,
															 &item->t_tid,
															 scan->snapshot,
															 buf);
					ptr += SizeofHeapTupleHeader;
				}
				else
					*visible = true;

				*datum = fetchatt(attr, ptr);
				*datum = datumCopy(*datum, attr->attbyval, attr->attlen);
				*tid = item->t_tid;

				scan->nexttid = *tid;
				ItemPointerIncrement(&scan->nexttid);

				return true;
			}
		}

		for (;;)
		{
			buf = scan->lastbuf;
			page = BufferGetPage(buf);
			opaque = ZSBtreePageGetOpaque(page);

			LockBuffer(buf, BUFFER_LOCK_SHARE);

			/* TODO: check that the page is a valid zs btree page */

			/* TODO: check the last offset first, as an optimization */
			maxoff = PageGetMaxOffsetNumber(page);
			for (off = FirstOffsetNumber; off <= maxoff; off++)
			{
				ItemId		iid = PageGetItemId(page, off);
				ZSBtreeItem	*item = (ZSBtreeItem *) PageGetItem(page, iid);

				if ((item->t_flags & ZSBT_COMPRESSED) != 0)
				{
					if (ItemPointerCompare(&item->t_lasttid, &scan->nexttid) >= 0)
					{
						zs_decompress_chunk(&scan->decompressor, item);
						scan->has_decompressed = true;
						LockBuffer(buf, BUFFER_LOCK_UNLOCK);
						goto loop;
					}
				}
				else
				{
					if (ItemPointerCompare(&item->t_tid, &scan->nexttid) >= 0)
					{
						char		*ptr = ((char *) item) + offsetof(ZSBtreeItem, t_payload);

						/* first column stores the MVCC information */
						if (scan->attno == 1)
						{
							*visible = zs_tuple_satisfies_visibility((HeapTupleHeader)ptr,
																	   &item->t_tid,
																	   scan->snapshot,
																	   buf);
							ptr += SizeofHeapTupleHeader;
						}
						else
							*visible = true;

						*datum = fetchatt(attr, ptr);
						*datum = datumCopy(*datum, attr->attbyval, attr->attlen);
						*tid = item->t_tid;
						LockBuffer(buf, BUFFER_LOCK_UNLOCK);

						scan->lastbuf = buf;
						scan->lastoff = off;
						scan->nexttid = *tid;
						ItemPointerIncrement(&scan->nexttid);

						return true;
					}
				}
			}

			/* No more items on this page. Walk right, if possible */
			next = opaque->zs_next;
			if (next == BufferGetBlockNumber(buf))
				elog(ERROR, "btree page %u next-pointer points to itself", next);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);

			if (next == InvalidBlockNumber)
			{
				scan->active = false;
				ReleaseBuffer(scan->lastbuf);
				scan->lastbuf = InvalidBuffer;
				return false;
			}

			scan->lastbuf = ReleaseAndReadBuffer(scan->lastbuf, scan->rel, next);
		}
	}
}


/*
 * Return true if there was another tuple. The datum is returned in *datum,
 * and its TID in *tid. For a pass-by-ref datum, it's a palloc'd copy.
 */
bool
zsbt_scan_for_tuple_delete(ZSBtreeScanForTupleDelete *deldesc, ItemPointerData tid)
{
	Page		page;
	ZSBtreePageOpaque *opaque;
	OffsetNumber off;
	OffsetNumber maxoff;
	BlockNumber	next;

	BlockNumber	rootblk;
	Buffer		buf;
	AttrNumber attno = 1; /* for delete scan only first column */
	Relation rel = deldesc->rel;

	rootblk = zsmeta_get_root_for_attribute(rel, attno, false);

	if (rootblk == InvalidBlockNumber)
		return false;

	buf = zsbt_descend(rel, rootblk, tid);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	for (;;)
	{
loop:
		for (;;)
		{
			page = BufferGetPage(buf);
			opaque = ZSBtreePageGetOpaque(page);

			LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);

			/* TODO: check that the page is a valid zs btree page */

			/* TODO: check the last offset first, as an optimization */
			maxoff = PageGetMaxOffsetNumber(page);
			for (off = FirstOffsetNumber; off <= maxoff; off++)
			{
				ItemId		iid = PageGetItemId(page, off);
				ZSBtreeItem	*item = (ZSBtreeItem *) PageGetItem(page, iid);

				if ((item->t_flags & ZSBT_COMPRESSED) != 0)
				{
					if (ItemPointerCompare(&item->t_lasttid, &tid) >= 0)
					{
						/* TODO: lets deal with compressed items for delete later */
//						zs_decompress_chunk(&scan->decompressor, item);
						LockBuffer(buf, BUFFER_LOCK_UNLOCK);
						goto loop;
					}
				}
				else
				{
					if (ItemPointerCompare(&item->t_tid, &tid) >= 0)
					{
						char		*ptr = ((char *) item) + offsetof(ZSBtreeItem, t_payload);
						zs_tuple_delete(deldesc, (HeapTupleHeader)ptr, &tid, buf);
						Assert(ItemPointerEquals(&tid, &item->t_tid));
						LockBuffer(buf, BUFFER_LOCK_UNLOCK);
						ReleaseBuffer(buf);
						return true;
					}
				}
			}

			/* No more items on this page. Walk right, if possible */
			next = opaque->zs_next;
			if (next == BufferGetBlockNumber(buf))
				elog(ERROR, "btree page %u next-pointer points to itself", next);
			LockBuffer(buf, BUFFER_LOCK_UNLOCK);

			if (next == InvalidBlockNumber)
			{
				ReleaseBuffer(buf);
				return false;
			}
			buf = ReleaseAndReadBuffer(buf, rel, next);
		}
	}

	if (buf)
		ReleaseBuffer(buf);
}

/*
 * Get the last tid (plus one) in the tree.
 */
ItemPointerData
zsbt_get_last_tid(Relation rel, AttrNumber attno)
{
	BlockNumber	rootblk;
	ItemPointerData rightmostkey;
	ItemPointerData	tid;
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	OffsetNumber maxoff;

	/* Find the rightmost leaf */
	rootblk = zsmeta_get_root_for_attribute(rel, attno, true);
	ItemPointerSet(&rightmostkey, MaxBlockNumber, 0xfffe);
	buf = zsbt_descend(rel, rootblk, rightmostkey);
	page = BufferGetPage(buf);
	opaque = ZSBtreePageGetOpaque(page);

	/*
	 * Look at the last item, for its tid.
	 */
	maxoff = PageGetMaxOffsetNumber(page);
	if (maxoff >= FirstOffsetNumber)
	{
		ItemId		iid = PageGetItemId(page, maxoff);
		ZSBtreeItem	*hitup = (ZSBtreeItem *) PageGetItem(page, iid);

		tid = hitup->t_tid;
		ItemPointerIncrement(&tid);
	}
	else
	{
		tid = opaque->zs_lokey;
	}
	UnlockReleaseBuffer(buf);

	return tid;
}


static int
zsbt_binsrch_internal(ItemPointerData key, ZSBtreeInternalPageItem *arr, int arr_elems)
{
	int			low,
				high,
				mid;
	int			cmp;

	low = 0;
	high = arr_elems;
	while (high > low)
	{
		mid = low + (high - low) / 2;

		cmp = ItemPointerCompare(&key, &arr[mid].tid);
		if (cmp >= 0)
			low = mid + 1;
		else
			high = mid;
	}
	return low - 1;
}
