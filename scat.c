
/**
 * @file scat.c
 * @brief minimal zcat
 *
 * @author Hajime Suzuki
 * @date 2018/8/7
 * @license MIT
 */

#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#  define _POSIX_C_SOURCE		200112L
#endif
#if defined(__darwin__) && !defined(_DARWIN_C_FULL)
#  define _DARWIN_C_SOURCE		_DARWIN_C_FULL
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "rbread.h"

int main(int argc, char *argv[])
{
	if(argc > 1 && strcmp(argv[1], "-h") == 0) {
		fprintf(isatty(fileno(stdout)) ? stderr : stdout,
			"\n"
			"  scat -- minimal gzcat\n"
			"    please visit https://github.com/ocxtal/rbread for more information\n"
			"\n"
		);
		return(0);
	}

	if(argc == 1) {
		fprintf(stderr, "input from stdin is not supported.\n");
		return(1);
	}

	int ret = 1;
	size_t const buf_size = RB_BUF_SIZE;
	uint8_t *buf = malloc(buf_size);
	for(char *const *p = &argv[1]; *p != NULL; p++) {
		rbread_t *rb = rbopen(*p);
		if(rb == NULL) {
			fprintf(stderr, "failed to open file `%s'\n", *p);
			goto _main_error;
		}
		while(rbeof(rb) == 0) {
			fwrite(buf, 1, rbread(rb, buf, buf_size), stdout);
		}
		rbclose(rb);
	}
	ret = 0;
_main_error:;
	free(buf);
	return(ret);
}

/* end of scat.c */
