#ifndef MXB_INFLATE_H
#define MXB_INFLATE_H
#include <stddef.h>
/* données « deflate » brutes (zip, méthode 8) ; 0 = ok */
int inflate_raw(unsigned char *dest, size_t destlen, const unsigned char *src, size_t srclen, size_t *outlen);
#endif
