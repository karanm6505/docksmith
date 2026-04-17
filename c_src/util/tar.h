#pragma once
#include <stddef.h>

/* Opaque buffer holding raw bytes */
typedef struct {
    unsigned char *data;
    size_t         len;
} Buffer;

void buffer_free(Buffer *b);

/* create_tar: recursively tar baseDir. Returns heap Buffer on success (caller frees),
   data=NULL on failure.  errno/msg printed to stderr. */
Buffer create_tar(const char *base_dir);

/* create_tar_delta: compare new_root vs old_root, tar only changed/new files.
   Returns heap Buffer (caller frees). data=NULL on failure. */
Buffer create_tar_delta(const char *old_root, const char *new_root);

/* extract_tar: extract raw tar bytes into dest_dir.
   Returns 0 on success, -1 on failure (message printed to stderr). */
int extract_tar(const unsigned char *data, size_t len, const char *dest_dir);
