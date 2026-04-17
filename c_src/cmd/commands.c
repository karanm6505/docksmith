#include "../store/store.h"
#include "../store/image.h"
#include "../store/layer.h"
#include "../build/engine.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

/* parse_ref: split "name:tag" into name and tag; tag defaults to "latest".
   Both name_out and tag_out must be pre-allocated with at least 256 bytes. */
static void parse_ref(const char *ref, char *name_out, char *tag_out) {
    const char *colon = strchr(ref, ':');
    if (colon) {
        size_t nlen = colon - ref;
        memcpy(name_out, ref, nlen); name_out[nlen] = '\0';
        snprintf(tag_out, 256, "%s", colon+1);
    } else {
        snprintf(name_out, 256, "%s", ref);
        snprintf(tag_out,  256, "latest");
    }
}

/* ---- build ---- */
int cmd_build(int argc, char **argv) {
    const char *file = "Docksmithfile";
    const char *name_tag = NULL;
    const char *context_dir = ".";
    int no_cache = 0;

    for (int i = 0; i < argc; i++) {
        if ((strcmp(argv[i],"-f")==0 || strcmp(argv[i],"--file")==0) && i+1<argc)
            { file = argv[++i]; }
        else if ((strcmp(argv[i],"-t")==0 || strcmp(argv[i],"--tag")==0) && i+1<argc)
            { name_tag = argv[++i]; }
        else if (strcmp(argv[i],"--no-cache")==0)
            no_cache = 1;
        else
            context_dir = argv[i];
    }

    if (!name_tag) { fprintf(stderr,"image name required: use -t name:tag\n"); return 1; }

    char name[256], tag[256];
    parse_ref(name_tag, name, tag);

    /* resolve context_dir to absolute */
    char abs_context[4096];
    if (realpath(context_dir, abs_context) == NULL) {
        fprintf(stderr,"resolve context dir: %s\n", context_dir); return 1;
    }

    /* resolve file */
    char abs_file[4096];
    if (file[0] != '/') snprintf(abs_file, sizeof(abs_file), "%s/%s", abs_context, file);
    else snprintf(abs_file, sizeof(abs_file), "%s", file);

    struct stat st;
    if (stat(abs_file, &st) != 0) {
        fprintf(stderr,"Docksmithfile not found: %s\n", abs_file); return 1;
    }

    Store *s = store_new(); if (!s) return 1;
    Engine *e = engine_new(s, abs_context, no_cache);
    Image *img = engine_build(e, abs_file, name, tag);
    engine_free(e);
    if (img) image_free(img);
    store_free(s);
    return img ? 0 : 1;
}

/* ---- run ---- */
int cmd_run(int argc, char **argv) {
    if (argc < 1) {
        fprintf(stderr,"usage: docksmith run [-e KEY=VALUE] <image>[:<tag>] [command...]\n");
        return 1;
    }

    char **env_overrides = NULL; int env_count = 0, env_cap = 0;
    char **remaining = NULL;     int rem_count = 0, rem_cap = 0;

    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i],"-e")==0 && i+1<argc) {
            if (env_count >= env_cap) { env_cap = env_cap?env_cap*2:4;
                env_overrides = realloc(env_overrides, (env_cap+1)*sizeof(char*)); }
            env_overrides[env_count++] = argv[++i];
        } else {
            if (rem_count >= rem_cap) { rem_cap = rem_cap?rem_cap*2:4;
                remaining = realloc(remaining, (rem_cap+1)*sizeof(char*)); }
            remaining[rem_count++] = argv[i];
        }
    }
    if (env_overrides) env_overrides[env_count] = NULL;

    if (rem_count < 1) {
        fprintf(stderr,"usage: docksmith run [-e KEY=VALUE] <image>[:<tag>] [command...]\n");
        free(env_overrides); free(remaining); return 1;
    }

    char name[256], tag[256];
    parse_ref(remaining[0], name, tag);

    char **cmd_override = NULL;
    if (rem_count > 1) {
        cmd_override = calloc(rem_count, sizeof(char*));
        for (int i = 1; i < rem_count; i++) cmd_override[i-1] = remaining[i];
        cmd_override[rem_count-1] = NULL;
    }
    free(remaining);

    Store *s = store_new(); if (!s) { free(env_overrides); free(cmd_override); return 1; }

    /* container_run declared in container/run.h */
    extern int container_run(Store*, const char*, const char*, char**, char**);
    int rc = container_run(s, name, tag, cmd_override, env_overrides);
    store_free(s);
    free(env_overrides);
    free(cmd_override);
    return rc;
}

/* ---- images ---- */
int cmd_images(int argc, char **argv) {
    (void)argc; (void)argv;
    Store *s = store_new(); if (!s) return 1;
    int rc = store_print_images(s);
    store_free(s);
    return rc;
}

/* ---- rmi ---- */
int cmd_rmi(int argc, char **argv) {
    if (argc < 1) { fprintf(stderr,"usage: docksmith rmi <image>[:<tag>]\n"); return 1; }
    char name[256], tag[256];
    parse_ref(argv[0], name, tag);

    Store *s = store_new(); if (!s) return 1;
    Image *img = store_load_image(s, name, tag);
    if (!img) { store_free(s); return 1; }

    int layers_deleted = 0;
    for (int i = 0; i < img->layer_count; i++) {
        if (store_remove_layer(s, img->layers[i].digest) == 0) layers_deleted++;
        else printf("Warning: could not remove layer %.*s\n", 19, img->layers[i].digest);
    }
    int rc = store_remove_image(s, name, tag);
    if (rc == 0)
        printf("Removed image %s:%s (%d layers deleted)\n", name, tag, layers_deleted);
    image_free(img);
    store_free(s);
    return rc;
}

/* ---- cache ---- */
#include "../build/cache.h"
static void print_cache_entry(const char *key, const char *digest, void *ud) {
    (void)ud;
    char sk[24]={0}, sd[24]={0};
    snprintf(sk, sizeof(sk), "%.*s", 19, key);
    snprintf(sd, sizeof(sd), "%.*s", 19, digest);
    printf("%-22s  %s\n", sk, sd);
}

int cmd_cache(int argc, char **argv) {
    (void)argc; (void)argv;
    Store *s = store_new(); if (!s) return 1;
    Cache *c = cache_new(s);
    int count = cache_entry_count(c);
    if (count == 0) { printf("Build cache is empty.\n"); cache_free(c); store_free(s); return 0; }
    printf("%-22s  %s\n", "CACHE KEY", "LAYER DIGEST");
    cache_each_entry(c, print_cache_entry, NULL);
    printf("\nTotal entries: %d\n", count);
    cache_free(c); store_free(s);
    return 0;
}

/* ---- import ---- */
int cmd_import(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,"usage: docksmith import <name>[:<tag>] <rootfs.tar>\n"); return 1;
    }
    char name[256], tag[256];
    parse_ref(argv[0], name, tag);
    const char *tar_path = argv[1];

    FILE *f = fopen(tar_path, "rb");
    if (!f) { perror(tar_path); return 1; }
    fseek(f,0,SEEK_END); long sz = ftell(f); fseek(f,0,SEEK_SET);
    unsigned char *tar_data = malloc(sz);
    fread(tar_data, 1, sz, f); fclose(f);

    printf("Importing %s (%ld bytes) as %s:%s...\n", tar_path, sz, name, tag);

    Store *s = store_new(); if (!s) { free(tar_data); return 1; }

    char *digest = NULL; long long layer_size = 0;
    if (store_write_layer(s, tar_data, (size_t)sz, &digest, &layer_size) < 0) {
        free(tar_data); store_free(s); return 1;
    }
    free(tar_data);

    char created_by[512];
    snprintf(created_by, sizeof(created_by), "imported from %s", tar_path);

    Image img = {0};
    img.name = name;
    img.tag  = tag;
    ImageConfig cfg = {0};
    static char *default_cmd[] = {"/bin/sh", NULL};
    cfg.cmd = default_cmd; cfg.cmd_count = 1;
    cfg.working_dir = "/";
    img.config = cfg;
    LayerRef lr = { .digest=digest, .size=layer_size, .created_by=created_by };
    img.layers = &lr; img.layer_count = 1;

    if (store_save_image(s, &img) < 0) { free(digest); store_free(s); return 1; }

    printf("Successfully imported %s:%s\n", name, tag);
    printf("  Layer: %.*s (%.1f MB)\n", 19, digest, (double)layer_size/1024/1024);
    printf("  Digest: %s\n", img.digest ? img.digest : "");

    /* verify */
    Image *check = store_load_image(s, name, tag);
    if (!check) { fprintf(stderr,"verification failed\n"); free(digest); store_free(s); return 1; }

    /* integrity check */
    unsigned char *stored; size_t stored_len;
    if (store_read_layer(s, digest, &stored, &stored_len) == 0) {
        extern char *hash_bytes(const unsigned char*,size_t);
        char *verify_digest = hash_bytes(stored, stored_len);
        free(stored);
        if (verify_digest && strcmp(verify_digest, digest) != 0)
            fprintf(stderr,"layer integrity mismatch\n");
        free(verify_digest);
    }
    image_free(check);
    free(img.digest);
    free(digest);
    store_free(s);
    return 0;
}
