#include <linux/buffer_head.h>
#include <linux/fs.h>

#include "fs.h"
#include "fsinfo.h"

static const struct file_operations xv6_file_ops = {
    .owner = THIS_MODULE,
    .read = xv6_file_read,
    .llseek = xv6_lseek,
    .read_iter = xv6_file_read_iter,
    .iterate_shared = NULL,
};

static const struct file_operations xv6_directory_ops = {
    .owner = THIS_MODULE,
    .llseek = xv6_lseek,
    .read_iter = xv6_file_read_iter,
    .iterate_shared = xv6_readdir,
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

static ssize_t xv6_file_read(struct file *file, char __user *buf,
            size_t len, loff_t *ppos) {
    struct inode *ino = file->f_inode;
    struct dinode dino;
    struct super_block *sb = ino->i_sb;
    ssize_t nread = 0;
    loff_t cpos = *ppos;
    uint block = cpos / BSIZE;
    uint boff = cpos % BSIZE;

    xv6_lock(sb);
    size_t file_size = ino->i_size;
    loff_t rest = file_size - cpos;
    if (len > rest) {
        len = rest;
    }

    if ((nread = xv6_dget(ino, &dino)) < 0) {
        goto read_fini;
    }

    struct buffer_head *bh = NULL;
    while (len) {
        int error = xv6_file_block(sb, &dino, block, &bh);
        if (error) {
            nread = error;
            break;
        }
        size_t to_read = BSIZE - boff;
        if (to_read > len) {
            to_read = len;
        }
        if (to_read > file_size - cpos) {
            to_read = file_size - cpos;
        }
        bool page_fault = false;
        if (bh == NULL) {
            /* 
             * This is a virtual data block filled with 0. 
             * Set user memory [buf, buf + to_read) to 0.
             */
            page_fault = clear_user(buf, to_read);
        } else {
            page_fault = copy_to_user(buf, bh->b_data + boff, to_read);
            brelse(bh);
            bh = NULL;
        }
        if (page_fault) {
            nread = -EFAULT;
            break;
        }
        buf += to_read;
        len -= to_read;
        nread += to_read;
        cpos += to_read;
        boff = 0;
        block += 1;
    }

read_fini:
    xv6_unlock(sb);
    *ppos = cpos;
    return nread;
}
