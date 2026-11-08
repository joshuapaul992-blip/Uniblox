/* Uniblox - core fixed-width types and small helpers.
 * Only freestanding C99 headers are allowed here: no hosted libc exists. */
#ifndef UB_TYPES_H
#define UB_TYPES_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t   i8;
typedef int16_t  i16;
typedef int32_t  i32;
typedef int64_t  i64;
typedef float    f32;
typedef double   f64;

#define UB_ARRAY_COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))
#define UB_MIN(a, b) ((a) < (b) ? (a) : (b))
#define UB_MAX(a, b) ((a) > (b) ? (a) : (b))
#define UB_CLAMP(x, lo, hi) UB_MIN(UB_MAX((x), (lo)), (hi))

#endif
