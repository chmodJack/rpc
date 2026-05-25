CC = gcc
CFLAGS = -Wall -Wextra -g -Iinclude -Isrc
LDFLAGS =

SRCS = src/rpc.c src/ws.c src/event_loop.c src/protocol.c
OBJS = $(SRCS:.c=.o)

all: server client

server: examples/server.c $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

client: examples/client.c $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f $(OBJS) server client

.PHONY: all clean
