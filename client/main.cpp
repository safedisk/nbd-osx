#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <string.h>
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <thread>

#include "nbd_api.h"

#ifdef WORDS_BIGENDIAN
uint64_t ntohll(uint64_t a)
{
	return a;
}
#else
uint64_t ntohll(uint64_t a)
{
	uint32_t lo = a & 0xffffffff;
	uint32_t hi = a >> 32U;
	lo = ntohl(lo);
	hi = ntohl(hi);
	return ((uint64_t)lo) << 32U | hi;
}
#endif

static
const char* nbd_cmd_str(int cmd)
{
	#define CODE_CASE(code) case code: return #code;
	switch (cmd) {
		CODE_CASE(NBD_CMD_READ);
		CODE_CASE(NBD_CMD_WRITE);
		CODE_CASE(NBD_CMD_DISC);
		CODE_CASE(NBD_CMD_FLUSH);
		CODE_CASE(NBD_CMD_TRIM);
		default:
			return "Unknown";
	}
	#undef CODE_CASE
}

static
void read_all(int fd, char* buf, size_t count)
{
	while (count) {
		int len = read(fd, buf, count);
		buf += len;
		count -= len;
	}
}

static
void write_all(int fd, char* buf, size_t count)
{
	while (count) {
		int len = write(fd, buf, count);
		buf += len;
		count -= len;
	}
}

static
int nbd_thread(int nbd, int sock)
{
	printf("nbd_thread> NBD_SET_SOCK\n");
	int ret = ioctl(nbd, NBD_SET_SOCK, sock);
	if (ret == -1) {
		fprintf(stderr, "ioctl(NBD_SET_SOCK) failed: %s (%d)\n", strerror(errno), errno);
		return 1;
	}

	printf("nbd_thread> NBD_DO_IT\n");
	ret = ioctl(nbd, NBD_DO_IT);
	if (ret == -1) {
		fprintf(stderr, "ioctl(NBD_DO_IT) failed: %s (%d)\n", strerror(errno), errno);
		return 1;
	}

	printf("nbd_thread> NBD_CLEAR_QUE\n");
	ret = ioctl(nbd, NBD_CLEAR_QUE);
	if (ret == -1) {
		fprintf(stderr, "ioctl(NBD_CLEAR_QUE) failed: %s (%d)\n", strerror(errno), errno);
		return 1;
	}

	printf("nbd_thread> NBD_CLEAR_SOCK\n");
	ret = ioctl(nbd, NBD_CLEAR_SOCK);
	if (ret == -1) {
		fprintf(stderr, "ioctl(NBD_CLEAR_SOCK) failed: %s (%d)\n", strerror(errno), errno);
		return 1;
	}

	printf("nbd_thread> done\n");
	return 0;
}

static
void nbd_cmd_read(int sock, uint64_t from, uint32_t len, struct nbd_reply* reply)
{
	void* ptr = *(void**)reply->handle;
	printf("nbd_cmd_read> from: %llu, len: %d, handle: %p\n", from, len, ptr);
	reply->error = 0;
	write_all(sock, (char*)reply, sizeof(*reply));
	char* chunk = (char*)calloc(1, len);
	write_all(sock, chunk, len);
	free(chunk);
}

static
void nbd_cmd_write(int sock, uint64_t from, uint32_t len, struct nbd_reply* reply)
{
	printf("nbd_cmd_write> from: %llu, len: %d\n", from, len);
	char* chunk = (char*)calloc(1, len);
	read_all(sock, chunk, len);
	reply->error = 0;
	free(chunk);
	write_all(sock, (char*)reply, sizeof(*reply));
}

static
void nbd_cmd_loop(int sock)
{
	struct nbd_reply reply;
	bzero(&reply, sizeof(reply));
	reply.magic = htonl(NBD_REPLY_MAGIC);

	struct nbd_request request;
	ssize_t bytes_read;
	while ((bytes_read = read(sock, &request, sizeof(request))) > 0) {
		uint32_t type = ntohl(request.type);
		printf("%s\n", nbd_cmd_str(type));
		memcpy(reply.handle, request.handle, sizeof(reply.handle));
		uint64_t from = ntohll(request.from);
		uint32_t len = ntohl(request.len);

		switch (type) {
			case NBD_CMD_READ:
				nbd_cmd_read(sock, from, len, &reply);
				break;
			case NBD_CMD_WRITE:
				nbd_cmd_write(sock, from, len, &reply);
				break;
			case NBD_CMD_DISC:
				return;
			case NBD_CMD_FLUSH:
				break;
			case NBD_CMD_TRIM:
				break;
		}
	}

	printf("nbd_cmd_loop> done\n");
}

int main(int argc, char *argv[])
{
	int pair[2];
	int ret = socketpair(AF_UNIX, SOCK_STREAM, 0, pair);
	if (ret == -1) {
		fprintf(stderr, "socketpair() failed: %s (%d)\n", strerror(errno), errno);
		return 1;
	}

	int nbd = open("/dev/nbd1", O_RDWR);
	if (nbd == -1) {
		fprintf(stderr, "open() failed: %s (%d)\n", strerror(errno), errno);
		return 1;
	}

	ret = ioctl(nbd, NBD_SET_SIZE, 100*1024*1024);
	if (ret == -1) {
		fprintf(stderr, "ioctl(NBD_SET_SIZE) failed: %s (%d)\n", strerror(errno), errno);
		return 1;
	}

	ret = ioctl(nbd, NBD_CLEAR_SOCK);
	if (ret == -1) {
		fprintf(stderr, "ioctl(NBD_CLEAR_SOCK) failed: %s (%d)\n", strerror(errno), errno);
		return 1;
	}

	std::thread thread(nbd_thread, nbd, pair[1]);
	nbd_cmd_loop(pair[0]);
	printf("Waiting for thread...\n");
	thread.join();
	printf("Done\n");

	close(nbd);
	return 0;
}
