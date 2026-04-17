#include "layer.h"
#include "store.h"
#include "../util/hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>

static char *layer_filename(const char *digest) {
    /* replace ':' with '-' in the digest to make a valid filename */
    char *fn = strdup(digest);
    if (!fn) return NULL;
    for (char *p = fn; *p; p++)
        if (*p == ':') *p = '-';
    return fn;
}

char *store_layer_path(Store *s, const char *digest) {
    char *fn = layer_filename(digest);
    if (!fn) return NULL;
    const char *ldir = store_layers_dir(s);
    size_t len = strlen(ldir) + 1 + strlen(fn) + 1;
    char *path = malloc(len);
    snprintf(path, len, "%s/%s", ldir, fn);
    free(fn);
    return path;
}

int store_layer_exists(Store *s, const char *digest) {
    char *path = store_layer_path(s, digest);
    if (!path) return 0;
    struct stat st;
    int exists = (stat(path, &st) == 0);
    free(path);
    return exists;
}

int store_write_layer(Store *s, const unsigned char *tar_data, size_t len,
                      char **out_digest, long long *out_size) {
    char *digest = hash_bytes(tar_data, len);
    if (!digest) return -1;

    char *path = store_layer_path(s, digest);
    if (!path) { free(digest); return -1; }

    FILE *f = fopen(path, "wb");
    if (!f) {
        fprintf(stderr, "write layer %s: %s\n", path, strerror(errno));
        free(path); free(digest); return -1;
    }
    fwrite(tar_data, 1, len, f);
    fclose(f);
    store_fix_ownership(s, path);
    free(path);

    *out_digest = digest;
    *out_size   = (long long)len;
    return 0;
}

int store_read_layer(Store *s, const char *digest,
                     unsigned char **out_data, size_t *out_len) {
    char *path = store_layer_path(s, digest);
    if (!path) return -1;

    FILE *f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "read layer %s: %s\n", digest, strerror(errno));
        free(path); return -1;
    }
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);

    unsigned char *buf = malloc(sz);
    if (!buf) { fclose(f); free(path); return -1; }
    fread(buf, 1, sz, f);
    fclose(f);
    free(path);

    *out_data = buf;
    *out_len  = (size_t)sz;
    return 0;
}

int store_remove_layer(Store *s, const char *digest) {
    char *path = store_layer_path(s, digest);
    if (!path) return -1;
    int rc = remove(path);
    if (rc && errno == ENOENT) {
        fprintf(stderr, "layer %s not found\n", digest);
        free(path); return -1;
    } else if (rc) {
        fprintf(stderr, "remove layer: %s\n", strerror(errno));
        free(path); return -1;
    }
    free(path);
    return 0;
}
