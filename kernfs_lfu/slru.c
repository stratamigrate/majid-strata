#include "slru.h"
#ifdef KERNFS
#include "shared.h"
#elif LIBFS
#include "filesystem/shared.h"
#endif
#include "fs.h"
#include "global/util.h"

lru_node_t *lru_hash;

lfu_node_t* get_new_lfu_node(uint64_t count, lfu_node_t* prev, lfu_node_t* next)
{
	lfu_node_t* node = (lfu_node_t *)mlfs_zalloc(sizeof(lfu_node_t));
	node->n = 0;
	node->count = count;
	node->prev = prev;
	node->next = next;
	prev->next = node;
	next->next = node;
	return node;
}

void delete_lfu_node(lfu_node_t* node)
{
	node->prev->next = node->next;
	node->next->prev = node->prev;
}

int slru_upsert(struct inode *inode, struct list_head *lru_head, lru_key_t k, lru_val_t v) 
{
	lru_node_t *node, search;

	memset(&search, 0, sizeof(lru_node_t));

	search.key = k;

#ifdef LIBFS
	pthread_rwlock_wrlock(shm_lru_rwlock);
#endif

	HASH_FIND(hh, lru_hash, &search.key, sizeof(lru_key_t), node);

	if (node) {
		list_del_init(&node->list);
		list_add(&node->list, lru_head);
	}
	else {
	//if (!node) {
		//node = (lru_node_t *)mlfs_alloc(sizeof(lru_node_t));
		node = (lru_node_t *)mlfs_alloc_shared(sizeof(lru_node_t));
		// if forgot to this memset, UThash does not work.
		memset(node, 0, sizeof(lru_node_t));

		mlfs_debug("add a new key: dev %u, block%lx\n", k.dev, k.block);

		node->key = k;
		node->val = v;
		//memset(&node->access_freq, 0, LRU_ENTRY_SIZE >> g_block_size_shift);

		INIT_LIST_HEAD(&node->list);
		INIT_LIST_HEAD(&node->per_inode_list);

		HASH_ADD(hh, lru_hash, key, sizeof(lru_key_t), node);

		node->sync = 0;
		list_add(&node->list, lru_head);
	}

	/*
	if (inode) {
		if (!is_del_entry(&node->per_inode_list))
			list_del(&node->per_inode_list);

		list_add(&node->per_inode_list, &inode->i_slru_head);
	}
	*/

	// Add to head of lru_list.
	//list_del_init(&node->list);
	//list_add(&node->list, lru_head);

	// update access frequency information.
	//node->access_freq[(ALIGN_FLOOR(k.offset, g_block_size_bytes)) >> g_block_size_shift]++;

#ifdef LIBFS
	pthread_rwlock_unlock(shm_lru_rwlock);
#endif

	return 0;
}

