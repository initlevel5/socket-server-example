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

#define ADDR "127.0.0.1"
#define PORT (8082)

#define BUF_SIZE (1024)
#define INVALID_SOCKET (-1)
#define QLEN (65536)

#define CONN_TIMEOUT (10)

#define MAX_NUM_THREADS (10)

#define FD_CLOSE(x) do {\
	while (close((x)) == -1 && errno == EINTR);\
	(x) = INVALID_SOCKET;\
} while (0)

enum conn_state {
	ST_READ,
	ST_WRITE,
};

//@TODO implement real thread pool
struct thread {
	pthread_t id;
	int fd;
};

//@TODO implement list of connections
/* strict conn {
	int fd;
	enum conn_state state;

	...
}*/

static volatile sig_atomic_t g_shutdown = 0;
static struct thread *th = NULL;
static pthread_mutex_t th_mutex = PTHREAD_MUTEX_INITIALIZER;

static int server_init(void);
static void sig_term_handler(int sig);
static void conf_sig_term_handler(void);
static int server_listen(const char *addr, uint16_t port);
static int make_socket_nonblocking(int fd);
static void server_clean(void);
static void serve(int listener);
static inline int server_accept(int listener);
static void *th_proc(void *arg);

int main(int argc, char const *argv[]) {
	int listener;
	
	if (server_init() != 0) _exit(EXIT_FAILURE);

	if ((listener = server_listen(ADDR, PORT)) == INVALID_SOCKET) _exit(EXIT_FAILURE);

	serve(listener);

	FD_CLOSE(listener);

	server_clean();

	(void)argc;
	(void)argv;

	return EXIT_SUCCESS;
}

/*
 * Allocates resources for thread pool and mutex.
 *
 * If successful, the function will return 0 or -1 if an error occurred.
 */
static int server_init(void) {
	int err;

	conf_sig_term_handler();

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
static int server_listen(const char *addr, uint16_t port) {
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
static void server_clean(void) {
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
			if (FD_ISSET(listener, &rfds) && server_accept(listener) != 0) g_shutdown = 1;
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
static inline int server_accept(int listener) {
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
				if (pthread_create(&th[i].id, NULL, th_proc, (void *)&th[i].fd) != 0) {
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
static void *th_proc(void *arg) {
	int *fd = (int *)arg;
	int err = 0, len, n, n_avail = 0, req_len = 0, n_to_write = 0, n_written = 0, res;
	enum conn_state state = ST_READ;
	struct sockaddr_in addr_in;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	memset(&addr_in, 0, addrlen);
	fd_set rfds, wfds;
	struct timeval tv = {0, 0};
	time_t timeout;
	unsigned char buf[BUF_SIZE];
	memset(buf, 0, BUF_SIZE);

	timeout = time(NULL) + CONN_TIMEOUT;

	while (!g_shutdown) {
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);

		if (*fd != INVALID_SOCKET) {
			if (state == ST_READ) {
				FD_SET(*fd, &rfds);
			} else if (state == ST_WRITE) {
				FD_SET(*fd, &wfds);
			}
		}

		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if ((res = select(*fd + 1, &rfds, &wfds, NULL, &tv)) == -1) {
			err = errno;
			if (err != EINTR) {
				printf("th_proc(): select(): %s (%d)\n", strerror(err), err);
				break;
			}
		} else if (res == 0) {
			if (*fd != INVALID_SOCKET && state == ST_READ && timeout < time(NULL)) {
				printf("th_proc(): connection timeout\n");
				if ((err = pthread_mutex_lock(&th_mutex)) != 0) {
					printf("th_proc(): pthread_mutex_lock(): %s (%d)\n", strerror(err), err);
					break;
				}
				FD_CLOSE(*fd);
				pthread_mutex_unlock(&th_mutex);
				n_avail = req_len = 0;
			}
		} else {
			if (FD_ISSET(*fd, &rfds)) {
				n = recv(*fd, buf + n_avail, BUF_SIZE - n_avail, 0);

				if (n < 1) {
					if (n == -1) {
						err = errno;
						if (err != EINTR) {
							printf("recv(): %s (%d)\n", strerror(err), err);
							break;
						}
					} else {
						printf("recv(): connection closed by peer\n");
						if ((err = pthread_mutex_lock(&th_mutex)) != 0) {
							printf("recv(): pthread_mutex_lock(): %s (%d)\n", strerror(err), err);
							break;
						}
						FD_CLOSE(*fd);
						pthread_mutex_unlock(&th_mutex);
					}
					continue;
				}

				n_avail += n;

				if (req_len == 0 && n > 2) {
					len = (int)((uint16_t)buf[1] >> 8) + (uint16_t)buf[0];
					if (len < 3 || len > BUF_SIZE) {
						printf("recv(): invalid request len (%d)\n", len);
						if ((err = pthread_mutex_lock(&th_mutex)) != 0) {
							printf("recv(): pthread_mutex_lock(): %s (%d)\n", strerror(err), err);
							break;
						}
						FD_CLOSE(*fd);
						pthread_mutex_unlock(&th_mutex);
						continue;
					}
					req_len = len;
				}

				if (n_avail == req_len) {
					printf("%s\n", buf + 2);

					n_to_write = n_avail;	//echo
					state = ST_WRITE;

					n_avail = req_len = 0;
					timeout = time(NULL) + CONN_TIMEOUT;
				}

			} else if (FD_ISSET(*fd, &wfds)) {
				n = send(*fd, buf + n_written, n_to_write - n_written, 0);

				if (n < 1) {
					if (n == -1) {
						err = errno;
						if (err != EINTR) {
							printf("send(): %s (%d)\n", strerror(err), err);
							break;
						}
					} else {
						printf("send(): connection closed by peer\n");
						if ((err = pthread_mutex_lock(&th_mutex)) != 0) {
							printf("send(): pthread_mutex_lock(): %s (%d)\n", strerror(err), err);
							break;
						}
						FD_CLOSE(*fd);
						pthread_mutex_unlock(&th_mutex);
					}
					continue;
				}

				n_written += n;

				if (n_written == n_to_write) {
					n_to_write = n_written = 0;
					state = ST_READ;
				}
			}
		}
	}

	pthread_mutex_lock(&th_mutex);
	FD_CLOSE(*fd);
	pthread_mutex_unlock(&th_mutex);

	return NULL;
}

/*
static int server_read(struct conn *pc) {

}

static int server_write(struct conn *pc) {

}
*/
