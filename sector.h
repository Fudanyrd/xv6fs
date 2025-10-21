#ifndef _SECTOR_H
#define _SECTOR_H 1

#include <linux/fs.h>

#include "fs.h"

typedef char * xv6_block_t;
typedef const char * xv6_cst_block_t;

/** Read a xv6 virtual 1024-byte block. 
 * @return error if failed.
 */
extern int read_xv6_block(xv6_block_t buf, struct super_block *sb, uint sect);
extern int write_xv6_block(xv6_cst_block_t buf, struct super_block *sb , uint sect);

#endif // _SECTOR_H

