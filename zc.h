
/**
 * @file zc.h
 * @brief minimal gzread implementation for gzip, bz2, and xz (lzma)
 *
 * @author Hajime Suzuki
 * @date 2018/8/7
 * @license MIT
 */
#ifndef _ZC_H_INCLUDED
#define _ZC_H_INCLUDED

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <bzlib.h>
#include <lzma.h>
#include <zlib.h>

/* unaligned 64bit load / store */
#define _zc_loadu_u64(p)		({ uint8_t const *_p = (uint8_t const *)(p); *((uint64_t const *)_p); })
#define _zc_storeu_u64(p, e)	{ uint8_t *_p = (uint8_t *)(p); *((uint64_t *)(_p)) = (e); }
#define ZC_BUF_SIZE				( 2ULL * 1024 * 1024 )

/**
 * @struct zc_s
 * @brief context (gzFile)
 */
typedef struct zc_s {
	FILE *fp;
	uint8_t *ibuf, *buf;	/* input buffer and output buffer; ibuf is unused for transparent mode */
	size_t head, len;
	size_t bulk_size, bulk_thresh;
	uint8_t eof, _pad[7];
	size_t (*read)(struct zc_s *, void *, size_t);
	union {					/* exclusive */
		z_stream z;			/* .gz */
		bz_stream b;		/* .bz2 */
		lzma_stream x;		/* .xz */
	} s;
} zc_t;

/**
 * @fn zc_read_gzip, zc_read_bz2, zc_read_xz, zc_read_transparent
 * @brief block fetcher
 */
#define _zc_read(_sfx, _ctx, _type_t, _body) \
	static inline size_t zc_read_##_sfx(zc_t *zc, void *dst, size_t len) \
	{ \
		if(zc->s._ctx.avail_in < sizeof(uint64_t) && zc->eof == 0) { \
			/* copy to the head then fill */ \
			_zc_storeu_u64(zc->ibuf, _zc_loadu_u64(zc->s._ctx.next_in)); \
			zc->s._ctx.next_in = (_type_t *)&zc->ibuf[zc->s._ctx.avail_in]; \
			zc->s._ctx.avail_in += fread((void *)zc->s._ctx.next_in, sizeof(char), len, zc->fp); \
			if(feof(zc->fp)) { zc->eof = 1; } \
		} \
		zc->s._ctx.next_out = (_type_t *)dst; \
		zc->s._ctx.avail_out = len; \
		_body; \
		return(zc->eof == 2 ? 0 : len - zc->s._ctx.avail_out); \
	}

_zc_read(gzip, z, uint8_t, {
	switch(inflate(&zc->s.z, Z_NO_FLUSH)) {
		default: zc->eof = 2;
		case Z_OK: case Z_STREAM_END: break;
	}
});
_zc_read(bz2, b, char, {
	switch(BZ2_bzDecompress(&zc->s.b)) {
		default: zc->eof = 2;
		case BZ_OK: case BZ_STREAM_END: break;
	}
});
_zc_read(xz, x, uint8_t, {
	switch(lzma_code(&zc->s.x, zc->eof == 1 ? LZMA_FINISH : LZMA_RUN)) {
		default: zc->eof = 2;
		case LZMA_OK: case LZMA_STREAM_END: break;
	}
});
#undef _zc_read

static inline
size_t zc_read_transparent(zc_t *zc, void *dst, size_t len)
{
	return(fread(dst, sizeof(char), len, zc->fp));
}

/**
 * @fn zcread
 * @brief gzread compatible
 */
static inline
size_t zcread(zc_t *zc, void *dst, size_t len)
{
	if(zc->eof > 1) { return(0); }
	size_t rem = len;
	uint8_t *p = dst;

	#define zc_min2(x,y) 			( (x) < (y) ? (x) : (y) )

	/* flush remaining content in buffer */
	size_t hlen = zc_min2(rem, zc->len - zc->head);
	memcpy(&p[len - rem], &zc->buf[zc->head], hlen);
	rem -= hlen; zc->head += hlen;

	while(zc->eof < 2 && rem > zc->bulk_thresh) {
		rem -= zc->read(zc, &p[len - rem], rem);
		if(zc->eof == 1 && zc->s.z.avail_in == 0) { zc->eof = 2; }
	}
	while(zc->eof < 2 && rem > 0) {
		zc->len = zc->read(zc, zc->buf, zc->bulk_size);
		zc->head = 0;
		size_t tlen = zc_min2(rem, zc->len - zc->head);
		memcpy(p, &zc->buf[zc->head], tlen);
		rem -= tlen; zc->head += tlen;
		if(zc->eof == 1 && zc->head == zc->len) { zc->eof = 2; }
	}
	return(len - rem);

	#undef zc_min2
}

/**
 * @fn zcopen, zcclose, zceof
 * @brief open decompressor (reader stream); always single-threaded (seek, flush, ... are not implemented)
 */
static inline
zc_t *zcopen(char const *fn)
{
	zc_t *zc = calloc(1, sizeof(zc_t));
	if(zc == NULL) { return(NULL); }
	zc->fp = fopen(fn, "rb");							/* always binary read */
	if(zc->fp == NULL) { goto _zcopen_fail; }
	zc->bulk_size = ZC_BUF_SIZE;
	zc->bulk_thresh = 2 * zc->bulk_size;

	/* read the first chunk */
	uint8_t *buf = malloc(2 * zc->bulk_size);
	size_t rlen = fread(buf, sizeof(char), zc->bulk_size, zc->fp);
	if(feof(zc->fp)) { zc->eof = 1; }

	static struct { uint64_t magic, mask; } const x[4] = {
		{ 0x000000088b1fULL, 0x000000ffffffULL },		/* gzip */
		{ 0x000000685a42ULL, 0x000000ffffffULL },		/* bz2 */
		{ 0x005a587a37fdULL, 0xffffffffffffULL }		/* lzma */
	};

	/* determine file type; FIXME: refactor redundant code */
	#define _zc_init(_sfx, _ctx, _type_t, _body) { \
		zc->read = zc_read_##_sfx; \
		zc->ibuf = buf; \
		zc->buf = malloc(2 * zc->bulk_size); \
		zc->s._ctx.next_in = (_type_t *)buf; \
		zc->s._ctx.avail_in = rlen; \
		zc->s._ctx.next_out = (_type_t *)zc->buf; \
		zc->s._ctx.avail_out = 2 * zc->bulk_size; \
		if(!(_body)) { goto _zcopen_fail; } \
	}
	if(rlen >= sizeof(uint64_t)) {
		uint64_t h = _zc_loadu_u64(buf);
		for(uint64_t i = 0; x[i].magic != 0; i++) {
			if((h & x[i].mask) != x[i].magic) { continue; }
			switch(i) {
				case 0: _zc_init(gzip, z, uint8_t, ({ inflateInit2(&zc->s.z, 15 + 16) == Z_OK; })); break;
				case 1: _zc_init(bz2,  b, char,    ({ BZ2_bzDecompressInit(&zc->s.b, 0, 0) == BZ_OK; })); break;
				case 2: _zc_init(xz,   x, uint8_t, ({ lzma_stream_decoder(&zc->s.x, UINT64_MAX, LZMA_CONCATENATED) == LZMA_OK; })); break;
				default: break;
			}
		}
	}
	#undef _zc_init

	if(zc->read == NULL) {
		zc->read = zc_read_transparent;
		zc->buf = buf;
		zc->len = rlen;
	}
	return(zc);
_zcopen_fail:;
	fclose(zc->fp); free(zc);
	return(NULL);
}
static inline
void zcclose(zc_t *zc)
{
	if(zc->read == zc_read_gzip) {
		deflateEnd(&zc->s.z);
	} if(zc->read == zc_read_bz2) {
		BZ2_bzDecompressEnd(&zc->s.b);
	}
	free(zc->buf);  free(zc->ibuf);
	fclose(zc->fp); free(zc);
	return;
}
#define zceof(_zc)				( (_zc)->eof >= 2 )

#endif	/* _ZC_H_INCLUDED */

/* end of zc.h */
