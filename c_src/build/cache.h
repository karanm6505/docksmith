#pragma once
#include "../store/store.h"

/* ComputeCacheKey: returns heap string (sha256 of parent|instruction|content).
   Caller must free. */
char *cache_compute_key(const char *parent_digest, const char *instruction,
                        const char *content_hash);

typedef struct Cache Cache;

Cache *cache_new(Store *s);
void   cache_free(Cache *c);

/* Lookup: if found sets *out_digest (heap string, caller frees) and returns 1.
   Returns 0 if not found. */
int  cache_lookup(Cache *c, const char *key, char **out_digest);

/* Store: write cache entry. Returns 0 on success. */
int  cache_store(Cache *c, const char *key, const char *digest);

/* Entries callback: called for each (key, digest) pair. */
typedef void (*CacheEntryCb)(const char *key, const char *digest, void *userdata);
void cache_each_entry(Cache *c, CacheEntryCb cb, void *userdata);
int  cache_entry_count(Cache *c);
