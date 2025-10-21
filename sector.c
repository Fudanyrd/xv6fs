#include <linux/blkdev.h>
#include <linux/buffer_head.h>
#include "sector.h"

int read_xv6_block(xv6_block_t buf, struct super_block *sb, uint sect) {
    struct buffer_head *bh = sb_bread(sb, sect);
    if (bh == NULL /*|| !buffer_uptodate(bh) */) {
        return -EIO;
    }
  
    xv6_assert(sb->s_blocksize == BSIZE);
    memcpy(buf, bh->b_data, BSIZE);
    brelse(bh);
    return 0;
}

int write_xv6_block(xv6_cst_block_t buf, struct super_block *sb , uint sect) {
    /* Not implemented. */
    return -EIO;
}
