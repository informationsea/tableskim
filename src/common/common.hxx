#ifndef _COMMON_HXX_
#define _COMMON_HXX_

// ssize_t is POSIX-only; MSVC does not provide it.
#ifdef _MSC_VER
#include <BaseTsd.h>
typedef SSIZE_T ssize_t;
#else
#include <sys/types.h>
#endif

#endif // _COMMON_HXX_