#include "cmd/commands.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *USAGE =
"Docksmith — a simplified Docker-like build and runtime system\n"
"\n"
"Usage:\n"
"  docksmith <command> [arguments]\n"
"\n"
"Commands:\n"
"  build     Build an image from a Docksmithfile\n"
"  run       Run a command in a new container\n"
"  images    List images\n"
"  rmi       Remove an image and its layers\n"
"  cache     Show build cache entries\n"
"  import    Import a base image from a rootfs tarball\n"
"\n"
"Build Usage:\n"
"  docksmith build -t <name:tag> [-f Docksmithfile] [--no-cache] [context_dir]\n"
"\n"
"Run Usage:\n"
"  docksmith run [-e KEY=VALUE] <image>[:<tag>] [command...]\n"
"\n"
"Import Usage:\n"
"  docksmith import <name>[:<tag>] <rootfs.tar>\n";

int main(int argc, char **argv) {
    if (argc < 2) { fputs(USAGE, stdout); return 1; }

    const char *command = argv[1];
    int sub_argc = argc - 2;
    char **sub_argv = argv + 2;

    if      (strcmp(command, "build" ) == 0) return cmd_build (sub_argc, sub_argv);
    else if (strcmp(command, "run"   ) == 0) return cmd_run   (sub_argc, sub_argv);
    else if (strcmp(command, "images") == 0) return cmd_images(sub_argc, sub_argv);
    else if (strcmp(command, "rmi"   ) == 0) return cmd_rmi   (sub_argc, sub_argv);
    else if (strcmp(command, "cache" ) == 0) return cmd_cache (sub_argc, sub_argv);
    else if (strcmp(command, "import") == 0) return cmd_import(sub_argc, sub_argv);
    else if (strcmp(command, "help"  ) == 0 ||
             strcmp(command, "-h"    ) == 0 ||
             strcmp(command, "--help") == 0) {
        fputs(USAGE, stdout); return 0;
    } else {
        fprintf(stderr, "Unknown command: %s\n\n", command);
        fputs(USAGE, stdout);
        return 1;
    }
}
