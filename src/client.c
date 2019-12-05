/*
 * Copyright (c) 2019 <initlevel5@gmail.com>
 * 
 * The socket client simple implementation
 */
#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

#define ADDR "127.0.0.1"
#define PORT (8082)

#define BUF_SIZE (1024)
#define INVALID_SOCKET (-1)

#define CONN_TIMEOUT (10)
#define N (10)

#define FD_CLOSE(x) do {\
	while (close((x)) == -1 && errno == EINTR);\
	(x) = INVALID_SOCKET;\
} while (0)

enum conn_state {
	ST_READ,
	ST_WRITE,
};

int main(int argc, char const *argv[]) {
	int count = 0, fd, err = 0, len, n, n_avail = 0, req_len = 0, n_to_write = 0, n_written = 0, res;
	enum conn_state state = ST_WRITE;
	struct sockaddr_in addr_in;
	socklen_t addrlen = sizeof(struct sockaddr_in);
	memset(&addr_in, 0, addrlen);
	fd_set rfds, wfds;
	struct timeval tv = {0, 0};
	time_t timeout;
	unsigned char buf[BUF_SIZE];
	memset(buf, 0, BUF_SIZE);
	const char *msg = "Hello World!";

	//connect to the server
	addr_in.sin_family = AF_INET;
	addr_in.sin_addr.s_addr = inet_addr(ADDR);//INADDR_ANY;
	addr_in.sin_port = htons((uint16_t)PORT);

	if ((fd = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
		err = errno;
		printf("socket(): %s (%d)\n", strerror(err), err);
		_exit(EXIT_FAILURE);
	}

	printf("attempt to connect to peer: %s:%d ...\n",
			(char *)inet_ntoa(addr_in.sin_addr),
			ntohs(addr_in.sin_port));

	if (connect(fd, (const struct sockaddr *)&addr_in, addrlen) != 0) {
		err = errno;
		printf("connect(): %s (%d)", strerror(err), err);
		FD_CLOSE(fd);
		_exit(EXIT_FAILURE);
	}

	printf("connected successfully\n");

	//build the message
	n_to_write = 2 + strlen(msg);
	buf[0] = (unsigned char)n_to_write;
	buf[1] = (unsigned char)(n_to_write >> 8);
	strncpy((char *)(buf + 2), msg, BUF_SIZE - 2 - 1);

	timeout = time(NULL) + CONN_TIMEOUT;

	//main loop
	for (;;) {
		FD_ZERO(&rfds);
		FD_ZERO(&wfds);

		if (state == ST_READ) {
			FD_SET(fd, &rfds);
		} else if (state == ST_WRITE) {
			FD_SET(fd, &wfds);
		}

		tv.tv_sec = 1;
		tv.tv_usec = 0;

		if ((res = select(fd + 1, &rfds, &wfds, NULL, &tv)) == -1) {
			err = errno;
			if (err != EINTR) {
				printf("select(): %s (%d)\n", strerror(err), err);
				break;
			}
		} else if (res == 0) {
			if (state == ST_READ && timeout < time(NULL)) {
				printf("connection timeout\n");
				break;
			}
		} else {
			if (FD_ISSET(fd, &rfds)) {
				n = recv(fd, buf + n_avail, BUF_SIZE - n_avail, 0);

				if (n < 1) {
					if (n == -1) {
						err = errno;
						if (err == EINTR) continue;
						printf("recv(): %s (%d)\n", strerror(err), err);
					} else {
						printf("recv(): connection closed by peer\n");
					}
					break;
				}

				n_avail += n;

				if (req_len == 0 && n > 2) {
					len = (int)((uint16_t)buf[1] >> 8) + (uint16_t)buf[0];
					if (len == 0 || len > BUF_SIZE) {
						printf("invalid request len (%d)\n", len);
						break;
					}
					req_len = len;
				}

				if (n_avail == req_len) {
					printf("%s\n", buf + 2);
					if (++count > N) continue;

					n_to_write = n_avail;	//echo
					state = ST_WRITE;

					n_avail = req_len = 0;
					timeout = time(NULL) + CONN_TIMEOUT;
				}

			} else if (FD_ISSET(fd, &wfds)) {
				n = send(fd, buf + n_written, n_to_write - n_written, 0);

				if (n < 1) {
					if (n == -1) {
						err = errno;
						if (err == EINTR) continue;
						printf("send(): %s (%d)\n", strerror(err), err);
					} else {
						printf("send(): connection closed by peer\n");
					}
					break;
				}

				n_written += n;

				if (n_written == n_to_write) {
					n_to_write = n_written = 0;
					state = ST_READ;
				}
			}
		}
	}

	FD_CLOSE(fd);

	(void)argc;
	(void)argv;

	return EXIT_SUCCESS;
}
