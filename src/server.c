#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#define ADDR "127.0.0.1"
#define PORT (8082)

#define INVALID_SOCKET (-1)
#define QLEN (65536)

#define FD_CLOSE(x) do {\
	while (close((x)) == -1 && errno == EINTR);\
	(x) = INVALID_SOCKET;\
} while (0)

int main(int argc, char const *argv[]) {
	int listener, flag = 1, rc = 0;
	struct sockaddr_in addr_in;
	socklen_t addrlen = sizeof(struct sockaddr_in);

	if ((listener = socket(AF_INET, SOCK_STREAM, 0)) == INVALID_SOCKET) {
		rc = errno;
		printf("socket(): %s (%d)\n", strerror(rc), rc);
		_exit(EXIT_FAILURE);
	}

	if (setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &flag, sizeof(flag)) != 0) {
		rc = errno;
		printf("setsockopt(): %s (%d)\n", strerror(rc), rc);
		FD_CLOSE(listener);
		_exit(EXIT_FAILURE);
	}

	addr_in.sin_family = AF_INET;
	addr_in.sin_addr.s_addr = inet_addr(ADDR);//INADDR_ANY;
	addr_in.sin_port = htons((uint16_t)PORT);

	if (bind(listener, (struct sockaddr *) &addr_in, addrlen) != 0) {
		rc = errno;
		printf("bind(%s:%d): %s (%d)\n",
				(char *)inet_ntoa(addr_in.sin_addr),
				ntohs(addr_in.sin_port),
				strerror(rc),
				rc);
		FD_CLOSE(listener);
		_exit(EXIT_FAILURE);
	}

	if (listen(listener, QLEN) != 0) {
		rc = errno;
		printf("listen(): %s (%d)\n", strerror(rc), rc);
		FD_CLOSE(listener);
		_exit(EXIT_FAILURE);
	}

	printf("socket %d binded, listen clients on: %s:%d\n",
		  listener,
		  (char *)inet_ntoa(addr_in.sin_addr),
		  ntohs(addr_in.sin_port));

	//for (;;) {}

	FD_CLOSE(listener);

	(void)argc;
	(void)argv;

	return EXIT_SUCCESS;
}
