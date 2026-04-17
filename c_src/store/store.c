#include "store.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <unistd.h>
#include <pwd.h>

struct Store {
    char root[4096];
    char images_dir[4096];
    char layers_dir[4096];
    char cache_dir[4096];
    int  uid;  /* -1 if not needed */
    int  gid;
};

static int mkdirp(const char *path) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
            *p = '/';
        }
    }
    if (mkdir(tmp, 0755) && errno != EEXIST) return -1;
    return 0;
}

Store *store_new(void) {
    Store *s = calloc(1, sizeof(Store));
    if (!s) { perror("calloc"); return NULL; }
    s->uid = -1;
    s->gid = -1;

    char home[4096];
    const char *sudo_user = getenv("SUDO_USER");
    if (sudo_user && sudo_user[0]) {
        struct passwd *pw = getpwnam(sudo_user);
        if (!pw) {
            fprintf(stderr, "lookup SUDO_USER %s: not found\n", sudo_user);
            free(s); return NULL;
        }
        snprintf(home, sizeof(home), "%s", pw->pw_dir);
        s->uid = (int)pw->pw_uid;
        s->gid = (int)pw->pw_gid;
    } else {
        const char *h = getenv("HOME");
        if (!h) {
            struct passwd *pw = getpwuid(getuid());
            h = pw ? pw->pw_dir : "/tmp";
        }
        snprintf(home, sizeof(home), "%s", h);
    }

    snprintf(s->root,       sizeof(s->root),       "%s/.docksmith",        home);
    snprintf(s->images_dir, sizeof(s->images_dir), "%s/.docksmith/images", home);
    snprintf(s->layers_dir, sizeof(s->layers_dir), "%s/.docksmith/layers", home);
    snprintf(s->cache_dir,  sizeof(s->cache_dir),  "%s/.docksmith/cache",  home);

    const char *dirs[] = { s->root, s->images_dir, s->layers_dir, s->cache_dir };
    for (int i = 0; i < 4; i++) {
        if (mkdirp(dirs[i]) < 0) {
            fprintf(stderr, "create dir %s: %s\n", dirs[i], strerror(errno));
            free(s); return NULL;
        }
        store_fix_ownership(s, dirs[i]);
    }
    return s;
}

void store_free(Store *s) { free(s); }

const char *store_root(const Store *s)       { return s->root;       }
const char *store_images_dir(const Store *s) { return s->images_dir; }
const char *store_layers_dir(const Store *s) { return s->layers_dir; }
const char *store_cache_dir(const Store *s)  { return s->cache_dir;  }

void store_fix_ownership(const Store *s, const char *path) {
    if (s->uid >= 0 && s->gid >= 0)
        chown(path, (uid_t)s->uid, (gid_t)s->gid);
}
