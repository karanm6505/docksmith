#pragma once
#include <stddef.h>

/* hash_bytes: returns a malloc'd string "sha256:<hex64>" of the data,
   or NULL on allocation failure. Caller must free. */
char *hash_bytes(const unsigned char *data, size_t len);

/* hash_file: like hash_bytes but reads the file at path.
   Returns NULL on error. Caller must free. */
char *hash_file(const char *path);
