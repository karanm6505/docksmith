#pragma once

typedef struct Store Store;

/* NewStore: allocates and initialises the store.
   Returns NULL and prints error on failure. Caller must call store_free. */
Store *store_new(void);
void   store_free(Store *s);

const char *store_root(const Store *s);
const char *store_images_dir(const Store *s);
const char *store_layers_dir(const Store *s);
const char *store_cache_dir(const Store *s);

/* fix_ownership: chown path to real user when under sudo (no-op if not sudo). */
void store_fix_ownership(const Store *s, const char *path);
