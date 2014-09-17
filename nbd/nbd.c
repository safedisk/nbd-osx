//
//  nbd.c
//  nbd
//
//  Created by Frank Laub on 9/13/14.
//  Copyright (c) 2014 JFInc. All rights reserved.
//

#include "nbd_api.h"

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/buf.h>
#include <sys/disk.h>
#include <sys/file.h>
#include <sys/kpi_socket.h>
#include <miscfs/devfs/devfs.h>
#include <libkern/OSByteOrder.h>

#define NBD_DEVICE_BASENAME "nbd"

static const size_t NBD_BLOCK_SIZE = 4096;
static const size_t NBD_DEVICES = 16;
static int nbd_bdev_major       = -1;

struct nbd_device
{
	int        unit;
    pid_t      pid;
    dev_t      dev;
    void*      bdev;
    socket_t   sock;
    int        flags;
    uint32_t   block_size;
    uint64_t   disk_size;
} nbd_table[NBD_DEVICES];

int nbd_open(dev_t dev, int flags, int devtype, struct proc* p);
int nbd_close(dev_t dev, int flags, int devtype, struct proc* p);
void nbd_strategy(buf_t bp);
int nbd_ioctl(dev_t dev, u_long cmd, caddr_t data, int fflag, struct proc* p);
int nbd_dump(void);
int nbd_psize(dev_t dev);

static struct bdevsw nbd_bdevsw = {
	nbd_open,
	nbd_close,
	nbd_strategy,
	nbd_ioctl,
	eno_dump,
	nbd_psize,
	D_DISK,
};

const char* ioctl_cmd(u_long cmd)
{
	#define CODE_CASE(code) case code: return #code;
	switch (cmd) {
		// Disk commands
		CODE_CASE(DKIOCEJECT);
		CODE_CASE(DKIOCSYNCHRONIZECACHE);
		CODE_CASE(DKIOCFORMAT);
		CODE_CASE(DKIOCGETFORMATCAPACITIES);
		CODE_CASE(DKIOCGETBLOCKSIZE);
		CODE_CASE(DKIOCGETBLOCKCOUNT);
		CODE_CASE(DKIOCGETFIRMWAREPATH);
		CODE_CASE(DKIOCISFORMATTED);
		CODE_CASE(DKIOCISWRITABLE);
		CODE_CASE(DKIOCREQUESTIDLE);
		CODE_CASE(DKIOCUNMAP);
		CODE_CASE(DKIOCGETMAXBLOCKCOUNTREAD);
		CODE_CASE(DKIOCGETMAXBLOCKCOUNTWRITE);
		CODE_CASE(DKIOCGETMAXBYTECOUNTREAD);
		CODE_CASE(DKIOCGETMAXBYTECOUNTWRITE);
		CODE_CASE(DKIOCGETMAXSEGMENTCOUNTREAD);
		CODE_CASE(DKIOCGETMAXSEGMENTCOUNTWRITE);
		CODE_CASE(DKIOCGETMAXSEGMENTBYTECOUNTREAD);
		CODE_CASE(DKIOCGETMAXSEGMENTBYTECOUNTWRITE);
		CODE_CASE(DKIOCGETMINSEGMENTALIGNMENTBYTECOUNT);
		CODE_CASE(DKIOCGETMAXSEGMENTADDRESSABLEBITCOUNT);
		CODE_CASE(DKIOCGETFEATURES);
		CODE_CASE(DKIOCGETPHYSICALBLOCKSIZE);
		CODE_CASE(DKIOCGETCOMMANDPOOLSIZE);
		CODE_CASE(DKIOCGETBLOCKCOUNT32);
		CODE_CASE(DKIOCSETBLOCKSIZE);
		CODE_CASE(DKIOCGETBSDUNIT);
		CODE_CASE(DKIOCISSOLIDSTATE);
		CODE_CASE(DKIOCISVIRTUAL);
		CODE_CASE(DKIOCGETBASE);
		CODE_CASE(DKIOCGETTHROTTLEMASK);
		CODE_CASE(DKIOCLOCKPHYSICALEXTENTS);
		CODE_CASE(DKIOCGETPHYSICALEXTENT);
		CODE_CASE(DKIOCUNLOCKPHYSICALEXTENTS);
		CODE_CASE(DKIOCGETMAXPRIORITYCOUNT);
		// NBD commands
		CODE_CASE(NBD_SET_SOCK);
		CODE_CASE(NBD_SET_BLKSIZE);
		CODE_CASE(NBD_SET_SIZE);
		CODE_CASE(NBD_DO_IT);
		CODE_CASE(NBD_CLEAR_SOCK);
		CODE_CASE(NBD_CLEAR_QUE);
		CODE_CASE(NBD_PRINT_DEBUG);
		CODE_CASE(NBD_SET_SIZE_BLOCKS);
		CODE_CASE(NBD_DISCONNECT);
		CODE_CASE(NBD_SET_TIMEOUT);
		CODE_CASE(NBD_SET_FLAGS);
		default:
			return "Unknown";
	}
	#undef CODE_CASE
}

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
struct nbd_device* nbd_get_dev(dev_t dev)
{
	int unit = minor(dev);
	if (unit >= 0 && unit < NBD_DEVICES) {
		return &nbd_table[unit];
	}
	return NULL;
}

kern_return_t nbd_start(kmod_info_t* ki, void* d)
{
	int i;

	printf("nbd_start>\n");
	// asm("int $3");
	
	bzero(nbd_table, sizeof(nbd_table));

	if ((nbd_bdev_major = bdevsw_add(-1, &nbd_bdevsw)) == -1) {
		goto error;
	}

	for (i = 0; i < NBD_DEVICES; i++) {
		dev_t dev = makedev(nbd_bdev_major, i);
		nbd_table[i].bdev = devfs_make_node(
			dev,
			DEVFS_BLOCK,
			UID_ROOT,
			GID_OPERATOR,
			0666,
			NBD_DEVICE_BASENAME "%d",
			i
		);
		if (nbd_table[i].bdev == NULL) {
			goto error;
		}

		nbd_table[i].unit = i;
		nbd_table[i].dev = dev;
		nbd_table[i].pid = -1;
		nbd_table[i].block_size = NBD_BLOCK_SIZE;
		nbd_table[i].disk_size = 1024*1024*1024; // 1 GB
	}

	return KERN_SUCCESS;

error:
	for (--i; i >= 0; i--) {
		devfs_remove(nbd_table[i].bdev);
		nbd_table[i].bdev = NULL;
		nbd_table[i].dev = 0;
		// lck_mtx_free(nbd_table[i].mtx, nbd_lock_group);
	}

	bdevsw_remove(nbd_bdev_major, &nbd_bdevsw);
	nbd_bdev_major = -1;

	return KERN_FAILURE;
}

kern_return_t nbd_stop(kmod_info_t* ki, void* d)
{
	printf("nbd_stop>\n");

	if (nbd_bdev_major == -1) {
		return KERN_SUCCESS;
	}

    for (int i = 0; i < NBD_DEVICES; i++) {
		printf("nbd_stop> devfs_remove(%d)\n", i);
    	devfs_remove(nbd_table[i].bdev);
    	nbd_table[i].bdev = NULL;
    	nbd_table[i].dev  = 0;
    	nbd_table[i].pid  = -1;
    }

	printf("nbd_stop> bdevsw_remove(%d)\n", nbd_bdev_major);
	bdevsw_remove(nbd_bdev_major, &nbd_bdevsw);
	nbd_bdev_major = -1;

	return KERN_SUCCESS;
}

static
errno_t sock_xmit(struct nbd_device* nbd, int send, void* buf, int size)
{
	if (!nbd->sock) {
		printf("sock_xmit[%d]> socket closed\n", nbd->unit);
		return EINVAL;
	}

	do {
		struct iovec iov;
		struct msghdr msg;
		size_t msglen;

		iov.iov_base = buf;
		iov.iov_len = size;
		bzero(&msg, sizeof(msg));
		msg.msg_iov = &iov;
		msg.msg_iovlen = 1;

		errno_t err;

		if (send) {
			err = sock_send(nbd->sock, &msg, 0, &msglen);
		}
		else {
			err = sock_receive(nbd->sock, &msg, MSG_WAITALL, &msglen);
		}

		if (err) {
			return err;
		}

		size -= msglen;
		buf += msglen;
	} while (size > 0);

	return 0;
}

static
void nbd_end_request(buf_t bp, uint32_t remain, errno_t err)
{
	printf("nbd_end_request> remain: %u, error: %u\n", remain, err);
	buf_setresid(bp, remain);
	buf_seterror(bp, err);
	buf_biodone(bp);
}

static
errno_t nbd_read_reply(struct nbd_device* nbd)
{
	printf("nbd_read_reply>\n");

	struct nbd_reply reply;

	bzero(&reply, sizeof(reply));
	errno_t err = sock_xmit(nbd, 0, &reply, sizeof(reply));
	if (err) {
		printf("nbd_read_reply> sock_xmit() failed: %d\n", err);
		return err;
	}

	uint32_t magic = ntohl(reply.magic);
	if (magic != NBD_REPLY_MAGIC) {
		printf("nbd_read_reply> wrong magic (0x%8.8x)\n", magic);
		return EINVAL;
	}

	// TODO: validate this handle, make sure it's legit
	buf_t bp = *(buf_t*)reply.handle;

	err = ntohl(reply.error);
	if (err) {
		printf("nbd_read_reply> error from server: %d\n", err);
		if (bp) {
			nbd_end_request(bp, buf_count(bp), err);
		}
		return err;
	}

	printf("nbd_read_reply> got reply: %p\n", bp);

	if (bp) {
		if (buf_flags(bp) & B_READ) {
			printf("nbd_read_reply> B_READ\n");
			uintptr_t data = buf_dataptr(bp);
			uint32_t count = buf_count(bp);
			err = sock_xmit(nbd, 0, (void*)data, count);
			if (err) {
				printf("nbd_read_reply> sock_xmit(data) failed: %d\n", err);
				nbd_end_request(bp, buf_count(bp), err);
				return err;
			}
		}
		nbd_end_request(bp, 0, 0);
	}

	return 0;
}

static
errno_t nbd_send_request(struct nbd_device* nbd, buf_t bp, int cmd)
{
	printf("nbd_send_request[%d]> cmd: %d, bp: %p\n", nbd->unit, cmd, bp);

	struct nbd_request request;
	bzero(&request, sizeof(request));
	request.magic = htonl(NBD_REQUEST_MAGIC);
	request.type = htonl(cmd);

	if (bp) {
		uint32_t count = buf_count(bp);
		uint64_t start = (uint64_t)buf_blkno(bp) * nbd->block_size;
		memcpy(request.handle, &bp, sizeof(bp));
		request.from = OSSwapHostToBigInt64(start);
		request.len = htonl(count);
	}

	errno_t err = sock_xmit(nbd, 1, &request, sizeof(request));
	if (err) {
		printf("nbd_send_request> sock_xmit(request) failed: %d\n", err);
		return err;
	}

	if (cmd == NBD_CMD_WRITE) {
		uint32_t count = buf_count(bp);
		uintptr_t data = buf_dataptr(bp);
		err = sock_xmit(nbd, 1, (void*)data, count);
		if (err) {
			printf("nbd_send_request> sock_xmit(data) failed: %d\n", err);
			return err;
		}
	}

	return 0;
}

static
errno_t nbd_do_it(struct nbd_device* nbd)
{
	errno_t err;
	printf("nbd_do_it>\n");
	while ((err = nbd_read_reply(nbd)) == 0) {
	}
	printf("nbd_do_it> done: %d\n", err);
	nbd->sock = NULL;
	return err;
}

int nbd_open(dev_t dev, int flags, int devtype, struct proc* p)
{
	struct nbd_device* nbd = nbd_get_dev(dev);
	if (!nbd) {
		printf("nbd_open[%d]> ENXIO\n", minor(dev));
		return (ENXIO);
	}

	printf("nbd_open[%d]>\n", nbd->unit);

	return(0);
}

int nbd_close(dev_t dev, int flags, int devtype, struct proc* p)
{
	struct nbd_device* nbd = nbd_get_dev(dev);
	if (!nbd) {
		printf("nbd_close[%d]> ENXIO\n", minor(dev));
		return (ENXIO);
	}

	printf("nbd_close[%d]>\n", nbd->unit);
	return(0);
}

void nbd_strategy(buf_t bp)
{
	dev_t dev = buf_device(bp);
	struct nbd_device* nbd = nbd_get_dev(dev);
	if (!nbd) {
		printf("nbd_strategy[%d]> ENXIO\n", minor(dev));
		buf_setresid(bp, buf_count(bp));
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
		return;
	}

	// TODO: check that buf lies within media
	errno_t err = ENXIO;
	if (buf_flags(bp) & B_READ) {
		err = nbd_send_request(nbd, bp, NBD_CMD_READ);
	}
	else {
		err = nbd_send_request(nbd, bp, NBD_CMD_WRITE);
	}
	if (err) {
		printf("nbd_strategy[%d]> error: %d\n", nbd->unit, err);
		buf_setresid(bp, buf_count(bp));
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
	}
}

int nbd_ioctl(dev_t dev, u_long cmd, caddr_t data, int fflag, struct proc* p)
{
	struct nbd_device* nbd = nbd_get_dev(dev);
	if (!nbd) {
		printf("nbd_ioctl[%d]> ENXIO\n", minor(dev));
		return (ENXIO);
	}

	printf("nbd_ioctl[%d]> cmd: %s (0x%8.8lx)\n", nbd->unit, ioctl_cmd(cmd), cmd);

	int err = 0;
	uint32_t* arg32 = (uint32_t *)data;
	uint64_t* arg64 = (uint64_t *)data;

	switch (cmd) {
		case DKIOCGETBLOCKSIZE:
			*arg32 = nbd->block_size;
			return 0;
		case DKIOCSETBLOCKSIZE:
			if (*arg32 > 0) {
				printf("nbd_ioctl> DKIOCSETBLOCKSIZE: %u\n", *arg32);
				nbd->block_size = *arg32;
				return 0;
			}
			return EINVAL;
		case DKIOCGETBLOCKCOUNT:
			if (nbd->block_size) {
				*arg64 = nbd->disk_size / nbd->block_size;
			}
			else {
				*arg64 = 0;
			}
			return 0;
		case NBD_SET_SOCK: {
			printf("nbd_ioctl> NBD_SET_SOCK> fd: %u, pid: %u\n", *arg32, proc_pid(p));
			if (nbd->sock) {
				return (EBUSY);
			}
			socket_t sock;
			err = file_socket(*arg32, &sock);
			if (!err) {
				nbd->sock = sock;
			}
			return err;
		}
		case NBD_SET_BLKSIZE:
			printf("nbd_ioctl> NBD_SET_BLKSIZE: %u\n", *arg32);
			nbd->block_size = *arg32;
			nbd->disk_size &= ~(nbd->block_size - 1);
			return 0;
		case NBD_SET_SIZE:
			printf("nbd_ioctl> NBD_SET_SIZE: %llu\n", *arg64);
			nbd->disk_size = (*arg64) & ~(nbd->block_size - 1);
			return 0;
		case NBD_DO_IT:
			return nbd_do_it(nbd);
		case NBD_CLEAR_SOCK:
			nbd->sock = NULL;
			return 0;
		case NBD_CLEAR_QUE:
			break;
		case NBD_PRINT_DEBUG:
			break;
		case NBD_SET_SIZE_BLOCKS:
			nbd->disk_size = (*arg64) * nbd->block_size;
			return 0;
		case NBD_DISCONNECT:
			return nbd_send_request(nbd, NULL, NBD_CMD_DISC);
		case NBD_SET_TIMEOUT:
			break;
		case NBD_SET_FLAGS:
			nbd->flags = *arg32;
			return 0;
		default:
			return ENOTTY;
	}

	return (err);
}

int nbd_psize(dev_t dev)
{
	struct nbd_device* nbd = nbd_get_dev(dev);
	if (!nbd) {
		printf("nbd_psize[%d]> ENXIO\n", minor(dev));
		return (ENXIO);
	}

	printf("nbd_psize[%d]>\n", nbd->unit);

	return nbd->block_size;
}
