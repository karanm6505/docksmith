#include "cache.h"
#include "../util/hash.h"
#include "../../vendor/cjson/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

struct CacheEntry {
    char *key;
    char *digest;
};

struct Cache {
    char             path[4096];
    struct CacheEntry *entries;
    int               count;
    int               cap;
};

char *cache_compute_key(const char *parent_digest, const char *instruction,
                        const char *content_hash) {
    size_t len = strlen(parent_digest) + 1 + strlen(instruction) + 1 +
                 (content_hash ? strlen(content_hash) : 0) + 1;
    char *raw = malloc(len);
    snprintf(raw, len, "%s|%s|%s",
             parent_digest, instruction, content_hash ? content_hash : "");
    char *key = hash_bytes((unsigned char*)raw, strlen(raw));
    free(raw);
    return key;
}

static char *read_file_str(const char *path) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1); fread(buf, 1, sz, f); buf[sz] = '\0';
    fclose(f); return buf;
}

Cache *cache_new(Store *s) {
    Cache *c = calloc(1, sizeof(Cache));
    snprintf(c->path, sizeof(c->path), "%s/index.json", store_cache_dir(s));

    char *json = read_file_str(c->path);
    if (json) {
        cJSON *root = cJSON_Parse(json);
        free(json);
        if (root) {
            cJSON *entries = cJSON_GetObjectItem(root, "entries");
            if (entries) {
                cJSON *item;
                cJSON_ArrayForEach(item, entries) {
                    if (c->count >= c->cap) {
                        c->cap = c->cap ? c->cap*2 : 16;
                        c->entries = realloc(c->entries, c->cap * sizeof(struct CacheEntry));
                    }
                    c->entries[c->count].key    = strdup(item->string);
                    c->entries[c->count].digest = strdup(item->valuestring);
                    c->count++;
                }
            }
            cJSON_Delete(root);
        }
    }
    return c;
}

void cache_free(Cache *c) {
    if (!c) return;
    for (int i = 0; i < c->count; i++) {
        free(c->entries[i].key); free(c->entries[i].digest);
    }
    free(c->entries); free(c);
}

int cache_lookup(Cache *c, const char *key, char **out_digest) {
    for (int i = 0; i < c->count; i++)
        if (strcmp(c->entries[i].key, key) == 0) {
            *out_digest = strdup(c->entries[i].digest);
            return 1;
        }
    return 0;
}

static int cache_save(Cache *c) {
    cJSON *root = cJSON_CreateObject();
    cJSON *entries = cJSON_CreateObject();
    for (int i = 0; i < c->count; i++)
        cJSON_AddStringToObject(entries, c->entries[i].key, c->entries[i].digest);
    cJSON_AddItemToObject(root, "entries", entries);
    char *json = cJSON_Print(root);
    cJSON_Delete(root);

    FILE *f = fopen(c->path, "w");
    if (!f) { free(json); return -1; }
    fputs(json, f); fclose(f); free(json);
    return 0;
}

int cache_store(Cache *c, const char *key, const char *digest) {
    /* update existing */
    for (int i = 0; i < c->count; i++)
        if (strcmp(c->entries[i].key, key) == 0) {
            free(c->entries[i].digest);
            c->entries[i].digest = strdup(digest);
            return cache_save(c);
        }
    /* new entry */
    if (c->count >= c->cap) {
        c->cap = c->cap ? c->cap*2 : 16;
        c->entries = realloc(c->entries, c->cap * sizeof(struct CacheEntry));
    }
    c->entries[c->count].key    = strdup(key);
    c->entries[c->count].digest = strdup(digest);
    c->count++;
    return cache_save(c);
}

void cache_each_entry(Cache *c, CacheEntryCb cb, void *userdata) {
    for (int i = 0; i < c->count; i++)
        cb(c->entries[i].key, c->entries[i].digest, userdata);
}

int cache_entry_count(Cache *c) { return c->count; }
