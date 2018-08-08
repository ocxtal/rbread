
/**
 * @file rbread.h
 * @brief minimal gzread implementation for gzip, bz2, and xz (lzma)
 *
 * @author Hajime Suzuki
 * @date 2018/8/7
 * @license MIT
 */
#ifndef _RB_H_INCLUDED
#define _RB_H_INCLUDED

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>

/* unaligned 64bit load / store */
#define _rb_loadu_u64(p)		({ uint8_t const *_p = (uint8_t const *)(p); *((uint64_t const *)_p); })
#define _rb_storeu_u64(p, e)	{ uint8_t *_p = (uint8_t *)(p); *((uint64_t *)(_p)) = (e); }
#define RB_BUF_SIZE				( 2ULL * 1024 * 1024 )

/**
 * @struct rbread_s
 * @brief context (gzFile)
 */
typedef struct rbread_s {
	FILE *fp;
	uint8_t *ibuf, *buf;	/* input buffer and output buffer; ibuf is unused for transparent mode */
	size_t head, len;
	size_t bulk_size;
	uint8_t eof, _pad[7];
	size_t (*read)(struct rbread_s *, void *, size_t);
	union {					/* exclusive */
		z_stream z;			/* .gz */
		bz_stream b;		/* .bz2 */
		lzma_stream x;		/* .xz */
	} s;
} rbread_t;

/**
 * @fn rb_read_gzip, rb_read_bz2, rb_read_xz, rb_read_transparent
 * @brief block fetcher
 */
#define _rb_reader(_sfx, _ctx, _type_t, _body) \
	static inline size_t rb_read_##_sfx(rbread_t *rb, void *dst, size_t len) \
	{ \
		if(rb->s._ctx.avail_in < sizeof(uint64_t) && rb->eof == 0) { \
			/* copy to the head then fill */ \
			_rb_storeu_u64(rb->ibuf, _rb_loadu_u64(rb->s._ctx.next_in)); \
			rb->s._ctx.next_in = (_type_t *)&rb->ibuf[rb->s._ctx.avail_in]; \
			rb->s._ctx.avail_in += fread((void *)rb->s._ctx.next_in, sizeof(char), len, rb->fp); \
			if(feof(rb->fp)) { rb->eof = 1; } \
		} \
		rb->s._ctx.next_out = (_type_t *)dst; \
		rb->s._ctx.avail_out = len; \
		_body; \
		return(rb->eof == 2 ? 0 : len - rb->s._ctx.avail_out); \
	}

_rb_reader(gzip, z, uint8_t, {
	switch(inflate(&rb->s.z, Z_NO_FLUSH)) {
		default: rb->eof = 2;
		case Z_STREAM_END: inflateReset(&rb->s.z);
		case Z_OK: break;
	}
});
_rb_reader(bz2, b, char, {
	switch(BZ2_bzDecompress(&rb->s.b)) {
		default: rb->eof = 2;
		case BZ_STREAM_END: case BZ_OK: break;
	}
});
_rb_reader(xz, x, uint8_t, {
	switch(lzma_code(&rb->s.x, rb->eof == 1 ? LZMA_FINISH : LZMA_RUN)) {
		default: rb->eof = 2;
		case LZMA_STREAM_END: case LZMA_OK: break;
	}
});
#undef _rb_reader

static inline
size_t rb_read_transparent(rbread_t *rb, void *dst, size_t len)
{
	return(fread(dst, sizeof(char), len, rb->fp));
}

/**
 * @fn rbread
 * @brief gzread compatible
 */
static inline
size_t rbread(rbread_t *rb, void *dst, size_t len)
{
	if(rb->eof > 1) { return(0); }
	size_t rem = len;
	uint8_t *p = dst;

	#define rb_min2(x,y) 			( (x) < (y) ? (x) : (y) )

	/* flush remaining content in buffer */
	size_t hlen = rb_min2(rem, rb->len - rb->head);
	memcpy(&p[len - rem], &rb->buf[rb->head], hlen);
	rem -= hlen; rb->head += hlen;

	while(rb->eof < 2 && rem > 2 * rb->bulk_size) {
		rem -= rb->read(rb, &p[len - rem], rem);
		if(rb->eof == 1 && rb->s.z.avail_in == 0) { rb->eof = 2; }
	}
	while(rb->eof < 2 && rem > 0) {
		rb->len = rb->read(rb, rb->buf, rb->bulk_size);
		rb->head = 0;
		size_t tlen = rb_min2(rem, rb->len - rb->head);
		memcpy(&p[len - rem], &rb->buf[rb->head], tlen);
		rem -= tlen; rb->head += tlen;
		if(rb->eof == 1 && rb->head == rb->len) { rb->eof = 2; }
	}
	return(len - rem);

	#undef rb_min2
}

/**
 * @fn rbopen, rbclose, rbeof
 * @brief open decompressor (reader stream); always single-threaded (seek, flush, ... are not implemented)
 */
static inline
rbread_t *rbopen(char const *fn)
{
	rbread_t *rb = calloc(1, sizeof(rbread_t));
	if(rb == NULL) { return(NULL); }
	rb->fp = fopen(fn, "rb");							/* always binary read */
	if(rb->fp == NULL) { goto _rbopen_fail; }
	rb->bulk_size = RB_BUF_SIZE;

	/* read the first chunk */
	uint8_t *buf = malloc(2 * rb->bulk_size);
	size_t rlen = fread(buf, sizeof(char), rb->bulk_size, rb->fp);
	if(feof(rb->fp)) { rb->eof = 1; }

	static struct { uint64_t magic, mask; } const x[4] = {
		{ 0x000000088b1fULL, 0x000000ffffffULL },		/* gzip */
		{ 0x000000685a42ULL, 0x000000ffffffULL },		/* bz2 */
		{ 0x005a587a37fdULL, 0xffffffffffffULL }		/* lzma */
	};

	/* determine file type; FIXME: refactor redundant code */
	#define _rb_init(_sfx, _ctx, _type_t, _body) { \
		rb->read = rb_read_##_sfx; \
		rb->ibuf = buf; \
		rb->buf = malloc(2 * rb->bulk_size); \
		rb->s._ctx.next_in = (_type_t *)buf; \
		rb->s._ctx.avail_in = rlen; \
		rb->s._ctx.next_out = (_type_t *)rb->buf; \
		rb->s._ctx.avail_out = 2 * rb->bulk_size; \
		if(!(_body)) { goto _rbopen_fail; } \
	}
	if(rlen >= sizeof(uint64_t)) {
		uint64_t h = _rb_loadu_u64(buf);
		for(uint64_t i = 0; x[i].magic != 0; i++) {
			if((h & x[i].mask) != x[i].magic) { continue; }
			switch(i) {
				case 0: _rb_init(gzip, z, uint8_t, ({ inflateInit2(&rb->s.z, 15 + 16) == Z_OK; })); break;
				case 1: _rb_init(bz2,  b, char,    ({ BZ2_bzDecompressInit(&rb->s.b, 0, 0) == BZ_OK; })); break;
				case 2: _rb_init(xz,   x, uint8_t, ({ lzma_stream_decoder(&rb->s.x, UINT64_MAX, LZMA_CONCATENATED) == LZMA_OK; })); break;
				default: break;
			}
		}
	}
	#undef _rb_init

	if(rb->read == NULL) {
		rb->read = rb_read_transparent;
		rb->buf = buf;
		rb->len = rlen;
	}
	return(rb);
_rbopen_fail:;
	if(rb->fp) { fclose(rb->fp); } free(rb);
	return(NULL);
}
static inline
void rbclose(rbread_t *rb)
{
	if(rb->read == rb_read_gzip) {
		deflateEnd(&rb->s.z);
	} else if(rb->read == rb_read_bz2) {
		BZ2_bzDecompressEnd(&rb->s.b);
	} else if(rb->read == rb_read_xz) {
		lzma_end(&rb->s.x);
	}
	free(rb->buf);  free(rb->ibuf);
	fclose(rb->fp); free(rb);
	return;
}
#define rbeof(_rb)				( (_rb)->eof >= 2 )

#endif	/* _RB_H_INCLUDED */

/* end of rbread.h */
