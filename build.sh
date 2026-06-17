#!/bin/sh
# Build script for Linux (gcc/clang).
set -e

CC="${CC:-gcc}"
CFLAGS="-Wall -Wextra -g -Iinclude -Isrc"

mkdir -p build

SRCS="src/rpc.c src/ws.c src/event_loop.c src/protocol.c src/compat.c"
OBJS=""

for f in $SRCS; do
    name=$(basename "$f" .c)
    echo "Compiling $f"
    $CC $CFLAGS -c "$f" -o "build/$name.o"
    OBJS="$OBJS build/$name.o"
done

echo "Linking server"
$CC $CFLAGS examples/server.c $OBJS -o server

echo "Linking client"
$CC $CFLAGS examples/client.c $OBJS -o client

echo
echo "Build complete: server client"
