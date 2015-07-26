#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64

#include <stdio.h>
#include <stdint.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "clog.h"
#include "dir_scan.h"
#include "dj_internal.h"
#include "heap.h"
#include "listsort.h"
#include "util.h"

// inode indexes are unsigned ints, so don't subtract them; semicolons seem to
// help Sublime Text's syntax parser
SORT_FUNC(inode_list_sort, struct inode_list, (p->index < q->index ? -1 : 1));
SORT_FUNC(block_list_sort, struct block_list,
          (p->physical_block < q->physical_block ? -1 : 1));

/*
 * Callback (indirectly) invoked by libext2fs for each block of a file.
 * - Increments the reference count of the block's inode.
 * - Sets the block's metadata (such as logical and physical numbers and length)
 *   in a block_list struct.
 * - Inserts the block_list struct into the inode's linked list of its blocks.
 * - Recursively calls itself to add blocks for holes.
 */
void scan_block(uint64_t block_size, blk64_t physical_block,
                e2_blkcnt_t logical_block, struct scan_blocks_info *scan_info)
{
    // Ignore the extra "empty" block at the end of a file, to allow appending
    // for writers, when we pass in BLOCK_FLAG_HOLE, unless the file is empty
    if (logical_block * block_size >= scan_info->inode_info->len
        || scan_info->inode_info->len == 0)
    {
        return;
    }

    struct inode_list *inode_list = scan_info->inode_list;
    struct block_list *blocks_end = inode_list->blocks_end;
    struct block_list *list;

    if (blocks_end != NULL && (blocks_end->physical_block + blocks_end->num_blocks) == physical_block)
    {
        list = blocks_end;
    }
    else
    {
        list = ecalloc(sizeof(struct block_list));
        list->inode_info = scan_info->inode_info;
        list->inode_info->references++;
        list->physical_block = physical_block;
        list->logical_block = logical_block;

        // FIXME what happens when holes are at the end of the file?
        for (e2_blkcnt_t i = list->inode_info->blocks_scanned;
             i < logical_block; i++)
        {
            // sparse files' hole blocks should be passed to this function, since we
            // passed BLOCK_FLAG_HOLE to the iterator function, but that doesn't
            // seem to be happening - so fix it
            scan_block(block_size, 0, i, scan_info);
        }

        if (inode_list->blocks_start == NULL)
        {
            inode_list->blocks_start = list;
            inode_list->blocks_end = list;
        }
        else
        {
            inode_list->blocks_end->next = list;
            inode_list->blocks_end = list;
        }
    }

    scan_info->inode_info->blocks_scanned++;
    list->num_blocks++;

    uint64_t logical_pos = list->logical_block * block_size;
    uint64_t remaining_len = list->inode_info->len - logical_pos;
    uint64_t simple_len = list->num_blocks * block_size;
    list->stripe_ptr.len = simple_len > remaining_len ? remaining_len : simple_len;

    LogTrace("Physical block %lu (%lu) is logical block %lu (%lu) of size %lu for inode %d", list->physical_block + list->num_blocks - 1, list->physical_block, list->logical_block + list->num_blocks - 1, list->logical_block, list->stripe_ptr.len, scan_info->inode_info->inode);
}

/*
 * Wraps the actual callback with a function whose signature libext2fs expects,
 * which makes the actual function easier to test.
 */
int scan_block_cb(ext2_filsys fs, blk64_t *blocknr, e2_blkcnt_t blockcnt,
                  blk64_t ref_blk, int ref_offset, void *private)
{
    scan_block(fs->blocksize, *blocknr, blockcnt, private);
    return 0;
}

void scan_blocks(ext2_filsys fs, block_cb cb, struct inode_list *inode_list)
{
    char block_buf[fs->blocksize * 3];
    struct scan_blocks_info scan_info = { cb, NULL, NULL };

    /*
     * For each inode, add the metadata for each of that inode's blocks to the
     * inode's block list.
     */
    struct inode_list *scan_inode_list = inode_list;
    while (scan_inode_list != NULL)
    {
        struct inode_cb_info *info = ecalloc(sizeof(struct inode_cb_info));
        info->inode = scan_inode_list->index;
        info->path = malloc(strlen(scan_inode_list->path)+1);
        strcpy(info->path, scan_inode_list->path);
        info->len = scan_inode_list->len;

        LogDebug("Scanning blocks of inode %d: %s", info->inode, info->path);

        // there's some duplication of information (path and len) between
        // scan_info.inode_info and .inode_list, but that's ok
        scan_info.inode_info = info;
        scan_info.inode_list = scan_inode_list;

        int iter_flags = BLOCK_FLAG_HOLE | BLOCK_FLAG_DATA_ONLY
                         | BLOCK_FLAG_READ_ONLY;
        CHECK_FATAL(ext2fs_block_iterate3(fs, info->inode, iter_flags,
                                          block_buf, scan_block_cb, &scan_info),
                "while iterating over blocks of inode %d", info->inode);

        if (info->references == 0)
        {
            // empty files generate no blocks, so we'd get into an infinite loop
            // below
            scan_info.cb(info->inode, info->path, 0, 0, NULL, 0,
                         &info->cb_private);
            free(info->path);
            free(info);
        }

        scan_inode_list = scan_inode_list->next;
    }
}

void flush_inode_blocks(uint64_t block_size, struct inode_cb_info *inode_info,
                        block_cb cb, int *open_inodes_count)
{
    while (heap_size(inode_info->block_cache) > 0)
    {
        struct block_list *next_block = heap_min(inode_info->block_cache);
        if (next_block->logical_block == inode_info->blocks_read)
            heap_delmin(inode_info->block_cache);
        else
            break;

        if (inode_info->references <= 0)
        {
            exit_str("inode %d has %d references\n", inode_info->path,
                     inode_info->references);
        }
        
        uint64_t logical_pos = next_block->logical_block * block_size;
        char *block_data =
            next_block->stripe_ptr.stripe->data + next_block->stripe_ptr.pos;
        cb(inode_info->inode, inode_info->path, logical_pos, inode_info->len,
           block_data, next_block->stripe_ptr.len, &inode_info->cb_private);

        inode_info->blocks_read += next_block->num_blocks;

        if (--next_block->stripe_ptr.stripe->references == 0)
        {
            free(next_block->stripe_ptr.stripe->data);
            free(next_block->stripe_ptr.stripe);
        }

        free(next_block);

        if (--inode_info->references == 0)
        {
            if (inode_info->block_cache != NULL)
                heap_destroy(inode_info->block_cache);
            free(inode_info->path);
            free(inode_info);
            (*open_inodes_count)--;
            break;
        }
    }
}

struct inode_list *get_inode_list(ext2_filsys fs, char *target_path)
{
    // look up the file whose blocks we want to read, or the directory whose
    // constituent files (and their block) we want to read
    ext2_ino_t ino;
    CHECK_FATAL(ext2fs_namei_follow(fs, EXT2_ROOT_INO, EXT2_ROOT_INO,
                                    target_path, &ino),
            "while looking up path %s", target_path);

    // get that inode
    struct dir_entry_cb_data cb_data = { fs, NULL, NULL, NULL };
    struct ext2_inode inode_contents;
    CHECK_FATAL(ext2fs_read_inode(fs, ino, &inode_contents),
            "while reading inode contents");

    if (LINUX_S_ISDIR(inode_contents.i_mode))
    {
        // if it's a directory, recursively iterate through its contents
        LogInfo("Getting inodes of start directory %s", target_path);
        struct dir_tree_entry dir = { target_path, NULL };
        cb_data.dir = &dir;
        CHECK_FATAL(ext2fs_dir_iterate2(fs, ino, 0, NULL, dir_entry_cb,
                                        &cb_data),
                "while iterating over directory %s", target_path);
    }
    else if (!S_ISLNK(inode_contents.i_mode))
    {
        // if it's a regular file, just add it
        int dir_path_len = strrchr(target_path, '/') - target_path;
        char dir_path[dir_path_len+1];
        memcpy(dir_path, target_path, dir_path_len);;
        dir_path[dir_path_len] = '\0';

        struct dir_tree_entry dir = { dir_path, NULL };
        cb_data.dir = &dir;
        dir_entry_add_file(ino, strrchr(target_path, '/')+1, &cb_data,
                           inode_contents.i_size);

        LogDebug("Added start file %s", target_path);
    }
    else
        exit_str("Unexpected file mode %x", inode_contents.i_mode);

    return cb_data.list_start;
}

/*
 * Read ahead of the current block (block_list) to determine the longest stripe
 * we can read all in one go, that satisfies the following conditions.
 *   1) The number of cached blocks in the heap of the inode of any block in the
 *      stripe is not greater than max_inode_blocks.
 *   2) The physical distance between any two blocks in the stripe that we care
 *      about (i.e., the ones that will be passed to the callback) is not
 *      greater than coalesce_distance.
 */
struct stripe *next_stripe(uint64_t block_size, int coalesce_distance,
                           int max_inode_blocks, struct block_list *block_list)
{
    struct stripe *stripe = ecalloc(sizeof(struct stripe));

    // we use this pointer to read ahead of the current block without losing our
    // place in the overall iteration
    struct block_list *fwd_block_list = block_list;

    // and this awfully-named number tracks the previous value of
    // fwd_block_list, in order to track how far we're jumping over blocks we
    // don't care about, so as not to exceed coalesce_distance
    struct block_list *prev_fwd_block = NULL;

    while (fwd_block_list != NULL)
    {
        // check condition (1)
        /*e2_blkcnt_t max_logical_block =
            fwd_block_list->inode_info->blocks_read + max_inode_blocks - 1;
        if (fwd_block_list->logical_block > max_logical_block)
            break;*/

        // check condition (2)
        e2_blkcnt_t physical_block_diff = prev_fwd_block == NULL
            ? 0
            : fwd_block_list->physical_block - (prev_fwd_block->physical_block + prev_fwd_block->num_blocks);
        if (physical_block_diff > coalesce_distance)
            break;

        stripe->consecutive_blocks += fwd_block_list->num_blocks;

        fwd_block_list->stripe_ptr.stripe = stripe;
        stripe->references++;

        // set the block's start point relative to the stripe
        blk64_t physical_block_offset =
            fwd_block_list->physical_block - block_list->physical_block;
        fwd_block_list->stripe_ptr.pos = physical_block_offset * block_size;

        stripe->consecutive_len += fwd_block_list->num_blocks * block_size; // actual block length
        stripe->consecutive_len += physical_block_diff * block_size; // gap between blocks

        prev_fwd_block = fwd_block_list;
        fwd_block_list = fwd_block_list->next;
    }

    return stripe;
}

/*
 * Read data from device into stripe.
 */
void read_stripe_data(off_t block_size, blk64_t physical_block, int direct,
                      int fd, struct stripe *stripe)
{
    if (physical_block != 0)
    {
        // If opened with O_DIRECT, the device needs to be read in multiples of
        // 512 bytes into a buffer that is 512-byte aligned. Only the latter is
        // documented; the former is documented as being undocumented.
        size_t physical_read_len = direct
            ? ((stripe->consecutive_len+511)/512)*512
            : stripe->consecutive_len;

        if (posix_memalign((void **)&stripe->data, 512, physical_read_len))
            perror("Error allocating aligned memory");

        ssize_t read_len = pread(fd, stripe->data, physical_read_len,
                                 physical_block * block_size);
        if (read_len < stripe->consecutive_len)
            perror("Error reading from block device");
    }
    else
        stripe->data = ecalloc(stripe->consecutive_len);
}

/*
 * For each block in the stripe, insert the block into its inode's heap. Then
 * flush that heap out to the client, if possible.
 */
struct block_list *heapify_stripe(ext2_filsys fs, block_cb cb,
                                  struct block_list *block_list,
                                  struct stripe *stripe, int max_inode_blocks,
                                  int *open_inodes_count)
{
    e2_blkcnt_t consecutive_blocks = stripe->consecutive_blocks; // stripe can be freed during iteration, so save the number of blocks here
    for (e2_blkcnt_t read_blocks = 0; read_blocks < consecutive_blocks;)
    {
        struct inode_cb_info *inode_info = block_list->inode_info;
        if (inode_info->block_cache == NULL)
            inode_info->block_cache = heap_create(inode_info->len/fs->blocksize+1 /*max_inode_blocks*/); // +1 so that it's never 0

        LogTrace("Heapifying physical block %lu, logical block %lu (num blocks %lu) of inode %d", block_list->physical_block, block_list->logical_block, block_list->num_blocks, inode_info->inode);
        heap_insert(inode_info->block_cache, block_list->logical_block,
                    block_list);

        read_blocks += block_list->num_blocks;

        // block_list could be freed if it's the heap minimum, so iterate to the
        // next block before flushing cached blocks
        block_list = block_list->next;
        flush_inode_blocks(fs->blocksize, inode_info, cb, open_inodes_count);
    }

    return block_list;
}

void iterate_dir(char *dev_path, char *target_path, block_cb cb, int max_inodes,
                 int max_blocks, int coalesce_distance, int flags, int advice_flags)
{
    // open file system from block device
    ext2_filsys fs;
    CHECK_FATAL(ext2fs_open(dev_path, 0, 0, 0, unix_io_manager, &fs),
            "while opening file system on device %s", dev_path);

    // open the block device in order to read data from it later
    int fd;
    int open_flags = O_RDONLY;
    if (flags & ITERATE_OPT_DIRECT)
        open_flags |= O_DIRECT;
    if ((fd = open(dev_path, open_flags)) < 0)
        exit_str("Error opening block device %s", dev_path);

    CHECK_WARN(posix_fadvise(fd, 0, 0, advice_flags), "setting advice flags 0x%x", advice_flags);

    LogInfo("BEGIN INODE SCAN");

    struct inode_list *inode_list = get_inode_list(fs, target_path);

    /*
     * We now have a linked list of file paths to be scanned in
     * cb_data.list_start.
     */

    LogInfo("END INODE SCAN");

    inode_list = inode_list_sort(inode_list);

    LogInfo("BEGIN BLOCK SCAN");

    scan_blocks(fs, cb, inode_list);

    LogInfo("END BLOCK SCAN");

    int open_inodes_count = 0;
    uint64_t seeks = 0;
    uint64_t total_blocks = 0;

    struct block_list *block_list_start = NULL;
    struct block_list *block_list_end = NULL;

    while (inode_list != NULL)
    {
        /*
         * While there are inodes remaining and we're below the limit on open
         * inodes, add those inodes' blocks to the global list.
         */
        while (inode_list != NULL && open_inodes_count < max_inodes)
        {
            LogDebug("Adding blocks of inode %s (%llu bytes) to block read list", inode_list->path, inode_list->len);

            if (inode_list->blocks_start != NULL)
            {
                if (block_list_start == NULL)
                {
                    block_list_start = inode_list->blocks_start;
                    block_list_end = inode_list->blocks_end;
                }
                else
                {
                    block_list_end->next = inode_list->blocks_start;
                    block_list_end = inode_list->blocks_end;
                }
                open_inodes_count++;
            }

            struct inode_list *old = inode_list;
            inode_list = inode_list->next;

            free(old->path);
            free(old);
        }

        // sort the blocks into the order in which they're laid out on disk
        block_list_start = block_list_sort(block_list_start);

        struct block_list *block_list = block_list_start;
        block_list_start = NULL;
        struct block_list **prev_next_ptr = &block_list_start;

        int max_inode_blocks = open_inodes_count > 0
            ? (max_blocks+open_inodes_count-1)/open_inodes_count : max_blocks;

        LogInfo("BEGIN BLOCK READ");

        while (block_list != NULL)
        {
            total_blocks++;

            struct stripe *stripe = next_stripe(fs->blocksize,
                                                coalesce_distance,
                                                max_inode_blocks, block_list);

            LogDebug("Found stripe of %lu blocks", stripe->consecutive_blocks);
            if (stripe->consecutive_blocks > 0)
            {
                read_stripe_data(fs->blocksize, block_list->physical_block,
                                 flags & ITERATE_OPT_DIRECT, fd, stripe);
            }
            else
                free(stripe);

            // stripe can be freed in heapify_stripe, so save consecutive_len here
            size_t consecutive_len = stripe->consecutive_len;

            block_list = heapify_stripe(fs, cb, block_list, stripe,
                                        max_inode_blocks, &open_inodes_count);

            // block is out of range
            if (consecutive_len == 0 && block_list != NULL)
            {
                struct block_list *old_next = block_list->next;
                *(prev_next_ptr) = block_list;
                block_list_end = block_list;
                prev_next_ptr = &block_list->next;
                block_list->next = NULL;

                block_list = old_next;
            }
        }

        LogInfo("END BLOCK READ");
    }

    double seeks_percentage = total_blocks == 0 ? 0. : ((double)seeks)/total_blocks * 100.;

    if (close(fd) != 0)
        exit_str("Error closing block device");

    if (ext2fs_close(fs) != 0)
        exit_str("Error closing file system");
}

void initialize_dj(char *error_prog_name)
{
    prog_name = error_prog_name;
    initialize_ext2_error_table();
    clog_init();
}
