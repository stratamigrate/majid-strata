#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>
#include <errno.h>

#include "common.h"
#include "async.h"
#include "global/defs.h"
#include "global/mem.h"
#include "global/util.h"

#define Q_DEPTH 256

//#define INNER_IO_SIZE (4U << 20);
//#define INNER_IO_SIZE (512 << 10);
#define INNER_IO_SIZE (2U << 20);

static struct readahead {
	uint64_t blockno;
	uint32_t size;
	uint8_t *ra_buf;
} ra;

static struct spdk_async_data {
	unsigned int issued;
	unsigned int inner_io_size;
	unsigned int ra_issued;
} async_data;

struct spdk_async_io {
	void* user_arg;
	void(* user_cb)(void*);
	int ios_left;
	char *buffer;
	char *guest_buffer;
	uint64_t start, len;
};

static struct spdk_nvme_qpair *read_qpair;
static uint8_t do_readahead = 0;
static struct spdk_async_io *ra_io = NULL;
static struct spdk_async_io *write_io = NULL;
static uint8_t *read_buffer;
static uint8_t *write_buffer;

/**********************************
 * 
 *      GENERAL FUNCTIONS   
 *      
 **********************************/

int spdk_process_completions(void) 
{
	struct ns_entry *ns_entry = g_namespaces;
	int r = spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
	//if not an error
	if (r > 0) {
		async_data.issued -= r;
		//printf("completed %d, oustanding: %d\n", r, async_data.issued);
	}
	return r;
}

void spdk_wait_completions(void)
{
	struct ns_entry *ns_entry = g_namespaces;

	/* Waiting all outstanding IOs */
	while(async_data.issued != 0) {
		int r = spdk_nvme_qpair_process_completions(ns_entry->qpair, 0);
		if(r > 0) {
			async_data.issued -= r;
			//printf("WAITING oustanding: %d\n", r, async_data.issued);
		}
	}
}

void spdk_async_io_exit(void) 
{
	libspdk_exit();
}

unsigned int spdk_async_get_n_lbas(void) 
{
	return libspdk_get_n_lbas();
}

int spdk_async_io_init(void)
{
	int ret;
	//TODO: c doesnt have default parameters, gotta figure this out
	printf("Intializing spdk async engine\n");
	async_data.inner_io_size = INNER_IO_SIZE;
	async_data.issued = 0;

	ret = libspdk_init();

	if (ret < 0)
		panic("cannot initialized libspdk\n");

	// allocate separate q_pair for readahead
	read_qpair = spdk_nvme_ctrlr_alloc_io_qpair(g_namespaces->ctrlr, 0);

	if (!read_qpair) 
		panic("cannot allocated qpair\n");

	read_buffer = spdk_zmalloc((2 << 20), 0x1000, NULL);
	write_buffer = spdk_zmalloc((2 << 20), 0x1000, NULL);

	write_io = mlfs_alloc(sizeof(struct spdk_async_io));

	ra_io = mlfs_alloc(sizeof(struct spdk_async_io));
	ra.ra_buf = spdk_zmalloc((4 << 20), 0x1000, NULL); 

	return 0;
}

/**********************************
 * 
 *      READ FUNCTIONS   
 *      
 **********************************/

static int spdk_readahead_completions(void)
{
	int r = spdk_nvme_qpair_process_completions(read_qpair, 0);

	if (r > 0) 
		async_data.ra_issued -= r;

	return r;
}

static void spdk_async_readahead_callback(void *arg,
		const struct spdk_nvme_cpl *completion) 
{
	struct spdk_async_io *io = arg;

	mlfs_debug("ra done %lu\n", ra.blockno);

	io->ios_left -= 1;
}

int spdk_async_readahead(unsigned long blockno, unsigned int io_size)
{
	// blockno is reserved for future
	struct ns_entry *ns_entry = g_namespaces;
	uint32_t n_blocks;
	
	if (!do_readahead)
		return 0;

	ra.blockno = blockno;
	ra.size = io_size;

	ra_io->user_arg = NULL;
	ra_io->user_cb = NULL;
	ra_io->buffer = ra.ra_buf;
	ra_io->ios_left = 1;
	ra_io->len = io_size;

	if (io_size < g_block_size_bytes)
		n_blocks = 1;
	else {
		n_blocks = io_size >> g_block_size_shift; 	
		if (io_size % g_block_size_bytes)
			n_blocks++;
	}

	if (spdk_nvme_ns_cmd_read(
				ns_entry->ns, read_qpair, ra_io->buffer,
				ra.blockno, /* LBA start */
				n_blocks, /* number of LBAs */
				spdk_async_readahead_callback, ra_io, 0) != 0) {
		fprintf(stderr, "readahead I/O failed\n");
		return -1;
	}

	async_data.ra_issued++;

	return 0;
}

static void spdk_async_io_read_callback(void *arg,
		const struct spdk_nvme_cpl *completion) 
{
	uint64_t start_tsc = 0;
	struct spdk_async_io *io = arg;

	mlfs_debug("copy to user_buffer start %d len %d\n", io->start, io->len);

	if (g_enable_perf_stats) 
		start_tsc = asm_rdtscp();
	memcpy(io->guest_buffer + io->start, io->buffer + io->start, io->len);
	if (g_enable_perf_stats) 
		g_spdk_perf_stats.memcpy_tsc += (asm_rdtscp() - start_tsc);

	io->ios_left -= 1;

	mlfs_debug("Had %d ios left, now its %d\n",  io->ios_left+1,  io->ios_left);
	if(io->ios_left == 0) {
		if(io->user_cb) {
			(*(io->user_cb))(io->user_arg);
		}
		mlfs_free(arg);
	}
}

/*
 * io size is in bytes. blockno is the actual block we want to read
 * cannot assume guest buffer is in pinned memory, so need a copy
 */
int spdk_async_io_read(uint8_t *guest_buffer, unsigned long blockno, 
		uint32_t bytes_to_read, void(* cb)(void*), void* arg)
{
	uint32_t i;
	int n_blocks;
	//how many ios we need to submit
	int n_ios = ceil((float)bytes_to_read/(float)async_data.inner_io_size);
	struct ns_entry *ns_entry = g_namespaces;
	struct spdk_async_io* read_io;
	
	/* check whether data can be served from readahead buffer */
	if (blockno >= ra.blockno &&
		(blockno + (bytes_to_read >> g_block_size_shift) <= 
		 (ra.blockno) + (ra.size >> g_block_size_shift))) {
		uint32_t ra_offset;

		ra_offset = (blockno - ra.blockno) << g_block_size_shift;

		// check whether it can get data from readahead buffer.
		if (do_readahead)
			while(!spdk_readahead_completions());

		do_readahead = 0;

		memcpy(guest_buffer, ra.ra_buf + ra_offset, bytes_to_read);

		mlfs_debug("Get from RA buffer: req=%lu-%lu, ra=%lu-%lu\n", 
				blockno, (bytes_to_read >> 12),
				ra.blockno, (ra.size >> 12));

		return bytes_to_read;
	} 

	do_readahead = 1;

	mlfs_debug("RA is not available: req=%lu-%lu, ra=%lu-%lu\n", 
			blockno, (bytes_to_read >> 12),
			ra.blockno, (ra.size >> 12));

	//if it wont fit all, dont issue any
	//TODO: possibly read as many bytes as we can and return this amount
	if(n_ios > Q_DEPTH) {
	  errno = EFBIG;
	  return -1;
	}

	if(async_data.issued + n_ios > Q_DEPTH) {
		errno = EBUSY;
		return -1;
	}

	if (bytes_to_read < g_block_size_bytes)
		n_blocks = 1;
	else {
		n_blocks = bytes_to_read >> g_block_size_shift; 	
		if (bytes_to_read % g_block_size_bytes)
			n_blocks++;
	}

	for(i = 0 ; i < bytes_to_read ; i+= async_data.inner_io_size) {
		//min (io_size, remaining bytes)
		int to_read = bytes_to_read - i < async_data.inner_io_size ? 
			bytes_to_read - i : async_data.inner_io_size;
		int inner_blocks = ceil(to_read/BLOCK_SIZE);
		unsigned long to_block = blockno+(i/BLOCK_SIZE);

		read_io = mlfs_alloc(sizeof(struct spdk_async_io));
		read_io->buffer = read_buffer;
		read_io->user_arg = arg;
		read_io->user_cb = cb;
		read_io->guest_buffer = guest_buffer;
		read_io->ios_left = n_ios;

		read_io->start = i;
		read_io->len = to_read;

		mlfs_debug("Issuing read of %d to block %d\n", to_block, inner_blocks);
		if(spdk_nvme_ns_cmd_read(
					ns_entry->ns, 
					ns_entry->qpair, 
					read_io->buffer + read_io->start,
					to_block, /* LBA start */
					inner_blocks, /* number of LBAs */
					spdk_async_io_read_callback, read_io, 0) != 0) {
			fprintf(stderr, "starting write I/O failed\n");
			return -1;
		}
		//could add all at once, but this will prevent infinite waiting in case
		//one write fails
		async_data.issued++;
	}

	return bytes_to_read;
}

/**********************************
 * 
 *      WRITE FUNCTIONS   
 *      
 **********************************/
static void spdk_async_io_write_callback(void *arg,
		const struct spdk_nvme_cpl *completion) 
{
	struct spdk_async_io *io = arg;
	//decrement how many ios are left
	io->ios_left -= 1;

	mlfs_debug("Had %d ios left, now its %d\n", io->ios_left+1, io->ios_left);
	//if we were the last one, finish and cleanup
	if (io->ios_left == 0) {
		if(io->user_cb) {
			(*(io->user_cb))(io->user_arg);
		}
		//spdk_free(io->buffer);
		//mlfs_free(arg);
	}
	mlfs_debug("Write callback done\n");
}

int spdk_async_io_write(uint8_t *guest_buffer, unsigned long blockno, 
		uint32_t bytes_to_write, void(* cb)(void*), void* arg) 
{
	//struct spdk_async_io* io;
	uint64_t start_tsc = 0;
	int n_blocks;
	struct ns_entry *ns_entry = g_namespaces;
	//how many ios we need to submit
	int n_ios = ceil((float)bytes_to_write/(float)async_data.inner_io_size);
	//printf("issuing %d ios\n", n_ios);

	//if it wont fit all, dont issue any
	//TODO: possibly write as many bytes as we can and return this amount
	if(n_ios > Q_DEPTH) {
		errno = EFBIG;
		return -1;
	}

	if(async_data.issued + n_ios > Q_DEPTH) {
		errno = EBUSY;
		return -1;
	}

	//n_blocks = ceil(bytes_to_write/(double)BLOCK_SIZE);
	if (bytes_to_write < g_block_size_bytes)
		n_blocks = 1;
	else {
		n_blocks = bytes_to_write >> g_block_size_shift; 	
		if (bytes_to_write % g_block_size_bytes)
			n_blocks++;
	}

	//io = mlfs_alloc(sizeof(struct spdk_async_io));
	write_io->user_arg = arg;
	write_io->user_cb = cb;
	write_io->buffer = write_buffer;
	mlfs_debug("%d / %d    = ios left %d\n", 
			bytes_to_write, async_data.inner_io_size, n_ios);
	write_io->ios_left = n_ios;

	mlfs_assert(write_io->buffer);

	//this memcpy segfaults if we have too long of a queue
	//and large io size because we couldnt alloc that many bytes
	if (g_enable_perf_stats) 
		start_tsc = asm_rdtscp();
	memcpy(write_io->buffer, guest_buffer, bytes_to_write);
	if (g_enable_perf_stats) 
		g_spdk_perf_stats.memcpy_tsc += (asm_rdtscp() - start_tsc);

	for(unsigned int i = 0 ; i < bytes_to_write ; i+= async_data.inner_io_size) {
		//min (io_size, remaining bytes)
		int to_write = bytes_to_write - i < async_data.inner_io_size ? 
			bytes_to_write - i : async_data.inner_io_size;
		int inner_blocks = ceil(to_write/BLOCK_SIZE);
		int to_block = blockno+(i/BLOCK_SIZE);

		mlfs_debug("Issuing write of %d to block %d\n", to_block, inner_blocks);
		if(spdk_nvme_ns_cmd_write(
					ns_entry->ns, ns_entry->qpair, 
					write_io->buffer + i,
					to_block, /* LBA start */
					inner_blocks, /* number of LBAs */
					spdk_async_io_write_callback, write_io, 0) != 0) {
			fprintf(stderr, "starting write I/O failed\n");
			return -1;
		}
		//could add all at once, but this will prevent infinite waiting in case
		//one write fails
		async_data.issued++;
	}

	return bytes_to_write;
}

static void spdk_sync_io_trim_callback(void *arg, 
		const struct spdk_nvme_cpl *completion)
{
}

int spdk_io_trim(unsigned long blockno, unsigned int n_bytes)
{
	int ret;
	struct spdk_nvme_dsm_range ranges[256];
	struct ns_entry *ns_entry = g_namespaces;

	uint32_t blocks_left = ceil(n_bytes/(double)BLOCK_SIZE);
	uint32_t max_range = 1 << 16;
	uint32_t start_block = blockno;
	uint32_t count = 0;

	while (blocks_left > 0) {
		int blocks;
		if(blocks_left >= max_range) {
			blocks = max_range;
		}
		else {
			blocks = blocks_left;
		}
		blocks_left -= blocks;

		ranges[count].starting_lba = start_block;
		ranges[count].length = blocks;
		ranges[count].attributes.raw = 0;
		count++;

		start_block += blocks;
	}

	//printf("Issuing %d ranges trim\n", count);
	ret = spdk_nvme_ns_cmd_dataset_management(ns_entry->ns, 
			ns_entry->qpair,
			SPDK_NVME_DSM_ATTR_DEALLOCATE, 
			ranges, 
			count, 
			spdk_sync_io_trim_callback, NULL);

	//wait until its done
	while(!spdk_nvme_qpair_process_completions(ns_entry->qpair, 0));

	return 0;
}
