#include "fs.h"
#include "ds/list.h"
#include "global/util.h"
#include "migrate.h"
#include "extents.h"
#include "slru.h"

#define NON_EXTRA -1

lru_node_t *g_lru_hash[g_n_devices + 1];
struct lru g_lru[g_n_devices + 1];

lru_node_t *g_lru_hash_extra[EXTRA_NVM_LRU_LIST];
struct lru g_lru_extra[EXTRA_NVM_LRU_LIST];
// 0: not used
// 1: g_root_dev
// 2: g_ssd_dev
// 3: g_hdd_dev (not used)
// 4~ unused
static int wb_threshold[g_n_devices + 1] = {0, 60, 80, 100, 0};
//static int migrate_threshold[g_n_devices + 1] = {0, 91, 95, 100, 0};
static int migrate_threshold[g_n_devices + 1] = {0, 60, 95, 100, 0};

static inline uint8_t get_lower_dev(uint8_t dev)
{
	uint8_t lower_dev;

	if (dev == g_root_dev) {
#ifdef USE_SSD
		lower_dev = g_ssd_dev;
#else
		lower_dev = 0;
#endif
	} else if (dev == g_ssd_dev) {
#ifdef USE_HDD
		lower_dev = g_hdd_dev;
#else
		lower_dev = 0;
#endif
	} else if (dev == g_hdd_dev) {
		lower_dev = 0;
	}

	return lower_dev;
}

static void evict_slru_lists(void)
{
  uint8_t dev;

  for(dev = g_root_dev; get_lower_dev(dev) != 0; dev = get_lower_dev(dev)) {
    // Evict last one if over limit, move to next lower LRU
    while(g_lru[dev].n >= (disk_sb[dev].ndatablocks * migrate_threshold[dev]) / 100) {
      lru_node_t *n = list_last_entry(&g_lru[dev].lru_head, lru_node_t, list);
      uint8_t ndev = get_lower_dev(dev);

      /* mlfs_info("Evicting %u -> %u.\n", dev, ndev); */

      list_move(&n->list, &g_lru[ndev].lru_head);
      HASH_DEL(g_lru_hash[dev], n);
      HASH_ADD(hh, g_lru_hash[ndev], key, sizeof(lru_key_t), n);

      g_lru[dev].n--;
      g_lru[ndev].n++;
    }
  }
}

// TODO: Protect by lock
int update_slru_list_from_digest(uint8_t dev, lru_key_t k, lru_val_t v)
{
	mlfs_assert(dev == g_root_dev); // assume only NVM shared area is using segmented-LRU

	struct inode *inode;
	lru_node_t *node;
	
	// Search the original LRU list first
	HASH_FIND(hh, g_lru_hash[dev], &k, sizeof(lru_key_t), node);

	if (node) {
		list_del_init(&node->list);
		//node->access_freq[(ALIGN_FLOOR(search_key.offset, g_block_size_bytes)) >> g_block_size_shift]++;
		list_add(&node->list, &g_lru[dev].lru_head);
	} else {

#if EXTRA_NVM_LRU_LIST	
		for (uint8_t i=0; i<EXTRA_NVM_LRU_LIST; i++) {
			HASH_FIND(hh, g_lru_hash_extra[i], &k, sizeof(lru_key_t), node);
			if (node) { // hit: move to the head of previous list

				list_del_init(&node->list);
				if (i == 0) { // hit at first extra list
      					//mlfs_info("Promote %lu.\n", k.block); 
      					HASH_DEL(g_lru_hash_extra[0], node);
					HASH_ADD(hh, g_lru_hash[dev], key, sizeof(lru_key_t), node);
					list_add(&node->list, &g_lru[dev].lru_head);
					g_lru_extra[0].n--;	
					g_lru[dev].n++;
				} else { // hit at subsequent extra lists
      					HASH_DEL(g_lru_hash_extra[i], node);
					HASH_ADD(hh, g_lru_hash_extra[i-1], key, sizeof(lru_key_t), node);
					list_add(&node->list, &g_lru_extra[i-1].lru_head);
					g_lru_extra[i].n--;	
					g_lru_extra[i-1].n++;	
				}	
				return 0;
			}
		}		
#endif
		// miss in all lists: add to the head of last list
		node = (lru_node_t *)mlfs_zalloc(sizeof(lru_node_t));

		node->key = k;
		node->val = v;
		//memset(&node->access_freq, 0, LRU_ENTRY_SIZE >> g_block_size_shift);
		INIT_LIST_HEAD(&node->list);
		
#if EXTRA_NVM_LRU_LIST	
		
		uint8_t last_idx = EXTRA_NVM_LRU_LIST - 1;
		HASH_ADD(hh, g_lru_hash_extra[last_idx], key, sizeof(lru_key_t), node);
		node->sync = 0;
		list_add(&node->list, &g_lru_extra[last_idx].lru_head);
		g_lru_extra[last_idx].n++;	
#else
		HASH_ADD(hh, g_lru_hash[dev], key, sizeof(lru_key_t), node);
		node->sync = 0;
		list_add(&node->list, &g_lru[dev].lru_head);
		g_lru[dev].n++;
		
#endif 

		//evict_slru_lists();
	}

#if 0
	inode = icache_find(g_root_dev, k.inum);
	
	if (inode) {
		// update per-inode lru list.
		if (!is_del_entry(&node->per_inode_list))
			list_del(&node->per_inode_list);

		list_add(&node->per_inode_list, &inode->i_slru_head);
	}
#endif
}

int do_migrate_blocks(uint8_t from_dev, uint8_t to_dev, uint32_t file_inum, 
		offset_t offset, uint32_t length, addr_t blknr)
{
	int ret;
	struct inode *file_inode;
	struct dinode *file_dinode;
	struct buffer_head *bh_data, *bh;
	uint8_t *data;
	struct mlfs_ext_path *path = NULL;
	struct mlfs_map_blocks map;
	uint32_t nr_blocks = 0, nr_digested_blocks = 0;
	offset_t cur_offset;

	/* block migration is much simpler case than file digest */
	mlfs_assert((length >= g_block_size_bytes) && 
			(offset % g_block_size_bytes == 0) &&
			(length % g_block_size_bytes == 0));

	nr_blocks = (length >> g_block_size_shift);

	mlfs_assert(nr_blocks > 0);

	/* optimization: it does not need to read blocks from NVM.
	 * Instead, it is possible to storage to storage copy.  */
	if (from_dev == g_root_dev) {
		data = g_bdev[from_dev]->map_base_addr + (blknr << g_block_size_shift);
		bh = NULL;
	} else {
		uint8_t migrate_buffer[LRU_ENTRY_SIZE];
		struct buffer_head *bh;

		bh = bh_get_sync_IO(from_dev, blknr, BH_NO_DATA_ALLOC);

		bh->b_data = (uint8_t *)migrate_buffer;
		bh->b_size = LRU_ENTRY_SIZE;

		bh_submit_read_sync_IO(bh);
		if (from_dev == g_ssd_dev)
			mlfs_io_wait(g_ssd_dev, 1);

		data = bh->b_data;
		bh_release(bh);
	}

	file_inode = icache_find(g_root_dev, file_inum);
	mlfs_assert(file_inode);

	nr_digested_blocks = 0;
	cur_offset = offset;

	// multiple trials of block writing.
	// when extent tree has holes in a certain offset (due to data migration),
	// an extent is split at the hole. Kernfs should call mlfs_ext_get_blocks()
	// with setting m_lblk to the offset having a the hole to fill it.
	while (nr_digested_blocks < nr_blocks) {
		int nr_block_get = 0;
		handle_t handle = {.dev = to_dev};

		mlfs_assert((cur_offset % g_block_size_bytes) == 0);

		map.m_lblk = (cur_offset >> g_block_size_shift);
		map.m_pblk = 0;
		map.m_len = nr_blocks - nr_digested_blocks;
		map.m_flags = 0;

		// find block address of offset and update extent tree
		nr_block_get = mlfs_ext_get_blocks(&handle, file_inode, &map, 
				MLFS_GET_BLOCKS_CREATE);

		mlfs_assert(map.m_pblk != 0);

		mlfs_assert(nr_block_get <= (nr_blocks - nr_digested_blocks));
		mlfs_assert(nr_block_get > 0);

		nr_digested_blocks += nr_block_get;

		mlfs_debug("[migrate] inum %d, offset %lu len %u (dev %d:%lu) -> (dev %d:%lu)\n", 
				file_inode->inum, cur_offset, nr_block_get << g_block_size_shift, 
				from_dev, blknr, to_dev, map.m_pblk);

		bh_data = bh_get_sync_IO(to_dev, map.m_pblk, BH_NO_DATA_ALLOC);

		bh_data->b_data = data;
		bh_data->b_blocknr = map.m_pblk;
		bh_data->b_size = nr_block_get * g_block_size_bytes;
		bh_data->b_offset = 0;

		ret = mlfs_write(bh_data);
		mlfs_assert(!ret);
		clear_buffer_uptodate(bh_data);
		bh_release(bh_data);

		cur_offset += nr_block_get * g_block_size_bytes;
		data += nr_block_get * g_block_size_bytes;
	}

	mlfs_assert(nr_blocks == nr_digested_blocks);

	if (file_inode->size < offset + length)
		file_inode->size = offset + length;

	return 0;
}

//int migrate_blocks(uint8_t from_dev, uint8_t to_dev, isolated_list_t *migrate_list)
int migrate_blocks(uint8_t from_dev, uint8_t to_dev, isolated_list_t *migrate_list, uint8_t extra_idx)
{
	int ret;
	lru_node_t *l, *tmp;
	mlfs_fsblk_t blknr;
	struct inode *file_inode;
	struct mlfs_map_blocks map;
	offset_t cur_lblk;
	uint32_t nr_blocks, nr_done = 0;
	uint32_t migrated_success = 0;
	uint8_t lower_dev;
	struct list_head migrate_success_list;
	handle_t handle = {.dev = from_dev};

	INIT_LIST_HEAD(&migrate_success_list);

	mlfs_info("Enter migrate_blocks with extra_idx: %d\n", extra_idx);

	list_for_each_entry_safe_reverse(l, tmp, &migrate_list->head, list) {
		file_inode = icache_find(g_root_dev, l->val.inum);
		mlfs_assert(file_inode);

		/* mlfs_assert(l->key.offset % g_block_size_bytes == 0); */
	
		cur_lblk = l->val.lblock;
		nr_blocks = (LRU_ENTRY_SIZE >> g_block_size_shift);
		
		nr_done = 0;
again:
		map.m_len = nr_blocks - nr_done;
		map.m_lblk = cur_lblk;
		map.m_flags = 0;

		ret = mlfs_ext_get_blocks(&handle, file_inode, &map, 0);

		mlfs_assert(ret >= 0);
		//FIXME: figure out when this condition does not meet.
		//mlfs_assert(map.m_pblk == l->key.block);

		if (ret == 0) {
			list_del_init(&l->list);
			list_add_tail(&l->list, &migrate_list->fail_head);
			
			//chunkai: debug why migration fails
			//mlfs_debug("migrate fail (%d->%d): inum %d offset %lu(0x%lx)\n", 
			mlfs_info("migrate fail (%d->%d): inum %d offset %lu(0x%lx)\n", 
					from_dev, to_dev, l->val.inum, 
					cur_lblk << g_block_size_shift, 
					cur_lblk << g_block_size_shift);
			continue;
		}

		mlfs_assert(map.m_pblk != 0);

		nr_done += ret;

		mlfs_debug("migrate (%d->%d): inum %d offset %lu(0x%lx)\n", 
				from_dev, to_dev, l->val.inum, 
				cur_lblk << g_block_size_shift, 
				cur_lblk << g_block_size_shift);

		do_migrate_blocks(from_dev, to_dev, 
				l->val.inum, 
				cur_lblk << g_block_size_shift, 
				ret << g_block_size_shift, 
				map.m_pblk);


		if (nr_blocks > nr_done) {
			cur_lblk += nr_done;
			//chunkai: debug why migration fails
			//mlfs_debug("current extent is small than LRY_ENTRY_SIZE: block got %u\n",
			mlfs_info("current extent is small than LRY_ENTRY_SIZE: block got %u\n",
					nr_done);
			goto again;
		}

		list_del_init(&l->list);
		list_add(&l->list, &migrate_success_list);

#if EXTRA_NVM_LRU_LIST	
		if (extra_idx == NON_EXTRA) {
			g_lru[from_dev].n--;
		} else {
			g_lru_extra[extra_idx].n--;
		}	
#else
		g_lru[from_dev].n--;
#endif
		migrated_success++;
	}

	mlfs_info("After all do_migrate_blocks with extra_idx: %d\n", extra_idx);
	// Wait for finishing all outstanding IO.
	if (to_dev == g_ssd_dev)
		mlfs_io_wait(g_ssd_dev, 1);

	list_for_each_entry_safe(l, tmp, &migrate_success_list, list) {
		handle_t handle;
		mlfs_lblk_t start, end;
		file_inode = icache_find(g_root_dev, l->val.inum);
		mlfs_assert(file_inode);

		start = l->val.lblock;
		end = start + (LRU_ENTRY_SIZE >> g_block_size_shift);

		mlfs_assert((end << g_block_size_shift) <= file_inode->size);

		handle.dev = from_dev;
		ret = mlfs_ext_truncate(&handle, file_inode, start, end - 1);

		if (ret != 0) {
			list_del_init(&l->list);
			list_add(&l->list, &migrate_list->fail_head);
			continue;
		}

		mlfs_debug("[truncate] dev %d inum = %d offset %u ~ %u\n",
				from_dev, file_inode->inum, start, end);

		list_del_init(&l->list);

#if EXTRA_NVM_LRU_LIST	
		if (extra_idx == NON_EXTRA) {
			HASH_DEL(g_lru_hash[from_dev], l);
		} else {
			HASH_DEL(g_lru_hash_extra[extra_idx], l);
		}	
#else
		HASH_DEL(g_lru_hash[from_dev], l);
#endif
		if (to_dev == g_hdd_dev) {
			mlfs_free(l);
		} else {
			// put successful entries to lru list of to_dev
			HASH_ADD(hh, g_lru_hash[to_dev], key, sizeof(lru_key_t), l);
			l->sync = 0;
			list_add(&l->list, &g_lru[to_dev].lru_head);
			g_lru[to_dev].n++;
		}
	}

	mlfs_info("Data migration (%d -> %d) is done (%u / %u): %u MB\n", 
			from_dev, to_dev,
			migrated_success, migrate_list->n,
			(migrated_success * LRU_ENTRY_SIZE) >> 20);

	g_perf_stats.total_migrated_mb += 
		((migrated_success * LRU_ENTRY_SIZE) >> 20);

	show_storage_stats();

	// cascading data migration to lower than to_dev
	lower_dev = get_lower_dev(to_dev);

	if (lower_dev != 0) 
		try_migrate_blocks(to_dev, lower_dev, 0, 0);

	return migrated_success;
}

/* nr_blocks: minimum amount of blocks to migrate */
int try_migrate_blocks(uint8_t from_dev, uint8_t to_dev, uint32_t nr_blocks, uint8_t force)
{
#ifdef MIGRATION
	uint32_t n_entries = 0, i = 0, ret, do_migrate = 0;
	uint64_t used_blocks, datablocks;
	struct isolated_list migrate_list;

#if EXTRA_NVM_LRU_LIST	
	struct isolated_list extra_migrate_list[EXTRA_NVM_LRU_LIST];
#endif
	lru_node_t *node, *tmp;

#ifndef USE_SSD
	return 0;
#endif

	if (force) {
		do_migrate = 1;
		goto do_force_migration;
	}

	used_blocks = sb[from_dev]->used_blocks;
	datablocks = disk_sb[from_dev].ndatablocks;

	if (used_blocks > (migrate_threshold[from_dev] * datablocks) / 100) {
		n_entries = BLOCKS_TO_LRU_ENTRIES(
				used_blocks - ((migrate_threshold[from_dev] * datablocks) / 100));
		do_migrate = 1;
	} else {
		// do not support writeback.
		return 0;

		if (used_blocks > (wb_threshold[from_dev] * datablocks) / 100) {
			n_entries = 0;
			do_migrate = 0;
		} else
			return 0;
	}

do_force_migration:
	if (nr_blocks == 0) 
		nr_blocks = BLOCKS_PER_LRU_ENTRY * MIN_MIGRATE_ENTRY;

	INIT_LIST_HEAD(&migrate_list.head);
	INIT_LIST_HEAD(&migrate_list.fail_head);

#if EXTRA_NVM_LRU_LIST	
	uint8_t j = 0;
	for (j=0; j<EXTRA_NVM_LRU_LIST; j++) {
		INIT_LIST_HEAD(&extra_migrate_list[j].head);
		INIT_LIST_HEAD(&extra_migrate_list[j].fail_head);
		extra_migrate_list[j].n = 0;
	}
#endif

	n_entries = max(n_entries, BLOCKS_TO_LRU_ENTRIES(nr_blocks));
	mlfs_info("migration: n_entries %u\n", n_entries);

	migrate_list.n = 0;

#if EXTRA_NVM_LRU_LIST	
	// collect entries to migrate starting from the last extra list
	uint8_t list_idx = EXTRA_NVM_LRU_LIST - 1;
	do {
		list_for_each_entry_safe_reverse(node, tmp, &g_lru_extra[list_idx].lru_head, list) {
			list_del_init(&node->list);

			list_add(&node->list, &extra_migrate_list[list_idx].head);
			extra_migrate_list[list_idx].n++;

			i++;

			if (i >= n_entries)
				break;	
		}

		migrate_blocks(from_dev, to_dev, &extra_migrate_list[list_idx], list_idx);

		list_for_each_entry_safe_reverse(node, tmp, &extra_migrate_list[list_idx].fail_head, list) {
			mlfs_info("migrate fail (%d->%d): inum %d offset %lu(0x%lx)\n", 
					from_dev, to_dev, node->val.inum, 
					node->val.lblock, node->val.lblock);
	
			list_del_init(&node->list);
			list_add_tail(&node->list, &g_lru_extra[list_idx].lru_head);
		}

		list_idx -= 1;
	} while (i < n_entries && list_idx > 0);	
		
#endif
	if (i < n_entries) {
		list_for_each_entry_safe_reverse(node, tmp, &g_lru[from_dev].lru_head, list) {
			list_del_init(&node->list);

			list_add(&node->list, &migrate_list.head);
			migrate_list.n++;

			i++;

			mlfs_debug("try migrate (%d->%d): inum %d offset %lu(0x%lx) phys %lu\n", 
					from_dev, to_dev, node->val.inum, node->val.lblock, 
					node->val.lblock, node->key.block);

			if (i >= n_entries)
				break;
		}

		ret = migrate_blocks(from_dev, to_dev, &migrate_list, NON_EXTRA); 

		// TODO: figure out why the list fails.
		// Put back the failed list to lru_head
		list_for_each_entry_safe_reverse(node, tmp, &migrate_list.fail_head, list) {
			mlfs_info("migrate fail (%d->%d): inum %d offset %lu(0x%lx)\n", 
					from_dev, to_dev, node->val.inum, 
					node->val.lblock, node->val.lblock);

			list_del_init(&node->list);
			list_add_tail(&node->list, &g_lru[from_dev].lru_head);
		}

	} 
	return 0;
#endif // MIGRATION
}
