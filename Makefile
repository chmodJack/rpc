CC ?= gcc
CFLAGS = -Wall -Wextra -g -Iinclude -Isrc
LDFLAGS =

SRCS = src/rpc.c src/ws.c src/event_loop.c src/protocol.c src/compat.c
OBJS = $(SRCS:.c=.o)

# Platform detection
ifeq ($(OS),Windows_NT)
    EXE = .exe
    LDFLAGS += -lws2_32
else
    EXE =
endif

SERVER = server$(EXE)
CLIENT = client$(EXE)

all: $(SERVER) $(CLIENT)

$(SERVER): examples/server.c $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(CLIENT): examples/client.c $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
ifeq ($(OS),Windows_NT)
	-del /Q $(subst /,\,$(OBJS)) $(SERVER) $(CLIENT) 2>nul
else
	rm -f $(OBJS) $(SERVER) $(CLIENT)
endif

.PHONY: all clean
