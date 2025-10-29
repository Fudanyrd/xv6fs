# Duplicate Code Elimination

In development I copy-pasted a great many code (😭😭).
It may be helpful to remove them. For example,

<ul>
    <li>xv6_find_inum</li>
    <li>xv6_dentry_insert</li>
    <li>xv6_dir_erase</li>
    <li>xv6_dir_rmtest</li>
</ul>

all contain code to iterate the directory; and 

<ul>
    <li>xv6_inode_block</li>
    <li>xv6_inode_wblock</li>
</ul>

have the same logic of getting a data block given the offset.

A little about how I may want to resolve this:

## Use an Iterate Callback

```c
struct xv6_diter_action {
    unsigned char cont : 1, /* Should continue iteration */
        de_dirty: 1, /* dirent is dirty */
        dir_ext : 1, /* Do not stop at end of dir. */ 
        dir_dirty : 1, /* The inode of the dir is dirty; */
        padding : 4; /* Unused */
};

typedef struct xv6_diter_action (*xv6_diter_callback)(
    uint, /* dirent number to locate dirent */
    struct dirent *, /* The dirent for read/write */
    void *ctx, /* Other context needed. */);
```

The `dir_ext` and `dir_dirty` bit are here for inserting
entries. If the iterator cannot find an empty entry, it must
extend the directory by one entry. When the callback sets
this bit, the iterator will not stop at the end of directory,
but go beyond a little.

For example, the callback for `xv6_dir_erase` could be:

```c
struct xv6_erase_ctx {
    const char *name;
    bool success; /* determine return value, -ENOENT or 0 */
};
static
struct xv6_diter_action xv6_erase_callback(uint dnum,
            struct dirent *de, void *ctx) {
    struct xv6_diter_action ret;
    struct xv6_erase_ctx *ectx = ctx;
    ectx->success = false;
    const char *name = ectx->name;
    *(char *) &ret = 0; ret.cont = 1;
    if (dnum <= 1) { /* skip . and .. */
        return ret;
    }
    if (strncmp(de->name, name, DIRSIZ) == 0) {
        /* erase this entry. */
        memset(de, 0, sizeof(*de));
        ectx->success = true;
        ret.de_dirty = 1; /* the iterator should flush buffer. */
        ret.cont = 0; /* the iterator can stop. */
        return ret;
    }
    return ret;
}
```

## Use a Generic Directory Entry Iterator

```c++
int xv6_dir_iterate(struct inode *dir, 
        xv6_diter_callback callback, /* iteration callback. */
        void *ctx, /* context should be passed to callback. */) {
    for (auto [dnum, dentry] : dir) {
        auto action = callback(dnum, &dentry, ctx);
        if (!action.cont) { break; }
        /* more work of sync dirty buffer, inode, etc. */
    }
    /* Do clean up, such as free memory; release buffer. */
}
```

# Add More Checks in checker.cpp

Currently checker only checks the super block of xv6 file system.
In the future, it will likely perform more checks, and be helpful
for unit testing

> How to reuse as much code as possible between kernel module and this checker?
>
> May have to export xv6_dir_iterate and xv6_inode_block, and pass proper
> bread/bdata/bfree method to it.

