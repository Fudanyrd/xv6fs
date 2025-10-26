#ifndef _FSINFO_H
#define _FSINFO_H 1

#include <linux/fs.h>
#include <linux/mutex.h>
#include <linux/vfs.h>

/*
 * Xv6 filesystem info struct are defined here.
 */

/*
 * This struct is passed to super_block::s_fs_info
 */
struct xv6_fs_info;

/*
 * This struct is passed to fs_context::fs_private
 */
struct xv6_mount_options;

struct xv6_mount_options {
    kuid_t uid;
    kgid_t gid;
};

struct xv6_fs_info {
    struct mutex build_inode_lock;
    struct mutex balloc_lock;
    uint size;         // Size of file system image (blocks)
    uint nblocks;      // Number of data blocks
    uint ninodes;      // Number of inodes.
    uint nlog;         // Number of log blocks
    uint logstart;     // Block number of first log block
    uint inodestart;   // Block number of first inode block
    uint bmapstart;    // Block number of first free map block
    uint ninode_blocks; // Number of inode blocks
    uint nbmap_blocks;  // Number of bitmap blocks
    struct inode *root_dir;
    struct xv6_mount_options options;
    u64 balloc_hint; /* block allocation hint */
    struct rb_root inode_tree; /* tree of active inodes */
    struct mutex itree_lock; /* lock for inode_tree */
};

/* Used by struct inode::i_private. */
struct xv6_inode_info {
    uint addrs[NDIRECT + 1];
};

struct xv6_inode {
    struct rb_node rbnode; /* must be first */
    struct inode inode;    /* An inode in this filesystem */
    long refcount;         /* reference count */
};

#endif // _FSINFO 1
