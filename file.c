#include <linux/buffer_head.h>
#include <linux/fs.h>

#include "fs.h"
#include "fsinfo.h"

static const struct file_operations xv6_file_ops = {
    .owner = THIS_MODULE,
    .read = xv6_file_read,
    .write = xv6_file_write,
    .llseek = xv6_lseek,
    .read_iter = xv6_file_read_iter,
    .write_iter = generic_file_write_iter,
    .iterate_shared = NULL,
    .fsync = xv6_file_sync,
    .flush = xv6_file_flush,
};

static const struct file_operations xv6_directory_ops = {
    .owner = THIS_MODULE,
    .llseek = xv6_lseek,
    .read = generic_read_dir,
    .read_iter = xv6_file_read_iter,
    .iterate_shared = xv6_readdir,
    .fsync = xv6_file_sync,
    .flush = xv6_file_flush,
};

__attribute__((unused))
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
    if (data_block == 0) {
        goto file_end;
    }
    bh = sb_bread(sb, data_block);
    *bhptr = bh;
    return bh ? 0 : -EIO;
file_end:
    *bhptr = 0;
    return 0;
}

static ssize_t xv6_file_read(struct file *file, char __user *buf,
            size_t len, loff_t *ppos) {
    struct inode *ino = file->f_inode;
    ssize_t nread = 0;
    loff_t cpos = *ppos;
    uint block = cpos / BSIZE;
    uint boff = cpos % BSIZE;
    int error = 0;

    /* Like ext4_file_read_iter, only lock(read) inode. */
    xv6_ilock_shared(ino);
    size_t file_size = ino->i_size;
    loff_t rest = file_size - cpos;
    if (len > rest) {
        len = rest;
    }

    struct buffer_head *bh = NULL;
    while (len) {
        error = xv6_inode_block(ino, block, &bh);
        if (error) {
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
            error = -EFAULT;
            break;
        }
        buf += to_read;
        len -= to_read;
        nread += to_read;
        cpos += to_read;
        boff = 0;
        block += 1;
    }

    xv6_iunlock_shared(ino);
    if (error && nread <= 0) {
        nread = error;
    }
    *ppos = cpos;
    return nread;
}

static ssize_t xv6_file_write(struct file *file, const char __user *buf,
            size_t len, loff_t *ppos) {
    struct inode *ino = file->f_inode;
    ssize_t nwrite = 0;
    loff_t cpos = *ppos;
    if (file->f_flags & O_APPEND) {
        /*
         * When the file is opened with O_APPEND, write(2) will set
         * file offset to the end.
         * See man 2 open.
         */
        cpos = ino->i_size;
    }
    uint block = cpos / BSIZE;
    uint boff = cpos % BSIZE;
    struct buffer_head *bh = NULL;
    int error = 0;

    /* Like ext4_file_write_iter, only lock(write) inode. */
    xv6_ilock_exclusive(ino);
    while (len) {
        error = xv6_inode_wblock(ino, block, &bh);
        if (error) {
            break;
        }
        size_t to_write = BSIZE - boff;
        to_write = xv6_min(to_write, len);
        bool page_fault = copy_from_user(bh->b_data + boff, buf, to_write);
        if (page_fault) {
            /* wrote something before */
            error = -EFAULT;
            mark_buffer_dirty(bh);
            (void) sync_dirty_buffer(bh);
            brelse(bh);
            break;
        }
        mark_buffer_dirty(bh);
        error = sync_dirty_buffer(bh);
        brelse(bh); 
        if (error) { break; }
        bh = NULL;
        buf += to_write;
        len -= to_write;
        nwrite += to_write;
        cpos += to_write;
        boff = 0;
        block += 1;
    }

    ino->i_size = xv6_max(ino->i_size, cpos);
    xv6_iunlock_exclusive(ino);
    *ppos = cpos;
    if (error && nwrite <= 0) {
        nwrite = error;
    }
    return nwrite;
}

static int xv6_unlink(struct inode *dir, struct dentry *entry) {
    struct super_block *sb = dir->i_sb;
    struct inode *file_ino = entry->d_inode;
    uint file_inum = file_ino->i_ino;
    inode_dec_link_count(file_ino);
    uint nlink = file_ino->i_nlink;
    if (nlink) {
        return xv6_dir_erase(dir, entry->d_name.name);
    }
    xv6_assert(file_inum);
    if (file_inum == ROOTINO) {
        /* Cannot remove root. */
        return -EBUSY;
    }
    if (file_inum == dir->i_ino) {
        /* Trying to unlink . */
        return -EINVAL;
    }
    
    /* Firstly, clear the inode. */
    int error = xv6_inode_clear(file_ino);
    if (error) {
        return error;
    }

    /* Next, find it in the directory and remove the entry. */
    error = xv6_dir_erase(dir, entry->d_name.name);

    /* Last, safely free the inode in the itable. */
    error = xv6_ifree(sb, file_inum);
    return error;
}

static int xv6_link(struct dentry *oldentry, struct inode *dir, 
            struct dentry *entry) {
    
    struct inode *oldino = oldentry->d_inode;
    uint oldinum = oldino->i_ino;
    int error ; 
    const char *name = entry->d_name.name;
    uint dnum;
    if (strlen(name) > DIRSIZ) {
        return -ENAMETOOLONG;
    }
    if (unlikely(oldino->i_nlink >= __INT16_MAX__)) {
        xv6_error("hard links reach limit(%u)", __INT16_MAX__);
        return -ETOOMANYREFS;
    }

    /* allocate directory entry. */
    error = xv6_dentry_alloc(dir, name, &dnum);
    if (error) {
        return error;
    }

    /* Write directory entry. */
    error = xv6_dentry_write(dir, dnum, name, oldinum);
    if (error) {
        return error;
    }

    /* Increment link count and sync. */
    inode_inc_link_count(oldino);
    error = xv6_sync_inode(oldino);
    
    /* Finish. */
    d_instantiate(entry, oldino);
    return error;
}
