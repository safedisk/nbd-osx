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
#include <miscfs/devfs/devfs.h>

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
    uint32_t   block_size;
    uint64_t   disk_size;
} nbd_table[NBD_DEVICES];

int nbd_open(dev_t dev, int flags, int devtype, struct proc* p);
int nbd_close(dev_t dev, int flags, int devtype, struct proc* p);
void nbd_strategy(struct buf* bp);
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
	
	bzero((void*)nbd_table, sizeof(nbd_table));

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
    	devfs_remove(nbd_table[i].bdev);
    	nbd_table[i].bdev = NULL;
    	nbd_table[i].dev  = 0;
    	nbd_table[i].pid  = -1;
    }

	bdevsw_remove(nbd_bdev_major, &nbd_bdevsw);
	nbd_bdev_major = -1;

	return KERN_SUCCESS;
}


int nbd_open(dev_t dev, int flags, int devtype, struct proc* p)
{
	struct nbd_device* nbd_dev = nbd_get_dev(dev);
	if (!nbd_dev) {
		return (ENXIO);
	}

	printf("nbd_open> unit: %d\n", nbd_dev->unit);

	return(0);
}

int nbd_close(dev_t dev, int flags, int devtype, struct proc* p)
{
	struct nbd_device* nbd_dev = nbd_get_dev(dev);
	if (!nbd_dev) {
		return (ENXIO);
	}

	printf("nbd_close> unit: %d\n", nbd_dev->unit);
	return(0);
}

void nbd_async_read(struct nbd_device* nbd_dev, struct buf* bp)
{
	uint32_t count = buf_count(bp);
	uint64_t start = (uint64_t)buf_blkno(bp) * nbd_dev->block_size;
	printf("nbd_async_read> unit: %d, start: %llu, count: %u\n", nbd_dev->unit, start, count);
}

void nbd_async_write(struct nbd_device* nbd_dev, struct buf* bp)
{
	uint32_t count = buf_count(bp);
	uint64_t start = (uint64_t)buf_blkno(bp) * nbd_dev->block_size;
	printf("nbd_async_write> unit: %d, start: %llu, count: %u\n", nbd_dev->unit, start, count);
}

void nbd_strategy(struct buf* bp)
{
	dev_t dev = buf_device(bp);
	struct nbd_device* nbd_dev = nbd_get_dev(dev);
	if (!nbd_dev) {
		buf_setresid(bp, buf_count(bp));
		buf_seterror(bp, ENXIO);
		buf_biodone(bp);
		return;
	}

	// uintptr_t data = buf_dataptr(bp);
	// bufattr_t attr = buf_attr(bp);

	// if (start >= mediaSize) {
	// }

	if (buf_flags(bp) & B_READ) {
		nbd_async_read(nbd_dev, bp);
	}
	else {
		nbd_async_write(nbd_dev, bp);
	}

	buf_setresid(bp, 0);
	buf_seterror(bp, 0);
	buf_biodone(bp);
}

int nbd_ioctl(dev_t dev, u_long cmd, caddr_t data, int fflag, struct proc* p)
{
	struct nbd_device* nbd_dev = nbd_get_dev(dev);
	if (!nbd_dev) {
		return (ENXIO);
	}

	printf("nbd_ioctl> unit: %d, cmd: %s (0x%8.8lx)\n", nbd_dev->unit, ioctl_cmd(cmd), cmd);

	int error = 0;
	uint32_t* arg32 = (uint32_t *)data;
	uint64_t* arg64 = (uint64_t *)data;

	switch (cmd) {
		case DKIOCGETBLOCKSIZE:
			*arg32 = nbd_dev->block_size;
			break;
		case DKIOCSETBLOCKSIZE:
			if (*arg32 > 0) {
				printf("nbd_ioctl> DKIOCSETBLOCKSIZE: %u\n", *arg32);
				nbd_dev->block_size = *arg32;
			}
			else {
				error = EINVAL;
			}
			break;
		case DKIOCGETBLOCKCOUNT:
			if (nbd_dev->block_size) {
				*arg64 = nbd_dev->disk_size / nbd_dev->block_size;
			}
			else {
				*arg64 = 0;
			}
			break;
		default:
			error = ENOTTY;
			break;
	}

	return (error);
}

int nbd_psize(dev_t dev)
{
	struct nbd_device* nbd_dev = nbd_get_dev(dev);
	if (!nbd_dev) {
		return (ENXIO);
	}

	printf("nbd_psize> unit: %d\n", nbd_dev->unit);

	return nbd_dev->block_size;
}
