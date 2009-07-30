/* io.c - Alpine Package Keeper (APK)
 *
 * Copyright (C) 2005-2008 Natanael Copa <n@tanael.org>
 * Copyright (C) 2008 Timo Teräs <timo.teras@iki.fi>
 * All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2 as published
 * by the Free Software Foundation. See http://www.gnu.org/ for details.
 */

#include <errno.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>
#include <sys/mman.h>

#include "apk_defines.h"
#include "apk_io.h"

struct apk_fd_istream {
	struct apk_istream is;
	int fd;
};

static ssize_t fdi_read(void *stream, void *ptr, size_t size)
{
	struct apk_fd_istream *fis =
		container_of(stream, struct apk_fd_istream, is);
	ssize_t i = 0, r;

	if (ptr == NULL) {
		if (lseek(fis->fd, size, SEEK_CUR) < 0)
			return -errno;
		return size;
	}

	while (i < size) {
		r = read(fis->fd, ptr + i, size - i);
		if (r < 0)
			return -errno;
		if (r == 0)
			return i;
		i += r;
	}

	return i;
}

static void fdi_close(void *stream)
{
	struct apk_fd_istream *fis =
		container_of(stream, struct apk_fd_istream, is);

	close(fis->fd);
	free(fis);
}

struct apk_istream *apk_istream_from_fd(int fd)
{
	struct apk_fd_istream *fis;

	if (fd < 0)
		return NULL;

	fis = malloc(sizeof(struct apk_fd_istream));
	if (fis == NULL)
		return NULL;

	*fis = (struct apk_fd_istream) {
		.is.read = fdi_read,
		.is.close = fdi_close,
		.fd = fd,
	};

	return &fis->is;
}

struct apk_istream *apk_istream_from_file(const char *file)
{
	int fd;

	fd = open(file, O_RDONLY);
	if (fd < 0)
		return NULL;

	fcntl(fd, F_SETFD, FD_CLOEXEC);

	return apk_istream_from_fd(fd);
}

size_t apk_istream_skip(struct apk_istream *is, size_t size)
{
	unsigned char buf[2048];
	size_t done = 0, r, togo;

	while (done < size) {
		togo = size - done;
		if (togo > sizeof(buf))
			togo = sizeof(buf);
		r = is->read(is, buf, togo);
		if (r < 0)
			return r;
		done += r;
		if (r != togo)
			break;
	}
	return done;
}

size_t apk_istream_splice(void *stream, int fd, size_t size,
			  apk_progress_cb cb, void *cb_ctx)
{
	struct apk_istream *is = (struct apk_istream *) stream;
	unsigned char *buf = MAP_FAILED;
	size_t bufsz, done = 0, r, togo, mmapped = 0;

	bufsz = size;
	if (size > 128 * 1024) {
		if (ftruncate(fd, size) == 0)
			buf = mmap(NULL, size, PROT_READ | PROT_WRITE,
				   MAP_SHARED, fd, 0);
		if (buf != MAP_FAILED) {
			mmapped = 1;
			if (bufsz > 2*1024*1024)
				bufsz = 2*1024*1024;
		}
	}
	if (!mmapped) {
		if (bufsz > 256*1024)
			bufsz = 256*1024;

		buf = malloc(bufsz);
		if (buf == NULL)
			return -ENOMEM;
	}

	while (done < size) {
		if (done != 0 && cb != NULL)
			cb(cb_ctx, muldiv(APK_PROGRESS_SCALE, done, size));

		togo = size - done;
		if (togo > bufsz)
			togo = bufsz;
		r = is->read(is, buf, togo);
		if (r < 0)
			goto err;

		if (!mmapped) {
			if (write(fd, buf, r) != r) {
				if (r < 0)
					r = -errno;
				goto err;
			}
		}

		done += r;
		if (r != togo)
			break;
	}
	r = done;
err:
	if (mmapped)
		munmap(buf, size);
	else
		free(buf);
	return r;
}

struct apk_istream_bstream {
	struct apk_bstream bs;
	struct apk_istream *is;
	apk_blob_t left;
	char buffer[8*1024];
	size_t size;
};

static apk_blob_t is_bs_read(void *stream, apk_blob_t token)
{
	struct apk_istream_bstream *isbs =
		container_of(stream, struct apk_istream_bstream, bs);
	ssize_t size;
	apk_blob_t ret;

	/* If we have cached stuff, first check if it full fills the request */
	if (isbs->left.len != 0) {
		if (!APK_BLOB_IS_NULL(token)) {
			/* If we have tokenized thingy left, return it */
			if (apk_blob_split(isbs->left, token, &ret, &isbs->left))
				goto ret;
		} else
			goto ret_all;
	}

	/* If we've exchausted earlier, it's end of stream */
	if (APK_BLOB_IS_NULL(isbs->left))
		return APK_BLOB_NULL;

	/* We need more data */
	if (isbs->left.len != 0)
		memcpy(isbs->buffer, isbs->left.ptr, isbs->left.len);
	isbs->left.ptr = isbs->buffer;
	size = isbs->is->read(isbs->is, isbs->buffer + isbs->left.len,
			      sizeof(isbs->buffer) - isbs->left.len);
	if (size > 0) {
		isbs->size += size;
		isbs->left.len += size;
	} else if (size == 0) {
		if (isbs->left.len == 0)
			isbs->left = APK_BLOB_NULL;
		goto ret_all;
	}

	if (!APK_BLOB_IS_NULL(token)) {
		/* If we have tokenized thingy left, return it */
		if (apk_blob_split(isbs->left, token, &ret, &isbs->left))
			goto ret;
		/* No token found; just return the full buffer */
	}

ret_all:
	/* Return all that is in cache */
	ret = isbs->left;
	isbs->left.len = 0;
ret:
	return ret;
}

static void is_bs_close(void *stream, size_t *size)
{
	struct apk_istream_bstream *isbs =
		container_of(stream, struct apk_istream_bstream, bs);

	if (size != NULL)
		*size = isbs->size;

	isbs->is->close(isbs->is);
	free(isbs);
}

struct apk_bstream *apk_bstream_from_istream(struct apk_istream *istream)
{
	struct apk_istream_bstream *isbs;

	isbs = malloc(sizeof(struct apk_istream_bstream));
	if (isbs == NULL)
		return NULL;

	isbs->bs = (struct apk_bstream) {
		.read = is_bs_read,
		.close = is_bs_close,
	};
	isbs->is = istream;
	isbs->left = APK_BLOB_PTR_LEN(isbs->buffer, 0),
	isbs->size = 0;

	return &isbs->bs;
}

struct apk_mmap_bstream {
	struct apk_bstream bs;
	int fd;
	size_t size;
	unsigned char *ptr;
	apk_blob_t left;
};

static apk_blob_t mmap_read(void *stream, apk_blob_t token)
{
	struct apk_mmap_bstream *mbs =
		container_of(stream, struct apk_mmap_bstream, bs);
	apk_blob_t ret;

	if (!APK_BLOB_IS_NULL(token) && !APK_BLOB_IS_NULL(mbs->left)) {
		if (apk_blob_split(mbs->left, token, &ret, &mbs->left))
			return ret;
	}

	ret = mbs->left;
	mbs->left = APK_BLOB_NULL;
	mbs->bs.flags |= APK_BSTREAM_EOF;

	return ret;
}

static void mmap_close(void *stream, size_t *size)
{
	struct apk_mmap_bstream *mbs =
		container_of(stream, struct apk_mmap_bstream, bs);

	if (size != NULL)
		*size = mbs->size;

	munmap(mbs->ptr, mbs->size);
	close(mbs->fd);
	free(mbs);
}

static struct apk_bstream *apk_mmap_bstream_from_fd(int fd)
{
	struct apk_mmap_bstream *mbs;
	struct stat st;
	void *ptr;

	if (fstat(fd, &st) < 0)
		return NULL;

	ptr = mmap(NULL, st.st_size, PROT_READ, MAP_SHARED, fd, 0);
	if (ptr == MAP_FAILED)
		return NULL;

	mbs = malloc(sizeof(struct apk_mmap_bstream));
	if (mbs == NULL) {
		munmap(ptr, st.st_size);
		return NULL;
	}

	mbs->bs = (struct apk_bstream) {
		.flags = APK_BSTREAM_SINGLE_READ,
		.read = mmap_read,
		.close = mmap_close,
	};
	mbs->fd = fd;
	mbs->size = st.st_size;
	mbs->ptr = ptr;
	mbs->left = APK_BLOB_PTR_LEN(ptr, mbs->size);

	return &mbs->bs;
}

struct apk_bstream *apk_bstream_from_fd(int fd)
{
	struct apk_bstream *bs;

	if (fd < 0)
		return NULL;

	bs = apk_mmap_bstream_from_fd(fd);
	if (bs != NULL)
		return bs;

	return apk_bstream_from_istream(apk_istream_from_fd(fd));
}

struct apk_bstream *apk_bstream_from_file(const char *file)
{
	int fd;

	fd = open(file, O_RDONLY);
	if (fd < 0)
		return NULL;

	fcntl(fd, F_SETFD, FD_CLOEXEC);
	return apk_bstream_from_fd(fd);
}


struct apk_tee_bstream {
	struct apk_bstream bs;
	struct apk_bstream *inner_bs;
	int fd;
	size_t size;
};

static apk_blob_t tee_read(void *stream, apk_blob_t token)
{
	struct apk_tee_bstream *tbs =
		container_of(stream, struct apk_tee_bstream, bs);
	apk_blob_t blob;

	blob = tbs->inner_bs->read(tbs->inner_bs, token);
	if (!APK_BLOB_IS_NULL(blob))
		tbs->size += write(tbs->fd, blob.ptr, blob.len);

	return blob;
}

static void tee_close(void *stream, size_t *size)
{
	struct apk_tee_bstream *tbs =
		container_of(stream, struct apk_tee_bstream, bs);

	tbs->inner_bs->close(tbs->inner_bs, NULL);
	if (size != NULL)
		*size = tbs->size;
	close(tbs->fd);
	free(tbs);
}

struct apk_bstream *apk_bstream_tee(struct apk_bstream *from, const char *to)
{
	struct apk_tee_bstream *tbs;
	int fd;

	fd = creat(to, S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH);
	if (fd < 0)
		return NULL;

	tbs = malloc(sizeof(struct apk_tee_bstream));
	if (tbs == NULL) {
		close(fd);
		return NULL;
	}

	tbs->bs = (struct apk_bstream) {
		.read = tee_read,
		.close = tee_close,
	};
	tbs->inner_bs = from;
	tbs->fd = fd;
	tbs->size = 0;

	return &tbs->bs;
}

apk_blob_t apk_blob_from_istream(struct apk_istream *is, size_t size)
{
	void *ptr;
	size_t rsize;

	ptr = malloc(size);
	if (ptr == NULL)
		return APK_BLOB_NULL;

	rsize = is->read(is, ptr, size);
	if (rsize < 0) {
		free(ptr);
		return APK_BLOB_NULL;
	}
	if (rsize != size)
		ptr = realloc(ptr, rsize);

	return APK_BLOB_PTR_LEN(ptr, rsize);
}

apk_blob_t apk_blob_from_file(const char *file)
{
	int fd;
	struct stat st;
	char *buf;

	fd = open(file, O_RDONLY);
	if (fd < 0)
		return APK_BLOB_NULL;

	if (fstat(fd, &st) < 0)
		goto err_fd;

	buf = malloc(st.st_size);
	if (buf == NULL)
		goto err_fd;

	if (read(fd, buf, st.st_size) != st.st_size)
		goto err_read;

	close(fd);
	return APK_BLOB_PTR_LEN(buf, st.st_size);
err_read:
	free(buf);
err_fd:
	close(fd);
	return APK_BLOB_NULL;
}

int apk_file_get_info(const char *filename, int checksum, struct apk_file_info *fi)
{
	struct stat st;
	struct apk_bstream *bs;

	if (stat(filename, &st) != 0)
		return -errno;

	*fi = (struct apk_file_info) {
		.size = st.st_size,
		.uid = st.st_uid,
		.gid = st.st_gid,
		.mode = st.st_mode,
		.mtime = st.st_mtime,
		.device = st.st_dev,
	};

	if (checksum == APK_CHECKSUM_NONE)
		return 0;

	bs = apk_bstream_from_file(filename);
	if (bs != NULL) {
		EVP_MD_CTX mdctx;
		apk_blob_t blob;

		EVP_DigestInit(&mdctx, apk_get_digest(checksum));
		if (bs->flags & APK_BSTREAM_SINGLE_READ)
			EVP_MD_CTX_set_flags(&mdctx, EVP_MD_CTX_FLAG_ONESHOT);
		while (!APK_BLOB_IS_NULL(blob = bs->read(bs, APK_BLOB_NULL)))
			EVP_DigestUpdate(&mdctx, (void*) blob.ptr, blob.len);
		fi->csum.type = EVP_MD_CTX_size(&mdctx);
		EVP_DigestFinal(&mdctx, fi->csum.data, NULL);

		bs->close(bs, NULL);
	}

	return 0;
}

struct apk_istream *apk_istream_from_file_gz(const char *file)
{
	return apk_bstream_gunzip(apk_bstream_from_file(file));
}

struct apk_fd_ostream {
	struct apk_ostream os;
	int fd;
	size_t bytes;
	char buffer[1024];
};

static ssize_t safe_write(int fd, const void *ptr, size_t size)
{
	ssize_t i = 0, r;

	while (i < size) {
		r = write(fd, ptr + i, size - i);
		if (r < 0)
			return -errno;
		if (r == 0)
			return i;
		i += r;
	}

	return i;
}

static ssize_t fdo_flush(struct apk_fd_ostream *fos)
{
	ssize_t r;

	if (fos->bytes == 0)
		return 0;

	if ((r = safe_write(fos->fd, fos->buffer, fos->bytes)) != fos->bytes)
		return r;

	fos->bytes = 0;
	return 0;
}

static ssize_t fdo_write(void *stream, const void *ptr, size_t size)
{
	struct apk_fd_ostream *fos =
		container_of(stream, struct apk_fd_ostream, os);
	ssize_t r;

	if (size + fos->bytes >= sizeof(fos->buffer)) {
		r = fdo_flush(fos);
		if (r != 0)
			return r;
		if (size >= sizeof(fos->buffer) / 2)
			return safe_write(fos->fd, ptr, size);
	}

	memcpy(&fos->buffer[fos->bytes], ptr, size);
	fos->bytes += size;

	return size;
}

static void fdo_close(void *stream)
{
	struct apk_fd_ostream *fos =
		container_of(stream, struct apk_fd_ostream, os);

	fdo_flush(fos);
	close(fos->fd);
	free(fos);
}

struct apk_ostream *apk_ostream_to_fd(int fd)
{
	struct apk_fd_ostream *fos;

	if (fd < 0)
		return NULL;

	fos = malloc(sizeof(struct apk_fd_ostream));
	if (fos == NULL)
		return NULL;

	*fos = (struct apk_fd_ostream) {
		.os.write = fdo_write,
		.os.close = fdo_close,
		.fd = fd,
	};

	return &fos->os;
}

struct apk_ostream *apk_ostream_to_file(const char *file, mode_t mode)
{
	int fd;

	fd = creat(file, mode);
	if (fd < 0)
		return NULL;

	fcntl(fd, F_SETFD, FD_CLOEXEC);

	return apk_ostream_to_fd(fd);
}

struct apk_counter_ostream {
	struct apk_ostream os;
	off_t *counter;
};

static ssize_t co_write(void *stream, const void *ptr, size_t size)
{
	struct apk_counter_ostream *cos =
		container_of(stream, struct apk_counter_ostream, os);

	*cos->counter += size;
	return size;
}

static void co_close(void *stream)
{
	struct apk_counter_ostream *cos =
		container_of(stream, struct apk_counter_ostream, os);

	free(cos);
}

struct apk_ostream *apk_ostream_counter(off_t *counter)
{
	struct apk_counter_ostream *cos;

	cos = malloc(sizeof(struct apk_counter_ostream));
	if (cos == NULL)
		return NULL;

	*cos = (struct apk_counter_ostream) {
		.os.write = co_write,
		.os.close = co_close,
		.counter = counter,
	};

	return &cos->os;
}

size_t apk_ostream_write_string(struct apk_ostream *os, const char *string)
{
	size_t len;

	len = strlen(string);
	if (os->write(os, string, len) != len)
		return -1;

	return len;
}
