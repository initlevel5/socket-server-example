CC := gcc

IDIR := ./include
SDIR := ./src

CFLAGS := -std=c11 -W -Wall -Wundef -Werror -Wno-deprecated-declarations -Wno-multichar -O0 -pedantic \
		 -D_XOPEN_SOURCE=600 -D_POSIX_C_SOURCE=200112L -I$(IDIR) -DDEBUG

LDFLAGS := -lm -lpthread

SRC := $(SDIR)/proto.c

.PHONY: server client

all: server client 

server:
	$(CC) $(SDIR)/server.c $(SRC) $(CFLAGS) -o $@ $(LDFLAGS)

client:
	$(CC) $(SDIR)/client.c $(SRC) $(CFLAGS) -o $@ $(LDFLAGS)

clean:
	rm -f server client *~