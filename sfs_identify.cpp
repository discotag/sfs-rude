/*
 * Copyright 2026, liilliil <haiku@liilliil.net>
 * All rights reserved. Distributed under the terms of the MIT license.

 * sfs_identify.cpp — Haiku kernel file_system add-on
 * Identifies Amiga Smart FileSystem (SFS) partitions and extracts
 * the volume (disk) name.
 *
 * The on-disk layout and checksum are validated against an actual SFS
 * volume image (SFS v3, 512-byte blocks).
 */

#include <fs_interface.h>
#include <KernelExport.h>
#include <string.h>
#include <stdlib.h>
#include <ByteOrder.h>
#include "sfs_charset.h"

#define SFS_ROOTBLOCK_ID	0x53465300   /* "SFS\0" big-endian */
#define SFS_OBJC_ID			0x4F424A43
#define SFS_OBJC_HDR      24
#define SFS_OBJ_NAME_OFF  25

typedef struct {
    uint32 id, checksum, ownblock;
    uint16 version, sequence;
    uint32 date_created;
    uint8 bits, pad1;
    uint16 pad2;
    uint32 reserved1[2];
    uint32 firstbyte_h, firstbyte, lastbyte_h, lastbyte;
    uint32 total_blocks, block_size;
    uint32 reserved2[2], reserved3[8];
    uint32 bitmap_base, admin_container, root_container, extent_container, object_node_root;
    uint32 reserved4[3];
} __attribute__((packed)) sfs_root_block;

typedef struct {
    int fd;
    uint32 block_size, total_blocks, root_container;
} sfs_identify_cookie;

static bool sfs_checksum_ok(const uint8* b, uint32 size) {
    const uint32* p = (const uint32*)b;
    uint32 sum = 0;
    for (uint32 i = 0; i < size / 4; i++) sum += B_BENDIAN_TO_HOST_INT32(p[i]);
    return sum == 0xffffffffUL;
}

static status_t sfs_read_block(int fd, uint32 block, uint32 bs, void* buf) {
    if (read_pos(fd, (off_t)block * bs, buf, bs) != (ssize_t)bs) return B_IO_ERROR;
    return B_OK;
}

/*
 * identify_partition()
 * Возвращает приоритет (float)
 */
float sfs_identify_partition(int fd, partition_data* partition, void** _cookie) {
    uint8 first[512];
    if (sfs_read_block(fd, 0, 512, first) != B_OK) return -1.0f;
    sfs_root_block* r = (sfs_root_block*)first;
    uint32 rootID = B_BENDIAN_TO_HOST_INT32(r->id);
    if (rootID != SFS_ROOTBLOCK_ID && rootID != 0x53465302) return -1.0f;

    uint32 bs = B_BENDIAN_TO_HOST_INT32(r->block_size);
    if (bs < 512 || bs > 4096 || (bs & (bs - 1)) != 0) {
        dprintf("sfs: bad block size %u\n", (unsigned)bs);
        return -1.0f;
    }

    uint8* buf = (uint8*)malloc(bs);
    if (buf == NULL) return -1.0f;
    if (sfs_read_block(fd, 0, bs, buf) != B_OK) { free(buf); return -1.0f; }
    if (B_BENDIAN_TO_HOST_INT32(((sfs_root_block*)buf)->ownblock) != 0
        || B_BENDIAN_TO_HOST_INT16(((sfs_root_block*)buf)->version) != 3) {
        dprintf("sfs: root block ownblock != 0\n");
        free(buf);
        return -1.0f;
    }
    bool checksumOk = sfs_checksum_ok(buf, bs);
    if (!checksumOk) {
        dprintf("sfs: root block checksum mismatch\n");
        free(buf);
        return -1.0f;
    }

    sfs_identify_cookie* c = (sfs_identify_cookie*)malloc(sizeof(*c));
    if (c == NULL) { free(buf); return -1.0f; }
    r = (sfs_root_block*)buf;
    c->fd = fd;
    c->block_size = bs;
    c->total_blocks = B_BENDIAN_TO_HOST_INT32(r->total_blocks);
    c->root_container = B_BENDIAN_TO_HOST_INT32(r->root_container);
    free(buf);
    if (c->total_blocks == 0 || c->root_container >= c->total_blocks) {
        free(c);
        return -1.0f;
    }
    *_cookie = c;
    return 0.95f;
}

/*
 * scan_partition()
 *
 * Called after identify_partition() succeeds. Fills in partition_data
 * fields, including content_name (volume label) and content_size.
 */
status_t sfs_scan_partition(int fd, partition_data* partition, void* _cookie) {
    sfs_identify_cookie* c = (sfs_identify_cookie*)_cookie;
    char* name = NULL;
    uint8* buf = (uint8*)malloc(c->block_size);
    if (buf == NULL) return B_NO_MEMORY;

    if (sfs_read_block(fd, c->root_container, c->block_size, buf) == B_OK
        && B_BENDIAN_TO_HOST_INT32(((uint32*)buf)[0]) == SFS_OBJC_ID
        && B_BENDIAN_TO_HOST_INT32(((uint32*)buf)[2]) == c->root_container
        && sfs_checksum_ok(buf, c->block_size)) {
        size_t off = SFS_OBJC_HDR + SFS_OBJ_NAME_OFF, n = 0;
        if (B_BENDIAN_TO_HOST_INT32(((uint32*)buf)[7]) != 1
            || (buf[off - 1] & 0x80) == 0) {
            free(buf);
            return B_BAD_DATA;
        }
        while (off + n < c->block_size && buf[off + n] != 0) n++;
        if (off + n >= c->block_size) {
            free(buf);
            return B_BAD_DATA;
        }
        name = sfs_name_to_utf8(buf + off, n, SFS_CS_DEFAULT);
    } else {
        dprintf("sfs: root object container at block %u not found or damaged\n", (unsigned)c->root_container);
        free(buf);
        return B_BAD_DATA;
    }
    free(buf);
    if (name == NULL)
        return B_NO_MEMORY;

    partition->status = B_PARTITION_VALID;
    partition->flags |= B_PARTITION_FILE_SYSTEM;
    partition->content_size = (off_t)c->total_blocks * c->block_size;
    partition->block_size = c->block_size;
    partition->content_name = name;
    return B_OK;
}

void
sfs_free_identify_partition_cookie(partition_data *partition, void *_cookie){
    (void)partition;
    free(_cookie);
}

