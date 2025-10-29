#include <linux/buffer_head.h>
#include "fs.h"
#include "fsinfo.h"
#include "xv6.h"

static int xv6_balloc_rng(struct super_block *sb, uint *block, uint start, 
             uint end);

static inline int xv6_bzero(struct super_block *sb, uint block) {
    if (sb->s_flags & SB_RDONLY) {
        return -EROFS;
    }
    
    struct buffer_head *bh = sb_bread(sb, block);
    if (!bh) {
        xv6_error("unable to read block %u for zeroing", block);
        return -EIO;
    }
    memset(bh->b_data, 0, BSIZE);
    mark_buffer_dirty(bh);
    int error = sync_dirty_buffer(bh);
    brelse(bh);

    return error;
}

static int xv6_bfree_unsafe(struct super_block *sb, uint block) {
    if (sb->s_flags & SB_RDONLY) {
        return -EROFS;
    }
    struct xv6_fs_info *fsinfo = sb->s_fs_info;
    xv6_assert(block < fsinfo->size && "attempting out-of-bound access");
    xv6_assert(block >= fsinfo->bmapstart + BITMAP_BLOCKS(fsinfo->size) 
        && "attempting freeing metadata blocks");

    xv6_assert(BSIZE % sizeof(unsigned int) == 0);
    const uint bits_per_elem = sizeof(unsigned int) * 8;
    unsigned int mask = (block % bits_per_elem);
    mask = (1u << mask);
    unsigned int index = (block % BPB) / bits_per_elem;
    unsigned int bitmap_block = block / BPB;
    bitmap_block += fsinfo->bmapstart;

    struct buffer_head *bh = sb_bread(sb, bitmap_block);
    if (!bh) {
        return -EIO;
    }
    unsigned int *bitmap = (unsigned int *) bh->b_data;
    if (!(bitmap[index] & mask)) {
        xv6_warn("double free detected on block %u", block);
        brelse(bh);
        return 0;
    }

    bitmap[index] &= ~mask;
    mark_buffer_dirty(bh);
    int error = sync_dirty_buffer(bh);
    brelse(bh);
    return error;
}

static int xv6_balloc_unsafe(struct super_block *sb, uint *block) {
    if (sb->s_flags & SB_RDONLY) {
        return -EROFS;
    }
    *block = 0;
    struct xv6_fs_info *fsinfo = sb->s_fs_info;
    const uint data_start = fsinfo->bmapstart + BITMAP_BLOCKS(fsinfo->size);
    const uint data_end = fsinfo->size;
    uint hint = fsinfo->balloc_hint;
    
    int error = 0;
    /* Try alloc in range [hint, data_end) */
    if (hint < data_end) {
        error = xv6_balloc_rng(sb, block, hint, data_end);
        if (*block || error) {
            hint = fsinfo->balloc_hint;
            fsinfo->balloc_hint = (hint >= data_end) ? (data_start) : hint;
            return error;
        }
    }
    /* Try alloc in range [data_start, hint) */
    if (data_start < hint) {
        error = xv6_balloc_rng(sb, block, data_start, hint);
        if (*block || error) {
            hint = fsinfo->balloc_hint;
            fsinfo->balloc_hint = (hint >= data_end) ? (data_start) : hint;
            return error;
        }
    }

    /* Possibly disk full. */
    return 0;
}

static int xv6_balloc_rng(struct super_block *sb, uint *block, uint start, 
              uint end) {
    struct xv6_fs_info *fsinfo = sb->s_fs_info;
    const uint bits_per_elem = sizeof(unsigned int) * 8;
    uint alloc = start;
    struct buffer_head *bh = NULL;
    
    uint bitblock, index, mask;
    int error = 0;
    /* Try alloc in range [start, end] */
try_balloc:
    bitblock = alloc / BPB + fsinfo->bmapstart;
    bh = sb_bread(sb, bitblock);

    if (!bh) { 
        error = -EIO;
        fsinfo->balloc_hint = alloc;
        goto end_balloc;
    }
    unsigned int *bitarray = (unsigned int *) bh->b_data;
    uint iter_end = (alloc + BPB - 1) / BPB * BPB;
    iter_end = xv6_min(iter_end, end);
    bool succ = false;

    while (alloc < iter_end) {
        index = (alloc % BPB) / bits_per_elem;
        mask = (alloc % bits_per_elem);
        mask = (1u << mask);
    
        if ((mask & bitarray[index]) == 0) {
            if ((error = xv6_bzero(sb, alloc)) != 0) {
                /* Do not try to allocate. */
                fsinfo->balloc_hint = alloc;
            } else {
                bitarray[index] |= mask;
                mark_buffer_dirty(bh);
                error = sync_dirty_buffer(bh);
                *block = alloc;
                fsinfo->balloc_hint = alloc + 1;
            }
            succ = true;
            break;
        }             
    
        alloc += 1;
    }

    brelse(bh); bh = NULL;
    if (!succ && alloc < end) {
        bitblock++;
        goto try_balloc;
    }
end_balloc:
    return error;
}


static int xv6_balloc(void *privat, uint *block) {
    struct super_block *sb = privat;
    mutex_lock(xv6_balloc_lock(sb));
    int error = xv6_balloc_unsafe(sb, block);
    mutex_unlock(xv6_balloc_lock(sb));
    return error;
}
static int xv6_bfree(struct super_block *sb, uint block) {
    mutex_lock(xv6_balloc_lock(sb));
    int error = xv6_bfree_unsafe(sb, block);
    mutex_unlock(xv6_balloc_lock(sb));
    return error;
}
