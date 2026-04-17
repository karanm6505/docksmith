#pragma once
#include "../store/store.h"
#include "../store/image.h"

typedef struct Engine Engine;

Engine *engine_new(Store *s, const char *context_path, int no_cache);
void    engine_free(Engine *e);

/* Build: parses the Docksmithfile and produces an image.
   Returns heap Image* on success (caller must image_free), NULL on error. */
Image  *engine_build(Engine *e, const char *docksmithfile_path,
                     const char *name, const char *tag);
