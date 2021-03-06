#include "wt_internal.h"

static void __inmem_col_fix(WT_SESSION_IMPL *, WT_PAGE *);
static void __inmem_col_int(WT_SESSION_IMPL *, WT_PAGE *);
static int  __inmem_col_var(WT_SESSION_IMPL *, WT_PAGE *, size_t *);
static int  __inmem_row_int(WT_SESSION_IMPL *, WT_PAGE *, size_t *);
static int  __inmem_row_leaf(WT_SESSION_IMPL *, WT_PAGE *);
static int  __inmem_row_leaf_entries(WT_SESSION_IMPL *, const WT_PAGE_HEADER *, uint32_t *);

/*检查一个page是否可以强制驱逐（淘汰）出内存*/
static int __evict_force_check(WT_SESSION_IMPL* session, WT_PAGE* page, uint32_t flags)
{
	WT_BTREE* btree;
	btree = S2BT(session);

	/*page的内存没有达到驱逐的阈值，可以不用被驱逐出内存*/
	if (page->memory_footprint < btree->maxmempage)
		return 0;

	/*索引page暂不驱逐，只驱逐leaf page*/
	if (WT_PAGE_IS_INTERNAL(page))
		return 0;

	/*驱逐被关闭,不做驱逐动作*/
	if (LF_ISSET(WT_READ_NO_EVICT) || F_ISSET(btree, WT_BTREE_NO_EVICTION))
		return 0;

	/*没太弄明白为什么没有修改的page不做驱逐,如果没有做修改，即使是超过了最大内存限制，也不做evict操作，可能是为了保护读page*/
	if (page->modify == NULL)
		return 0;

	/*触发驱逐page动作,设置page的驱逐状态*/
	__wt_page_evict_soon(page);

	/*判断是否可以直接驱逐*/
	return __wt_page_can_evict(session, page, 1);
}

/*帮助ref对应的page获得一个hazard pointer,如果page是在磁盘上，将page读入memory中*/
int __wt_page_in_func(WT_SESSION_IMPL *session, WT_REF *ref, uint32_t flags)
{
	WT_DECL_RET;
	WT_PAGE *page;
	u_int sleep_cnt, wait_cnt;
	int busy, force_attempts, oldgen;

	for (force_attempts = oldgen = 0, wait_cnt = 0;;){
		switch (ref->state){
		case WT_REF_DISK:
		case WT_REF_DELETED:
			/*page已经做了cache read，无需read*/
			if (LF_ISSET(WT_READ_CACHE))
				return WT_NOTFOUND;
			/*检查lru cache中的数据是否满，如果超过95%的占有率，先要进行lru淘汰，再进行当前引用page的读取*/
			WT_RET(__wt_cache_full_check(session));
			WT_RET(__wt_cache_read(session, ref));
			oldgen = LF_ISSET(WT_READ_WONT_NEED) || F_ISSET(session, WT_SESSION_NO_CACHE);
			continue;

		case WT_REF_READING:
			if (LF_ISSET(WT_READ_CACHE))
				return WT_NOTFOUND;
			if (LF_ISSET(WT_READ_NO_WAIT))
				return WT_NOTFOUND;
			WT_STAT_FAST_CONN_INCR(session, page_read_blocked);
			break;

		case WT_REF_LOCKED:
			if (LF_ISSET(WT_READ_NO_WAIT))
				return WT_NOTFOUND;
			WT_STAT_FAST_CONN_INCR(session, page_locked_blocked);
			break;

		case WT_REF_SPLIT:
			/*ref对应的page在内存中被split后，对应的page在内存中不存在了，需要重新在上一层做一次检索定位新的ref*/
			return WT_RESTART;

			/*已经导入内存中了*/
		case WT_REF_MEM:
			/*如果page导入memory，需要占用一个hazard pointer，如果占用hazard pointer时返回busy，表示这个page可能已经正在被驱逐,需要重试*/
			WT_RET(__wt_hazard_set(session, ref, &busy)); 
			if (busy){
				WT_STAT_FAST_CONN_INCR(session, page_busy_blocked);
				break;
			}

			/*走到这一步，说明page获得一个hazard pointer*/
			page = ref->page;
			WT_ASSERT(session, page != NULL);

			/*如果这个page的内存占用超过了设置最大内存，进行evict操作，重整内存结构,有可能会触发split操作*/
			if (force_attempts < 10 && __evict_force_check(session, page, flags)){
				++force_attempts;
				ret = __wt_page_release_evict(session, ref);
				/*evict失败,退出循环重试*/
				if (ret == EBUSY) {
					ret = 0;
					wait_cnt += 1000;
					WT_STAT_FAST_CONN_INCR(session, page_forcible_evict_blocked);
					break;
				}
				else
					WT_RET(ret);

				continue;
			}

			/* Check if we need an autocommit transaction. */
			if ((ret = __wt_txn_autocommit_check(session)) != 0) {
				WT_TRET(__wt_hazard_clear(session, page)); /*清除hazard pointer占用*/
				return (ret);
			}

			/*如果page是没有设置evict操作的且read_gen是一个初始化状态，那么设置一个可以evict给它进行动态evict*/
			if (oldgen && page->read_gen == WT_READGEN_NOTSET)
				__wt_page_evict_soon(page);
			else if (!LF_ISSET(WT_READ_NO_GEN) && page->read_gen != WT_READGEN_OLDEST && page->read_gen < __wt_cache_read_gen(session))
				page->read_gen = __wt_cache_read_gen_set(session); /*为刚载入内存中的page分配一个lru read_gen版本号*/

			return 0;
			WT_ILLEGAL_VALUE(session);
		}

		/*spin waiting*/
		if (++wait_cnt < 1000)
			__wt_yield();
		else {
			sleep_cnt = WT_MIN(wait_cnt, 10000);
			wait_cnt *= 2;
			WT_STAT_FAST_CONN_INCRV(session, page_sleep, sleep_cnt);
			__wt_sleep(0, sleep_cnt);
		}
	}
}

/*根据page的entries分配一个完成的page内存对象*/
int __wt_page_alloc(WT_SESSION_IMPL* session, uint8_t type, uint64_t recno, uint32_t alloc_entries, int alloc_refs, WT_PAGE** pagep)
{
	WT_CACHE *cache;
	WT_DECL_RET;
	WT_PAGE *page;
	WT_PAGE_INDEX *pindex;
	size_t size;
	uint32_t i;
	void *p;

	*pagep = NULL;

	cache = S2C(session)->cache;
	page = NULL;

	size = sizeof(WT_PAGE);
	switch (type){
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		break;

	case WT_PAGE_COL_VAR:
		/*计算page的内存空间占用*/
		size += alloc_entries * sizeof(WT_COL);
		break;

	case WT_PAGE_ROW_LEAF:
		size += alloc_entries * sizeof(WT_ROW);
		break;

	WT_ILLEGAL_VALUE(session);
	}

	/*分配page对象空间并进行page类型设置*/
	WT_RET(__wt_calloc(session, 1, size, &page));
	page->type = type;
	/*设置一个初始化的read_gen值*/
	page->read_gen = WT_READGEN_NOTSET;

	switch (type){
	case WT_PAGE_COL_FIX:
		page->pg_fix_recno = recno;
		page->pg_fix_entries = alloc_entries;
		break;

	case WT_PAGE_COL_INT:
	case WT_PAGE_ROW_INT:
		page->pg_intl_recno = recno;
		/*创建内存中的WT_REF对象和对应的ref slot array空间*/
		WT_ERR(__wt_calloc(session, 1, sizeof(WT_PAGE_INDEX)+alloc_entries * sizeof(WT_REF *), &p));
		size += sizeof(WT_PAGE_INDEX)+alloc_entries * sizeof(WT_REF *);
		pindex = p;
		pindex->index = (WT_REF **)((WT_PAGE_INDEX *)p + 1);
		pindex->entries = alloc_entries;
		WT_INTL_INDEX_SET(page, pindex);
		if (alloc_refs){
			for (i = 0; i < pindex->entries; ++i) {
				WT_ERR(__wt_calloc_one(session, &pindex->index[i]));
				size += sizeof(WT_REF);
			}
		}
		if (0){
err:
			if ((pindex = WT_INTL_INDEX_GET_SAFE(page)) != NULL) {
				for (i = 0; i < pindex->entries; ++i)
					__wt_free(session, pindex->index[i]);
				__wt_free(session, pindex);
			}
			__wt_free(session, page);
			return (ret);
		}

	case WT_PAGE_COL_VAR:
		page->pg_var_recno = recno;
		page->pg_var_d = (WT_COL *)((uint8_t *)page + sizeof(WT_PAGE));
		page->pg_var_entries = alloc_entries;
		break;

	case WT_PAGE_ROW_LEAF:
		page->pg_row_d = (WT_ROW *)((uint8_t *)page + sizeof(WT_PAGE));
		page->pg_row_entries = alloc_entries;
		break;

		WT_ILLEGAL_VALUE(session);
	}
	/*更新cache的状态信息*/
	__wt_cache_page_inmem_incr(session, page, size);
	WT_ATOMIC_ADD8(cache->bytes_read, size);
	WT_ATOMIC_ADD8(cache->pages_inmem, 1);

	*pagep = page;
	return 0;
}

/*将一个page从磁盘上读入内存，并且构建其page的内存结构信息*/
int __wt_page_inmem(WT_SESSION_IMPL *session, WT_REF *ref, const void *image, size_t memsize, uint32_t flags, WT_PAGE **pagep)
{
	WT_DECL_RET;
	WT_PAGE *page;
	const WT_PAGE_HEADER *dsk;
	uint32_t alloc_entries;
	size_t size;

	*pagep = NULL;

	dsk = image;
	alloc_entries = 0;

	/*确定需要在内存中分配的entry数量*/
	switch(dsk->type){
	case WT_PAGE_COL_FIX:
	case WT_PAGE_COL_INT:
	case WT_PAGE_COL_VAR:
		alloc_entries = dsk->u.entries;
		break;

	case WT_PAGE_ROW_INT:
		/*
		* Row-store internal page entries map one-to-two to the number
		* of physical entries on the page (each entry is a key and
		* location cookie pair, KEY算一个entry,value算一个entry).
		*/
		alloc_entries = dsk->u.entries / 2;
		break;

	case WT_PAGE_ROW_LEAF:
		/*
		* If the "no empty values" flag is set, row-store leaf page
		* entries map one-to-one to the number of physical entries
		* on the page (each physical entry is a key or value item).
		* If that flag is not set, there are more keys than values,
		* we have to walk the page to figure it out.
		*/
		if (F_ISSET(dsk, WT_PAGE_EMPTY_V_ALL))
			alloc_entries = dsk->u.entries;
		else if (F_ISSET(dsk, WT_PAGE_EMPTY_V_NONE))
			alloc_entries = dsk->u.entries / 2;
		else /*根据实际情况扫描得到到底有多少个k/v entry*/
			WT_RET(__inmem_row_leaf_entries(session, dsk, &alloc_entries));
		break;

	WT_ILLEGAL_VALUE(session);
	}

	/*分配page对象的内存并初始化*/
	WT_RET(__wt_page_alloc(session, dsk->type, dsk->recno, alloc_entries, 1, &page));
	page->dsk = dsk;
	F_SET_ATOMIC(page, flags);

	size = LF_ISSET(WT_PAGE_DISK_ALLOC) ? memsize : 0;

	/*设置page内部内存对象结构和记录长度等*/
	switch(page->type){
	case WT_PAGE_COL_FIX:
		__inmem_col_fix(session, page);
		break;

	case WT_PAGE_COL_INT:
		__inmem_col_int(session, page);
		break;

	case WT_PAGE_COL_VAR:
		WT_ERR(__inmem_col_var(session, page, &size));
		break;

	case WT_PAGE_ROW_INT:
		WT_ERR(__inmem_row_int(session, page, &size));
		break;

	case WT_PAGE_ROW_LEAF:
		WT_ERR(__inmem_row_leaf(session, page));
		break;

	WT_ILLEGAL_VALUE_ERR(session);
	}

	/*修改page对应的内存cache统计*/
	__wt_cache_page_inmem_incr(session, page, size);

	if(ref != NULL){
		switch (page->type) {
		case WT_PAGE_COL_INT:
		case WT_PAGE_ROW_INT:
			page->pg_intl_parent_ref = ref;
			break;
		}
		ref->page = page;
	}

	*pagep = page;
	return 0;

err:
	__wt_page_out(session, &page); /*如果内存对象失败，将内存对象page废弃*/
	return ret;
}

/*如果是列式固定长度存储，指定存储数据开始的缓冲区位置*/
static void __inmem_col_fix(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	WT_BTREE *btree;
	const WT_PAGE_HEADER *dsk;

	btree = S2BT(session);
	dsk = page->dsk;

	page->pg_fix_bitf = WT_PAGE_HEADER_BYTE(btree, dsk);
}

/*构建列式存储 内存索引的结构*/
static void __inmem_col_int(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	const WT_PAGE_HEADER *dsk;
	WT_PAGE_INDEX *pindex;
	WT_REF **refp, *ref;
	uint32_t i;

	btree = S2BT(session);
	dsk = page->dsk;
	unpack = &_unpack;

	/*获得page的index指针数组*/
	pindex = WT_INTL_INDEX_GET_SAFE(page);
	refp = pindex->index;

	/*指定索引结构指向的下一级page位置*/
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i){
		ref = *refp ++;
		ref->home = page;
		/*读取cell对应的kv数据，并进行unpack，设置到ref上*/
		__wt_cell_unpack(cell, unpack);
		ref->addr = cell;
		ref->key.recno = unpack->v;
	}
}

/*统计page中重复entry的个数*/
static int __inmem_col_var_repeats(WT_SESSION_IMPL* session, WT_PAGE* page, uint32_t* np)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	const WT_PAGE_HEADER *dsk;
	uint32_t i;

	btree = S2BT(session);
	dsk = page->dsk;
	unpack = &_unpack;

	*np = 0;

	*np = 0;
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i) {
		__wt_cell_unpack(cell, unpack);
		if (__wt_cell_rle(unpack) > 1) /*unpack->v <= 1表示entries重复*/
			++*np;
	}

	return 0;
}

/*为变长的列式存储page构建内存中的索引结构对象*/
static int __inmem_col_var(WT_SESSION_IMPL *session, WT_PAGE *page, size_t *sizep)
{
	WT_BTREE *btree;
	WT_COL *cip;
	WT_COL_RLE *repeats;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	const WT_PAGE_HEADER *dsk;
	uint64_t recno, rle;
	size_t bytes_allocated;
	uint32_t i, indx, n, repeat_off;

	btree = S2BT(session);
	dsk = page->dsk;
	recno = page->pg_var_recno;

	repeats = NULL;
	repeat_off = 0;
	unpack = &_unpack;
	bytes_allocated = 0;

	indx = 0;
	cip = page->pg_var_d;

	WT_CELL_FOREACH(btree, dsk, cell, unpack, i){
		__wt_cell_unpack(cell, unpack);
		WT_COL_PTR_SET(cip, WT_PAGE_DISK_OFFSET(page, cell));
		cip++;

		/*为重复的entry分配内存并做对应的repeat标示*/
		rle = __wt_cell_rle(unpack);
		if (rle > 1) {
			if(repeats == NULL){
				WT_RET(__inmem_col_var_repeats(session, page, &n));
				WT_RET(__wt_realloc_def(session, &bytes_allocated, n + 1, &repeats));

				page->pg_var_repeats = repeats;
				page->pg_var_nrepeats = n;
				*sizep += bytes_allocated;
			}
			repeats[repeat_off].indx = indx;
			repeats[repeat_off].recno = recno;
			repeats[repeat_off++].rle = rle;
		}

		indx++;
		recno += rle;
	}

	return 0;
}

/*为行存储的page构建内部索引的内存对象,非叶子节点*/
static int __inmem_row_int(WT_SESSION_IMPL* session, WT_PAGE* page, size_t* sizep)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	WT_DECL_ITEM(current);
	WT_DECL_RET;
	const WT_PAGE_HEADER *dsk;
	WT_PAGE_INDEX *pindex;
	WT_REF *ref, **refp;
	uint32_t i;

	btree = S2BT(session);
	unpack = &_unpack;
	dsk = page->dsk;

	WT_RET(__wt_scr_alloc(session, 0, &current));

	/*设置索引对象指向下一层page的对象指针*/
	pindex = WT_INTL_INDEX_GET_SAFE(page);
	refp = pindex->index;
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i){
		ref = *refp;
		ref->home = page;

		__wt_cell_unpack(cell, unpack);
		switch(unpack->type){
		case WT_CELL_KEY:
			__wt_ref_key_onpage_set(page, ref, unpack);
			break;

		case WT_CELL_KEY_OVFL:
			/*overflow key的内容是存储在单独存储空间上，需要用overflow方式读取到内存对象中来并构建row key*/
			WT_ERR(__wt_dsk_cell_data_ref(session, page->type, unpack, current));
			WT_ERR(__wt_row_ikey_incr(session, page, WT_PAGE_DISK_OFFSET(page, cell), current->data, current->size, ref));
			*sizep += sizeof(WT_IKEY) + current->size;
			break;

			/*被del mark的cell数据是需要重建标示modified的*/
		case WT_CELL_ADDR_DEL:
			ref->addr = cell;
			ref->state = WT_REF_DELETED;
			++refp;
			if(btree->modified){
				WT_ERR(__wt_page_modify_init(session, page));
				__wt_page_modify_set(session, page);
			}
			break;

		case WT_CELL_ADDR_INT:
		case WT_CELL_ADDR_LEAF:
		case WT_CELL_ADDR_LEAF_NO:
			ref->addr = cell;
			++refp;
			break;

		WT_ILLEGAL_VALUE_ERR(session);
		}
	}

err:
	__wt_scr_free(session, &current);
	return ret;
}

/*计算从磁盘上刚读取的行存储page中的entry数量*/
static int __inmem_row_leaf_entries(WT_SESSION_IMPL* session, const WT_PAGE_HEADER* dsk, uint32_t* nindxp)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	uint32_t i, nindx;

	btree = S2BT(session);
	unpack = &_unpack;

	nindx = 0;

	WT_CELL_FOREACH(btree, dsk, cell, unpack, i){
		__wt_cell_unpack(cell, unpack);
		switch (unpack->type) {
		case WT_CELL_KEY:
		case WT_CELL_KEY_OVFL:
			++nindx;
			break;

		case WT_CELL_VALUE:
		case WT_CELL_VALUE_OVFL:
			break;

		WT_ILLEGAL_VALUE(session);
		}
	}

	*nindxp = nindx;

	return 0;
}

/*构建行存储leaf page的内存索引对象(row array)*/
static int __inmem_row_leaf(WT_SESSION_IMPL* session, WT_PAGE* page)
{
	WT_BTREE *btree;
	WT_CELL *cell;
	WT_CELL_UNPACK *unpack, _unpack;
	const WT_PAGE_HEADER *dsk;
	WT_ROW *rip;
	uint32_t i;

	btree = S2BT(session);
	dsk = page->dsk;
	unpack = &_unpack;

	rip = page->pg_row_d;
	WT_CELL_FOREACH(btree, dsk, cell, unpack, i){
		__wt_cell_unpack(cell, unpack);

		switch(unpack->type){
			case WT_CELL_KEY_OVFL:
				__wt_row_leaf_key_set_cell(page, rip, cell); /*直接设置成CELL_FLAG方式访问*/
				++rip;
				break;

			case WT_CELL_KEY:
				if (!btree->huffman_key && unpack->prefix == 0)
					__wt_row_leaf_key_set(page, rip, unpack); /*设置成K_FLAG方式访问*/
				else
					__wt_row_leaf_key_set_cell(page, rip, cell);
				++rip;
				break;

			case WT_CELL_VALUE:
				/*将K_CELL_FLAG设置成KV_CELL_FLAG,这里的rip -1的做法是因为WT_CELL_KEY先会被读出执行，造成rip ++,但是value是++之前的key对应的value*/
				if (!btree->huffman_value)
					__wt_row_leaf_value_set(page, rip - 1, unpack); 
				break;

			case WT_CELL_VALUE_OVFL:
				break;

			WT_ILLEGAL_VALUE(session);
		}
	}

	return 0;
}






