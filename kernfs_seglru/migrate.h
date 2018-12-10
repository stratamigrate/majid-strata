#ifndef _MIGRATE_H_
#define _MIGRATE_H_

#include "slru.h"

#ifdef __cplusplus
extern "C" {
#endif

#define EXTRA_NVM_LRU_LIST 2
//#define MIN_MIGRATE_ENTRY 512 
#define MIN_MIGRATE_ENTRY 1024
#define BLOCKS_PER_LRU_ENTRY (LRU_ENTRY_SIZE >> g_block_size_shift)
#define BLOCKS_TO_LRU_ENTRIES(x) ((x) / BLOCKS_PER_LRU_ENTRY)

typedef struct isolated_list {
	uint32_t n;
	struct list_head head;
	struct list_head fail_head;
} isolated_list_t;

int try_migrate_blocks(uint8_t from_dev, uint8_t to_dev, uint32_t nr_blocks, uint8_t force);
//int migrate_blocks(uint8_t from_dev, uint8_t to_dev, isolated_list_t *migrate_list);
int migrate_blocks(uint8_t from_dev, uint8_t to_dev, isolated_list_t *migrate_list, uint8_t extra_idx);
int try_writeback_blocks(uint8_t from_dev, uint8_t to_dev);
//int writeback_blocks(uint8_t from_dev, uint8_t to_dev, isolated_list_t *wb_list);
int update_slru_list(void);
int update_slru_list_from_digest(uint8_t dev, lru_key_t k, lru_val_t v);

extern lru_node_t *g_lru_hash[g_n_devices + 1];
extern struct lru g_lru[g_n_devices + 1];

//extra LRU lists and hash for segmented-LRU
extern lru_node_t *g_lru_hash_extra[EXTRA_NVM_LRU_LIST];
extern struct lru g_lru_extra[EXTRA_NVM_LRU_LIST];
#ifdef __cplusplus
}
#endif

#endif
