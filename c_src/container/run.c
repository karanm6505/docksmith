#include "run.h"
#include "../util/tar.h"
#include "../store/layer.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <errno.h>
#include <sys/utsname.h>

/* ---- helpers ---- */
static char *xstrdup(const char *s) { return s ? strdup(s) : strdup(""); }

static char *find_executable(const char *rootfs, const char *exe, char **env_list) {
    if (exe[0] == '/') {
        char abs[4096];
        snprintf(abs, sizeof(abs), "%s%s", rootfs, exe);
        struct stat st;
        if (stat(abs, &st) == 0 && !S_ISDIR(st.st_mode)) return xstrdup(exe);
        return NULL;
    }
    /* search PATH in env */
    const char *path_val = "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin";
    for (int i = 0; env_list && env_list[i]; i++)
        if (strncmp(env_list[i], "PATH=", 5) == 0) {
            path_val = env_list[i] + 5; break;
        }
    char path_copy[4096];
    snprintf(path_copy, sizeof(path_copy), "%s", path_val);
    char *dir = strtok(path_copy, ":");
    while (dir) {
        char candidate[4096];
        snprintf(candidate, sizeof(candidate), "%s%s/%s", rootfs, dir, exe);
        struct stat st;
        if (stat(candidate, &st) == 0 && !S_ISDIR(st.st_mode)) {
            char rel[4096];
            snprintf(rel, sizeof(rel), "%s/%s", dir, exe);
            return xstrdup(rel);
        }
        dir = strtok(NULL, ":");
    }
    return NULL;
}

typedef struct { char *k; char *v; } KV;

static void kv_set(KV **kvp, int *countp, int *capp, const char *k, const char *v) {
    for (int i = 0; i < *countp; i++)
        if (strcmp((*kvp)[i].k, k) == 0) {
            free((*kvp)[i].v); (*kvp)[i].v = xstrdup(v); return;
        }
    if (*countp >= *capp) {
        *capp = *capp ? *capp*2 : 32;
        *kvp = realloc(*kvp, (size_t)*capp * sizeof(KV));
    }
    (*kvp)[*countp].k = xstrdup(k);
    (*kvp)[*countp].v = xstrdup(v);
    (*countp)++;
}

char **build_environment(char **image_env, char **overrides) {
    KV *kv = NULL; int count = 0, cap = 0;
    kv_set(&kv, &count, &cap, "PATH", "/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin");
    kv_set(&kv, &count, &cap, "HOME", "/root");
    kv_set(&kv, &count, &cap, "TERM", "xterm");
    for (int i = 0; image_env && image_env[i]; i++) {
        char *eq = strchr(image_env[i], '='); if (!eq) continue;
        char k[256]; int klen = (int)(eq - image_env[i]);
        if (klen >= (int)sizeof(k)) continue;
        memcpy(k, image_env[i], klen); k[klen] = '\0';
        kv_set(&kv, &count, &cap, k, eq+1);
    }
    for (int i = 0; overrides && overrides[i]; i++) {
        char *eq = strchr(overrides[i], '='); if (!eq) continue;
        char k[256]; int klen = (int)(eq - overrides[i]);
        if (klen >= (int)sizeof(k)) continue;
        memcpy(k, overrides[i], klen); k[klen] = '\0';
        kv_set(&kv, &count, &cap, k, eq+1);
    }
    char **env = calloc((size_t)(count + 1), sizeof(char*));
    for (int i = 0; i < count; i++) {
        size_t len = strlen(kv[i].k) + 1 + strlen(kv[i].v) + 1;
        env[i] = malloc(len);
        snprintf(env[i], len, "%s=%s", kv[i].k, kv[i].v);
        free(kv[i].k); free(kv[i].v);
    }
    env[count] = NULL; free(kv);
    return env;
}

void free_env_list(char **env) {
    if (!env) return;
    for (int i = 0; env[i]; i++) free(env[i]);
    free(env);
}

int container_isolate(const char *rootfs, const char *workdir,
                      char **environ_list, char **cmd_slice) {
    char *exec_path = find_executable(rootfs, cmd_slice[0], environ_list);
    const char *actual_exec = exec_path ? exec_path : cmd_slice[0];

    pid_t pid = fork();
    if (pid < 0) { perror("fork"); free(exec_path); return -1; }
    if (pid == 0) {
        /* child: chroot then exec */
        if (chroot(rootfs) != 0) { perror("chroot"); _exit(1); }
        if (workdir && workdir[0])
            chdir(workdir);
        else
            chdir("/");
        execve(actual_exec, cmd_slice, environ_list);
        if (errno == ENOEXEC) {
            fprintf(stderr,
                    "%s: Exec format error (this usually means the binary is not executable on this host: unsupported OS or CPU architecture mismatch)\n",
                    actual_exec);
        }
        perror(actual_exec);
        _exit(127);
    }
    free(exec_path);

    int status;
    if (waitpid(pid, &status, 0) < 0) { perror("waitpid"); return -1; }
    if (WIFEXITED(status)) return WEXITSTATUS(status);
    if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
    return -1;
}

/* helper: extract all image layers into rootfs dir */
static int extract_image_layers(Store *s, const Image *img, const char *rootfs) {
    for (int i = 0; i < img->layer_count; i++) {
        unsigned char *tar_data = NULL;
        size_t tar_len = 0;
        if (store_read_layer(s, img->layers[i].digest, &tar_data, &tar_len) < 0)
            return -1;
        int rc = extract_tar(tar_data, tar_len, rootfs);
        free(tar_data);
        if (rc < 0) return -1;
    }
    return 0;
}

int container_run(Store *s, const char *name, const char *tag,
                  char **cmd_override, char **env_overrides) {
#ifndef __linux__
    struct utsname u;
    if (uname(&u) == 0) {
        fprintf(stderr,
                "docksmith run requires a Linux host kernel (current host: %s %s).\n"
                "Hint: run docksmith_c on Linux, or use a Linux VM/container runtime.\n",
                u.sysname, u.machine);
    } else {
        fprintf(stderr,
                "docksmith run requires a Linux host kernel.\n"
                "Hint: run docksmith_c on Linux, or use a Linux VM/container runtime.\n");
    }
    return -1;
#endif

    Image *img = store_load_image(s, name, tag);
    if (!img) return -1;

    char **cmd_slice = img->config.cmd;
    int   cmd_count  = img->config.cmd_count;
    if (cmd_override && cmd_override[0]) {
        cmd_slice = cmd_override;
        cmd_count = 0;
        for (int i = 0; cmd_override[i]; i++) cmd_count++;
    }
    if (cmd_count == 0) {
        fprintf(stderr, "no command specified and image %s:%s has no CMD defined\n", name, tag);
        image_free(img); return -1;
    }

    /* create temp rootfs */
    char rootfs[256] = "/tmp/docksmith-container-XXXXXX";
    if (!mkdtemp(rootfs)) { perror("mkdtemp"); image_free(img); return -1; }

    printf("Preparing container from %s:%s (%d layers)...\n", name, tag, img->layer_count);
    if (extract_image_layers(s, img, rootfs) < 0) {
        image_free(img);
        /* cleanup */
        char rm_cmd[512]; snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", rootfs);
        system(rm_cmd);
        return -1;
    }

    char **environ_list = build_environment(img->config.env, env_overrides);

    const char *workdir = img->config.working_dir;
    if (!workdir || !workdir[0]) workdir = "/";
    char wdir_abs[4096];
    snprintf(wdir_abs, sizeof(wdir_abs), "%s%s", rootfs, workdir);
    mkdir(wdir_abs, 0755);

    /* build null-terminated cmd slice from image config */
    char **execv = calloc(cmd_count + 1, sizeof(char*));
    for (int i = 0; i < cmd_count; i++) execv[i] = cmd_slice[i];
    execv[cmd_count] = NULL;

    printf("Running: ");
    for (int i = 0; execv[i]; i++) printf("%s%s", i?(" "):"", execv[i]);
    printf("\n");

    int exit_code = container_isolate(rootfs, workdir, environ_list, execv);
    free(execv);
    free_env_list(environ_list);
    image_free(img);

    /* cleanup rootfs */
    char rm_cmd[512];
    snprintf(rm_cmd, sizeof(rm_cmd), "rm -rf %s", rootfs);
    system(rm_cmd);

    printf("Container exited with code %d\n", exit_code);
    if (exit_code != 0) exit(exit_code);
    return 0;
}
