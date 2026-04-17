#include "engine.h"
#include "parser.h"
#include "cache.h"
#include "../util/hash.h"
#include "../util/tar.h"
#include "../store/layer.h"
#include "../container/run.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/stat.h>
#include <dirent.h>
#include <unistd.h>
#include <glob.h>
#include <errno.h>

struct Engine {
    Store  *store;
    Cache  *cache;
    char    context_path[4096];
    int     no_cache;

    /* build state */
    LayerRef *layers;
    int       layer_count;
    int       layer_cap;

    char    **env;
    int       env_count;
    int       env_cap;

    char      workdir[4096];
    char      config_state[8192];

    char **cmd;
    int    cmd_count;
};

static char *xstrdup(const char *s) { return s ? strdup(s) : strdup(""); }

Engine *engine_new(Store *s, const char *context_path, int no_cache) {
    Cache *c = cache_new(s);
    Engine *e = calloc(1, sizeof(Engine));
    e->store = s;
    e->cache = c;
    snprintf(e->context_path, sizeof(e->context_path), "%s", context_path);
    e->no_cache = no_cache;
    snprintf(e->workdir, sizeof(e->workdir), "/");
    return e;
}

void engine_free(Engine *e) {
    if (!e) return;
    cache_free(e->cache);
    free(e->layers);
    for (int i = 0; i < e->env_count; i++) free(e->env[i]);
    free(e->env);
    for (int i = 0; i < e->cmd_count; i++) free(e->cmd[i]);
    free(e->cmd);
    free(e);
}

/* ---- helpers ---- */
static void add_layer(Engine *e, LayerRef *lr) {
    if (e->layer_count >= e->layer_cap) {
        e->layer_cap = e->layer_cap ? e->layer_cap*2 : 8;
        e->layers = realloc(e->layers, e->layer_cap * sizeof(LayerRef));
    }
    e->layers[e->layer_count++] = *lr;
}

static void add_env(Engine *e, const char *kv) {
    if (e->env_count >= e->env_cap) {
        e->env_cap = e->env_cap ? e->env_cap*2 : 8;
        e->env = realloc(e->env, e->env_cap * sizeof(char*));
    }
    e->env[e->env_count++] = xstrdup(kv);
}

static char *chain_digest(Engine *e) {
    /* sha256 of all layer digests + config state */
    if (e->layer_count == 0 && e->config_state[0] == '\0') return xstrdup("sha256:empty");
    size_t total = 0;
    for (int i = 0; i < e->layer_count; i++) total += strlen(e->layers[i].digest) + 1;
    total += strlen(e->config_state) + 1;
    char *buf = malloc(total + 32);
    buf[0] = '\0';
    for (int i = 0; i < e->layer_count; i++) {
        if (i) strcat(buf, "+");
        strcat(buf, e->layers[i].digest);
    }
    if (e->config_state[0]) { strcat(buf, "+"); strcat(buf, e->config_state); }
    char prefix[16] = "chain:";
    size_t plen = strlen(prefix);
    memmove(buf + plen, buf, strlen(buf)+1);
    memcpy(buf, prefix, plen);
    return buf;
}

static void update_config_state(Engine *e) {
    char tmp[8192];
    snprintf(tmp, sizeof(tmp), "workdir:%s|env:", e->workdir);
    for (int i = 0; i < e->env_count; i++) {
        if (i) strncat(tmp, ",", sizeof(tmp)-strlen(tmp)-1);
        strncat(tmp, e->env[i], sizeof(tmp)-strlen(tmp)-1);
    }
    snprintf(e->config_state, sizeof(e->config_state), "%s", tmp);
}

/* ---- assemble rootfs ---- */
static char *assemble_rootfs(Engine *e) {
    char rootfs[256] = "/tmp/docksmith-rootfs-XXXXXX";
    if (!mkdtemp(rootfs)) { perror("mkdtemp"); return NULL; }
    for (int i = 0; i < e->layer_count; i++) {
        unsigned char *tar_data = NULL; size_t tar_len = 0;
        if (store_read_layer(e->store, e->layers[i].digest, &tar_data, &tar_len) < 0) {
            char rm[512]; snprintf(rm, sizeof(rm), "rm -rf %s", rootfs); system(rm);
            return NULL;
        }
        int rc = extract_tar(tar_data, tar_len, rootfs);
        free(tar_data);
        if (rc < 0) { char rm[512]; snprintf(rm,sizeof(rm),"rm -rf %s",rootfs); system(rm); return NULL; }
    }
    return xstrdup(rootfs);
}

static void ensure_workdir(Engine *e, const char *rootfs) {
    if (e->workdir[0]) {
        char fullpath[4096];
        snprintf(fullpath, sizeof(fullpath), "%s%s", rootfs, e->workdir);
        char *p = fullpath + 1;
        while (*p) {
            if (*p == '/') { *p = '\0'; mkdir(fullpath, 0755); *p = '/'; }
            p++;
        }
        mkdir(fullpath, 0755);
    }
}

static char *snapshot_dir(Engine *e __attribute__((unused)), const char *rootfs) {
    char snap[256] = "/tmp/docksmith-snap-XXXXXX";
    if (!mkdtemp(snap)) { perror("mkdtemp"); return NULL; }
    /* tar rootfs then extract into snap */
    Buffer tb = create_tar(rootfs);
    if (!tb.data) { char rm[512]; snprintf(rm,sizeof(rm),"rm -rf %s",snap); system(rm); return NULL; }
    int rc = extract_tar(tb.data, tb.len, snap);
    buffer_free(&tb);
    if (rc < 0) { char rm[512]; snprintf(rm,sizeof(rm),"rm -rf %s",snap); system(rm); return NULL; }
    return xstrdup(snap);
}

/* ---- hash build context ---- */
static char *hash_build_context(Engine *e, const char *args) {
    /* split args into src tokens (all but last) */
    char args_copy[4096];
    snprintf(args_copy, sizeof(args_copy), "%s", args);

    /* collect tokens */
    char *toks[256]; int ntok = 0;
    char *tok = strtok(args_copy, " \t");
    while (tok && ntok < 256) { toks[ntok++] = tok; tok = strtok(NULL, " \t"); }
    if (ntok < 2) return xstrdup("");

    /* srcs = toks[0..ntok-2] */
    size_t all_data_len = 0;
    unsigned char *all_data = NULL;

    for (int ti = 0; ti < ntok - 1; ti++) {
        char pattern[4096];
        snprintf(pattern, sizeof(pattern), "%s/%s", e->context_path, toks[ti]);
        glob_t gl; memset(&gl, 0, sizeof(gl));
        if (glob(pattern, 0, NULL, &gl) == 0) {
            for (size_t gi = 0; gi < gl.gl_pathc; gi++) {
                FILE *f = fopen(gl.gl_pathv[gi], "rb");
                if (!f) continue;
                fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
                all_data = realloc(all_data, all_data_len + sz);
                fread(all_data + all_data_len, 1, sz, f); fclose(f);
                all_data_len += sz;
            }
            globfree(&gl);
        }
    }
    char *h = hash_bytes(all_data ? all_data : (unsigned char*)"", all_data_len);
    free(all_data);
    return h;
}

/* ---- copy file/dir helpers ---- */
static int copy_file(const char *src, const char *dst) {
    FILE *in = fopen(src, "rb"); if (!in) return -1;
    struct stat st; stat(src, &st);
    FILE *out = fopen(dst, "wb"); if (!out) { fclose(in); return -1; }
    char buf[65536]; size_t n;
    while ((n = fread(buf,1,sizeof(buf),in)) > 0) fwrite(buf,1,n,out);
    fclose(in); fclose(out);
    chmod(dst, st.st_mode & 0777);
    return 0;
}

static int copy_dir(const char *src, const char *dst);
static int copy_dir(const char *src, const char *dst) {
    struct stat st; stat(src, &st);
    mkdir(dst, st.st_mode & 07777);
    DIR *d = opendir(src); if (!d) return -1;
    struct dirent *ent;
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name,".") || !strcmp(ent->d_name,"..")) continue;
        char sp[4096], dp[4096];
        snprintf(sp, sizeof(sp), "%s/%s", src, ent->d_name);
        snprintf(dp, sizeof(dp), "%s/%s", dst, ent->d_name);
        struct stat es; lstat(sp, &es);
        if (S_ISDIR(es.st_mode)) copy_dir(sp, dp);
        else copy_file(sp, dp);
    }
    closedir(d);
    return 0;
}

static void mkdir_p(const char *path) {
    char tmp[4096]; snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp+1; *p; p++) {
        if (*p=='/') { *p='\0'; mkdir(tmp,0755); *p='/'; }
    }
    mkdir(tmp, 0755);
}

/* ---- instruction executors ---- */
static int exec_FROM(Engine *e, const char *args) {
    /* parse name:tag */
    char name[256], tag[64];
    const char *colon = strchr(args, ':');
    if (colon) {
        size_t nl = colon - args;
        memcpy(name, args, nl); name[nl] = '\0';
        snprintf(tag, sizeof(tag), "%s", colon+1);
    } else {
        snprintf(name, sizeof(name), "%s", args);
        snprintf(tag, sizeof(tag), "latest");
    }

    Image *img = store_load_image(e->store, name, tag);
    if (!img) return -1;

    /* copy layers */
    free(e->layers); e->layer_count = 0; e->layer_cap = 0; e->layers = NULL;
    for (int i = 0; i < img->layer_count; i++) {
        LayerRef lr = {
            .digest     = xstrdup(img->layers[i].digest),
            .size       = img->layers[i].size,
            .created_by = xstrdup(img->layers[i].created_by)
        };
        add_layer(e, &lr);
    }
    /* inherit config */
    if (img->config.working_dir && img->config.working_dir[0])
        snprintf(e->workdir, sizeof(e->workdir), "%s", img->config.working_dir);
    for (int i = 0; i < img->config.env_count; i++)
        add_env(e, img->config.env[i]);

    printf(" -> Base image %s:%s (%d layers)\n", name, tag, img->layer_count);
    image_free(img);
    return 0;
}

static int exec_WORKDIR(Engine *e, const char *args) {
    char dir[4096]; snprintf(dir, sizeof(dir), "%s", args);
    /* trim */
    int l = strlen(dir);
    while (l > 0 && (dir[l-1]==' '||dir[l-1]=='\t')) dir[--l]='\0';
    if (!dir[0]) { fprintf(stderr,"WORKDIR requires a path\n"); return -1; }
    if (dir[0] != '/') {
        char joined[4096];
        snprintf(joined, sizeof(joined), "%s/%s", e->workdir, dir);
        snprintf(e->workdir, sizeof(e->workdir), "%s", joined);
    } else {
        snprintf(e->workdir, sizeof(e->workdir), "%s", dir);
    }
    printf(" -> Working directory: %s\n", e->workdir);
    return 0;
}

static int exec_ENV(Engine *e, const char *args) {
    /* support KEY=value and KEY value */
    if (strchr(args, '=')) {
        add_env(e, args);
    } else {
        char *sp = strchr(args, ' ');
        if (!sp) { fprintf(stderr,"ENV requires KEY=value or KEY value\n"); return -1; }
        *sp = '=';
        add_env(e, args);
        *sp = ' '; /* restore (args is const, make copy) */
        char kv[4096]; snprintf(kv, sizeof(kv), "%s", args);
        kv[sp - args] = '=';
        /* already added above - but with const cast issue; redo */
        free(e->env[--e->env_count]); /* remove last */
        add_env(e, kv);
    }
    printf(" -> Environment: %s\n", args);
    return 0;
}

static int exec_CMD(Engine *e, const char *args) {
    /* parse JSON array */
    char trimmed[4096];
    int l = strlen(args);
    while (l > 0 && (args[l-1]==' '||args[l-1]=='\t')) l--;
    memcpy(trimmed, args, l); trimmed[l]='\0';

    /* simple JSON array parser */
    if (trimmed[0] != '[') {
        fprintf(stderr, "CMD must be a JSON array, e.g. [\"cmd\",\"arg\"]\n"); return -1;
    }
    /* use cJSON */
    /* include cJSON inline here */
    extern void *cJSON_Parse(const char*);
    /* We use cJSON linked in; include from cache.c pattern */
    /* Simpler: parse manually */
    for (int i = 0; i < e->cmd_count; i++) free(e->cmd[i]);
    free(e->cmd); e->cmd = NULL; e->cmd_count = 0;
    /* strip [ and ] */
    char inner[4096];
    snprintf(inner, sizeof(inner), "%.*s", l-2, trimmed+1);
    /* tokenize by "," treating quoted strings */
    int cap = 8;
    e->cmd = calloc(cap, sizeof(char*));
    char *p = inner;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        if (*p == '"') {
            p++;
            char tok[1024]; int ti = 0;
            while (*p && *p != '"') {
                if (*p == '\\') p++;
                tok[ti++] = *p++;
            }
            tok[ti] = '\0';
            if (*p == '"') p++;
            if (e->cmd_count >= cap) { cap*=2; e->cmd = realloc(e->cmd, cap*sizeof(char*)); }
            e->cmd[e->cmd_count++] = xstrdup(tok);
        } else {
            char tok[1024]; int ti = 0;
            while (*p && *p != ',' && *p != ' ') tok[ti++] = *p++;
            tok[ti] = '\0';
            if (e->cmd_count >= cap) { cap*=2; e->cmd = realloc(e->cmd, cap*sizeof(char*)); }
            e->cmd[e->cmd_count++] = xstrdup(tok);
        }
    }
    printf(" -> Default command:");
    for (int i = 0; i < e->cmd_count; i++) printf(" %s", e->cmd[i]);
    printf("\n");
    return 0;
}

static int exec_COPY(Engine *e, const char *args) {
    char *rootfs = assemble_rootfs(e); if (!rootfs) return -1;
    ensure_workdir(e, rootfs);
    char *snapshot = snapshot_dir(e, rootfs);
    if (!snapshot) { char rm[512]; snprintf(rm,sizeof(rm),"rm -rf %s",rootfs); system(rm); free(rootfs); return -1; }

    /* parse srcs and dest */
    char args_copy[4096]; snprintf(args_copy, sizeof(args_copy), "%s", args);
    char *toks[256]; int ntok = 0;
    char *tok = strtok(args_copy, " \t");
    while (tok && ntok < 256) { toks[ntok++] = tok; tok = strtok(NULL, " \t"); }
    if (ntok < 2) {
        fprintf(stderr,"COPY requires <src> <dest>\n");
        goto cleanup;
    }

    {
        const char *dest = toks[ntok-1];
        char dest_path[4096];
        if (dest[0] == '/') snprintf(dest_path, sizeof(dest_path), "%s%s", rootfs, dest);
        else snprintf(dest_path, sizeof(dest_path), "%s%s/%s", rootfs, e->workdir, dest);

        for (int ti = 0; ti < ntok-1; ti++) {
            char pattern[4096];
            snprintf(pattern, sizeof(pattern), "%s/%s", e->context_path, toks[ti]);
            glob_t gl; memset(&gl,0,sizeof(gl));
            if (glob(pattern, 0, NULL, &gl) != 0) {
                fprintf(stderr,"COPY: no files match %s\n", toks[ti]);
                globfree(&gl);
                goto cleanup;
            }
            for (size_t gi = 0; gi < gl.gl_pathc; gi++) {
                const char *match = gl.gl_pathv[gi];
                char target[4096];
                /* if dest ends with / or multiple sources → copy into dir */
                int multi = (gl.gl_pathc > 1 || ntok > 2 || dest[strlen(dest)-1]=='/');
                if (multi) {
                    mkdir_p(dest_path);
                    const char *base = strrchr(match,'/'); base = base ? base+1 : match;
                    snprintf(target, sizeof(target), "%s/%s", dest_path, base);
                } else {
                    char parent[4096]; snprintf(parent, sizeof(parent), "%s", dest_path);
                    char *sl = strrchr(parent, '/'); if (sl) { *sl='\0'; mkdir_p(parent); }
                    snprintf(target, sizeof(target), "%s", dest_path);
                }
                struct stat st; lstat(match, &st);
                if (S_ISDIR(st.st_mode)) copy_dir(match, target);
                else copy_file(match, target);
            }
            globfree(&gl);
        }
    }

    {
        /* create delta layer */
        Buffer delta = create_tar_delta(snapshot, rootfs);
        char rm[512]; snprintf(rm,sizeof(rm),"rm -rf %s %s", snapshot, rootfs); system(rm);
        free(snapshot); free(rootfs);
        if (!delta.data) return -1;

        char *digest = NULL; long long sz = 0;
        if (store_write_layer(e->store, delta.data, delta.len, &digest, &sz) < 0) {
            buffer_free(&delta); return -1;
        }
        buffer_free(&delta);

        char created_by[4096+8]; snprintf(created_by, sizeof(created_by), "COPY %s", args);
        LayerRef lr = { .digest=digest, .size=sz, .created_by=xstrdup(created_by) };
        add_layer(e, &lr);
        printf(" -> Layer %.*s (%.1f KB)\n", 19, digest, (double)sz/1024);
        return 0;
    }

cleanup: {
    char rm[512]; snprintf(rm,sizeof(rm),"rm -rf %s %s", snapshot, rootfs); system(rm);
    free(snapshot); free(rootfs); return -1;
    }
}

static int exec_RUN(Engine *e, const char *args) {
    char *rootfs = assemble_rootfs(e); if (!rootfs) return -1;
    ensure_workdir(e, rootfs);
    char *snapshot = snapshot_dir(e, rootfs);
    if (!snapshot) { char rm[512]; snprintf(rm,sizeof(rm),"rm -rf %s",rootfs); system(rm); free(rootfs); return -1; }

    /* build env (NULL-terminated) */
    char **env_arr = calloc(e->env_count + 1, sizeof(char*));
    for (int i = 0; i < e->env_count; i++) env_arr[i] = e->env[i];
    env_arr[e->env_count] = NULL;
    char **environs = build_environment(env_arr, NULL);
    free(env_arr);

    char *cmd_slice[] = { "/bin/sh", "-c", (char*)args, NULL };
    int exit_code = container_isolate(rootfs, e->workdir, environs, cmd_slice);
    free_env_list(environs);

    if (exit_code != 0) {
        fprintf(stderr, "RUN command exited with code %d\n", exit_code);
        char rm[512]; snprintf(rm,sizeof(rm),"rm -rf %s %s", snapshot, rootfs); system(rm);
        free(snapshot); free(rootfs); return -1;
    }

    Buffer delta = create_tar_delta(snapshot, rootfs);
    char rm[512]; snprintf(rm,sizeof(rm),"rm -rf %s %s", snapshot, rootfs); system(rm);
    free(snapshot); free(rootfs);
    if (!delta.data) return -1;

    char *digest = NULL; long long sz = 0;
    if (store_write_layer(e->store, delta.data, delta.len, &digest, &sz) < 0) {
        buffer_free(&delta); return -1;
    }
    buffer_free(&delta);

    char created_by[4096+8]; snprintf(created_by, sizeof(created_by), "RUN %s", args);
    LayerRef lr = { .digest=digest, .size=sz, .created_by=xstrdup(created_by) };
    add_layer(e, &lr);
    return 0;
}

/* ---- Build ---- */
Image *engine_build(Engine *e, const char *docksmithfile_path,
                    const char *name, const char *tag) {
    InstructionList il = parse_docksmithfile(docksmithfile_path);
    if (!il.items) return NULL;

    printf("Building %s:%s from %s\n", name, tag, docksmithfile_path);
    int all_cache_hits = 1;
    struct timespec t0, t1;

    for (int i = 0; i < il.count; i++) {
        Instruction *inst = &il.items[i];
        clock_gettime(CLOCK_MONOTONIC, &t0);

        if (strcmp(inst->command, "FROM") == 0) {
            printf("\nStep %d/%d : %s %s\n", i+1, il.count, inst->command, inst->args);
            if (exec_FROM(e, inst->args) < 0) { instruction_list_free(&il); return NULL; }
            continue;
        }
        if (strcmp(inst->command, "WORKDIR") == 0 ||
            strcmp(inst->command, "ENV")     == 0 ||
            strcmp(inst->command, "CMD")     == 0) {
            printf("\nStep %d/%d : %s %s\n", i+1, il.count, inst->command, inst->args);
            int rc = 0;
            if      (strcmp(inst->command,"WORKDIR")==0) rc = exec_WORKDIR(e, inst->args);
            else if (strcmp(inst->command,"ENV"    )==0) rc = exec_ENV    (e, inst->args);
            else if (strcmp(inst->command,"CMD"    )==0) rc = exec_CMD    (e, inst->args);
            if (rc < 0) { instruction_list_free(&il); return NULL; }
            update_config_state(e);
            continue;
        }

        /* layer-producing: COPY, RUN */
        char *parent = chain_digest(e);
        char *content_hash = xstrdup("");
        if (strcmp(inst->command, "COPY") == 0)
            { free(content_hash); content_hash = hash_build_context(e, inst->args); }

        char inst_str[4096+64];
        snprintf(inst_str, sizeof(inst_str), "%s %s", inst->command, inst->args);
        char *cache_key = cache_compute_key(parent, inst_str, content_hash);
        free(parent); free(content_hash);

        /* check cache */
        if (!e->no_cache) {
            char *layer_digest = NULL;
            if (cache_lookup(e->cache, cache_key, &layer_digest)) {
                if (store_layer_exists(e->store, layer_digest)) {
                    unsigned char *td = NULL; size_t tl = 0;
                    if (store_read_layer(e->store, layer_digest, &td, &tl) == 0) {
                        free(td);
                        clock_gettime(CLOCK_MONOTONIC, &t1);
                        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec-t0.tv_nsec)/1e9;
                        printf("\nStep %d/%d : %s %s [CACHE HIT] %.2fs\n",
                               i+1, il.count, inst->command, inst->args, elapsed);
                        char created_by[4096+8];
                        snprintf(created_by, sizeof(created_by), "%s %s", inst->command, inst->args);
                        LayerRef lr = { .digest=layer_digest, .size=(long long)tl, .created_by=xstrdup(created_by) };
                        add_layer(e, &lr);
                        free(cache_key);
                        continue;
                    }
                }
                free(layer_digest);
            }
        }

        /* cache miss */
        all_cache_hits = 0;
        printf("\nStep %d/%d : %s %s", i+1, il.count, inst->command, inst->args);

        int rc = 0;
        if      (strcmp(inst->command,"COPY")==0) rc = exec_COPY(e, inst->args);
        else if (strcmp(inst->command,"RUN" )==0) rc = exec_RUN (e, inst->args);
        else { fprintf(stderr,"unknown instruction %s\n", inst->command); rc = -1; }
        if (rc < 0) { free(cache_key); instruction_list_free(&il); return NULL; }

        clock_gettime(CLOCK_MONOTONIC, &t1);
        double elapsed = (t1.tv_sec - t0.tv_sec) + (t1.tv_nsec-t0.tv_nsec)/1e9;
        printf(" [CACHE MISS] %.2fs\n", elapsed);

        /* store in cache */
        if (!e->no_cache && e->layer_count > 0) {
            LayerRef *last = &e->layers[e->layer_count-1];
            if (cache_store(e->cache, cache_key, last->digest) < 0)
                printf(" -> Warning: failed to cache layer\n");
        }
        free(cache_key);
    }
    instruction_list_free(&il);

    /* build final image */
    time_t now = time(NULL);
    struct tm *tm = gmtime(&now);
    char created[64];
    strftime(created, sizeof(created), "%Y-%m-%dT%H:%M:%SZ", tm);

    if (all_cache_hits) {
        Image *existing = store_load_image(e->store, name, tag);
        if (existing && existing->created && existing->created[0])
            snprintf(created, sizeof(created), "%s", existing->created);
        if (existing) image_free(existing);
    }

    Image *img = calloc(1, sizeof(Image));
    img->name    = xstrdup(name);
    img->tag     = xstrdup(tag);
    img->created = xstrdup(created);
    img->config.working_dir = xstrdup(e->workdir);
    img->config.env_count = e->env_count;
    img->config.env = calloc(e->env_count + 1, sizeof(char*));
    for (int i = 0; i < e->env_count; i++) img->config.env[i] = xstrdup(e->env[i]);
    img->config.cmd_count = e->cmd_count;
    img->config.cmd = calloc(e->cmd_count + 1, sizeof(char*));
    for (int i = 0; i < e->cmd_count; i++) img->config.cmd[i] = xstrdup(e->cmd[i]);
    img->layer_count = e->layer_count;
    img->layers = calloc(e->layer_count, sizeof(LayerRef));
    for (int i = 0; i < e->layer_count; i++) {
        img->layers[i].digest     = xstrdup(e->layers[i].digest);
        img->layers[i].size       = e->layers[i].size;
        img->layers[i].created_by = xstrdup(e->layers[i].created_by);
    }

    if (store_save_image(e->store, img) < 0) { image_free(img); return NULL; }

    printf("\nSuccessfully built %.*s %s:%s\n",
           (int)(img->digest && strlen(img->digest)>19 ? 19 : strlen(img->digest ? img->digest : "")),
           img->digest ? img->digest : "", name, tag);
    return img;
}
