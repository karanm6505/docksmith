#pragma once
#include "store.h"
#include <stddef.h>

/* ---- ImageConfig ---- */
typedef struct {
    char **env;        /* heap array of "KEY=VALUE" strings */
    int    env_count;
    char **cmd;        /* heap array of argv strings */
    int    cmd_count;
    char  *working_dir; /* heap string */
} ImageConfig;

/* ---- LayerRef ---- */
typedef struct {
    char  *digest;
    long long size;
    char  *created_by;
} LayerRef;

/* ---- Image ---- */
typedef struct {
    char        *name;
    char        *tag;
    char        *digest;
    char        *created;
    ImageConfig  config;
    LayerRef    *layers;
    int          layer_count;
} Image;

void image_free(Image *img);
Image *image_dup(const Image *src); /* deep copy */

/* Store operations */
int    store_save_image(Store *s, Image *img);
Image *store_load_image(Store *s, const char *name, const char *tag);
int    store_remove_image(Store *s, const char *name, const char *tag);
int    store_print_images(Store *s);

/* Internal: image file name (name:tag → name_tag.json, / → _) */
char *image_file_name(const char *name, const char *tag);
