#include "image.h"
#include "store.h"
#include "../util/hash.h"
#include "../../vendor/cjson/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <time.h>

/* ---- helpers ---- */
static char *xstrdup(const char *s) { return s ? strdup(s) : strdup(""); }

void image_free(Image *img) {
    if (!img) return;
    free(img->name); free(img->tag); free(img->digest); free(img->created);
    free(img->config.working_dir);
    for (int i = 0; i < img->config.env_count; i++) free(img->config.env[i]);
    free(img->config.env);
    for (int i = 0; i < img->config.cmd_count; i++) free(img->config.cmd[i]);
    free(img->config.cmd);
    for (int i = 0; i < img->layer_count; i++) {
        free(img->layers[i].digest); free(img->layers[i].created_by);
    }
    free(img->layers);
    free(img);
}

Image *image_dup(const Image *src) {
    Image *d = calloc(1, sizeof(Image));
    d->name    = xstrdup(src->name);
    d->tag     = xstrdup(src->tag);
    d->digest  = xstrdup(src->digest);
    d->created = xstrdup(src->created);
    d->config.working_dir = xstrdup(src->config.working_dir);
    d->config.env_count = src->config.env_count;
    d->config.env = calloc(d->config.env_count + 1, sizeof(char*));
    for (int i = 0; i < d->config.env_count; i++)
        d->config.env[i] = xstrdup(src->config.env[i]);
    d->config.cmd_count = src->config.cmd_count;
    d->config.cmd = calloc(d->config.cmd_count + 1, sizeof(char*));
    for (int i = 0; i < d->config.cmd_count; i++)
        d->config.cmd[i] = xstrdup(src->config.cmd[i]);
    d->layer_count = src->layer_count;
    d->layers = calloc(d->layer_count, sizeof(LayerRef));
    for (int i = 0; i < d->layer_count; i++) {
        d->layers[i].digest     = xstrdup(src->layers[i].digest);
        d->layers[i].size       = src->layers[i].size;
        d->layers[i].created_by = xstrdup(src->layers[i].created_by);
    }
    return d;
}

char *image_file_name(const char *name, const char *tag) {
    size_t len = strlen(name) + 1 + strlen(tag) + 6; /* _tag.json\0 */
    char *fn = malloc(len);
    snprintf(fn, len, "%s_%s.json", name, tag);
    /* replace / with _ */
    for (char *p = fn; *p; p++) if (*p == '/') *p = '_';
    return fn;
}

/* ---- JSON serialise ---- */
static char *image_to_json(const Image *img) {
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "name",    img->name    ? img->name    : "");
    cJSON_AddStringToObject(root, "tag",     img->tag     ? img->tag     : "");
    cJSON_AddStringToObject(root, "digest",  img->digest  ? img->digest  : "");
    cJSON_AddStringToObject(root, "created", img->created ? img->created : "");

    cJSON *cfg = cJSON_CreateObject();
    cJSON *env_arr = cJSON_CreateArray();
    for (int i = 0; i < img->config.env_count; i++)
        cJSON_AddItemToArray(env_arr, cJSON_CreateString(img->config.env[i]));
    cJSON_AddItemToObject(cfg, "Env", env_arr);

    cJSON *cmd_arr = cJSON_CreateArray();
    for (int i = 0; i < img->config.cmd_count; i++)
        cJSON_AddItemToArray(cmd_arr, cJSON_CreateString(img->config.cmd[i]));
    cJSON_AddItemToObject(cfg, "Cmd", cmd_arr);
    cJSON_AddStringToObject(cfg, "WorkingDir",
                            img->config.working_dir ? img->config.working_dir : "");
    cJSON_AddItemToObject(root, "config", cfg);

    cJSON *layers_arr = cJSON_CreateArray();
    for (int i = 0; i < img->layer_count; i++) {
        cJSON *l = cJSON_CreateObject();
        cJSON_AddStringToObject(l, "digest",     img->layers[i].digest     ? img->layers[i].digest     : "");
        cJSON_AddNumberToObject(l, "size",       (double)img->layers[i].size);
        cJSON_AddStringToObject(l, "createdBy",  img->layers[i].created_by ? img->layers[i].created_by : "");
        cJSON_AddItemToArray(layers_arr, l);
    }
    cJSON_AddItemToObject(root, "layers", layers_arr);

    char *s = cJSON_Print(root);
    cJSON_Delete(root);
    return s;
}

/* compute digest = sha256 of JSON with digest="" */
static char *compute_image_digest(Image *img) {
    char *saved = img->digest;
    img->digest = xstrdup("");
    char *json = image_to_json(img);
    free(img->digest);
    img->digest = saved;
    char *digest = hash_bytes((unsigned char*)json, strlen(json));
    free(json);
    return digest;
}

int store_save_image(Store *s, Image *img) {
    if (!img->created || !img->created[0]) {
        time_t now = time(NULL);
        struct tm *tm = gmtime(&now);
        char buf[64];
        strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", tm);
        free(img->created);
        img->created = xstrdup(buf);
    }
    free(img->digest);
    img->digest = compute_image_digest(img);
    if (!img->digest) return -1;

    char *json = image_to_json(img);
    char *fn   = image_file_name(img->name, img->tag);
    size_t pathlen = strlen(store_images_dir(s)) + 1 + strlen(fn) + 1;
    char *path = malloc(pathlen);
    snprintf(path, pathlen, "%s/%s", store_images_dir(s), fn);
    free(fn);

    FILE *f = fopen(path, "w");
    if (!f) {
        fprintf(stderr, "write image %s: %s\n", path, strerror(errno));
        free(path); free(json); return -1;
    }
    fputs(json, f); fclose(f);
    store_fix_ownership(s, path);
    free(path); free(json);
    return 0;
}

/* ---- JSON deserialise ---- */
static Image *image_from_json(const char *json_str) {
    cJSON *root = cJSON_Parse(json_str);
    if (!root) { fprintf(stderr, "unmarshal image JSON\n"); return NULL; }

    Image *img = calloc(1, sizeof(Image));
    cJSON *v;
    if ((v = cJSON_GetObjectItem(root, "name")))    img->name    = xstrdup(v->valuestring);
    if ((v = cJSON_GetObjectItem(root, "tag")))     img->tag     = xstrdup(v->valuestring);
    if ((v = cJSON_GetObjectItem(root, "digest")))  img->digest  = xstrdup(v->valuestring);
    if ((v = cJSON_GetObjectItem(root, "created"))) img->created = xstrdup(v->valuestring);

    cJSON *cfg = cJSON_GetObjectItem(root, "config");
    if (cfg) {
        cJSON *wd = cJSON_GetObjectItem(cfg, "WorkingDir");
        img->config.working_dir = wd ? xstrdup(wd->valuestring) : xstrdup("/");

        cJSON *env_arr = cJSON_GetObjectItem(cfg, "Env");
        if (env_arr) {
            img->config.env_count = cJSON_GetArraySize(env_arr);
            img->config.env = calloc(img->config.env_count + 1, sizeof(char*));
            for (int i = 0; i < img->config.env_count; i++)
                img->config.env[i] = xstrdup(cJSON_GetArrayItem(env_arr, i)->valuestring);
        }
        cJSON *cmd_arr = cJSON_GetObjectItem(cfg, "Cmd");
        if (cmd_arr) {
            img->config.cmd_count = cJSON_GetArraySize(cmd_arr);
            img->config.cmd = calloc(img->config.cmd_count + 1, sizeof(char*));
            for (int i = 0; i < img->config.cmd_count; i++)
                img->config.cmd[i] = xstrdup(cJSON_GetArrayItem(cmd_arr, i)->valuestring);
        }
    }

    cJSON *layers_arr = cJSON_GetObjectItem(root, "layers");
    if (layers_arr) {
        img->layer_count = cJSON_GetArraySize(layers_arr);
        img->layers = calloc(img->layer_count, sizeof(LayerRef));
        for (int i = 0; i < img->layer_count; i++) {
            cJSON *l = cJSON_GetArrayItem(layers_arr, i);
            cJSON *d = cJSON_GetObjectItem(l, "digest");
            cJSON *sz = cJSON_GetObjectItem(l, "size");
            cJSON *cb = cJSON_GetObjectItem(l, "createdBy");
            img->layers[i].digest     = d  ? xstrdup(d->valuestring)  : xstrdup("");
            img->layers[i].size       = sz ? (long long)sz->valuedouble : 0;
            img->layers[i].created_by = cb ? xstrdup(cb->valuestring) : xstrdup("");
        }
    }

    cJSON_Delete(root);
    return img;
}

static char *read_file_str(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long sz = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(sz + 1);
    fread(buf, 1, sz, f);
    buf[sz] = '\0';
    fclose(f);
    return buf;
}

Image *store_load_image(Store *s, const char *name, const char *tag) {
    char *fn = image_file_name(name, tag);
    size_t pathlen = strlen(store_images_dir(s)) + 1 + strlen(fn) + 1;
    char *path = malloc(pathlen);
    snprintf(path, pathlen, "%s/%s", store_images_dir(s), fn);
    free(fn);

    char *json = read_file_str(path);
    if (!json) {
        fprintf(stderr, "image %s:%s not found\n", name, tag);
        free(path); return NULL;
    }
    free(path);

    Image *img = image_from_json(json);
    free(json);
    return img;
}

int store_remove_image(Store *s, const char *name, const char *tag) {
    char *fn = image_file_name(name, tag);
    size_t pathlen = strlen(store_images_dir(s)) + 1 + strlen(fn) + 1;
    char *path = malloc(pathlen);
    snprintf(path, pathlen, "%s/%s", store_images_dir(s), fn);
    free(fn);

    if (remove(path) != 0) {
        if (errno == ENOENT)
            fprintf(stderr, "image %s:%s not found\n", name, tag);
        else
            fprintf(stderr, "remove image: %s\n", strerror(errno));
        free(path); return -1;
    }
    free(path);
    return 0;
}

int store_print_images(Store *s) {
    DIR *dir = opendir(store_images_dir(s));
    if (!dir) { perror(store_images_dir(s)); return -1; }

    printf("%-20s %-10s %-22s %-22s %s\n",
           "NAME", "TAG", "DIGEST", "CREATED", "SIZE");

    struct dirent *ent;
    while ((ent = readdir(dir))) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 5 || strcmp(ent->d_name + nlen - 5, ".json")) continue;

        size_t pathlen = strlen(store_images_dir(s)) + 1 + nlen + 1;
        char *path = malloc(pathlen);
        snprintf(path, pathlen, "%s/%s", store_images_dir(s), ent->d_name);

        char *json = read_file_str(path);
        free(path);
        if (!json) continue;

        Image *img = image_from_json(json);
        free(json);
        if (!img) continue;

        char short_digest[23] = {0};
        if (img->digest && strlen(img->digest) >= 19)
            snprintf(short_digest, sizeof(short_digest), "%.*s", 19, img->digest);
        else if (img->digest)
            snprintf(short_digest, sizeof(short_digest), "%s", img->digest);

        long long total_size = 0;
        for (int i = 0; i < img->layer_count; i++)
            total_size += img->layers[i].size;

        printf("%-20s %-10s %-22s %-22s %lld B\n",
               img->name ? img->name : "",
               img->tag  ? img->tag  : "",
               short_digest,
               img->created ? img->created : "",
               total_size);
        image_free(img);
    }
    closedir(dir);
    return 0;
}
