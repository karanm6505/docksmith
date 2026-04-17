#pragma once
#include "../store/store.h"
#include "../store/image.h"

/* Run: start a container from an image.
   cmd_override: NULL-terminated array of strings (may be NULL to use image CMD).
   env_overrides: NULL-terminated "KEY=VALUE" strings (may be NULL). */
int container_run(Store *s,
                  const char *name, const char *tag,
                  char **cmd_override,
                  char **env_overrides);

/* Isolate: shared isolation primitive (chroot + exec).
   Returns exit code, or -1 on error. */
int container_isolate(const char *rootfs, const char *workdir,
                      char **environ_list, char **cmd_slice);

/* BuildEnvironment: construct the env var list.
   image_env: NULL-terminated array; overrides: NULL-terminated array (may be NULL).
   Returns heap NULL-terminated array of "KEY=VALUE" strings. Caller must free. */
char **build_environment(char **image_env, char **overrides);
void   free_env_list(char **env);
