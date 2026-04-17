CC      = gcc
CFLAGS  = -std=gnu11 -O2 -Wall -Wextra \
           -Ivendor/cjson \
           -Ic_src \
           $(shell pkg-config --cflags openssl 2>/dev/null || echo "-I/usr/local/opt/openssl/include -I/opt/homebrew/opt/openssl/include")
LDFLAGS = $(shell pkg-config --libs openssl 2>/dev/null || echo "-L/usr/local/opt/openssl/lib -L/opt/homebrew/opt/openssl/lib") -lssl -lcrypto

TARGET  = docksmith_c

SRCS = \
	vendor/cjson/cJSON.c \
	c_src/util/hash.c \
	c_src/util/tar.c \
	c_src/store/store.c \
	c_src/store/layer.c \
	c_src/store/image.c \
	c_src/build/parser.c \
	c_src/build/cache.c \
	c_src/build/engine.c \
	c_src/container/run.c \
	c_src/cmd/commands.c \
	c_src/main.c

OBJS = $(SRCS:.c=.o)

.PHONY: all clean

all: $(TARGET)

$(TARGET): $(OBJS)
	$(CC) $(OBJS) $(LDFLAGS) -o $@
	@echo "Built $(TARGET)"

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

clean:
	rm -f $(OBJS) $(TARGET)
