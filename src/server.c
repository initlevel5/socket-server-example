/*
 * Copyright (c) 2019 <initlevel5@gmail.com>
 * 
 * The socket server simple implementation
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "proto.h"

#define ADDR "127.0.0.1"
#define PORT (8082)

#define BUF_SIZE (1412)
#define INVALID_SOCKET (-1)
#define QLEN (65536)

#define CONN_TIMEOUT (10)

#define MAX_NUM_THREADS (10)

#define VERSION (4211)

#define FD_CLOSE(x) do {\
	while (close((x)) == -1 && errno == EINTR);\
	(x) = INVALID_SOCKET;\
} while (0)

enum conn_state {
	ST_IDLE = 0,
	ST_READ = 1,
	ST_WAIT = 2,
	ST_WRITE = 3,
};

struct conn {
	enum conn_state state;
	unsigned char buf[BUF_SIZE];
	int n_avail;
	int req_len;
	int n_to_write;
	int n_written;
	time_t timeout;
};

//@TODO implement real thread pool
struct thread {
	pthread_t id;
	int fd;
};

static volatile sig_atomic_t g_shutdown = 0;
static struct thread *th = NULL;
static pthread_mutex_t th_mutex = PTHREAD_MUTEX_INITIALIZER;

static int init(void);
static void clean(void);
static void serve(int listener);

static void sig_term_handler(int sig);
static void conf_sig_term_handler(void);

static int get_listener(const char *addr, uint16_t port);
static int make_socket_nonblocking(int fd);

static inline int conn_accept(int listener);
static void *conn_proc(void *arg);
static void conn_close(int *fd, struct conn *pc);
static void conn_read(int *fd, struct conn *pc);
static void conn_write(int *fd, struct conn *pc);
static int conn_proto_handler(struct conn *pc);

int main(int argc, char const *argv[]) {
	int listener;
	
	if (init() != 0) _exit(EXIT_FAILURE);

	if ((listener = get_listener(ADDR, PORT)) == INVALID_SOCKET) _exit(EXIT_FAILURE);

	serve(listener);

	FD_CLOSE(listener);

	clean();

	(void)argc;
	(void)argv;

	return EXIT_SUCCESS;
}

/*
 * Allocates resources for thread pool and mutex.
 *
 * If successful, the function will return 0 or -1 if an error occurred.
 */
static int init(void) {
	int err;

	conf_sig_term_handler();

	make_crc16_table();

	//@TODO implement real thread pool instead
	if ((err = pthread_mutex_init(&th_mutex, NULL)) != 0) {
		printf("server_init(): pthread_mutex_init(): %s (%d)\n", strerror(err), err);
		return -1;
	}

	if ((th = (struct thread *)calloc(MAX_NUM_THREADS, sizeof(*th))) == NULL) {
		printf("server_init(): can't allocate memory\n");
		pthread_mutex_destroy(&th_mutex);
		return -1;
	}

	for (int i = 0; i < MAX_NUM_THREADS; i++) th[i].fd = INVALID_SOCKET;

	return 0;
}

static void sig_term_handler(int sig) {
	(void)sig;
	g_shutdown = 1;
}

static void conf_sig_term_handler(void) {
	struct sigaction sig_term_sa;

	signal(SIGPIPE, SIG_IGN);

	sig_term_sa.sa_handler = sig_term_handler;
	sigemptyset(&sig_term_sa.sa_mask);
	sig_term_sa.sa_flags = 0;
	sigaction(SIGTERM, &sig_term_sa, NULL);
}

/*
 * Creates the socket, binds to an address 'addr' and 'port',
 * and makes it listening for connections.
 *
 * Returns the socket that has been created or -1 if an error occurred.
 */
static int get_listener(const char *addr, uint16_t port) {
	int fd, flag = 1, err = 0;
	struct sockaddr_in addr_in;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	memset(&addr_in, 0, addrlen);

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
		err = errno;
		printf("socket(): %s (%d)\n", strerror(err), err);
		return -1;
	}

	if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) != 0) {
		err = errno;
		printf("setsockopt(): %s (%d)\n", strerror(err), err);
		FD_CLOSE(fd);
		return -1;
	}

	addr_in.sin_family = AF_INET;
	addr_in.sin_addr.s_addr = inet_addr(addr);
	addr_in.sin_port = htons(port);

	if (bind(fd, (struct sockaddr *) &addr_in, addrlen) != 0) {
		err = errno;
		printf("bind(%s:%d): %s (%d)\n",
				(char *)inet_ntoa(addr_in.sin_addr),
				ntohs(addr_in.sin_port),
				strerror(err),
				err);
		FD_CLOSE(fd);
		return -1;
	}

	if ((err = make_socket_nonblocking(fd)) != 0) {
		printf("make_socket_nonblocking(): %s (%d)\n", strerror(err), err);
		FD_CLOSE(fd);
		return -1;
	}

	if (listen(fd, QLEN) != 0) {
		err = errno;
		printf("listen(): %s (%d)\n", strerror(err), err);
		FD_CLOSE(fd);
		return -1;
	}

	printf("socket %d binded, listen clients on: %s:%d\n",
		  fd,
		  (char *)inet_ntoa(addr_in.sin_addr),
		  ntohs(addr_in.sin_port));

	return fd;
}

/* 
 * Marks the socket 'fd' as non-blocking.
 *
 * If successful, the function will return 0. Otherwise, an error number will be returned
 * to indicate the error.
 *
 */
static int make_socket_nonblocking(int fd) {
	int flags;
	if ((flags = fcntl(fd, F_GETFL, 0)) == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return errno;
	return 0;
}

/*
 * Suspends execution until the all threads terminate, unless the threads have already terminated.
 * Frees the resources allocated for thread pool and mutex.
 */
static void clean(void) {
	for (int i = 0; i < MAX_NUM_THREADS; i++) pthread_join(th[i].id, NULL);
	free(th);
	pthread_mutex_destroy(&th_mutex);
}

/*
 * Examines the 'listener' descriptor to see if it is ready for reading. It means, that
 * the queue of pending connections have the connection requests. If the listener is
 * contained in the descriptor set, the function 'server_accept' will be called to accept
 * the connection request.
 *
 * The argument 'listener' is a socket that has been created, bound to an address,
 * and is listening for connections. The 'listener' is marked as non-blocking.
 *
 */
static void serve(int listener) {
	int err, res;
	fd_set rfds;
	struct timeval tv = {0, 0};

	while (!g_shutdown) {
		FD_ZERO(&rfds);
		FD_SET(listener, &rfds);

		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if ((res = select(listener + 1, &rfds, NULL, NULL, &tv)) == -1) {
			err = errno;
			if (err != EINTR) {
				printf("select(): %s (%d)\n", strerror(err), err);
				g_shutdown = 1;
			}
		} else if (res > 0) {
			if (FD_ISSET(listener, &rfds) && conn_accept(listener) != 0) g_shutdown = 1;
		}
	}
}

/*
 * Extracts the first connection request on the queue of pending connections,
 * creates a new socket with the same properties of 'listener', allocates connection,
 * and assignes the worker thread from the pool.
 *
 * The argument 'listener' is a socket that has been created, bound to an address,
 * and is listening for connections. The 'listener' is marked as non-blocking.
 *
 * If successful, the function will return 0 or -1 if an error occurred.
 */
static inline int conn_accept(int listener) {
	int fd, err = 0, i;
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(struct sockaddr_in);

	for (;;) {
		if ((fd = accept(listener, (struct sockaddr *) &addr, &addrlen)) == INVALID_SOCKET) {
			err = errno;
			if (err == EINTR) continue;
			if (err == EAGAIN || err == EWOULDBLOCK) return 0;
			printf("conn_accept(): accept(%d): %s (%d)\n", listener, strerror(err), err);
			return -1;
		}
		break;
	}

	if ((err = make_socket_nonblocking(fd)) != 0) {
		printf("conn_accept(): make_socket_nonblocking(): %s (%d)\n", strerror(err), err);
		FD_CLOSE(fd);
		return -1;
	}

	//@TODO implement real thread pool instead
	if ((err = pthread_mutex_lock(&th_mutex)) != 0) {
		printf("conn_accept(): pthread_mutex_lock(): %s (%d)\n", strerror(err), err);
		FD_CLOSE(fd);
		return -1;
	}
	for (i = 0; i < MAX_NUM_THREADS; i++) {
		if (th[i].fd == INVALID_SOCKET) {
			th[i].fd = fd;
			if (th[i].id == 0) {
				if (pthread_create(&th[i].id, NULL, conn_proc, (void *)&th[i].fd) != 0) {
					err = errno;
					printf("conn_accept(): pthread_create(): %s (%d)\n", strerror(err), err);
					FD_CLOSE(th[i].fd);
					pthread_mutex_unlock(&th_mutex);
					return -1;
				}
			}
			break;
		}
	}
	pthread_mutex_unlock(&th_mutex);

	if (i == MAX_NUM_THREADS) {
		printf("conn_accept(): max number of threads reached\n");
		FD_CLOSE(fd);
	}

	return 0;
}

/*
 * Worker thread routine.
 *
 * Returns NULL.
 */
static void *conn_proc(void *arg) {
	int *fd = (int *)arg;
	struct conn *pc = NULL;
	int err = 0, res;
	struct sockaddr_in addr_in;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	memset(&addr_in, 0, addrlen);
	fd_set rfds, wfds;
	struct timeval tv = {0, 0};

	if ((pc = (struct conn *)calloc(1, sizeof(*pc))) == NULL) {
		printf("conn_proc(): can't allocate memory\n");
		return NULL;
	}

	while (!g_shutdown) {
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);

		if (*fd != INVALID_SOCKET) {
			if (pc->state == ST_IDLE) {
				pc->state = ST_READ;
				pc->timeout = time(NULL) + CONN_TIMEOUT;
			}
			if (pc->state == ST_READ) {
				FD_SET(*fd, &rfds);
			} else if (pc->state == ST_WRITE) {
				FD_SET(*fd, &wfds);
			}
		}

		tv.tv_sec = 0;
		tv.tv_usec = 100000; //100ms

		if ((res = select(*fd + 1, &rfds, &wfds, NULL, &tv)) == -1) {
			err = errno;
			if (err != EINTR) {
				printf("conn_proc(): select(): %s (%d)\n", strerror(err), err);
				break;
			}
		} else if (res == 0) {
			if (*fd != INVALID_SOCKET && pc->state == ST_READ && pc->timeout < time(NULL)) {
				printf("conn_proc(): connection timeout\n");
				conn_close(fd, pc);
			}
		} else {
			if (FD_ISSET(*fd, &rfds)) {
				conn_read(fd, pc);
			} else if (FD_ISSET(*fd, &wfds)) {
				conn_write(fd, pc);
			}
		}
	}

	conn_close(fd, pc);

	return NULL;
}

/*
 * Closes the given socket descriptor 'fd' and
 * cleans connection data.
 */
static void conn_close(int *fd, struct conn *pc) {
	int err;

	memset(pc->buf, 0, BUF_SIZE);

	pc->n_avail = pc->req_len = pc->n_to_write = pc->n_written = 0;
	pc->state = ST_IDLE;

	if ((err = pthread_mutex_lock(&th_mutex)) != 0) {
		printf("conn_close(): pthread_mutex_lock(): %s (%d)\n", strerror(err), err);
		return;
	}
	FD_CLOSE(*fd);
	pthread_mutex_unlock(&th_mutex);
}

/*
 * Receives messages from the given socket 'fd'.
 */
static void conn_read(int *fd, struct conn *pc) {
	int err, len, n;

	if ((n = recv(*fd, pc->buf + pc->n_avail, BUF_SIZE - pc->n_avail, 0)) < 1) {
		if (n == -1) {
			err = errno;
			if (err == EINTR || err == EAGAIN) return;
			printf("conn_read(): recv(): %s (%d)\n", strerror(err), err);
		} else {
			printf("conn_read(): recv(): connection closed by peer\n");
		}
		conn_close(fd, pc);
		return;
	}

	pc->n_avail += n;

	if (pc->req_len == 0 && n > 2) {
		len = (int)(((uint16_t)pc->buf[1] << 8) + (uint16_t)pc->buf[0]) + 4/*sizeof(header1)*/;
		if (len < PACKET_HEADER_SIZE || len > BUF_SIZE) {
			printf("conn_read(): invalid request len (%d)\n", len);
			conn_close(fd, pc);
			return;
		}
		pc->req_len = len;
	}

	if (pc->n_avail == pc->req_len) {
		if (conn_proto_handler(pc) != 0) {
			conn_close(fd, pc);
			return;
		}

#ifdef DEBUG
		printf("---> ");
		for(int i = 0; i < pc->n_to_write; i++) printf("%02X ", pc->buf[i]);
		printf("\n");
#endif

		pc->state = ST_WRITE;

		pc->n_avail = pc->req_len = 0;
		pc->timeout = time(NULL) + CONN_TIMEOUT;
	}
}

/*
 * Sends messages to the given socket 'fd'.
 */
static void conn_write(int *fd, struct conn *pc) {
	int err, n;

	if ((n = send(*fd, pc->buf + pc->n_written, pc->n_to_write - pc->n_written, 0)) < 1) {
		if (n == -1) {
			err = errno;
			if (err == EINTR || err == EAGAIN) return;
			printf("conn_write(): send(): %s (%d)\n", strerror(err), err);
		} else {
			printf("conn_write(): send(): connection closed by peer\n");
		}
		conn_close(fd, pc);
		return;
	}

	pc->n_written += n;

	if (pc->n_written == pc->n_to_write) {
		pc->n_to_write = pc->n_written = 0;
		pc->state = ST_READ;
	}
}

/*
 * Protocol specific handler.
 *
 * If successful, the function will return 0 or -1 if an error occurred.
 */
static int conn_proto_handler(struct conn *pc) {
	struct packet *p = (struct packet *)pc->buf;
	uint16_t crc = get_crc16(pc->buf + 4, p->len);
	uint16_t ver = ((uint16_t)p->data[1] << 8) + (uint16_t)p->data[0];
	
#ifdef DEBUG
	for(int i = 0; i < p->len + 4; i++) printf("%02X ", pc->buf[i]);
	printf(" <---\n");
	printf("len%d\ncrc%d\nseq%d\ntype%d\nver%d\n", p->len, p->crc, p->seq, p->type, ver);
#endif

	if (p->crc != crc) {
		printf("conn_proto_handler(): invalid crc (%d, expected %d)\n", p->crc, crc);
		return -1;
	}

	if (ver != VERSION) {
		printf("conn_proto_handler(): invalid version (%d, expected %d)\n", ver, VERSION);
		return -1;
	}

	switch (p->type) {
		case PTYPE_AUTH: {
			/*
			 * Build the response Auth packet
			 *
			 * 0C00 8B55 C8FD190568EA 00 01 0B99DE5D
			 */
			time_t now = time(NULL);

			p->len = 12;

			p->data[0] = (uint8_t)now;
			p->data[1] = (uint8_t)(now >> 8);
			p->data[2] = (uint8_t)(now >> 16);
			p->data[3] = (uint8_t)(now >> 24);

			p->crc = get_crc16(pc->buf + 4, p->len);
		}
		break;
		case PTYPE_PING: 
		case PTYPE_SENSOR_DATA:
		case PTYPE_FILE:
		case PTYPE_LOG:
		case PTYPE_NODE_PACKET:
		case PTYPE_BRIDGE_PACKET:
		case PTYPE_REAUTH:
		case PTYPE_CAM:
		case PTYPE_NODE_FILE:
		case PTYPE_VEND_FILE:
		case PTYPE_REGISTER_BRIDGE:
		case PTYPE_PURCHASE:
		case PTYPE_COUNT:
		break;
		default: printf("conn_proto_handler(): invalid type (%d)\n", p->type); return -1;
	}

	pc->n_to_write = p->len + 4/*sizeof(header1)*/;

	return 0;
}
