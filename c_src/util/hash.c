#include "hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <openssl/sha.h>

static char *hex_encode(const unsigned char digest[SHA256_DIGEST_LENGTH]) {
    /* "sha256:" + 64 hex chars + NUL */
    char *out = malloc(7 + 64 + 1);
    if (!out) return NULL;
    memcpy(out, "sha256:", 7);
    for (int i = 0; i < SHA256_DIGEST_LENGTH; i++)
        snprintf(out + 7 + i * 2, 3, "%02x", digest[i]);
    return out;
}

char *hash_bytes(const unsigned char *data, size_t len) {
    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256(data, len, digest);
    return hex_encode(digest);
}

char *hash_file(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) { perror(path); return NULL; }

    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    unsigned char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0)
        SHA256_Update(&ctx, buf, n);
    fclose(f);

    unsigned char digest[SHA256_DIGEST_LENGTH];
    SHA256_Final(digest, &ctx);
    return hex_encode(digest);
}
