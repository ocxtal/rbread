
/**
 * @file zc.c
 * @brief minimal gzcat implementation
 *
 * @author Hajime Suzuki
 * @date 2018/8/7
 * @license MIT
 */

#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <zlib.h>

#define _force_inline		inline
#define MIN2(x,y) 			( (x) < (y) ? (x) : (y) )

/* magics */
#define GZIP_HEADER			"\x1f\x8b"
// #define BZ2_HEAER			"BZh"

#define ZC_BUF_SIZE			( 2ULL * 1024 * 1024 )

/**
 * @struct zc_s
 * @brief context (gzFile)
 */
typedef struct zc_s {
	FILE *fp;
	uint8_t *ibuf, *buf;
	size_t head, len;
	size_t bulk_size, bulk_thresh;
	uint8_t eof, _pad[7];
	size_t (*read)(struct zc_s *, void *, size_t);
	z_stream zs;
} zc_t;

/**
 * @fn zc_read_inflate, zc_read_transparent
 * @brief block fetcher
 */
static _force_inline
size_t zc_read_inflate(zc_t *zc, void *dst, size_t len)
{
	if(zc->zs.avail_in < 2 && zc->eof == 0) {
		/* copy to the head */
		uint8_t *p = zc->ibuf;
		for(size_t i = 0; i < zc->zs.avail_in; i++) { *p++ = *zc->zs.next_in++; }
		zc->zs.next_in = p;

		/* fill */
		zc->zs.avail_in += fread(zc->zs.next_in, sizeof(char), len, zc->fp);
		if(feof(zc->fp)) { zc->eof = 1; }
	}
	zc->zs.next_out = dst;
	zc->zs.avail_out = len;
	if(inflate(&zc->zs, Z_NO_FLUSH) == Z_STREAM_ERROR) {
		zc->eof = 2;
		return(0);
	}
	return(len - zc->zs.avail_out);
}
static _force_inline
size_t zc_read_transparent(zc_t *zc, void *dst, size_t len)
{
	return(fread(dst, sizeof(char), len, zc->fp));
}

/**
 * @fn zcopen, zcclose, zceof
 * @brief open decompressor (reader stream); always single-threaded (seek, flush, ... are not implemented)
 */
static _force_inline
zc_t *zcopen(char const *fn)
{
	zc_t *zc = calloc(1, sizeof(zc_t));
	if(zc == NULL) { return(NULL); }
	zc->fp = fopen(fn, "rb");				/* always read and transparent */
	if(zc->fp == NULL) { goto _zcopen_fail; }
	zc->bulk_size = ZC_BUF_SIZE;
	zc->bulk_thresh = 2 * zc->bulk_size;

	/* read the first chunk */
	uint8_t *buf = malloc(2 * zc->bulk_size);
	size_t rlen = fread(buf, sizeof(char), zc->bulk_size, zc->fp);
	if(feof(zc->fp)) { zc->eof = 1; }

	/* determine file type */
	if(rlen >= 2 && memcmp(buf, GZIP_HEADER, strlen(GZIP_HEADER)) == 0) {
		zc->read = zc_read_inflate;
		zc->ibuf = buf;
		zc->buf = malloc(2 * zc->bulk_size);
		zc->zs.next_in = buf;
		zc->zs.avail_in = rlen;
		zc->zs.next_out = zc->buf;
		zc->zs.avail_out = 2 * zc->bulk_size;
		if(inflateInit2(&zc->zs, 15 + 16) != Z_OK) { goto _zcopen_fail; }		/* make sure the stream is gzip one because determined by 0x8b1f */
	} else {
		zc->read = zc_read_transparent;
		zc->buf = buf;
		zc->len = rlen;
	}
	return(zc);
_zcopen_fail:;
	fclose(zc->fp); free(zc);
	return(NULL);
}
static _force_inline
void zcclose(zc_t *zc)
{
	fclose(zc->fp); free(zc);
	return;
}
#define zceof(_zc)				( (_zc)->eof >= 2 )

/**
 * @fn zcread
 * @brief gzread compatible
 */
static _force_inline
size_t zcread(zc_t *zc, void *dst, size_t len)
{
	size_t rem = len;
	if(zc->eof > 1) { return(0); }

	/* flush remaining content in buffer */
	size_t hlen = MIN2(rem, zc->len - zc->head);
	memcpy(&dst[len - rem], &zc->buf[zc->head], hlen);
	rem -= hlen; zc->head += hlen;

	while(zc->eof < 2 && rem > zc->bulk_thresh) {
		rem -= zc->read(zc, &dst[len - rem], rem);
		if(zc->eof == 1 && zc->zs.avail_in == 0) { zc->eof = 2; }
	}
	while(zc->eof < 2 && rem > 0) {
		zc->len = zc->read(zc, zc->buf, zc->bulk_size);
		zc->head = 0;
		size_t tlen = MIN2(rem, zc->len - zc->head);
		memcpy(dst, &zc->buf[zc->head], tlen);
		rem -= tlen; zc->head += tlen;
		if(zc->eof == 1 && zc->head == zc->len) { zc->eof = 2; }
	}
	return(len - rem);
}

int main(int argc, char *argv[])
{
	if(argc > 1 && strcmp(argv[1], "-h") == 0) {
		fprintf(isatty(fileno(stdout)) ? stdout : stderr,
			"\n"
			"  zc -- minimal gzcat\n"
			"    please visit https://github.com/ocxtal/zc for more information\n"
			"\n"
		);
		return(0);
	}

	int ret = 1;
	size_t const buf_size = ZC_BUF_SIZE;
	uint8_t *buf = malloc(buf_size);
	for(char *const *p = &argv[1]; *p != NULL; p++) {
		zc_t *zc = zcopen(*p);
		if(zc == NULL) {
			fprintf(stderr, "failed to open file `%s'\n", *p);
			goto _main_error;
		}
		while(zceof(zc) == 0) {
			fwrite(buf, 1, zcread(zc, buf, buf_size), stdout);
		}
		zcclose(zc);
	}
	ret = 0;
_main_error:;
	free(buf);
	return(ret);
}

/* end of zc.c */
