/* Uniblox - bytes for the network: a writer and a reader for messages.
 * Little-endian; compact unsigned (LEB128) and signed (zigzag) integers;
 * rotations as their quaternion's three smallest parts (6 bytes). A
 * reader never reads past its end: it sets `err` and returns zeros. */
#ifndef UB_NETBUF_H
#define UB_NETBUF_H

#include "core/ub_vec.h"

typedef struct {
    u8 *p;
    u32 len, cap;
    bool err;      /* out of memory, or a fixed buffer full */
    bool fixed;    /* p was given: it never grows */
} ub_wbuf;

/* fixed != NULL: write into it (cap bytes); else a buffer that grows. */
void ub_wbuf_init(ub_wbuf *b, u8 *fixed, u32 cap);
void ub_wbuf_free(ub_wbuf *b);
static inline void ub_wbuf_clear(ub_wbuf *b) { b->len = 0; b->err = false; }
void ub_w_bytes(ub_wbuf *b, const void *d, u32 n);
void ub_w_u8(ub_wbuf *b, u8 v);
void ub_w_u16(ub_wbuf *b, u16 v);
void ub_w_u32(ub_wbuf *b, u32 v);
void ub_w_f32(ub_wbuf *b, f32 v);
void ub_w_f64(ub_wbuf *b, f64 v);
void ub_w_varu(ub_wbuf *b, u32 v);
void ub_w_vari(ub_wbuf *b, i32 v);
void ub_w_str(ub_wbuf *b, const char *s, u32 n);   /* length, then the bytes */
void ub_w_vec3(ub_wbuf *b, vec3 v);
void ub_w_rot(ub_wbuf *b, const mat3 *m);         /* 6 bytes, within 1e-4 per element */

typedef struct {
    const u8 *p;
    u32 len, pos;
    bool err;      /* read past the end, or a malformed value */
} ub_rbuf;

static inline void ub_rbuf_init(ub_rbuf *r, const void *p, u32 len) { r->p = (const u8 *)p; r->len = len; r->pos = 0; r->err = false; }
static inline u32 ub_r_left(const ub_rbuf *r) { return r->len - r->pos; }
bool ub_r_bytes(ub_rbuf *r, void *d, u32 n);
const u8 *ub_r_span(ub_rbuf *r, u32 n);           /* n bytes in place, NULL if fewer are left */
u8 ub_r_u8(ub_rbuf *r);
u16 ub_r_u16(ub_rbuf *r);
u32 ub_r_u32(ub_rbuf *r);
f32 ub_r_f32(ub_rbuf *r);
f64 ub_r_f64(ub_rbuf *r);
u32 ub_r_varu(ub_rbuf *r);
i32 ub_r_vari(ub_rbuf *r);
/* a string: its bytes in place (not NUL-terminated) and length; NULL on error */
const char *ub_r_str(ub_rbuf *r, u32 *n);
vec3 ub_r_vec3(ub_rbuf *r);
mat3 ub_r_rot(ub_rbuf *r);

/* the conversions behind ub_w_rot / ub_r_rot (columns = the rotated axes) */
void ub_mat3_to_quat(const mat3 *m, f32 q[4]);
mat3 ub_quat_to_mat3(const f32 q[4]);

#endif
