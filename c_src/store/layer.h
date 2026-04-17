#pragma once
#include "store.h"
#include <stddef.h>

/* WriteLayer: write tarData into layers/ named by its SHA-256.
   Sets *out_digest (heap-allocated, caller frees) and *out_size.
   Returns 0 on success, -1 on error. */
int store_write_layer(Store *s, const unsigned char *tar_data, size_t len,
                      char **out_digest, long long *out_size);

/* ReadLayer: reads the layer blob. Sets *out_data (heap) and *out_len.
   Returns 0 on success, -1 on error. */
int store_read_layer(Store *s, const char *digest,
                     unsigned char **out_data, size_t *out_len);

/* LayerPath: returns heap string for the layer's filesystem path. Caller frees. */
char *store_layer_path(Store *s, const char *digest);

/* LayerExists: 1 if exists, 0 otherwise. */
int store_layer_exists(Store *s, const char *digest);

/* RemoveLayer: 0 on success, -1 on error. */
int store_remove_layer(Store *s, const char *digest);
