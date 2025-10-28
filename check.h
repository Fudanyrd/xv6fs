#ifndef _CHECK_H
#define _CHECK_H 1

#include "fs.h"

#ifdef __cplusplus
extern "C" {
#endif /* C++ */

struct checker {
    void *(* bread)(uint); /**< Read a disk block, return the buffer struct. */
    void *(* bdata)(void *); /**< Given the buffer, get its internal data. */
    void (*bfree)(void *); /**< Free an buffer. */

    const char *warn; /**< Prefix of warning message. */
    void (*warning)(const char *fmt, ...);

    const char *err; /**< Prefix of error message. */
    void (*error)(const char *fmt, ...);
};

/** Run the xv6 filesystem checker. */
extern int xv6_docheck(struct checker *check) __attribute__((nothrow));

#ifdef __cplusplus
}
#endif /* C++ */

#endif /* _CHECK_H 1 */
