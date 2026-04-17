# Docksmith

A simplified Docker-like container build and runtime system written in C. Docksmith lets you build layered images from a `Docksmithfile`, import rootfs tarballs as base images, and run isolated containers using `chroot` + `execve` on Linux.

## Features

- **Image building** — parse a `Docksmithfile` (supports `FROM`, `COPY`, `RUN`, `WORKDIR`, `ENV`, `CMD`) and produce a layered image stored locally
- **Layer-based storage** — content-addressable layer store with SHA-256 digests
- **Build cache** — skip unchanged layers on rebuild; bypass with `--no-cache`
- **Container runtime** — isolate processes via `chroot` + `execve` (Linux kernel required)
- **Image management** — list, remove, and inspect stored images
- **rootfs import** — bootstrap a base image from any rootfs tarball (e.g. Alpine minirootfs)

## Requirements

| Dependency | Notes |
|---|---|
| GCC (C11) | `gcc` with `-std=gnu11` |
| OpenSSL (`libssl`, `libcrypto`) | SHA-256 hashing for layer digests |
| Linux kernel | Required only for `run`; build/import work on any OS |

Install OpenSSL on macOS via Homebrew: `brew install openssl`

## Building

```bash
make
```

This produces the `docksmith_c` binary.

```bash
make clean   # remove object files and binary
```

## Usage

```
./docksmith_c <command> [arguments]
```

### Commands

| Command | Description |
|---|---|
| `build` | Build an image from a Docksmithfile |
| `run` | Run a command in a new container |
| `images` | List stored images |
| `rmi` | Remove an image and its layers |
| `cache` | Show build cache entries |
| `import` | Import a base image from a rootfs tarball |

---

### `build`

Build an image from a `Docksmithfile`.

```bash
./docksmith_c build -t <name>[:<tag>] [-f Docksmithfile] [--no-cache] [context_dir]
```

- `-t` / `--tag` is required. If tag is omitted, it defaults to `latest`.
- `context_dir` defaults to the current directory.
- Relative `-f` paths are resolved against `context_dir`.
- `--no-cache` forces all layers to be rebuilt.

```bash
./docksmith_c build -t sample:latest
./docksmith_c build -t myapp:v1 -f Docksmithfile .
./docksmith_c build -t myapp:v2 --no-cache ./sample-app
```

---

### `run`

Run a command inside a container built from a stored image.

```bash
./docksmith_c run [-e KEY=VALUE] <image>[:<tag>] [command...]
```

- `-e` sets an environment variable; can be passed multiple times.
- If tag is omitted, defaults to `latest`.
- If no command is given, the image's `CMD` is used.
- **Requires a Linux host kernel** (`chroot` + `execve`). Will not work on macOS directly.
- Imported rootfs binaries must match the host CPU architecture.

```bash
./docksmith_c run sample:latest
./docksmith_c run -e GREETING=hello sample:latest /bin/sh /app/main.sh
./docksmith_c run -e A=1 -e B=2 sample /bin/sh -c "echo $A $B"
```

---

### `images`

List all stored images with their tags, digests, layer counts, and sizes.

```bash
./docksmith_c images
```

---

### `rmi`

Remove an image and attempt to delete its layers.

```bash
./docksmith_c rmi <image>[:<tag>]
```

If tag is omitted, defaults to `latest`.

```bash
./docksmith_c rmi sample:latest
./docksmith_c rmi sample
```

---

### `cache`

Display all build cache entries (instruction hash → layer digest mappings).

```bash
./docksmith_c cache
```

---

### `import`

Import a base image from a rootfs tarball. This is how you provide a `FROM` image.

```bash
./docksmith_c import <name>[:<tag>] <rootfs.tar>
```

If tag is omitted, defaults to `latest`.

```bash
./docksmith_c import alpine:3.19 alpine-minirootfs-3.19.1-x86_64.tar
./docksmith_c import base ./rootfs.tar
```

An Alpine minirootfs tarball is included in this repo (`alpine-minirootfs-3.19.1-x86_64.tar`) for x86-64 hosts.

---

## Typical End-to-End Flow

```bash
# 1. Build the binary
make

# 2. Import a base rootfs
./docksmith_c import alpine:3.19 alpine-minirootfs-3.19.1-x86_64.tar

# 3. Build an image from the Docksmithfile
./docksmith_c build -t demo:v1 -f Docksmithfile .

# 4. List images
./docksmith_c images

# 5. Run the container (Linux only)
./docksmith_c run -e GREETING=hello demo:v1 /bin/sh /app/main.sh

# 6. Inspect the build cache
./docksmith_c cache
```

## Docksmithfile Syntax

The `Docksmithfile` uses a subset of Dockerfile syntax:

| Instruction | Description |
|---|---|
| `FROM <image>[:<tag>]` | Set the base image (must already be imported or built) |
| `WORKDIR <path>` | Set the working directory inside the container |
| `COPY <src> <dest>` | Copy files from the build context into the image |
| `RUN <command>` | Execute a command during the build (Linux only) |
| `ENV <KEY>=<VALUE>` | Set an environment variable in the image |
| `CMD ["cmd", "arg1", ...]` | Set the default command to run in the container |

Example `Docksmithfile`:

```dockerfile
FROM alpine:latest
WORKDIR /app
COPY main.sh /app/main.sh
RUN chmod +x /app/main.sh
ENV GREETING=hello
CMD ["sh", "/app/main.sh"]
```

## Project Structure

```
docksmith/
├── c_src/
│   ├── main.c              # Entry point and command dispatch
│   ├── build/
│   │   ├── parser.c/h      # Docksmithfile parser
│   │   ├── engine.c/h      # Build engine (layer creation, RUN execution)
│   │   └── cache.c/h       # Build cache (instruction hash → layer digest)
│   ├── container/
│   │   └── run.c/h         # Container runtime (chroot + execve)
│   ├── store/
│   │   ├── store.c/h       # Local image/layer store
│   │   ├── image.c/h       # Image metadata and serialization
│   │   └── layer.c/h       # Layer read/write operations
│   ├── util/
│   │   ├── hash.c/h        # SHA-256 hashing via OpenSSL
│   │   └── tar.c/h         # Tar creation and extraction
│   └── cmd/
│       └── commands.c/h    # CLI command handlers
├── vendor/
│   └── cjson/              # cJSON library for JSON serialization
├── Docksmithfile           # Example Docksmithfile
├── main.sh                 # Example container entrypoint script
├── Makefile
└── alpine-minirootfs-3.19.1-x86_64.tar  # Alpine base rootfs (x86-64)
```
