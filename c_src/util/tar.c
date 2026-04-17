#include "tar.h"
#include "hash.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>

/* ---- simple POSIX tar implementation (ustar format) ---- */

#define TAR_BLOCK 512

typedef struct {
    char name[100];
    char mode[8];
    char uid[8];
    char gid[8];
    char size[12];
    char mtime[12];
    char checksum[8];
    char typeflag;
    char linkname[100];
    char magic[6];
    char version[2];
    char uname[32];
    char gname[32];
    char devmajor[8];
    char devminor[8];
    char prefix[155];
    char pad[12];
} TarHeader;

/* --- dynamic byte buffer --- */
typedef struct {
    unsigned char *data;
    size_t         len;
    size_t         cap;
} DynBuf;

static int dynbuf_append(DynBuf *b, const void *src, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 65536;
        while (nc < b->len + n) nc *= 2;
        unsigned char *nd = realloc(b->data, nc);
        if (!nd) return -1;
        b->data = nd;
        b->cap  = nc;
    }
    memcpy(b->data + b->len, src, n);
    b->len += n;
    return 0;
}

static int dynbuf_zero(DynBuf *b, size_t n) {
    if (b->len + n > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 65536;
        while (nc < b->len + n) nc *= 2;
        unsigned char *nd = realloc(b->data, nc);
        if (!nd) return -1;
        b->data = nd;
        b->cap  = nc;
    }
    memset(b->data + b->len, 0, n);
    b->len += n;
    return 0;
}

void buffer_free(Buffer *b) {
    if (b) { free(b->data); b->data = NULL; b->len = 0; }
}

/* --- ustar header helpers --- */
static unsigned int tar_checksum(TarHeader *hdr) {
    memset(hdr->checksum, ' ', 8);
    unsigned int sum = 0;
    unsigned char *p = (unsigned char*)hdr;
    for (int i = 0; i < TAR_BLOCK; i++) sum += p[i];
    return sum;
}

static int write_tar_header(DynBuf *b, const char *name, const struct stat *st,
                             char typeflag, const char *linkname) {
    TarHeader hdr;
    memset(&hdr, 0, sizeof(hdr));
    snprintf(hdr.name, sizeof(hdr.name), "%s", name);
    snprintf(hdr.mode, sizeof(hdr.mode), "%07o", (unsigned)(st->st_mode & 07777));
    snprintf(hdr.uid,  sizeof(hdr.uid),  "%07o", 0);
    snprintf(hdr.gid,  sizeof(hdr.gid),  "%07o", 0);
    snprintf(hdr.size, sizeof(hdr.size), "%011llo",
             (typeflag == '0' || typeflag == '\0') ? (unsigned long long)st->st_size : 0ULL);
    snprintf(hdr.mtime, sizeof(hdr.mtime), "%011o", 0); /* zero timestamps for reproducibility */
    hdr.typeflag = typeflag;
    if (linkname) snprintf(hdr.linkname, sizeof(hdr.linkname), "%s", linkname);
    memcpy(hdr.magic, "ustar", 5);
    memcpy(hdr.version, "00", 2);
    unsigned int csum = tar_checksum(&hdr);
    snprintf(hdr.checksum, sizeof(hdr.checksum), "%06o", csum);
    hdr.checksum[6] = '\0'; hdr.checksum[7] = ' ';
    return dynbuf_append(b, &hdr, TAR_BLOCK);
}

/* --- sorted directory listing (for reproducibility) --- */
typedef struct { char **names; int count; } DirList;
static int cmp_str(const void *a, const void *b) {
    return strcmp(*(const char**)a, *(const char**)b);
}
static DirList list_dir_sorted(const char *path) {
    DirList dl = {0};
    DIR *d = opendir(path);
    if (!d) return dl;
    struct dirent *ent;
    int cap = 0;
    while ((ent = readdir(d))) {
        if (!strcmp(ent->d_name, ".") || !strcmp(ent->d_name, "..")) continue;
        if (dl.count >= cap) {
            cap = cap ? cap*2 : 16;
            dl.names = realloc(dl.names, cap * sizeof(char*));
        }
        dl.names[dl.count++] = strdup(ent->d_name);
    }
    closedir(d);
    if (dl.count > 1)
        qsort(dl.names, dl.count, sizeof(char*), cmp_str);
    return dl;
}
static void free_dirlist(DirList *dl) {
    for (int i = 0; i < dl->count; i++) free(dl->names[i]);
    free(dl->names);
    dl->names = NULL; dl->count = 0;
}

/* --- recursive walk + write to DynBuf --- */
static int tar_walk(DynBuf *b, const char *abs_path, const char *rel_path) {
    struct stat st;
    if (lstat(abs_path, &st) != 0) { perror(abs_path); return -1; }

    if (S_ISDIR(st.st_mode)) {
        /* write directory entry (rel_path + /) */
        char name[4096];
        if (rel_path && rel_path[0])
            snprintf(name, sizeof(name), "%s/", rel_path);
        else
            name[0] = '\0';
        if (name[0] && write_tar_header(b, name, &st, '5', NULL) < 0) return -1;

        DirList dl = list_dir_sorted(abs_path);
        for (int i = 0; i < dl.count; i++) {
            char child_abs[4096], child_rel[4096];
            snprintf(child_abs, sizeof(child_abs), "%s/%s", abs_path, dl.names[i]);
            if (rel_path && rel_path[0])
                snprintf(child_rel, sizeof(child_rel), "%s/%s", rel_path, dl.names[i]);
            else
                snprintf(child_rel, sizeof(child_rel), "%s", dl.names[i]);
            if (tar_walk(b, child_abs, child_rel) < 0) {
                free_dirlist(&dl);
                return -1;
            }
        }
        free_dirlist(&dl);
    } else if (S_ISLNK(st.st_mode)) {
        char link[4096] = {0};
        readlink(abs_path, link, sizeof(link)-1);
        if (write_tar_header(b, rel_path, &st, '2', link) < 0) return -1;
    } else if (S_ISREG(st.st_mode)) {
        if (write_tar_header(b, rel_path, &st, '0', NULL) < 0) return -1;
        /* write file data padded to 512-byte blocks */
        FILE *f = fopen(abs_path, "rb");
        if (!f) { perror(abs_path); return -1; }
        unsigned char fbuf[65536];
        size_t written = 0;
        size_t n;
        while ((n = fread(fbuf, 1, sizeof(fbuf), f)) > 0) {
            if (dynbuf_append(b, fbuf, n) < 0) { fclose(f); return -1; }
            written += n;
        }
        fclose(f);
        /* pad to block boundary */
        size_t rem = written % TAR_BLOCK;
        if (rem) {
            size_t pad = TAR_BLOCK - rem;
            if (dynbuf_zero(b, pad) < 0) return -1;
        }
    }
    return 0;
}

Buffer create_tar(const char *base_dir) {
    Buffer result = {0};
    DynBuf b = {0};
    if (tar_walk(&b, base_dir, "") < 0) { free(b.data); return result; }
    /* two 512-byte zero blocks end-of-archive */
    if (dynbuf_zero(&b, 2 * TAR_BLOCK) < 0) { free(b.data); return result; }
    result.data = b.data;
    result.len  = b.len;
    return result;
}

/* --- delta --- */
typedef struct { char **paths; int count, cap; } PathList;
static void pathlist_add(PathList *pl, const char *p) {
    if (pl->count >= pl->cap) {
        pl->cap = pl->cap ? pl->cap*2 : 16;
        pl->paths = realloc(pl->paths, pl->cap * sizeof(char*));
    }
    pl->paths[pl->count++] = strdup(p);
}
static void pathlist_free(PathList *pl) {
    for (int i = 0; i < pl->count; i++) free(pl->paths[i]);
    free(pl->paths); pl->paths=NULL; pl->count=0; pl->cap=0;
}

static int walk_delta(const char *new_root, const char *old_root,
                      const char *rel, PathList *changed) {
    char new_abs[4096], old_abs[4096];
    snprintf(new_abs, sizeof(new_abs), "%s/%s", new_root, rel);
    snprintf(old_abs, sizeof(old_abs), "%s/%s", old_root, rel);

    struct stat ns, os_;
    if (lstat(new_abs, &ns) != 0) return 0; /* shouldn't happen */
    int old_exists = (lstat(old_abs, &os_) == 0);

    if (!old_exists) { pathlist_add(changed, rel[0] ? rel : "."); goto descend; }

    /* type changed */
    if ((ns.st_mode & S_IFMT) != (os_.st_mode & S_IFMT)) {
        pathlist_add(changed, rel[0] ? rel : "."); goto descend;
    }
    if (S_ISDIR(ns.st_mode)) goto descend;
    if (S_ISLNK(ns.st_mode)) {
        char nl[4096]={0}, ol[4096]={0};
        readlink(new_abs, nl, sizeof(nl)-1);
        readlink(old_abs, ol, sizeof(ol)-1);
        if (strcmp(nl, ol)) pathlist_add(changed, rel);
        return 0;
    }
    /* regular file: compare sizes then hashes */
    if (ns.st_size != os_.st_size) { pathlist_add(changed, rel); return 0; }
    char *nh = hash_file(new_abs), *oh = hash_file(old_abs);
    if (!nh || !oh || strcmp(nh, oh)) pathlist_add(changed, rel);
    free(nh); free(oh);
    return 0;

descend:
    if (S_ISDIR(ns.st_mode)) {
        DirList dl = list_dir_sorted(new_abs);
        for (int i = 0; i < dl.count; i++) {
            char child_rel[4096];
            if (rel[0])
                snprintf(child_rel, sizeof(child_rel), "%s/%s", rel, dl.names[i]);
            else
                snprintf(child_rel, sizeof(child_rel), "%s", dl.names[i]);
            walk_delta(new_root, old_root, child_rel, changed);
        }
        free_dirlist(&dl);
    }
    return 0;
}

Buffer create_tar_delta(const char *old_root, const char *new_root) {
    Buffer result = {0};
    PathList changed = {0};
    /* walk new root relative paths */
    DirList dl = list_dir_sorted(new_root);
    for (int i = 0; i < dl.count; i++)
        walk_delta(new_root, old_root, dl.names[i], &changed);
    free_dirlist(&dl);

    if (changed.count == 0) {
        /* empty tar */
        DynBuf b = {0};
        dynbuf_zero(&b, 2 * TAR_BLOCK);
        result.data = b.data; result.len = b.len;
        pathlist_free(&changed);
        return result;
    }

    /* tar only the changed paths from new_root */
    DynBuf b = {0};
    for (int i = 0; i < changed.count; i++) {
        char abs[4096];
        snprintf(abs, sizeof(abs), "%s/%s", new_root, changed.paths[i]);
        if (tar_walk(&b, abs, changed.paths[i]) < 0) {
            free(b.data); pathlist_free(&changed); return result;
        }
    }
    dynbuf_zero(&b, 2 * TAR_BLOCK);
    result.data = b.data; result.len = b.len;
    pathlist_free(&changed);
    return result;
}

/* --- extract --- */
static int safe_mkdir_p(const char *path, mode_t mode) {
    char tmp[4096];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(tmp, mode);
            *p = '/';
        }
    }
    return mkdir(tmp, mode);
}

int extract_tar(const unsigned char *data, size_t len, const char *dest_dir) {
    size_t off = 0;
    while (off + TAR_BLOCK <= len) {
        TarHeader *hdr = (TarHeader*)(data + off);
        off += TAR_BLOCK;
        if (hdr->name[0] == '\0') { off += TAR_BLOCK; continue; } /* end block pair */

        /* null-terminate name */
        char name[256];
        snprintf(name, sizeof(name), "%s", hdr->name);
        /* strip leading ./ or / */
        char *np = name;
        while (*np == '/' || (np[0] == '.' && np[1] == '/')) np += (np[0]=='.'?2:1);
        if (!np[0]) continue; /* skip root entry */

        /* path traversal check */
        char target[4096];
        snprintf(target, sizeof(target), "%s/%s", dest_dir, np);

        unsigned long long file_size = strtoull(hdr->size, NULL, 8);
        mode_t mode = (mode_t)strtoul(hdr->mode, NULL, 8);

        switch (hdr->typeflag) {
            case '5': { /* directory */
                safe_mkdir_p(target, mode | 0755);
                break;
            }
            case '0': case '\0': { /* regular file */
                /* ensure parent */
                char parent[4096];
                snprintf(parent, sizeof(parent), "%s", target);
                char *slash = strrchr(parent, '/');
                if (slash) { *slash = '\0'; safe_mkdir_p(parent, 0755); }
                FILE *f = fopen(target, "wb");
                if (!f) { perror(target); return -1; }
                size_t remaining = (size_t)file_size;
                while (remaining > 0 && off < len) {
                    size_t chunk = remaining < TAR_BLOCK ? remaining : TAR_BLOCK;
                    if (remaining <= TAR_BLOCK) {
                        fwrite(data + off, 1, remaining, f);
                    } else {
                        fwrite(data + off, 1, TAR_BLOCK, f);
                    }
                    off += TAR_BLOCK;
                    remaining -= chunk;
                }
                fclose(f);
                chmod(target, mode);
                continue; /* off already advanced */
            }
            case '2': { /* symlink */
                char linkname[256];
                snprintf(linkname, sizeof(linkname), "%s", hdr->linkname);
                remove(target);
                char parent[4096];
                snprintf(parent, sizeof(parent), "%s", target);
                char *slash = strrchr(parent, '/');
                if (slash) { *slash = '\0'; safe_mkdir_p(parent, 0755); }
                symlink(linkname, target);
                break;
            }
            case '1': { /* hard link */
                char linkname[256];
                snprintf(linkname, sizeof(linkname), "%s", hdr->linkname);
                char link_target[4096];
                char *lnp = linkname;
                while (*lnp == '/') lnp++;
                snprintf(link_target, sizeof(link_target), "%s/%s", dest_dir, lnp);
                remove(target);
                char parent[4096];
                snprintf(parent, sizeof(parent), "%s", target);
                char *slash = strrchr(parent, '/');
                if (slash) { *slash = '\0'; safe_mkdir_p(parent, 0755); }
                link(link_target, target);
                break;
            }
        }
        /* advance past file data blocks */
        size_t blocks = ((size_t)file_size + TAR_BLOCK - 1) / TAR_BLOCK;
        off += blocks * TAR_BLOCK;
    }
    return 0;
}
