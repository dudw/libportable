#include "cpu_info.h"
#include <stdio.h>
#include <immintrin.h>

#define UU16(x) ( (uint8_t)x | (((uint16_t)(x)&0xff) << 8) )
#define UU32(y) ( UU16(y) | (((uint32_t)(UU16(y))&0xffff) << 16) )

static LIB_INLINE void*
memset_less32(void *dst, int a, size_t n)
{
    uint8_t *dt = (uint8_t *)dst;
    if (n & 0x01)
    {
        *dt++ = (uint8_t)a;
    }
    if (n & 0x02)
    {
        *(uint16_t *)dt = UU16(a);
        dt += 2;
    }
    if (n & 0x04)
    {
        _mm_stream_si32((int *)dt, UU32(a));
        dt += 4;
    }
    if (n & 0x08)
    {
        uint32_t c = UU32(a);
        _mm_stream_si32((int *)dt+0, c);
        _mm_stream_si32((int *)dt+1, c);
        dt += 8;
    }
    if (n & 0x10)
    {
        uint32_t c = UU32(a);
        _mm_stream_si32((int *)dt+0, c);
        _mm_stream_si32((int *)dt+1, c);
        _mm_stream_si32((int *)dt+2, c);
        _mm_stream_si32((int *)dt+3, c);
        dt += 16;
    }
    return dst;
}

/* using non-temporal avx */
void* __cdecl
memset_avx(void* dst, int c, size_t size)
{
    __m256i   vals;
    uint8_t   *buffer = (uint8_t *)dst;
    const uint8_t non_aligned = (uintptr_t)buffer % 32;
     /* memory address not 32-byte aligned */
    if ( non_aligned )
    {
        /* fill head */
        uintptr_t head = 32 - non_aligned;
        memset_less32(buffer, c, head);
        buffer += head;
        size -= head;
    }
    if ( c )
    {
        vals = _mm256_set1_epi8(c);
    }
    else
    {
        vals = _mm256_setzero_si256();
    }
    while (size >= 32)
    {
        _mm256_stream_si256((__m256i*)buffer, vals);
        buffer += 32;
        size -= 32;
    }
    if ( size > 0 )
    {
        /* fill tail */
        memset_less32(buffer, c, size);
    }
    return dst;
}
