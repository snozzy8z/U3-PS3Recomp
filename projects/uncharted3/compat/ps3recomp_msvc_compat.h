/* ps3recomp - compat MSVC pour les sources PPU "per-game" (style GCC/MinGW).
 * Force-inclus via /FI sur ppu_loader/hle/fs/sysprx (voir CMakeLists).
 * No-op sur clang/gcc (qui ont ces builtins nativement). */
#ifndef PS3RECOMP_MSVC_COMPAT_H
#define PS3RECOMP_MSVC_COMPAT_H

#if defined(_MSC_VER) && !defined(__clang__)
#include <stdlib.h>
#include <intrin.h>
#pragma intrinsic(_ReturnAddress)

/* __attribute__((weak)) etc. -> ignore sous MSVC. */
#ifndef __attribute__
#define __attribute__(x)
#endif

/* byteswaps GCC -> intrinsics MSVC. */
static __forceinline unsigned short     __builtin_bswap16(unsigned short v){ return _byteswap_ushort(v); }
static __forceinline unsigned int       __builtin_bswap32(unsigned int v){ return _byteswap_ulong(v); }
static __forceinline unsigned long long __builtin_bswap64(unsigned long long v){ return _byteswap_uint64(v); }

#ifndef __builtin_return_address
#define __builtin_return_address(n) _ReturnAddress()
#endif
#endif /* _MSC_VER && !__clang__ */

#endif
