/********************************************
 * lsm tree cursor的增删查改实现
 *******************************************/

#include "wt_internal.h"

/*遍历所有lsm chunk中的cursor对象*/
#define WT_FORALL_CURSORS(clsm, c, i)					\
	for((i) = (clsm)->nchunks; (i) > 0;)				\
		if(((c) = (clsm)->cursors[--i]) != NULL)		

/*用lsm_tree的比较器比较c1和c2的值大小*/
#define WT_LSM_CURCMP(s, lsm_tree, c1, c2, cmp)			\
	__wt_compare(s, (lsm_tree)->collator, &(c1)->key, &(c2)->key, &cmp)


static int __clsm_lookup(WT_CURSOR_LSM*, WT_ITEM* );
static int __clsm_open_cursors(WT_CURSOR_LSM*, int, u_int, uint32_t);
static int __clsm_reset_cursors(WT_CURSOR_LSM*, WT_CURSOR *);

/*产生一个LSM tree的switch操作*/
static inline int __clsm_request_switch(WT_CURSOR_LSM* clsm)
{
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION_IMPL *session;

	lsm_tree = clsm->lsm_tree;
	session = (WT_SESSION_IMPL *)clsm->iface.session;

	if(!F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH)){
		/*
		 * Check that we are up-to-date: don't set the switch if the
		 * tree has changed since we last opened cursors: that can lead
		 * to switching multiple times when only one switch is
		 * required, creating very small chunks.
		 */

		WT_RET(__wt_lsm_tree_readlock(session, lsm_tree));
		if(lsm_tree->nchunks == 0 || 
			(clsm->dsk_gen == lsm_tree->dsk_gen && !F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH))){
			ret = __wt_lsm_manager_push_entry(session, WT_LSM_WORK_SWITCH, 0, lsm_tree);
			F_SET(lsm_tree, WT_LSM_TREE_NEED_SWITCH);
		}
		WT_TRET(__wt_lsm_tree_readunlock(session, lsm_tree));
	}
	return ret;
}

/*确保lsm tree是否可以执行一个update操作*/
static int __clsm_enter_update(WT_CURSOR_LSM* clsm)
{
	WT_CURSOR *primary;
	WT_LSM_CHUNK *primary_chunk;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION_IMPL *session;
	int hard_limit, have_primary, ovfl, waited;

	lsm_tree = clsm->lsm_tree;
	ovfl = 0;
	session = (WT_SESSION_IMPL* )clsm->iface.session;

	/*确定primary的cursor/chunk/txnid等信息，从而判断对于update操作是否有primary*/
	if(clsm->nchunks == 0){
		primary = NULL;
		have_primary = 0;
	}
	else{
		primary = clsm->cursors[clsm->nchunks - 1];
		primary_chunk = clsm->primary_chunk;
		WT_ASSERT(session, F_ISSET(&session->txn, TXN_HAS_ID));
		have_primary = (primary != NULL && primary_chunk != NULL 
			&& (primary_chunk->switch_txn == WT_TXN_NONE || TXNID_LT(session->txn.id, primary_chunk->switch_txn)));
	}

	/*
	 * In LSM there are multiple btrees active at one time. The tree
	 * switch code needs to use btree API methods, and it wants to
	 * operate on the btree for the primary chunk. Set that up now.
	 *
	 * If the primary chunk has grown too large, set a flag so the worker
	 * thread will switch when it gets a chance to avoid introducing high
	 * latency into application threads.  Don't do this indefinitely: if a
	 * chunk grows twice as large as the configured size, block until it
	 * can be switched.
	 */
	hard_limit = F_ISSET(lsm_tree, WT_LSM_TREE_NEED_SWITCH) ? 1 : 0;
	if (have_primary) {
		WT_ENTER_PAGE_INDEX(session);
		WT_WITH_BTREE(session, ((WT_CURSOR_BTREE *)primary)->btree,
			ovfl = __wt_btree_lsm_size(session, hard_limit ? 2 * lsm_tree->chunk_size : lsm_tree->chunk_size));
		WT_LEAVE_PAGE_INDEX(session);

		/* If there was no overflow, we're done. */
		if (!ovfl)
			return (0);
	}

	/* Request a switch. */
	WT_RET(__clsm_request_switch(clsm));
	/*仅仅是超过了overflow设定，可以update*/
	if(have_primary && !hard_limit)
		return 0;

	/*如果没有pprimary又超过了overflow设置，而且这个时候处在一个hard_limit状态，那么必须
	 *让lsm tree进行一次switch以达到可以update的要求为止。
	 */
	for(waited = 0; lsm_tree->nchunks == 0 || clsm->dsk_gen == lsm_tree->dsk_gen; ++waited){
		if (waited % 1000 == 0)
			WT_RET(__wt_lsm_manager_push_entry(session, WT_LSM_WORK_SWITCH, 0, lsm_tree));
		__wt_sleep(0, 10);
	}

	return 0;
}

/*开始一个lsm tree的操作*/
static inline int __clsm_enter(WT_CURSOR_LSM* clsm, int reset, int update)
{
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION_IMPL *session;
	uint64_t *switch_txnp;
	uint64_t snap_min;

	lsm_tree = clsm->lsm_tree;
	session = (WT_SESSION_IMPL *)clsm->iface.session;

	/*merge过程是不能update*/
	if(F_ISSET(clsm, WT_CLSM_MERGE))
		return 0;

	if(reset){
		WT_ASSERT(session, !F_ISSET(&clsm->iface, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT));
		WT_RET(__clsm_reset_cursors(clsm, NULL));
	}

	for(;;){
		/*
		 * If the cursor looks up-to-date, check if the cache is full.
		 * In case this call blocks, the check will be repeated before
		 * proceeding.
		 */
		if (clsm->dsk_gen != lsm_tree->dsk_gen && lsm_tree->nchunks != 0)
			goto open;

		if(update){
			WT_RET(__wt_txn_autocommit_check(session));
			WT_RET(__wt_txn_id_check(session));

			WT_RET(__clsm_enter_update(clsm));
			if(clsm->dsk_gen != clsm->lsm_tree->dsk_gen)
				goto open;

			if (session->txn.isolation == TXN_ISO_SNAPSHOT)
				__wt_txn_cursor_op(session);

			clsm->nupdates = 1;
			if(session->txn.isolation == TXN_ISO_SNAPSHOT && F_ISSET(clsm, WT_CLSM_OPEN_SNAPSHOT)){
				WT_ASSERT(session, F_ISSET(&session->txn, TXN_HAS_SNAPSHOT));
				snap_min = session->txn.snap_min;
				/*确保其他chunk的switch txn事务都在这次udate操作的snapshot事务快照中*/
				for (switch_txnp = &clsm->switch_txn[clsm->nchunks - 2]; clsm->nupdates < clsm->nchunks; clsm->nupdates++, switch_txnp--) {
					if (TXNID_LT(*switch_txnp, snap_min))
						break;
					WT_ASSERT(session, !__wt_txn_visible_all(session, *switch_txnp));
				}
			}
		}
		/*
		 * Stop when we are up-to-date, as long as this is:
		 *   - a snapshot isolation update and the cursor is set up for
		 *     that;
		 *   - an update operation with a primary chunk, or
		 *   - a read operation and the cursor is open for reading.
		 */
		if ((!update || session->txn.isolation != TXN_ISO_SNAPSHOT ||
			F_ISSET(clsm, WT_CLSM_OPEN_SNAPSHOT)) &&((update && clsm->primary_chunk != NULL) ||
			(!update && F_ISSET(clsm, WT_CLSM_OPEN_READ))))
			break;

open:
		WT_WITH_SCHEMA_LOCK(session, ret = __clsm_open_cursors(clsm, update, 0, 0));
		WT_RET(ret);
	}

	if (!F_ISSET(clsm, WT_CLSM_ACTIVE)) {
		WT_RET(__cursor_enter(session));
		F_SET(clsm, WT_CLSM_ACTIVE);
	}

	return 0;
}

/*结束一个lsm tree上的操作*/
static void __clsm_leave(WT_CURSOR_LSM* clsm)
{
	WT_SESSION_IMPL *session;

	session = (WT_SESSION_IMPL *)clsm->iface.session;
	if (F_ISSET(clsm, WT_CLSM_ACTIVE)) {
		__cursor_leave(session);
		F_CLR(clsm, WT_CLSM_ACTIVE);
	}
}

/*
 * We need a tombstone to mark deleted records, and we use the special
 * value below for that purpose.  We use two 0x14 (Device Control 4) bytes to
 * minimize the likelihood of colliding with an application-chosen encoding
 * byte, if the application uses two leading DC4 byte for some reason, we'll do
 * a wasted data copy each time a new value is inserted into the object.
 * 标记删除标示
 */
static const WT_ITEM __tombstone = { "\x14\x14", 2, 0, NULL, 0 };

/*判断标记删除*/
static inline int __clsm_deleted(WT_CURSOR_LSM* clsm, const WT_ITEM* item)
{
	return (!F_ISSET(clsm, WT_CLSM_MINOR_MERGE) &&
		item->size == __tombstone.size && memcmp(item->data, __tombstone.data, __tombstone.size) == 0);
}

/*值编码*/
static inline int __clsm_deleted_encode(WT_SESSION_IMPL* session, const WT_ITEM* value, WT_ITEM* final_value, WT_ITEM** tmpp)
{
	WT_ITEM *tmp;
	if (value->size >= __tombstone.size &&
		memcmp(value->data, __tombstone.data, __tombstone.size) == 0) { /*value的值和tombstone碰撞了，那么构建一个新的item，并在这个值后面添加一个tomstone的第一个字符来标记这个item*/
			WT_RET(__wt_scr_alloc(session, value->size + 1, tmpp));
			tmp = *tmpp;

			memcpy(tmp->mem, value->data, value->size);
			memcpy((uint8_t *)tmp->mem + value->size, __tombstone.data, 1);
			final_value->data = tmp->mem;
			final_value->size = value->size + 1;
	} 
	else {
		final_value->data = value->data;
		final_value->size = value->size;
	}

	return 0;
}

/*值还原*/
static inline void __clsm_deleted_decode(WT_CURSOR_LSM *clsm, WT_ITEM *value)
{

	if (!F_ISSET(clsm, WT_CLSM_MERGE) && value->size > __tombstone.size &&
	    memcmp(value->data, __tombstone.data, __tombstone.size) == 0)
		--value->size;
}

/*关闭lsm tree上不需要的cursor的对象*/
static int __clsm_close_cursors(WT_CURSOR_LSM* clsm, u_int start, u_int end)
{
	WT_BLOOM *bloom;
	WT_CURSOR *c;
	u_int i;

	if (clsm->cursors == NULL || clsm->nchunks == 0)
		return (0);

	for(i = start; i < end; ++i){
		if ((c = (clsm)->cursors[i]) != NULL) {
			clsm->cursors[i] = NULL;
			WT_RET(c->close(c));
		}
		if ((bloom = clsm->blooms[i]) != NULL) {
			clsm->blooms[i] = NULL;
			WT_RET(__wt_bloom_close(bloom));
		}
	}

	return 0;
}

/*打开lsm tree上对应chunk的cursor*/
static int __clsm_open_cursors(WT_CURSOR_LSM *clsm, int update, u_int start_chunk, uint32_t start_id)
{
	WT_BTREE *btree;
	WT_CURSOR *c, **cp, *primary;
	WT_DECL_RET;
	WT_LSM_CHUNK *chunk;
	WT_LSM_TREE *lsm_tree;
	WT_SESSION_IMPL *session;
	WT_TXN *txn;
	const char *checkpoint, *ckpt_cfg[3];
	uint64_t saved_gen;
	u_int i, nchunks, ngood, nupdates;
	u_int close_range_end, close_range_start;
	int locked;

	c = &clsm->iface;
	session = (WT_SESSION_IMPL *)c->session;
	txn = &session->txn;
	lsm_tree = clsm->lsm_tree;
	chunk = NULL;

	if(update){
		if(txn->isolation == TXN_ISO_SNAPSHOT)
			F_SET(clsm, WT_CLSM_OPEN_SNAPSHOT);
	}
	else
		F_SET(clsm, WT_CLSM_OPEN_READ);

	if(lsm_tree->nchunks == 0)
		return 0;

	ckpt_cfg[0] = WT_CONFIG_BASE(session, session_open_cursor);
	ckpt_cfg[1] = "checkpoint=" WT_CHECKPOINT ",raw";
	ckpt_cfg[2] = NULL;

	if (F_ISSET(c, WT_CURSTD_KEY_INT) && !WT_DATA_IN_ITEM(&c->key))
		WT_RET(__wt_buf_set(session, &c->key, c->key.data, c->key.size));

	F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);
	WT_RET(__wt_lsm_tree_readlock(session, lsm_tree));
	locked = 1;

retry:
	/*发生了merge过程，那么start chunk需要重新根据stard chunk id来重新定位一下*/
	if(F_ISSET(clsm, WT_CLSM_MERGE)){
		nchunks = clsm->nchunks;
		ngood = 0;

		/*
		 * We may have raced with another merge completing.  Check that
		 * we're starting at the right offset in the chunk array.
		 */
		if (start_chunk >= lsm_tree->nchunks || lsm_tree->chunk[start_chunk]->id != start_id) {
			for (start_chunk = 0; start_chunk < lsm_tree->nchunks; start_chunk++) {
				chunk = lsm_tree->chunk[start_chunk];
				if (chunk->id == start_id)
					break;
			}
			/* We have to find the start chunk: merge locked it. */
			WT_ASSERT(session, start_chunk < lsm_tree->nchunks);
		}

		WT_ASSERT(session, start_chunk + nchunks <= lsm_tree->nchunks);
	}
	else{
		nchunks = lsm_tree->nchunks;
		/*
		 * If we are only opening the cursor for updates, only open the
		 * primary chunk, plus any other chunks that might be required
		 * to detect snapshot isolation conflicts.
		 */
		if (F_ISSET(clsm, WT_CLSM_OPEN_SNAPSHOT))
			WT_ERR(__wt_realloc_def(session, &clsm->txnid_alloc, nchunks, &clsm->switch_txn));

		/*根据事务隔离来确定nupdates和ngood的值*/
		if(F_ISSET(clsm, WT_CLSM_OPEN_READ))
			ngood = nupdates = 0;
		else if(F_ISSET(clsm, WT_CLSM_OPEN_SNAPSHOT)){
			for (ngood = nchunks - 1, nupdates = 1;ngood > 0;ngood--, nupdates++) {
				chunk = lsm_tree->chunk[ngood - 1];
				clsm->switch_txn[ngood - 1] = chunk->switch_txn;
				if (__wt_txn_visible_all(session, chunk->switch_txn))
					break;
			}
		}
		else{
			nupdates = 1;
			ngood = nchunks - 1;
		}

		/*检查已经打开cursor的个数*/
		for(cp = clsm->cursors + ngood; ngood < clsm->nchunks && ngood < nchunks; cp++, ngood++){
			chunk = lsm_tree->chunk[ngood];

			/* If the cursor isn't open yet, we're done. */
			if (*cp == NULL)
				break;

			/* Easy case: the URIs don't match. */
			if (strcmp((*cp)->uri, chunk->uri) != 0)
				break;

			/* Make sure the checkpoint config matches. */
			checkpoint = ((WT_CURSOR_BTREE *)*cp)->btree->dhandle->checkpoint;
			if (checkpoint == NULL && F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) && !chunk->empty)
				break;

			/* Make sure the Bloom config matches. */
			if (clsm->blooms[ngood] == NULL && F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
				break;
		}

		/* Spurious generation bump? */
		if (ngood == clsm->nchunks && clsm->nchunks == nchunks) {
			clsm->dsk_gen = lsm_tree->dsk_gen;
			goto err;
		}

		/*
		 * Close any cursors we no longer need.
		 *
		 * Drop the LSM tree lock while we do this: if the cache is
		 * full, we may block while closing a cursor.  Save the
		 * generation number and retry if it has changed under us.
		 */
		if (clsm->cursors != NULL && ngood < clsm->nchunks) {
			close_range_start = ngood;
			close_range_end = clsm->nchunks;
		} 
		else if (!F_ISSET(clsm, WT_CLSM_OPEN_READ) && nupdates > 0 ) {
			close_range_start = 0;
			close_range_end = WT_MIN(nchunks, clsm->nchunks);
			if (close_range_end > nupdates)
				close_range_end -= nupdates;
			else
				close_range_end = 0;
			WT_ASSERT(session, ngood >= close_range_end);
		} 
		else {
			close_range_end = 0;
			close_range_start = 0;
		}
		if (close_range_end > close_range_start) {
			saved_gen = lsm_tree->dsk_gen;
			locked = 0;
			WT_ERR(__wt_lsm_tree_readunlock(session, lsm_tree));
			WT_ERR(__clsm_close_cursors(clsm, close_range_start, close_range_end));
			WT_ERR(__wt_lsm_tree_readlock(session, lsm_tree));
			locked = 1;
			if (lsm_tree->dsk_gen != saved_gen)
				goto retry;
		}

		/* Detach from our old primary. */
		clsm->primary_chunk = NULL;
		clsm->current = NULL;
	}

	WT_ERR(__wt_realloc_def(session, &clsm->bloom_alloc, nchunks, &clsm->blooms));
	WT_ERR(__wt_realloc_def(session, &clsm->cursor_alloc, nchunks, &clsm->cursors));

	clsm->nchunks = nchunks;

	/* Open the cursors for chunks that have changed. */
	for (i = ngood, cp = clsm->cursors + i; i != nchunks; i++, cp++) {
		chunk = lsm_tree->chunk[i + start_chunk];
		/* Copy the maximum transaction ID. */
		if (F_ISSET(clsm, WT_CLSM_OPEN_SNAPSHOT))
			clsm->switch_txn[i] = chunk->switch_txn;

		/*
		 * Read from the checkpoint if the file has been written.
		 * Once all cursors switch, the in-memory tree can be evicted.
		 */
		WT_ASSERT(session, *cp == NULL);
		ret = __wt_open_cursor(session, chunk->uri, c, (F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) && !chunk->empty) ? ckpt_cfg : NULL, cp);

		/*
		 * XXX kludge: we may have an empty chunk where no checkpoint
		 * was written.  If so, try to open the ordinary handle on that
		 * chunk instead.
		 */
		if (ret == WT_NOTFOUND && F_ISSET(chunk, WT_LSM_CHUNK_ONDISK)) {
			ret = __wt_open_cursor(session, chunk->uri, c, NULL, cp);
			if (ret == 0)
				chunk->empty = 1;
		}
		WT_ERR(ret);

		/*
		 * Setup all cursors other than the primary to only do conflict
		 * checks on insert operations. This allows us to execute
		 * inserts on non-primary chunks as a way of checking for
		 * write conflicts with concurrent updates.
		 */
		if (i != nchunks - 1)
			(*cp)->insert = __wt_curfile_update_check;

		if (!F_ISSET(clsm, WT_CLSM_MERGE) && F_ISSET(chunk, WT_LSM_CHUNK_BLOOM))
			WT_ERR(__wt_bloom_open(session, chunk->bloom_uri, lsm_tree->bloom_bit_count, c, &clsm->blooms[i]));

		/* Child cursors always use overwrite and raw mode. */
		F_SET(*cp, WT_CURSTD_OVERWRITE | WT_CURSTD_RAW);
	}

	/* The last chunk is our new primary. */
	if (chunk != NULL && !F_ISSET(chunk, WT_LSM_CHUNK_ONDISK) && chunk->switch_txn == WT_TXN_NONE) {
		clsm->primary_chunk = chunk;
		primary = clsm->cursors[clsm->nchunks - 1];
		/*
		 * Disable eviction for the in-memory chunk.  Also clear the
		 * bulk load flag here, otherwise eviction will be enabled by
		 * the first update.
		 */
		btree = ((WT_CURSOR_BTREE *)(primary))->btree;
		if (btree->bulk_load_ok) {
			btree->bulk_load_ok = 0;
			WT_WITH_BTREE(session, btree, __wt_btree_evictable(session, 0));
		}
	}

	clsm->dsk_gen = lsm_tree->dsk_gen;

err:
	if(locked)
		WT_TRET(__wt_lsm_tree_readunlock(session, lsm_tree));

	return ret;
}

/*初始化lsm tree的merge操作的cursor*/
int __wt_clsm_init_merge(WT_CURSOR *cursor, u_int start_chunk, uint32_t start_id, u_int nchunks)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;
	session = (WT_SESSION_IMPL *)cursor->session;

	F_SET(clsm, WT_CLSM_MERGE);
	if (start_chunk != 0)
		F_SET(clsm, WT_CLSM_MINOR_MERGE);
	clsm->nchunks = nchunks;

	WT_WITH_SCHEMA_LOCK(session, ret = __clsm_open_cursors(clsm, 0, start_chunk, start_id));
	return ret;
}

/*获得lsm tree中所有打开的cursor对最小或者最大的cursor，
 *并将lsm tree中当前的cursor中的值拷贝到定位到的cursor中
 */
static int __clsm_get_current(WT_SESSION_IMPL *session, WT_CURSOR_LSM *clsm, int smallest, int *deletedp)
{
	WT_CURSOR *c, *current;
	int cmp, multiple;
	u_int i;

	current = NULL;
	multiple = 0;

	WT_FORALL_CURSORS(clsm, c, i){
		if (!F_ISSET(c, WT_CURSTD_KEY_INT))
			continue;
		/*保存最新的cursor句柄*/
		if (current == NULL) {
			current = c;
			continue;
		}

		/*获得最小或者最大的cursor对象*/
		WT_RET(WT_LSM_CURCMP(session, clsm->lsm_tree, c, current, cmp));
		if (smallest ? cmp < 0 : cmp > 0) {
			current = c;
			multiple = 0;
		} 
		else if (cmp == 0)
			multiple = 1;
	}

	c = &clsm->iface;
	if ((clsm->current = current) == NULL) {
		F_CLR(c, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
		return (WT_NOTFOUND);
	}

	if (multiple)
		F_SET(clsm, WT_CLSM_MULTIPLE);
	else
		F_CLR(clsm, WT_CLSM_MULTIPLE);

	WT_RET(current->get_key(current, &c->key));
	WT_RET(current->get_value(current, &c->value));

	F_CLR(c, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	if ((*deletedp = __clsm_deleted(clsm, &c->value)) == 0)
		F_SET(c, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);

	return 0;
}

/*lsm cursor type的比较器函数*/
static int __clsm_compare(WT_CURSOR* a, WT_CURSOR* b, int *cmpp)
{
	WT_CURSOR_LSM *alsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	alsm = (WT_CURSOR_LSM*)a;
	CURSOR_API_CALL(a, session, compare, NULL);

	if (strcmp(a->uri, b->uri) != 0)
		WT_ERR_MSG(session, EINVAL, "comparison method cursors must reference the same object");

	WT_CURSOR_NEEDKEY(a);
	WT_CURSOR_NEEDKEY(b);

	WT_ERR(__wt_compare(session, alsm->lsm_tree->collator, &a->key, &b->key, cmpp));

err:
	API_END_RET(session, ret);
}

/*lsm cursor type的next函数实现*/
static int __clsm_next(WT_CURSOR* cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	u_int i;
	int check, cmp, deleted;

	clsm = (WT_CURSOR_LSM *)cursor;

	CURSOR_API_CALL(cursor, session, next, NULL);
	WT_CURSOR_NOVALUE(cursor);
	WT_ERR(__clsm_enter(clsm, 0, 0));

	if(clsm->current == NULL || !F_ISSET(clsm, WT_CLSM_ITERATE_NEXT)){
		F_CLR(clsm, WT_CLSM_MULTIPLE);

		WT_FORALL_CURSORS(clsm, c, i) {
			if (!F_ISSET(cursor, WT_CURSTD_KEY_SET)) {
				WT_ERR(c->reset(c));
				ret = c->next(c);
			} 
			else if (c != clsm->current) {
				c->set_key(c, &cursor->key);
				if ((ret = c->search_near(c, &cmp)) == 0) {
					if (cmp < 0)
						ret = c->next(c);
					else if (cmp == 0) {
						if (clsm->current == NULL)
							clsm->current = c;
						else
							F_SET(clsm, WT_CLSM_MULTIPLE);
					}
				} 
				else
					F_CLR(c, WT_CURSTD_KEY_SET);
			}
			WT_ERR_NOTFOUND_OK(ret);
		}
		F_SET(clsm, WT_CLSM_ITERATE_NEXT);
		F_CLR(clsm, WT_CLSM_ITERATE_PREV);
		if(clsm->current != NULL)
			goto retry;
	}
	else{
retry:
		/*
		 * If there are multiple cursors on that key, move them
		 * forward.
		 */
		if (F_ISSET(clsm, WT_CLSM_MULTIPLE)) {
			check = 0;
			WT_FORALL_CURSORS(clsm, c, i) {
				if (!F_ISSET(c, WT_CURSTD_KEY_INT))
					continue;

				if (check) {
					WT_ERR(WT_LSM_CURCMP(session, clsm->lsm_tree, c, clsm->current, cmp));
					if (cmp == 0)
						WT_ERR_NOTFOUND_OK(c->next(c));
				}
				if (c == clsm->current)
					check = 1;
			}
		}

		/* Move the smallest cursor forward. */
		c = clsm->current;
		WT_ERR_NOTFOUND_OK(c->next(c));
	}

	/* Find the cursor(s) with the smallest key. 如果KV对被标记为删除，那么要继续向下找*/
	if ((ret = __clsm_get_current(session, clsm, 1, &deleted)) == 0 && deleted)
		goto retry;

err:	
	__clsm_leave(clsm);
	API_END(session, ret);
	if (ret == 0)
		__clsm_deleted_decode(clsm, &cursor->value);
	return (ret);

}

/*lsm cursor type的next函数实现*/
static int __clsm_prev(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_CURSOR *c;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	u_int i;
	int check, cmp, deleted;

	clsm = (WT_CURSOR_LSM *)cursor;

	CURSOR_API_CALL(cursor, session, prev, NULL);
	WT_CURSOR_NOVALUE(cursor);
	WT_ERR(__clsm_enter(clsm, 0, 0));

	/* If we aren't positioned for a reverse scan, get started. */
	if (clsm->current == NULL || !F_ISSET(clsm, WT_CLSM_ITERATE_PREV)) {
		F_CLR(clsm, WT_CLSM_MULTIPLE);
		WT_FORALL_CURSORS(clsm, c, i) {
			if (!F_ISSET(cursor, WT_CURSTD_KEY_SET)) {
				WT_ERR(c->reset(c));
				ret = c->prev(c);
			} 
			else if (c != clsm->current) {
				c->set_key(c, &cursor->key);
				if ((ret = c->search_near(c, &cmp)) == 0) {
					if (cmp > 0)
						ret = c->prev(c);
					else if (cmp == 0) {
						if (clsm->current == NULL)
							clsm->current = c;
						else
							F_SET(clsm, WT_CLSM_MULTIPLE);
					}
				}
			}
			WT_ERR_NOTFOUND_OK(ret);
		}
		F_SET(clsm, WT_CLSM_ITERATE_PREV);
		F_CLR(clsm, WT_CLSM_ITERATE_NEXT);

		/* We just positioned *at* the key, now move. */
		if (clsm->current != NULL)
			goto retry;
	} 
	else {
retry:	/*
		 * If there are multiple cursors on that key, move them
		 * backwards.
		 */
		if (F_ISSET(clsm, WT_CLSM_MULTIPLE)) {
			check = 0;
			WT_FORALL_CURSORS(clsm, c, i) {
				if (!F_ISSET(c, WT_CURSTD_KEY_INT))
					continue;
				if (check) {
					WT_ERR(WT_LSM_CURCMP(session, clsm->lsm_tree, c, clsm->current, cmp));
					if (cmp == 0)
						WT_ERR_NOTFOUND_OK(c->prev(c));
				}
				if (c == clsm->current)
					check = 1;
			}
		}

		/* Move the smallest cursor backwards. */
		c = clsm->current;
		WT_ERR_NOTFOUND_OK(c->prev(c));
	}

	/* Find the cursor(s) with the largest key. */
	if ((ret = __clsm_get_current(session, clsm, 0, &deleted)) == 0 && deleted)
		goto retry;

err:	
	__clsm_leave(clsm);
	API_END(session, ret);
	if (ret == 0)
		__clsm_deleted_decode(clsm, &cursor->value);
	return ret;
}

/*重置LSM tree cursor的位置，加入skip不为NULL，表示lsm tree cursor即将会用到这个cursor,不需要reset他*/
static int __clsm_reset_cursors(WT_CURSOR_LSM* clsm, WT_CURSOR *skip)
{
	WT_CURSOR *c;
	WT_DECL_RET;
	u_int i;

	/*已经在指定的位置上，无需reset*/
	if((clsm->current == NULL || clsm->current == skip) && !F_ISSET(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV))
		return 0;

	WT_FORALL_CURSORS(clsm, c, i) {
		if (c == skip)
			continue;
		if (F_ISSET(c, WT_CURSTD_KEY_INT))
			WT_TRET(c->reset(c));
	}

	clsm->current = NULL;
	F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);

	return ret;
}

/*lsm cursor的reset函数实现*/
static int __clsm_reset(WT_CURSOR* cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	/*
	 * Don't use the normal __clsm_enter path: that is wasted work when all
	 * we want to do is give up our position.
	 */
	clsm = (WT_CURSOR_LSM *)cursor;
	CURSOR_API_CALL(cursor, session, reset, NULL);
	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);

	WT_TRET(__clsm_reset_cursors(clsm, NULL));

	/* In case we were left positioned, clear that. */
	__clsm_leave(clsm);

err:
	API_END_RET(session, ret);
}

/*lsm tree的精确查找*/
static int __clsm_lookup(WT_CURSOR_LSM *clsm, WT_ITEM *value)
{
	WT_BLOOM *bloom;
	WT_BLOOM_HASH bhash;
	WT_CURSOR *c, *cursor;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	u_int i;
	int have_hash;

	c = NULL;
	cursor = &clsm->iface;
	have_hash = 0;
	session = (WT_SESSION_IMPL *)cursor->session;

	WT_FORALL_CURSORS(clsm, c, i){
		/*先对bloom filter做存在性检查*/
		bloom = NULL;
		if ((bloom = clsm->blooms[i]) != NULL) {
			if (!have_hash) {
				WT_ERR(__wt_bloom_hash(bloom, &cursor->key, &bhash));
				have_hash = 1;
			}

			ret = __wt_bloom_hash_get(bloom, &bhash);
			if (ret == WT_NOTFOUND) {
				WT_STAT_FAST_INCR(session, &clsm->lsm_tree->stats, bloom_miss);
				continue;
			} else if (ret == 0)
				WT_STAT_FAST_INCR(session, &clsm->lsm_tree->stats, bloom_hit);
			WT_ERR(ret);
		}

		/*在cursor对key做精确查找*/
		c->set_key(c, &cursor->key);
		if((ret = c->search(c)) == 0){
			WT_ERR(c->get_key(c, &cursor->key));
			WT_ERR(c->get_value(c, value));
			if (__clsm_deleted(clsm, value))
				ret = WT_NOTFOUND;
			goto done;
		}

		/* Update stats: the active chunk can't have a bloom filter. */
		if (bloom != NULL)
			WT_STAT_FAST_INCR(session, &clsm->lsm_tree->stats, bloom_false_positive);
		else if (clsm->primary_chunk == NULL || i != clsm->nchunks)
			WT_STAT_FAST_INCR(session, &clsm->lsm_tree->stats, lsm_lookup_no_bloom);
	}

	WT_ERR(WT_NOTFOUND);

done:
err:
	/*设置查找后的结果标示*/
	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	if (ret == 0) {
		clsm->current = c;
		F_SET(cursor, WT_CURSTD_KEY_INT);
		if (value == &cursor->value)
			F_SET(cursor, WT_CURSTD_VALUE_INT);
	} 
	else if (c != NULL)
		WT_TRET(c->reset(c));

	return ret;
}

/*LSM tree的cursor search方法实现*/
static int __clsm_search(WT_CURSOR* cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;

	CURSOR_API_CALL(cursor, session, search, NULL);
	WT_CURSOR_NEEDKEY(cursor);
	WT_CURSOR_NOVALUE(cursor);
	WT_ERR(__clsm_enter(clsm, 1, 0));

	ret = __clsm_lookup(clsm, &cursor->value);

err:
	API_END(session, ret);
	if (ret == 0)
		__clsm_deleted_decode(clsm, &cursor->value);
	return ret;
}

/*lsm tree cursor的search near定位，根据key定位到它所在的位置*/
static int __clsm_search_near(WT_CURSOR* cursor, int *exactp)
{
	WT_CURSOR *c, *closest;
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL *session;
	u_int i;
	int cmp, deleted, exact;

	closest = NULL;
	clsm = (WT_CURSOR_LSM *)cursor;
	deleted = exact = 0;

	CURSOR_API_CALL(cursor, session, search_near, NULL);
	WT_CURSOR_NEEDKEY(cursor);
	WT_CURSOR_NOVALUE(cursor);
	WT_ERR(__clsm_enter(clsm, 1, 0));
	F_CLR(clsm, WT_CLSM_ITERATE_NEXT | WT_CLSM_ITERATE_PREV);

	WT_FORALL_CURSORS(clsm, c, i){
		c->set_key(c, &cursor->key);
		if ((ret = c->search_near(c, &cmp)) == WT_NOTFOUND) {
			ret = 0;
			continue;
		} 
		else if (ret != 0)
			goto err;

		/* Do we have an exact match? */
		if (cmp == 0) {
			closest = c;
			exact = 1;
			break;
		}

		if (cmp < 0) {
			if ((ret = c->next(c)) == WT_NOTFOUND) {
				ret = 0;
				continue;
			} 
			else if (ret != 0)
				goto err;
		}

		/*确定最近的cursor对象*/
		if (closest == NULL)
			closest = c;
		else {
			WT_ERR(WT_LSM_CURCMP(session, clsm->lsm_tree, c, closest, cmp));
			if (cmp < 0)
				closest = c;
		}
	}

	cmp = exact ? 0 : 1;

	/*确定cursor对应的KV值，并将closest设置为clsm当前的cursor对象*/
	if (closest == NULL)
		deleted = 1;
	else {
		WT_ERR(closest->get_key(closest, &cursor->key));
		WT_ERR(closest->get_value(closest, &cursor->value));
		clsm->current = closest;
		closest = NULL;
		deleted = __clsm_deleted(clsm, &cursor->value);
		if (!deleted)
			__clsm_deleted_decode(clsm, &cursor->value);
		else if ((ret = cursor->next(cursor)) == 0) {
			cmp = 1;
			deleted = 0;
		}
		WT_ERR_NOTFOUND_OK(ret);
	}
	if (deleted) {
		clsm->current = NULL;
		WT_ERR(cursor->prev(cursor));
		cmp = -1;
	}

	*exactp = cmp;

err:	
	__clsm_leave(clsm);
	API_END(session, ret);
	if (closest != NULL)
		WT_TRET(closest->reset(closest));

	F_CLR(cursor, WT_CURSTD_KEY_SET | WT_CURSTD_VALUE_SET);
	if (ret == 0) {
		F_SET(cursor, WT_CURSTD_KEY_INT | WT_CURSTD_VALUE_INT);
	} else
		clsm->current = NULL;

	return (ret);
}

/*lsm tree的put实现，将一个entry实例put到lsm tree的内存部分中*/
static inline int __clsm_put(WT_SESSION_IMPL *session, WT_CURSOR_LSM *clsm, const WT_ITEM *key, const WT_ITEM *value, int position)
{
	WT_CURSOR *c, *primary;
	WT_LSM_TREE *lsm_tree;
	u_int i, slot;

	lsm_tree = clsm->lsm_tree;

	WT_ASSERT(session, F_ISSET(&session->txn, TXN_HAS_ID) && clsm->primary_chunk != NULL &&
		(clsm->primary_chunk->switch_txn == WT_TXN_NONE || TXNID_LE(session->txn.id, clsm->primary_chunk->switch_txn)));

	primary = clsm->cursors[clsm->nchunks - 1];
	WT_RET(__clsm_reset_cursors(clsm, primary));

	/* If necessary, set the position for future scans. */
	if (position)
		clsm->current = primary;

	for (i = 0, slot = clsm->nchunks - 1; i < clsm->nupdates; i++, slot--) {
		/* Check if we need to keep updating old chunks. */
		if (i > 0 && __wt_txn_visible(session, clsm->switch_txn[slot])) {
				clsm->nupdates = i;
				break;
		}

		c = clsm->cursors[slot];
		c->set_key(c, key);
		c->set_value(c, value);
		WT_RET((position && i == 0) ? c->update(c) : c->insert(c));
	}

	if ((++clsm->primary_chunk->count % 100 == 0 || ++clsm->update_count >= 100) &&
		lsm_tree->merge_throttle + lsm_tree->ckpt_throttle > 0) {
			clsm->update_count = 0;
			WT_STAT_FAST_INCRV(session, &clsm->lsm_tree->stats,
				lsm_checkpoint_throttle, lsm_tree->ckpt_throttle);
			WT_STAT_FAST_CONN_INCRV(session,
				lsm_checkpoint_throttle, lsm_tree->ckpt_throttle);
			WT_STAT_FAST_INCRV(session, &clsm->lsm_tree->stats,
				lsm_merge_throttle, lsm_tree->merge_throttle);
			WT_STAT_FAST_CONN_INCRV(session,
				lsm_merge_throttle, lsm_tree->merge_throttle);
			__wt_sleep(0, lsm_tree->ckpt_throttle + lsm_tree->merge_throttle);
	}

	return 0;
}

/*
 * __clsm_insert --
 *	WT_CURSOR->insert method for the LSM cursor type.
 */
static int __clsm_insert(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_ITEM value;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;

	CURSOR_UPDATE_API_CALL(cursor, session, insert, NULL);
	WT_CURSOR_NEEDKEY(cursor);
	WT_CURSOR_NEEDVALUE(cursor);
	WT_ERR(__clsm_enter(clsm, 0, 1));

	if (!F_ISSET(cursor, WT_CURSTD_OVERWRITE) && (ret = __clsm_lookup(clsm, &value)) != WT_NOTFOUND) {
		if (ret == 0)
			ret = WT_DUPLICATE_KEY;
		goto err;
	}

	WT_ERR(__clsm_deleted_encode(session, &cursor->value, &value, &buf));
	ret = __clsm_put(session, clsm, &cursor->key, &value, 0);

err:	
	__wt_scr_free(session, &buf);
	__clsm_leave(clsm);
	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*
 * __clsm_update --
 *	WT_CURSOR->update method for the LSM cursor type.
 */
static int __clsm_update(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_ITEM(buf);
	WT_DECL_RET;
	WT_ITEM value;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;

	CURSOR_UPDATE_API_CALL(cursor, session, update, NULL);
	WT_CURSOR_NEEDKEY(cursor);
	WT_CURSOR_NEEDVALUE(cursor);
	WT_ERR(__clsm_enter(clsm, 0, 1));

	if (F_ISSET(cursor, WT_CURSTD_OVERWRITE) ||(ret = __clsm_lookup(clsm, &value)) == 0) {
		WT_ERR(__clsm_deleted_encode(session, &cursor->value, &value, &buf));
		ret = __clsm_put(session, clsm, &cursor->key, &value, 1);
	}

err:	
	__wt_scr_free(session, &buf);
	__clsm_leave(clsm);
	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*
 * __clsm_remove --
 *	WT_CURSOR->remove method for the LSM cursor type.
 */
static int __clsm_remove(WT_CURSOR *cursor)
{
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_ITEM value;
	WT_SESSION_IMPL *session;

	clsm = (WT_CURSOR_LSM *)cursor;

	CURSOR_UPDATE_API_CALL(cursor, session, remove, NULL);
	WT_CURSOR_NEEDKEY(cursor);
	WT_CURSOR_NOVALUE(cursor);
	WT_ERR(__clsm_enter(clsm, 0, 1));

	if (F_ISSET(cursor, WT_CURSTD_OVERWRITE) || (ret = __clsm_lookup(clsm, &value)) == 0)
		ret = __clsm_put(session, clsm, &cursor->key, &__tombstone, 1);

err:	
	__clsm_leave(clsm);
	CURSOR_UPDATE_API_END(session, ret);
	return (ret);
}

/*关闭一个lsm cursor对象*/
static int __clsm_close(WT_CURSOR* cursor)
{
	WT_CURSOR_LSM* clsm;
	WT_DECL_RET;
	WT_SESSION_IMPL* session;

	clsm = (WT_CURSOR_LSM*)cursor;
	CURSOR_API_CALL(cursor, session, close, NULL);

	WT_TRET(__clsm_close_cursors(clsm, 0, clsm->nchunks));
	__wt_free(session, clsm->blooms);
	__wt_free(session, clsm->cursors);
	__wt_free(session, clsm->switch_txn);

	/*结束cursor操作*/
	__clsm_leave(clsm);

	/*关闭lsm treed对象*/
	cursor->uri = NULL;
	if (clsm->lsm_tree != NULL)
		__wt_lsm_tree_release(session, clsm->lsm_tree);

	WT_TRET(__wt_cursor_close(cursor));

err:
	API_END_RET(session, ret);
}

/*
 * __wt_clsm_open --
 *	WT_SESSION->open_cursor method for LSM cursors.
 */
int __wt_clsm_open(WT_SESSION_IMPL *session,
    const char *uri, WT_CURSOR *owner, const char *cfg[], WT_CURSOR **cursorp)
{
	WT_CONFIG_ITEM cval;
	WT_CURSOR_STATIC_INIT(iface,
	    __wt_cursor_get_key,	/* get-key */
	    __wt_cursor_get_value,	/* get-value */
	    __wt_cursor_set_key,	/* set-key */
	    __wt_cursor_set_value,	/* set-value */
	    __clsm_compare,		/* compare */
	    __wt_cursor_equals,		/* equals */
	    __clsm_next,		/* next */
	    __clsm_prev,		/* prev */
	    __clsm_reset,		/* reset */
	    __clsm_search,		/* search */
	    __clsm_search_near,		/* search-near */
	    __clsm_insert,		/* insert */
	    __clsm_update,		/* update */
	    __clsm_remove,		/* remove */
	    __wt_cursor_reconfigure,	/* reconfigure */
	    __clsm_close);		/* close */
	WT_CURSOR *cursor;
	WT_CURSOR_LSM *clsm;
	WT_DECL_RET;
	WT_LSM_TREE *lsm_tree;

	clsm = NULL;
	cursor = NULL;

	if (!WT_PREFIX_MATCH(uri, "lsm:"))
		return (EINVAL);

	WT_RET(__wt_config_gets_def(session, cfg, "checkpoint", 0, &cval));
	if (cval.len != 0)
		WT_RET_MSG(session, EINVAL, "LSM does not support opening by checkpoint");

	/* Get the LSM tree. */
	WT_WITH_DHANDLE_LOCK(session, ret = __wt_lsm_tree_get(session, uri, 0, &lsm_tree));
	WT_RET(ret);

	WT_ERR(__wt_calloc_one(session, &clsm));

	cursor = &clsm->iface;
	*cursor = iface;
	cursor->session = &session->iface;
	cursor->uri = lsm_tree->name;
	cursor->key_format = lsm_tree->key_format;
	cursor->value_format = lsm_tree->value_format;

	clsm->lsm_tree = lsm_tree;

	/*
	 * The tree's dsk_gen starts at one, so starting the cursor on zero
	 * will force a call into open_cursors on the first operation.
	 */
	clsm->dsk_gen = 0;

	WT_STATIC_ASSERT(offsetof(WT_CURSOR_LSM, iface) == 0);
	WT_ERR(__wt_cursor_init(cursor, cursor->uri, owner, cfg, cursorp));

	if (0) {
err:		if (clsm != NULL)
			WT_TRET(__clsm_close(cursor));
	}

	return (ret);
}



