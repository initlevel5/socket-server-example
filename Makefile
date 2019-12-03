CC := gcc

IDIR := ./include
SDIR := ./src

CFLAGS := -std=c11 -W -Wall -Wundef -Werror -Wno-deprecated-declarations -Wno-multichar -O0 -pedantic \
		 -D_XOPEN_SOURCE=600 -D_POSIX_C_SOURCE=200112L -I$(IDIR)

LDFLAGS := -lm -lpthread

.PHONY: server client

all: server client 

server:
	$(CC) $(SDIR)/server.c $(CFLAGS) -o $@ $(LDFLAGS)

client:
	$(CC) $(SDIR)/client.c $(CFLAGS) -o $@ $(LDFLAGS)

clean:
	rm -f server client *~