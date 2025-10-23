#include <linux/buffer_head.h>
#include <linux/fs.h>

#include "fs.h"
#include "fsinfo.h"

static const struct file_operations xv6_file_ops = {
    .owner = THIS_MODULE,
};

static int xv6_file_block(struct super_block *sb, const struct dinode *file,
            uint i, struct buffer_head **bhptr) {
    *bhptr = NULL;
    if (i >= MAXFILE) {
        goto file_end;
    }
    struct buffer_head *bh;
    struct buffer_head *indirect = NULL;
    if (i < NDIRECT) {
        uint block = __le32_to_cpu(file->addrs[i]); 
        if (block == 0) { goto file_end; }
        bh = sb_bread(sb, block);
        *bhptr = bh;
        return bh ? 0 : -EIO;
        /* FIXME: check block later */
    }

    i -= NDIRECT;   
    uint indirect_block = __le32_to_cpu(file->addrs[NDIRECT]);
    if (indirect_block == 0) { goto file_end; }

    indirect = sb_bread(sb, indirect_block);
    if (indirect == NULL) { 
        return -EIO;
    }
    const uint *addrs = (const uint *) indirect->b_data;
    uint data_block = __le32_to_cpu(addrs[i]);
    brelse(indirect);
    bh = sb_bread(sb, data_block);
    *bhptr = bh;
    return bh ? 0 : -EIO;
file_end:
    return 0;
}

