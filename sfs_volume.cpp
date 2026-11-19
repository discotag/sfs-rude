/*
 * Amiga Smart File System (SFS) read-only Haiku filesystem add-on.
 * Copyright 2026, liilliil <haiku@liilliil.net>
 * Distributed under the terms of the MIT license.
*/

#include <fs_interface.h>
#include <dirent.h>
#include <kernel/fs_info.h>
#include <KernelExport.h>
#include <ByteOrder.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <stddef.h>
#include <unistd.h>

#include "sfs_charset.h"

#define SFS_ROOT_ID       0x53465300UL /* SFS\\0 */
#define SFS_ROOT2_ID      0x53465302UL /* SFS\\2 */
#define SFS_OBJC_ID       0x4f424a43UL /* OBJC */
#define SFS_BNDC_ID       0x424e4443UL /* BNDC */
#define SFS_NDC_ID        0x4e444320UL /* NDC  */
#define SFS_HTAB_ID       0x48544142UL /* HTAB */
#define SFS_MAX_NAME      105
#define SFS_OBJECT_FIXED  25
#define SFS_CONTAINER_HDR 24
#define SFS_NODE_SIZE     10
#define SFS_TYPE_HIDDEN   0x01
#define SFS_TYPE_DIR      0x80

struct SFSRootBlock {
	uint32 id;
	uint32 checksum;
	uint32 ownblock;
	uint16 version;
	uint16 sequence;
	uint32 dateCreated;
	uint8 bits;
	uint8 pad1;
	uint16 pad2;
	uint32 reserved1[2];
	uint32 firstByteHigh;
	uint32 firstByte;
	uint32 lastByteHigh;
	uint32 lastByte;
	uint32 totalBlocks;
	uint32 blockSize;
	uint32 reserved2[2];
	uint32 reserved3[8];
	uint32 bitmapBase;
	uint32 adminContainer;
	uint32 rootContainer;
	uint32 extentBNodeRoot;
	uint32 objectNodeRoot;
	uint32 reserved4[3];
} __attribute__((packed));

struct SFSVolume {
	int fd;
	uint32 blockSize;
	uint32 totalBlocks;
	uint32 freeBlocks;
	uint32 rootContainer;
	uint32 extentRoot;
	uint32 objectNodeRoot;
	uint32 adminContainer;
	uint32 bitmapBase;
	uint8 rootBits;
	int charset;
	char name[128];
	char device[ B_PATH_NAME_LENGTH ];
};

struct SFSVNode {
	uint32 id;
	uint32 parent;
	uint32 containerBlock;
	uint32 firstDirBlock;
	uint32 hashTable;
	uint32 firstExtent;
	uint32 size;
	uint32 dateModified;
	uint32 protection;
	uint16 ownerUID;
	uint16 ownerGID;
	uint8 bits;
	bool isDirectory;
	char name[ SFS_MAX_NAME * 3 + 1 ];
};

static fs_vnode_ops *gSFSVnodeOpsPtr;

struct SFSFileCookie { uint32 unused; };

struct SFSDirCookie {
	uint32 block;
	uint32 loadedBlock;
	size_t offset;
	uint8 *buffer;
	uint32 blocksVisited;
	bool dotDone;
	bool dotDotDone;
	bool finished;
};

static uint32 sfs_be32(const void *p)
{
	uint32 value;
	memcpy(&value, p, sizeof(value));
	return B_BENDIAN_TO_HOST_INT32(value);
}

static uint16 sfs_be16(const void *p)
{
	uint16 value;
	memcpy(&value, p, sizeof(value));
	return B_BENDIAN_TO_HOST_INT16(value);
}

static bool sfs_checksum_ok(const uint8 *data, uint32 size)
{
	if ((size & 3) != 0)
		return false;
	uint32 sum = 0;
	for (uint32 i = 0; i < size; i += 4)
		sum += sfs_be32(data + i);
	return sum == 0xffffffffUL;
}

static status_t sfs_read_block(SFSVolume *volume, uint32 block, void *buffer)
{
	if (block >= volume->totalBlocks)
		return B_BAD_DATA;
	ssize_t bytes = read_pos(volume->fd,
		(off_t)block * volume->blockSize, buffer, volume->blockSize);
	return bytes == (ssize_t)volume->blockSize ? B_OK : B_IO_ERROR;
}

static bool sfs_block_ok(SFSVolume *volume, uint32 block, uint32 id,
	const uint8 *data)
{
	return sfs_be32(data) == id
		&& sfs_be32(data + 8) == block
		&& sfs_checksum_ok(data, volume->blockSize);
}

/* fsObject is a variable-length record. The name and comment are NUL
 * terminated; the next record starts on an even byte boundary. */
static size_t sfs_next_object(const uint8 *block, size_t offset, uint32 blockSize)
{
	if (offset + SFS_OBJECT_FIXED + 2 > blockSize)
		return blockSize;
	size_t p = offset + SFS_OBJECT_FIXED;
	int terminators = 0;
	while (p < blockSize && terminators < 2) {
		if (block[p++] == 0)
			terminators++;
	}
	if (terminators != 2)
		return blockSize;
	if (p & 1)
		p++;
	return p <= blockSize ? p : blockSize;
}

static status_t sfs_read_object_node(SFSVolume *volume, uint32 id,
	uint32 *containerBlock)
{
	uint8 *buffer = (uint8 *)malloc(volume->blockSize);
	if (buffer == NULL)
		return B_NO_MEMORY;
	uint32 block = volume->objectNodeRoot;
	status_t status = B_BAD_DATA;
	uint32 depth = 0;

	while (depth++ < 32) {
		if (sfs_read_block(volume, block, buffer) != B_OK
			|| !sfs_block_ok(volume, block, SFS_NDC_ID, buffer))
			break;
		uint32 first = sfs_be32(buffer + 12);
		uint32 span = sfs_be32(buffer + 16);
		if (span == 0 || id < first)
			break;
		if (span == 1) {
			uint32 index = id - first;
			size_t offset = 20 + (size_t)index * SFS_NODE_SIZE;
			if (offset + SFS_NODE_SIZE > volume->blockSize)
				break;
			uint32 data = sfs_be32(buffer + offset);
			if (data == 0 || data == 0xffffffffUL)
				break;
			*containerBlock = data;
			status = B_OK;
			break;
		}
		uint32 index = (id - first) / span;
		size_t offset = 20 + (size_t)index * 4;
		if (offset + 4 > volume->blockSize)
			break;
		uint32 packed = sfs_be32(buffer + offset);
		if (packed == 0)
			break;
		uint32 shift = 0;
		for (uint32 n = volume->blockSize; n > 1; n >>= 1)
			shift++;
		if (shift < 5)
			break;
		block = packed >> (shift - 5);
	}
	free(buffer);
	return status;
}

static status_t sfs_find_object_in_container(SFSVolume *volume,
	uint32 containerBlock, uint32 wantedID, const char *wantedName,
	uint8 *container, uint8 **foundObject)
{
	if (sfs_read_block(volume, containerBlock, container) != B_OK
		|| !sfs_block_ok(volume, containerBlock, SFS_OBJC_ID, container))
		return B_BAD_DATA;

	size_t offset = SFS_CONTAINER_HDR;
	while (offset + SFS_OBJECT_FIXED + 2 <= volume->blockSize) {
		uint8 *object = container + offset;
		uint32 id = sfs_be32(object + 4);
		if (id == 0)
			break;
		const uint8 *name = object + SFS_OBJECT_FIXED;
		size_t nameLength = strnlen((const char *)name,
			volume->blockSize - offset - SFS_OBJECT_FIXED);
		if (nameLength >= volume->blockSize - offset - SFS_OBJECT_FIXED)
			break;
		bool match = wantedID != 0 ? id == wantedID : false;
		if (wantedName != NULL) {
			char *utf8 = sfs_name_to_utf8(name, nameLength, volume->charset);
			if (utf8 != NULL) {
				match = strcmp(utf8, wantedName) == 0;
				free(utf8);
			}
		}
		if (match) {
			*foundObject = object;
			return B_OK;
		}
		size_t next = sfs_next_object(container, offset, volume->blockSize);
		if (next <= offset)
			break;
		offset = next;
	}
	return B_ENTRY_NOT_FOUND;
}

static status_t sfs_find_object(SFSVolume *volume, uint32 id,
	uint32 *containerBlock, uint8 *container, uint8 **object)
{
	status_t status = sfs_read_object_node(volume, id, containerBlock);
	if (status != B_OK)
		return status;
	return sfs_find_object_in_container(volume, *containerBlock, id, NULL,
		container, object);
}


static bool sfs_names_equal(SFSVolume *volume, const char *a, const char *b)
{
	if (volume->rootBits & 0x80)
		return strcmp(a, b) == 0;
	while (*a != 0 && *b != 0) {
		uint8 ca = (uint8)*a++;
		uint8 cb = (uint8)*b++;
		if (ca >= 'A' && ca <= 'Z') ca += 'a' - 'A';
		if (cb >= 'A' && cb <= 'Z') cb += 'a' - 'A';
		if (ca != cb) return false;
	}
	return *a == 0 && *b == 0;
}

static status_t sfs_find_in_directory(SFSVolume *volume, SFSVNode *directory,
	const char *name, uint32 *objectID)
{
	if (!directory->isDirectory)
		return B_NOT_A_DIRECTORY;
	uint8 *buffer = (uint8 *)malloc(volume->blockSize);
	if (buffer == NULL)
		return B_NO_MEMORY;
	uint32 block = directory->firstDirBlock;
	status_t status = B_ENTRY_NOT_FOUND;
	uint32 guard = 0;
	while (block != 0 && guard++ < volume->totalBlocks) {
		if (sfs_read_block(volume, block, buffer) != B_OK
			|| !sfs_block_ok(volume, block, SFS_OBJC_ID, buffer)) {
			status = B_BAD_DATA;
			break;
		}
		size_t offset = SFS_CONTAINER_HDR;
		while (offset + SFS_OBJECT_FIXED + 2 <= volume->blockSize) {
			uint8 *object = buffer + offset;
			uint32 id = sfs_be32(object + 4);
			if (id == 0)
				break;
			const uint8 *diskName = object + SFS_OBJECT_FIXED;
			size_t length = strnlen((const char *)diskName,
				volume->blockSize - offset - SFS_OBJECT_FIXED);
			if (length >= volume->blockSize - offset - SFS_OBJECT_FIXED)
				break;
			char *utf8 = sfs_name_to_utf8(diskName, length, volume->charset);
			if (utf8 != NULL) {
				bool equal = sfs_names_equal(volume, utf8, name);
				free(utf8);
				if (equal) {
					*objectID = id;
					status = B_OK;
					goto done;
				}
			}
			size_t next = sfs_next_object(buffer, offset, volume->blockSize);
			if (next <= offset)
				break;
			offset = next;
		}
		block = sfs_be32(buffer + 16);
	}
done:
	free(buffer);
	return status;
}

static status_t sfs_get_extent(SFSVolume *volume, uint32 key,
	uint8 *buffer, uint32 *physical, uint32 *next, uint16 *blocks)
{
	uint32 block = volume->extentRoot;
	uint32 guard = 0;
	while (guard++ < 32) {
		if (sfs_read_block(volume, block, buffer) != B_OK
			|| !sfs_block_ok(volume, block, SFS_BNDC_ID, buffer))
			return B_BAD_DATA;
		uint16 count = sfs_be16(buffer + 12);
		bool leaf = buffer[14] != 0;
		uint8 nodeSize = buffer[15];
		if (count == 0 || nodeSize == 0
			|| (size_t)16 + (size_t)count * nodeSize > volume->blockSize)
			return B_BAD_DATA;
		int selected = -1;
		for (uint16 i = 0; i < count; i++) {
			size_t offset = 16 + (size_t)i * nodeSize;
			uint32 nodeKey = sfs_be32(buffer + offset);
			if (nodeKey <= key)
				selected = i;
			else
				break;
		}
		if (selected < 0)
			selected = 0;
		size_t offset = 16 + (size_t)selected * nodeSize;
		if (!leaf) {
			if (nodeSize < 8)
				return B_BAD_DATA;
			block = sfs_be32(buffer + offset + 4);
			continue;
		}
		if (nodeSize < 14 || sfs_be32(buffer + offset) != key)
			return B_ENTRY_NOT_FOUND;
		*physical = sfs_be32(buffer + offset);
		*next = sfs_be32(buffer + offset + 4);
		*blocks = sfs_be16(buffer + offset + 12);
		return B_OK;
	}
	return B_BAD_DATA;
}

static status_t sfs_read_file(SFSVolume *volume, SFSVNode *node,
	off_t position, void *buffer, size_t *length)
{
	if (node->isDirectory)
		return B_IS_A_DIRECTORY;
	if (position < 0)
		return B_BAD_VALUE;
	if ((uint64)position >= node->size || *length == 0) {
		*length = 0;
		return B_OK;
	}
	if (*length > node->size - (size_t)position)
		*length = node->size - (size_t)position;
	if (node->firstExtent == 0)
		return B_BAD_DATA;

	uint8 *extentBuffer = (uint8 *)malloc(volume->blockSize);
	if (extentBuffer == NULL)
		return B_NO_MEMORY;
	uint32 logicalStart = 0;
	uint32 extentKey = node->firstExtent;
	uint32 guard = 0;
	size_t remaining = *length;
	size_t done = 0;
	status_t status = B_OK;

	while (remaining > 0 && guard++ < volume->totalBlocks) {
		uint32 physical, next;
		uint16 blocks;
		status = sfs_get_extent(volume, extentKey, extentBuffer,
			&physical, &next, &blocks);
		if (status != B_OK || blocks == 0 || physical >= volume->totalBlocks
			|| blocks > volume->totalBlocks - physical) {
			status = B_BAD_DATA;
			break;
		}
		uint64 extentBytes = (uint64)blocks * volume->blockSize;
		off_t extentStart = (off_t)logicalStart * volume->blockSize;
		off_t extentEnd = extentStart + extentBytes;
		off_t wanted = position + (off_t)done;
		if (wanted < extentStart || wanted >= extentEnd) {
			logicalStart += blocks;
			if (next == 0) {
				status = B_BAD_DATA;
				break;
			}
			extentKey = next;
			continue;
		}
		size_t within = (size_t)(wanted - extentStart);
		size_t chunk = (size_t)(extentEnd - wanted);
		if (chunk > remaining)
			chunk = remaining;
		uint32 physicalBlock = physical + (uint32)(within / volume->blockSize);
		ssize_t bytes = read_pos(volume->fd,
			(off_t)physicalBlock * volume->blockSize + (within % volume->blockSize),
			(uint8 *)buffer + done, chunk);
		if (bytes != (ssize_t)chunk) {
			status = B_IO_ERROR;
			break;
		}
		done += chunk;
		remaining -= chunk;
		if (remaining > 0 && (position + (off_t)done) >= extentEnd) {
			logicalStart += blocks;
			if (next == 0) {
				status = B_BAD_DATA;
				break;
			}
			extentKey = next;
		}
	}
	if (guard >= volume->totalBlocks && remaining > 0)
		status = B_BAD_DATA;
	*length = done;
	free(extentBuffer);
	return status;
}

static status_t sfs_read_vnode(fs_volume *fs, ino_t id, fs_vnode *node,
	int *type, uint32 *flags, bool reenter)
{
	(void)reenter;
	SFSVolume *volume = (SFSVolume *)fs->private_volume;
	SFSVNode *vnode = (SFSVNode *)calloc(1, sizeof(SFSVNode));
	if (vnode == NULL)
		return B_NO_MEMORY;
	uint8 *container = (uint8 *)malloc(volume->blockSize);
	if (container == NULL) { free(vnode); return B_NO_MEMORY; }
	uint8 *object = NULL;
	uint32 containerBlock = 0;
	status_t status = sfs_find_object(volume, (uint32)id, &containerBlock,
		container, &object);
	if (status != B_OK) {
		free(container);
		free(vnode);
		return status;
	}
	vnode->id = (uint32)id;
	vnode->containerBlock = containerBlock;
	vnode->parent = sfs_be32(container + 12);
	vnode->ownerUID = sfs_be16(object);
	vnode->ownerGID = sfs_be16(object + 2);
	vnode->protection = sfs_be32(object + 8);
	vnode->dateModified = sfs_be32(object + 20);
	vnode->bits = object[24];
	vnode->isDirectory = (vnode->bits & SFS_TYPE_DIR) != 0;
	if (vnode->isDirectory) {
		vnode->hashTable = sfs_be32(object + 12);
		vnode->firstDirBlock = sfs_be32(object + 16);
	} else {
		vnode->firstExtent = sfs_be32(object + 12);
		vnode->size = sfs_be32(object + 16);
	}
	const uint8 *diskName = object + SFS_OBJECT_FIXED;
	size_t nameLength = strnlen((const char *)diskName,
		volume->blockSize - (size_t)(diskName - container));
	if (nameLength < sizeof(vnode->name)) {
		char *utf8 = sfs_name_to_utf8(diskName, nameLength, volume->charset);
		if (utf8 != NULL) {
			strlcpy(vnode->name, utf8, sizeof(vnode->name));
			free(utf8);
		}
	}

	free(container);
	node->private_node = vnode;
	node->ops = gSFSVnodeOpsPtr;
	*type= (vnode->isDirectory ? S_IFDIR : S_IFREG) | 0555;
	*flags = 0;
	return B_OK;
}

static status_t sfs_put_vnode(fs_volume *fs, fs_vnode *node, bool reenter)
{
	(void)fs; (void)reenter;
	free(node->private_node);
	node->private_node = NULL;
	return B_OK;
}

static status_t sfs_lookup(fs_volume *fs, fs_vnode *dir,
	const char *name, ino_t *vnid)
{
	SFSVolume *volume = (SFSVolume *)fs->private_volume;
	SFSVNode *directory = (SFSVNode *)dir->private_node;
	if (directory == NULL || !directory->isDirectory || name == NULL
		|| vnid == NULL)
		return B_BAD_VALUE;

	uint32 id;
	if (strcmp(name, ".") == 0) {
		id = directory->id;
	} else if (strcmp(name, "..") == 0) {
		id = directory->parent == 0 ? 1 : directory->parent;
	} else {
		status_t status = sfs_find_in_directory(volume, directory, name, &id);
		if (status != B_OK)
			return status;
	}

	/*
	 * lookup() must acquire the vnode reference before returning its ID.
	 * The VFS relies on this reference when it resolves the directory entry.
	 * Without get_vnode(), lookup_dir_entry() can find an ID in the entry
	 * cache but cannot resolve it to a live vnode.
	 */
	*vnid = id;
	void *privateNode = NULL;
	status_t status = get_vnode(fs, id, &privateNode);
	if (status != B_OK)
		return status;
	return B_OK;
}

static status_t sfs_get_vnode_name(fs_volume *fs, fs_vnode *node,
	char *buffer, size_t bufferSize)
{
	SFSVNode *vnode = (SFSVNode *)node->private_node;
	SFSVolume *volume = (SFSVolume *)fs->private_volume;
	const char *name = vnode->id == 1 ? volume->name : vnode->name;
	if (strlen(name) + 1 > bufferSize)
		return B_BUFFER_OVERFLOW;
	strlcpy(buffer, name, bufferSize);
	return B_OK;
}

static status_t sfs_access(fs_volume *fs, fs_vnode *node, int mode)
{
	(void)fs; (void)node;
	return (mode & W_OK) != 0 ? B_READ_ONLY_DEVICE : B_OK;
}

static status_t sfs_read_stat(fs_volume *fs, fs_vnode *node, struct stat *st)
{
	SFSVolume *volume = (SFSVolume *)fs->private_volume;
	SFSVNode *vnode = (SFSVNode *)node->private_node;
	memset(st, 0, sizeof(*st));
	st->st_dev = fs->id;
	st->st_ino = vnode->id;
	st->st_mode = (vnode->isDirectory ? S_IFDIR : S_IFREG) | 0555;
	st->st_type = vnode->isDirectory ? S_IFDIR : S_IFREG;
	st->st_uid = vnode->ownerUID;
	st->st_gid = vnode->ownerGID;
	st->st_nlink = vnode->isDirectory ? 2 : 1;
	st->st_size = vnode->size;
	st->st_blksize = volume->blockSize;
	st->st_blocks = (vnode->size + 511) / 512;
	st->st_mtim.tv_sec = vnode->dateModified + 252460800;
	st->st_mtim.tv_nsec = 0;
	st->st_atim = st->st_ctim = st->st_crtim = st->st_mtim;
	return B_OK;
}

static status_t sfs_open(fs_volume *fs, fs_vnode *node, int openMode, void **cookie)
{
	(void)fs;
	if ((openMode & O_RWMASK) != O_RDONLY || (openMode & (O_CREAT | O_TRUNC)) != 0)
		return B_READ_ONLY_DEVICE;
//	if (((SFSVNode *)node->private_node)->isDirectory)
//		return B_IS_A_DIRECTORY;
	*cookie = calloc(1, sizeof(SFSFileCookie));
	return *cookie != NULL ? B_OK : B_NO_MEMORY;
}

static status_t sfs_close(fs_volume *fs, fs_vnode *node, void *cookie){
	(void)fs; (void)node; (void)cookie;
	return B_OK;
}

static status_t sfs_free_cookie(fs_volume *fs, fs_vnode *node, void *cookie){
	(void)fs; (void)node;
	free(cookie);
	return B_OK;
}

static status_t sfs_read(fs_volume *fs, fs_vnode *node, void *cookie,
	off_t pos, void *buffer, size_t *length){
	(void)cookie;
	SFSVolume *volume = (SFSVolume *)fs->private_volume;
	return sfs_read_file(volume, (SFSVNode *)node->private_node,
		pos, buffer, length);
}

static status_t sfs_open_dir(fs_volume *fs, fs_vnode *node, void **cookie)
{
	SFSVolume *volume = (SFSVolume *)fs->private_volume;
	SFSVNode *vnode = (SFSVNode *)node->private_node;
	if (!vnode->isDirectory)
		return B_NOT_A_DIRECTORY;
	SFSDirCookie *dir = (SFSDirCookie *)calloc(1, sizeof(SFSDirCookie));
	if (dir == NULL)
		return B_NO_MEMORY;
	dir->buffer = (uint8 *)malloc(volume->blockSize);
	if (dir->buffer == NULL) { free(dir); return B_NO_MEMORY; }
	dir->block = vnode->firstDirBlock;
	dir->offset = SFS_CONTAINER_HDR;
	*cookie = dir;
	return B_OK;
}

static status_t sfs_close_dir(fs_volume *fs, fs_vnode *node, void *cookie)
{
	(void)fs; (void)node; (void)cookie;
	return B_OK;
}

static status_t sfs_free_dir_cookie(fs_volume *fs, fs_vnode *node, void *cookie)
{
	(void)fs; (void)node;
	SFSDirCookie *dir = (SFSDirCookie *)cookie;
	if (dir != NULL) { free(dir->buffer); free(dir); }
	return B_OK;
}

static status_t sfs_set_dirent(struct dirent *entry, size_t size,
	const char *name, ino_t id, uint32 device){
	size_t length = strlen(name);
	size_t recordLength = offsetof(struct dirent, d_name) + length + 1;
	if (recordLength > size)
		return B_BUFFER_OVERFLOW;
	memcpy(entry->d_name, name, length + 1);
	entry->d_reclen = recordLength;
	entry->d_ino = id;
	entry->d_dev = device;
	return B_OK;
}

static status_t sfs_read_dir(fs_volume *fs, fs_vnode *node, void *cookie,
	struct dirent *entry, size_t bufferSize, uint32 *count){
	SFSVolume *volume = (SFSVolume *)fs->private_volume;
	SFSVNode *vnode = (SFSVNode *)node->private_node;
	SFSDirCookie *dir = (SFSDirCookie *)cookie;
	*count = 0;
	if (!dir->dotDone) {
		status_t status = sfs_set_dirent(entry, bufferSize, ".", vnode->id, fs->id);
		if (status == B_OK) { dir->dotDone = true; *count = 1; }
		return status;
	}
	if (!dir->dotDotDone) {
		ino_t parent = vnode->parent == 0 ? vnode->id : vnode->parent;
		status_t status = sfs_set_dirent(entry, bufferSize, "..", parent, fs->id);
		if (status == B_OK) { dir->dotDotDone = true; *count = 1; }
		return status;
	}
	while (dir->block != 0 && !dir->finished) {
		if (dir->loadedBlock != dir->block) {
			if (sfs_read_block(volume, dir->block, dir->buffer) != B_OK
				|| !sfs_block_ok(volume, dir->block, SFS_OBJC_ID, dir->buffer))
				return B_BAD_DATA;
			dir->loadedBlock = dir->block;
		}
		if (dir->offset < SFS_CONTAINER_HDR)
			dir->offset= SFS_CONTAINER_HDR;
		if (dir->offset + SFS_OBJECT_FIXED + 2 > volume->blockSize) {
			uint32 nextBlock = sfs_be32(dir->buffer + 16);
			if (nextBlock != 0 && ++dir->blocksVisited > volume->totalBlocks)
				return B_BAD_DATA;
			dir->block = nextBlock;
			dir->offset = SFS_CONTAINER_HDR;
			continue;
		}
		uint8 *object = dir->buffer + dir->offset;
		uint32 id = sfs_be32(object + 4);
		if (id == 0) {
			uint32 nextBlock = sfs_be32(dir->buffer + 16);
			if (nextBlock != 0 && ++dir->blocksVisited > volume->totalBlocks)
				return B_BAD_DATA;
			dir->block = nextBlock;
			dir->offset = SFS_CONTAINER_HDR;
			continue;
		}
		size_t nameLength = strnlen((const char *)object + SFS_OBJECT_FIXED,
			volume->blockSize - dir->offset - SFS_OBJECT_FIXED);
		if (nameLength >= volume->blockSize - dir->offset - SFS_OBJECT_FIXED)
			return B_BAD_DATA;
		char *name = sfs_name_to_utf8(object + SFS_OBJECT_FIXED,
			nameLength, volume->charset);
		size_t next = sfs_next_object(dir->buffer, dir->offset, volume->blockSize);
		if (next <= dir->offset)
			return B_BAD_DATA;
		if (name == NULL)
			return B_NO_MEMORY;
		bool hidden = (object[24] & SFS_TYPE_HIDDEN) != 0;
		if (!hidden) {
			status_t status = sfs_set_dirent(entry, bufferSize, name, id, fs->id);
			free(name);
			if (status == B_OK) {
				dir->offset = next;
				*count = 1;
			}
			return status;
		}
		dir->offset = next;
		free(name);
	}
	dir->finished = true;
	return B_OK;
}

static status_t sfs_rewind_dir(fs_volume *fs, fs_vnode *node, void *cookie)
{
	(void)fs;
	SFSVNode *vnode = (SFSVNode *)node->private_node;
	SFSDirCookie *dir = (SFSDirCookie *)cookie;
	dir->block = vnode->firstDirBlock;
	dir->offset = SFS_CONTAINER_HDR;
	dir->blocksVisited = 0;
	dir->dotDone = false;
	dir->dotDotDone = false;
	dir->finished = false;
	return B_OK;
}

static status_t sfs_unmount(fs_volume *fs)
{
	SFSVolume *volume = (SFSVolume *)fs->private_volume;
	if (volume != NULL) {
		if (volume->fd >= 0)
			close(volume->fd);
		free(volume);
	}
	return B_OK;
}

static uint64 sfs_count_free_blocks(SFSVolume *volume)
{
	const uint32 blocksPerBitmap = (volume->blockSize - 12) * 8;
	uint32 bitmapCount = (volume->totalBlocks + blocksPerBitmap - 1) / blocksPerBitmap;
	uint8 *buffer = (uint8 *)malloc(volume->blockSize);
	if (buffer == NULL)
		return 0;
	uint64 freeBlocks = 0;
	for (uint32 i = 0; i < bitmapCount; i++) {
		uint32 block = volume->bitmapBase + i;
		if (sfs_read_block(volume, block, buffer) != B_OK
			|| !sfs_block_ok(volume, block, 0x42544d50UL, buffer))
			break;
		uint32 base = i * blocksPerBitmap;
		for (uint32 offset = 12; offset + 4 <= volume->blockSize; offset += 4) {
			uint32 bits = sfs_be32(buffer + offset);
			for (uint32 bit = 0; bit < 32; bit++) {
				if (base >= volume->totalBlocks)
					break;
				if ((bits & (0x80000000UL >> bit)) != 0)
					freeBlocks++;
				base++;
			}
			if (base >= volume->totalBlocks)
				break;
		}
	}
	free(buffer);
	return freeBlocks;
}

static status_t sfs_read_fs_info(fs_volume *fs, struct fs_info *info)
{
	SFSVolume *volume = (SFSVolume *)fs->private_volume;
	/*
	 * The VFS owns fs_info::dev and fs_info::root. In particular, do not
	 * memset the structure here: fs_stat_dev() supplies those identifiers
	 * before calling the filesystem's read_fs_info hook. Clearing or
	 * replacing them breaks Tracker's volume identity and mount handling.
	 * Follow the BFS implementation and only fill filesystem properties.
	 */
	info->flags = B_FS_IS_PERSISTENT | B_FS_IS_READONLY;
	info->block_size = volume->blockSize;
	info->io_size = volume->blockSize;
	info->total_blocks = volume->totalBlocks;
//	info->free_blocks = sfs_count_free_blocks(volume);
	info->free_blocks = volume->freeBlocks;
	strlcpy(info->volume_name, volume->name, sizeof(info->volume_name));
	strlcpy(info->fsh_name, "sfs", sizeof(info->fsh_name));
	return B_OK;
}

static fs_volume_ops gSFSVolumeOps = {
	&sfs_unmount,
	&sfs_read_fs_info,
	NULL,
	NULL,
	&sfs_read_vnode
};

static fs_vnode_ops gSFSVnodeOps = {
	&sfs_lookup,
	&sfs_get_vnode_name,
	&sfs_put_vnode,
	NULL,
	NULL, NULL, NULL,
	NULL, NULL,
	NULL,
	NULL, NULL, NULL, NULL, NULL,
	NULL, NULL,
	NULL, NULL, NULL,
	&sfs_access,
	&sfs_read_stat,
	NULL,
	NULL,
	NULL,
	&sfs_open,
	&sfs_close,
	&sfs_free_cookie,
	&sfs_read,
	NULL,
	NULL, NULL,
	&sfs_open_dir,
	&sfs_close_dir,
	&sfs_free_dir_cookie,
	&sfs_read_dir,
	&sfs_rewind_dir
};

static status_t sfs_mount(fs_volume *fs, const char *device, uint32 flags,
	const char *parameters, ino_t *rootID)
{
	(void)flags;
	gSFSVnodeOpsPtr = &gSFSVnodeOps;
	SFSVolume *volume = (SFSVolume *)calloc(1, sizeof(SFSVolume));
	if (volume == NULL)
		return B_NO_MEMORY;
	volume->fd = -1;
	volume->charset = sfs_charset_parse(parameters);
	volume->fd = open(device, O_RDONLY);
	if (volume->fd < 0) { free(volume); return B_ENTRY_NOT_FOUND; }
	strlcpy(volume->device, device, sizeof(volume->device));

	uint8 first[512];
	if (read_pos(volume->fd, 0, first, sizeof(first)) != (ssize_t)sizeof(first)) {
		close(volume->fd); free(volume); return B_IO_ERROR;
	}
	uint32 id = sfs_be32(first);
	if (id != SFS_ROOT_ID && id != SFS_ROOT2_ID) {
		close(volume->fd); free(volume); return B_BAD_DATA;
	}
	uint32 blockSize = sfs_be32(first + 52);
	if (blockSize < 512 || blockSize > 4096
		|| (blockSize & (blockSize - 1)) != 0) {
		close(volume->fd); free(volume); return B_BAD_DATA;
	}
	uint8 *root = (uint8 *)malloc(blockSize);
	if (root == NULL) { close(volume->fd); free(volume); return B_NO_MEMORY; }
	ssize_t bytes = read_pos(volume->fd, 0, root, blockSize);
	if (bytes != (ssize_t)blockSize || !sfs_checksum_ok(root, blockSize)
		|| sfs_be32(root + 8) != 0 || sfs_be16(root + 12) != 3) {
		free(root); close(volume->fd); free(volume); return B_BAD_DATA;
	}
	volume->blockSize = blockSize;
	volume->totalBlocks = sfs_be32(root + 48);
	volume->bitmapBase = sfs_be32(root + 96);
	volume->adminContainer = sfs_be32(root + 100);
	volume->rootContainer = sfs_be32(root + 104);
	volume->extentRoot = sfs_be32(root + 108);
	volume->objectNodeRoot = sfs_be32(root + 112);
	volume->rootBits = root[20];
	if (volume->totalBlocks == 0 || volume->rootContainer >= volume->totalBlocks
		|| volume->extentRoot >= volume->totalBlocks
		|| volume->objectNodeRoot >= volume->totalBlocks) {
		free(root); close(volume->fd); free(volume); return B_BAD_DATA;
	}
	free(root);

	/* Root object node is always object #1. */
	uint8 *container = (uint8 *)malloc(blockSize);
	if (container == NULL) { close(volume->fd); free(volume); return B_NO_MEMORY; }
	if (sfs_read_block(volume, volume->rootContainer, container) != B_OK
		|| !sfs_block_ok(volume, volume->rootContainer, SFS_OBJC_ID, container)) {
		free(container); close(volume->fd); free(volume); return B_BAD_DATA;
	}
	uint8 *rootObject = container + SFS_CONTAINER_HDR;
	if (sfs_be32(rootObject + 4) != 1 || (rootObject[24] & SFS_TYPE_DIR) == 0) {
		free(container); close(volume->fd); free(volume); return B_BAD_DATA;
	}
	size_t nameLength = strnlen((const char *)rootObject + SFS_OBJECT_FIXED,
		blockSize - SFS_CONTAINER_HDR - SFS_OBJECT_FIXED);
	if (nameLength >= blockSize - SFS_CONTAINER_HDR - SFS_OBJECT_FIXED) {
		free(container); close(volume->fd); free(volume); return B_BAD_DATA;
	}
	char *name = sfs_name_to_utf8(rootObject + SFS_OBJECT_FIXED,
		nameLength, volume->charset);
	if (name == NULL) {
		free(container); close(volume->fd); free(volume); return B_NO_MEMORY;
	}
	strlcpy(volume->name, name, sizeof(volume->name));
	free(name);
	free(container);
	volume->freeBlocks= sfs_count_free_blocks(volume);

	/*
	 * The VFS expects the filesystem to publish and own its root vnode
	 * before fs_mount() returns. Merely returning rootID is not enough:
	 * fs_mount() checks that lookup_vnode(fs->id, *rootID) already exists
	 * and has exactly one reference.
	 */
	fs->private_volume = volume;
	fs->ops = &gSFSVolumeOps;

	fs_vnode rootNode;
	memset(&rootNode, 0, sizeof(rootNode));
	int rootType = 0;
	uint32 rootFlags = 0;
	status_t status = sfs_read_vnode(fs, 1, &rootNode, &rootType,
		&rootFlags, false);
	if (status != B_OK) {
		fs->private_volume = NULL;
		close(volume->fd);
		free(volume);
		return status;
	}

	status = publish_vnode(fs, 1, rootNode.private_node, rootNode.ops,
		rootType, rootFlags);
	if (status != B_OK) {
		free(rootNode.private_node);
		fs->private_volume = NULL;
		close(volume->fd);
		free(volume);
		return status;
	}

	*rootID = 1;
	return B_OK;
}

static status_t sfs_std_ops(int32 op, ...)
{
	switch (op) {
		case B_MODULE_INIT:
		case B_MODULE_UNINIT:
			return B_OK;
		default:
			return B_ERROR;
	}
}


extern float sfs_identify_partition(int fd, partition_data *partition,
	void **_cookie);
extern status_t sfs_scan_partition(int fd, partition_data *partition,
	void *cookie);
extern void sfs_free_identify_partition_cookie(partition_data *partition,
	void *cookie);

static file_system_module_info sSFSFileSystem = {
	{
		"file_systems/sfs" B_CURRENT_FS_API_VERSION,
		0,
		&sfs_std_ops
	},
	"sfs",
	"Amiga Smart File System",
	0,
	&sfs_identify_partition,
	&sfs_scan_partition,
	&sfs_free_identify_partition_cookie,
	NULL,
	&sfs_mount
};

module_info *modules[] = {
	(module_info *)&sSFSFileSystem,
	NULL
};
