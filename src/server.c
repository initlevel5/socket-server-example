#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
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

struct thread {
	pthread_t id;
	int fd;
};

static volatile int g_shutdown = 0;
static struct thread *th = NULL;

static int make_socket_nonblocking(int fd) {
	int flags;
	if ((flags = fcntl(fd, F_GETFL, 0)) == -1 || fcntl(fd, F_SETFL, flags | O_NONBLOCK) == -1) return errno;
	return 0;
}

static void *th_proc(void *arg) {
	int *fd = (int *)arg, err = 0, len, n, n_avail = 0, req_len = 0, n_to_write = 0, n_written = 0, res;
	enum conn_state state = ST_READ;
	struct sockaddr_in addr_in;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	memset(&addr_in, 0, addrlen);
	fd_set rfds, wfds;
	struct timeval tv = {0, 0};
	time_t timeout;
	unsigned char buf[BUF_SIZE];

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
				printf("select(): %s (%d)\n", strerror(err), err);
				break;
			}
		} else if (res > 0) {
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
						FD_CLOSE(*fd);
					}
					continue;
				}

				n_avail += n;

				if (req_len == 0 && n > 2) {
					len = (int)((uint16_t)buf[1] >> 8) + (uint16_t)buf[0];
					if (len < 3 || len > BUF_SIZE) {
						printf("invalid request len (%d)\n", len);
						FD_CLOSE(*fd);
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
						FD_CLOSE(*fd);
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

	FD_CLOSE(*fd);

	return NULL;
}

static inline int conn_accept(int listener) {
	int fd, err ,i;
	struct sockaddr_in addr;
	socklen_t addrlen = sizeof(struct sockaddr_in);

	for (;;) {
		if ((fd = accept(listener, (struct sockaddr *) &addr, &addrlen)) == INVALID_SOCKET) {
			err = errno;
			if (err == EINTR) continue;
			if (err == EAGAIN || err == EWOULDBLOCK) return 0;
			printf("accept(%d): %s (%d)\n", listener, strerror(err), err);
			return -1;
		}
		break;
	}

	//TODO implement real thread pool instead
	for (i = 0; i < MAX_NUM_THREADS; i++) {
		if (th[i].fd == INVALID_SOCKET) {
			th[i].fd = fd;
			if (th[i].id == 0) {
				if (pthread_create(&th[i].id, NULL, th_proc, (void *)&th[i].fd) != 0) {
					err = errno;
					printf("pthread_create(): %s (%d)\n", strerror(err), err);
					FD_CLOSE(th[i].fd);
					return -1;
				}
			}
			break;
		}
	}

	if (i == MAX_NUM_THREADS) FD_CLOSE(fd);

	return 0;
}

int main(int argc, char const *argv[]) {
	int flag = 1, err = 0, listener, i, res;
	struct sockaddr_in addr_in;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	memset(&addr_in, 0, addrlen);
	fd_set rfds;
	struct timeval tv = {0, 0};

	//
	if ((listener = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
		err = errno;
		printf("socket(): %s (%d)\n", strerror(err), err);
		_exit(EXIT_FAILURE);
	}

	if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) != 0) {
		err = errno;
		printf("setsockopt(): %s (%d)\n", strerror(err), err);
		FD_CLOSE(listener);
		_exit(EXIT_FAILURE);
	}

	addr_in.sin_family = AF_INET;
	addr_in.sin_addr.s_addr = inet_addr(ADDR);//INADDR_ANY;
	addr_in.sin_port = htons((uint16_t)PORT);

	if (bind(listener, (struct sockaddr *) &addr_in, addrlen) != 0) {
		err = errno;
		printf("bind(%s:%d): %s (%d)\n",
				(char *)inet_ntoa(addr_in.sin_addr),
				ntohs(addr_in.sin_port),
				strerror(err),
				err);
		FD_CLOSE(listener);
		_exit(EXIT_FAILURE);
	}

	if ((err = make_socket_nonblocking(listener)) != 0) {
		printf("make_socket_nonblocking(): %s (%d)\n", strerror(err), err);
		FD_CLOSE(listener);
		_exit(EXIT_FAILURE);
	}

	if (listen(listener, QLEN) != 0) {
		err = errno;
		printf("listen(): %s (%d)\n", strerror(err), err);
		FD_CLOSE(listener);
		_exit(EXIT_FAILURE);
	}

	printf("socket %d binded, listen clients on: %s:%d\n",
		  listener,
		  (char *)inet_ntoa(addr_in.sin_addr),
		  ntohs(addr_in.sin_port));

	//TODO implement real thread pool instead
	th = (struct thread *)calloc(MAX_NUM_THREADS, sizeof(*th));
	if (th == NULL) {
		printf("can't allocate memory\n");
		FD_CLOSE(listener);
		_exit(EXIT_FAILURE);
	}
	for (i = 0; i < MAX_NUM_THREADS; i++) th[i].fd = INVALID_SOCKET;

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

	FD_CLOSE(listener);

	for (i = 0; i < MAX_NUM_THREADS; i++) pthread_join(th[i].id, NULL);
	free(th);

	(void)argc;
	(void)argv;

	return EXIT_SUCCESS;
}
