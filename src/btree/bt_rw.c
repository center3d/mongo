/*-
 * See the file LICENSE for redistribution information.
 *
 * Copyright (c) 2008-2011 WiredTiger, Inc.
 *	All rights reserved.
 */

#include "wt_internal.h"

/*
 * Don't compress the first 32B of the block (almost all of the WT_PAGE_DISK
 * structure) because we need the block's checksum and on-disk and in-memory
 * page sizes to be immediately available without decompression (the checksum
 * and the on-disk page sizes are used during salvage to figure out where the
 * pages are, and the in-memory page size tells us how large a buffer we need
 * to decompress the file block.  We could take less than 32B, but a 32B
 * boundary is probably better alignment for the underlying compression engine,
 * and skipping 32B won't matter in terms of compression efficiency.
 */
#define	COMPRESS_SKIP    32

/*
 * __wt_disk_read --
 *	Read a block into a buffer.
 */
int
__wt_disk_read(
    WT_SESSION_IMPL *session, WT_BUF *buf, uint32_t addr, uint32_t size)
{
	WT_BTREE *btree;
	WT_BUF *tmp;
	WT_FH *fh;
	WT_ITEM src, dst;
	WT_PAGE_DISK *dsk;
	uint32_t checksum;
	int ret;

	btree = session->btree;
	tmp = NULL;
	fh = btree->fh;
	dsk = buf->mem;

	WT_RET(__wt_read(
	    session, fh, WT_ADDR_TO_OFF(btree, addr), size, buf->mem));

	dsk = buf->mem;
	checksum = dsk->checksum;
	dsk->checksum = 0;
	if (checksum != __wt_cksum(dsk, size))
		WT_FAILURE_RET(session, WT_ERROR,
		    "read checksum error: %" PRIu32 "/%" PRIu32, addr, size);

	WT_BSTAT_INCR(session, page_read);
	WT_CSTAT_INCR(session, block_read);

	WT_VERBOSE(session, READ,
	    "read addr/size %" PRIu32 "/%" PRIu32 ": %s",
	    addr, size, __wt_page_type_string(dsk->type));

	/*
	 * If the in-memory and on-disk sizes are the same, the buffer is not
	 * compressed.  Otherwise, allocate a scratch buffer and decompress
	 * into it.
	 */
	if (dsk->size == dsk->memsize)
		return (0);

	WT_RET(__wt_scr_alloc(session, dsk->memsize, &tmp));

	/* Copy the skipped bytes of the original image into place. */
	memcpy(tmp->mem, buf->mem, COMPRESS_SKIP);

	/* Decompress the buffer. */
	src.data = (uint8_t *)buf->mem + COMPRESS_SKIP;
	src.size = buf->size - COMPRESS_SKIP;
	dst.data = (uint8_t *)tmp->mem + COMPRESS_SKIP;
	dst.size = tmp->size - COMPRESS_SKIP;
	WT_ERR(btree->compressor->decompress(
	    btree->compressor, &session->iface, &src, &dst));
	__wt_buf_swap(tmp, buf);

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);
	return (ret);
}

/*
 * __wt_disk_write --
 *	Write a buffer to disk, returning the addr/size pair.
 */
int
__wt_disk_write(
    WT_SESSION_IMPL *session, WT_BUF *buf, uint32_t *addrp, uint32_t *sizep)
{
	WT_ITEM src, dst;
	WT_BTREE *btree;
	WT_BUF *tmp;
	WT_PAGE_DISK *dsk;
	uint32_t addr, align_size, orig_size, size;
	uint8_t orig_type;
	int ret;

	btree = session->btree;
	tmp = NULL;
	ret = 0;

	dsk = buf->mem;
	orig_size = buf->size;
	orig_type = dsk->type;

	/*
	 * We're passed in a WT_BUF that references some chunk of memory that's
	 * a table's page image.  WT_BUF->size is the byte count of the image,
	 * and WT_BUF->data is the image itself.
	 *
	 * Diagnostics: verify the disk page.  We have to set the disk size to
	 * the "current" value, otherwise verify will complain.  We have no
	 * disk address to use for error messages, use 0 (WT_ADDR_INVALID is a
	 * big, big number).  This violates some layering, but it's the place
	 * we can ensure we never write a corrupted page.
	 */
	dsk->size = dsk->memsize = buf->size;
	WT_ASSERT(session, __wt_verify_dsk(
	    session, buf->mem, WT_ADDR_INVALID, buf->size, 0) == 0);

	/* Align the in-memory size to an allocation unit. */
	align_size = WT_ALIGN(buf->size, btree->allocsize);

	/*
	 * Optionally stream-compress the data, but don't compress blocks that
	 * are already as small as they're going to get.
	 */
	if (btree->compressor == NULL || align_size == btree->allocsize) {
not_compressed:	/*
		 * If not compressing the buffer, we need to zero out any unused
		 * bytes at the end.
		 *
		 * We know the buffer is big enough for us to zero to the next
		 * allocsize boundary: our callers must allocate enough memory
		 * for the buffer so that we can do this operation.  Why don't
		 * our callers just zero out the buffer themselves?  Because we
		 * have to zero out the end of the buffer in the compression
		 * case: so, we can either test compression in our callers and
		 * zero or not-zero based on that test, splitting the code to
		 * zero out the buffer into two parts, or require our callers
		 * allocate enough memory for us to zero here without copying.
		 * Both choices suck.
		 */
		memset(
		    (uint8_t *)buf->mem + buf->size, 0, align_size - buf->size);
		buf->size = align_size;

		/*
		 * Set the in-memory size to the on-page size (we check the size
		 * to decide if a block is compressed: if the sizes match, the
		 * block is NOT compressed).
		 */
		dsk = buf->mem;
		dsk->size = dsk->memsize = align_size;
	} else {
		/*
		 * Allocate a buffer for disk compression; only allocate enough
		 * memory for a copy of the original, if any compressed version
		 * is bigger than the original, we won't use it.
		 */
		WT_RET(__wt_scr_alloc(session, buf->size, &tmp));

		/* Skip the first 32B of the data. */
		src.data = (uint8_t *)buf->mem + COMPRESS_SKIP;
		src.size = buf->size - COMPRESS_SKIP;
		dst.data = (uint8_t *)tmp->mem + COMPRESS_SKIP;
		dst.size = buf->size - COMPRESS_SKIP;

		/*
		 * If compression fails, fallback to the original version.  This
		 * isn't unexpected: if compression doesn't work for some chunk
		 * of bytes for some reason (noting there's likely additional
		 * format/header information which compressed output requires),
		 * it just means the uncompressed version is as good as it gets,
		 * and that's what we use.
		 */
		if ((ret = btree->compressor->compress(
		    btree->compressor, &session->iface, &src, &dst)) != 0)
			goto not_compressed;

		/*
		 * Set the final data size and see if compression was useful
		 * (if the final block size is smaller, use the compressed
		 * version, else use an uncompressed version because it will
		 * be faster to read).
		 */
		tmp->size = dst.size + COMPRESS_SKIP;
		size = WT_ALIGN(tmp->size, btree->allocsize);
		if (size >= align_size)
			goto not_compressed;
		align_size = size;

		/*
		 * Copy in the leading 32B of header (incidentally setting the
		 * in-memory page size), zero out any unused bytes.
		 *
		 * Set the final on-disk page size.
		 */
		memcpy(tmp->mem, buf->mem, COMPRESS_SKIP);
		memset(
		    (uint8_t *)tmp->mem + tmp->size, 0, align_size - tmp->size);

		dsk = tmp->mem;
		dsk->size = align_size;
	}

	/* Allocate blocks from the underlying file. */
	WT_ERR(__wt_block_alloc(session, &addr, align_size));

	/*
	 * The disk write function sets things in the WT_PAGE_DISK header simply
	 * because it's easy to do it here.  In a transactional store, things
	 * may be a little harder.
	 *
	 * We increment the page LSN in non-transactional stores so it's easy
	 * to identify newer versions of pages during salvage: both pages are
	 * likely to be internally consistent, and might have the same initial
	 * and last keys, so we need a way to know the most recent state of the
	 * page.  Alternatively, we could check to see which leaf is referenced
	 * by the internal page, which implies salvaging internal pages (which
	 * I don't want to do), and it's not quite as good anyway, because the
	 * internal page may not have been written to disk after the leaf page
	 * was updated.
	 */
	WT_LSN_INCR(btree->lsn);
	dsk->lsn = btree->lsn;

	/*
	 * Update the block's checksum: checksum the compressed contents, not
	 * the uncompressed contents.
	 */
	dsk->checksum = 0;
	dsk->checksum = __wt_cksum(dsk, align_size);
	WT_ERR(__wt_write(
	    session, btree->fh, WT_ADDR_TO_OFF(btree, addr), align_size, dsk));

	WT_BSTAT_INCR(session, page_write);
	WT_CSTAT_INCR(session, block_write);

	WT_VERBOSE(session, WRITE,
	    "write %" PRIu32 " at addr/size %" PRIu32 "/%" PRIu32 ", %s%s",
	    orig_size, addr, align_size,
	    dsk->size == dsk->memsize ? "" : "compressed, ",
	    __wt_page_type_string(orig_type));

	*addrp = addr;
	*sizep = align_size;

err:	if (tmp != NULL)
		__wt_scr_release(&tmp);

	return (ret);
}
