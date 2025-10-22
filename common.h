#ifndef _COMMON_H
#define _COMMON_H 1

typedef unsigned int uint;
typedef unsigned short ushort;
typedef unsigned char uchar;

#define xv6_assert(cond) do {                                         \
    if (! (cond)) {                                                   \
        panic("xv6: internal error at %s:%d\n", __FILE__, __LINE__);  \
    }                                                                 \
} while (0)

#define xv6_static_assert(cond) \
    do { switch(cond) { case (0): break; case (cond): break; }; } while (0)

#define xv6_warn(fmt, ...) do {                           \
    printk(KERN_WARNING "xv6: " fmt "\n", ##__VA_ARGS__); \
} while (0)

#define xv6_error(fmt, ...) do {                          \
    printk(KERN_ERR "xv6: " fmt "\n", ##__VA_ARGS__);     \
} while (0)

#endif // _COMMON_H 1

