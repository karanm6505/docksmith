# Docksmith C Binary Commands

This file lists actual commands for the C binary.

## Binary

- C binary name: docksmith_c
- Build it:

```bash
make
```

- Run help:

```bash
./docksmith_c --help
```

## Global Usage

```bash
./docksmith_c <command> [arguments]
```

Commands:

- build
- run
- images
- rmi
- cache
- import
- help

## build

Build an image from a Docksmithfile.

```bash
./docksmith_c build -t <name:tag> [-f Docksmithfile] [--no-cache] [context_dir]
```

Notes:

- -t or --tag is required.
- If no context_dir is given, current directory is used.
- If -f is relative, it is resolved against context_dir.
- If tag is omitted in -t value, tag defaults to latest.

Examples:

```bash
./docksmith_c build -t sample:latest
./docksmith_c build -t myapp:v1 -f Docksmithfile .
./docksmith_c build -t myapp:v2 --no-cache ./sample-app
```

## run

Run a command in a new container.

```bash
./docksmith_c run [-e KEY=VALUE] <image>[:<tag>] [command...]
```

Notes:

- You can pass -e multiple times.
- If image tag is omitted, tag defaults to latest.
- If [command...] is omitted, image default command is used.

Examples:

```bash
./docksmith_c run sample:latest
./docksmith_c run -e GREETING=hello sample:latest /bin/sh /app.sh
./docksmith_c run -e A=1 -e B=2 sample /bin/sh -c "echo $A $B"
```

## images

List images.

```bash
./docksmith_c images
```

## rmi

Remove an image and attempt to remove its layers.

```bash
./docksmith_c rmi <image>[:<tag>]
```

Notes:

- If tag is omitted, tag defaults to latest.

Examples:

```bash
./docksmith_c rmi sample:latest
./docksmith_c rmi sample
```

## cache

Show build cache entries.

```bash
./docksmith_c cache
```

## import

Import a base image from a rootfs tarball.

```bash
./docksmith_c import <name>[:<tag>] <rootfs.tar>
```

Notes:

- If tag is omitted, tag defaults to latest.

Examples:

```bash
./docksmith_c import alpine:3.19 alpine-minirootfs-3.19.1-x86_64.tar
./docksmith_c import base ./rootfs.tar
```

## Typical End-to-End Flow

```bash
make
./docksmith_c import alpine:3.19 alpine-minirootfs-3.19.1-x86_64.tar
./docksmith_c build -t demo:v1 -f Docksmithfile .
./docksmith_c images
./docksmith_c run -e GREETING=hello demo:v1 /bin/sh /app.sh
./docksmith_c cache
```
