#ifndef _COMMON_H
#define _COMMON_H 1

typedef unsigned int uint;
typedef unsigned short ushort;

#define xv6_assert(cond) do {} while (0)

#define xv6_static_assert(cond) \
    do { switch(cond) { case (0): break; case (cond): break; }; } while (0)

#endif // _COMMON_H 1

