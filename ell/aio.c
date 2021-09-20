/*
 *
 *  Embedded Linux library
 *
 *  Copyright (C) 2011-2014  Intel Corporation. All rights reserved.
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#define _GNU_SOURCE
#include <errno.h>

#include <fcntl.h>
#include <errno.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <linux/aio_abi.h>

#include "private.h"
#include "io.h"
#include "aio.h"


#define AIO_RING_MAGIC                  0xa10a10a1

struct aio_ring {
	unsigned id;     /* kernel internal index number */
	unsigned nr;     /* number of io_events */
	unsigned head;
	unsigned tail;

	unsigned magic;
	unsigned compat_features;
	unsigned incompat_features;
	unsigned header_length;  /* size of aio_ring */
};

struct l_aio_request {
	struct iocb iocb;
	l_aio_cb_t cb;
	void *user_data;
};

struct l_aio {
	aio_context_t ctx;
	struct l_io *evfd;
};



static bool io_ring_is_empty(aio_context_t ctx, struct timespec *timeout)
{
	struct aio_ring *ring = (struct aio_ring *)ctx;

	if (!ring || ring->magic != AIO_RING_MAGIC)
		return false;

	if (!timeout || timeout->tv_sec || timeout->tv_nsec)
		return false;

	if (ring->head != ring->tail)
		return false;

	return true;
}

static int io_setup(int maxevents, aio_context_t *ctx)
{
	return syscall(__NR_io_setup, maxevents, ctx);
}

static int io_destroy(aio_context_t ctx)
{
	return syscall(__NR_io_destroy, ctx);
}

static int io_submit(aio_context_t ctx, long nr, struct iocb *ios[])
{
	return syscall(__NR_io_submit, ctx, nr, ios);
}

static int io_cancel(aio_context_t ctx, struct iocb *iocb, struct io_event *event)
{
	return syscall(__NR_io_cancel, ctx, iocb, event);
}

static int io_getevents(aio_context_t ctx, long min_nr, long nr, struct io_event *events, struct timespec *timeout)
{
	if (io_ring_is_empty(ctx, timeout))
		return 0;

	return syscall(__NR_io_getevents, ctx, min_nr, nr, events, timeout);
}

static bool event_callback(struct l_io *io, void *user_data)
{
	struct l_aio * aio = user_data;

	uint64_t c;
	int r = read(l_io_get_fd(aio->evfd), &c, sizeof(c));

	for (;;) {
		static struct timespec timeout = { 0, 0 };
		struct io_event event;
	
		r = io_getevents(aio->ctx, 0, 1, &event, &timeout);

		if (r != 1)
			break;

		struct l_aio_request* req = (struct l_aio_request*)event.data;

		ssize_t result = event.res2;
		
		if (result >= 0)
			result = event.res;
			
		req->cb(result, req->user_data);
		l_free(req);
	}
	
	return true;
}

LIB_EXPORT struct l_aio * l_aio_create(int maxevents)
{
	struct l_aio *aio = l_new(struct l_aio, 1);
	long r = io_setup(maxevents, &aio->ctx);

	if (r < 0)
		goto error_init;

	int evfd = eventfd(0, O_NONBLOCK | O_CLOEXEC);

	if (evfd < 0)
		goto error_event;

	aio->evfd = l_io_new(evfd);

	if (!l_io_set_read_handler(aio->evfd, event_callback, aio, NULL))
		goto error_handler;

	return aio;

error_handler:
	close(evfd);
	l_io_destroy(aio->evfd);
error_event:
	io_destroy(aio->ctx);
error_init:
	l_free(aio);
	return NULL;
}

LIB_EXPORT int l_aio_read(struct l_aio *aio, l_aio_cb_t read_cb, int fd, long long offset,
               void *buffer, size_t count, void *user_data)
{
	struct l_aio_request *req = l_new(struct l_aio_request, 1);

	req->cb = read_cb;
	req->user_data = user_data;

	req->iocb = (struct iocb) {
		.aio_fildes = fd,
		.aio_lio_opcode = IOCB_CMD_PREAD,
		.aio_reqprio = 0,
		.aio_buf = (intptr_t)buffer,
		.aio_nbytes = count,
		.aio_offset = offset,
		.aio_flags = IOCB_FLAG_RESFD,
		.aio_resfd = l_io_get_fd(aio->evfd),
		.aio_data = (intptr_t)req
	};

	struct iocb *iocbv[] = { &req->iocb };

	return io_submit(aio->ctx, 1, iocbv);
}

LIB_EXPORT int l_aio_write(struct l_aio *aio, l_aio_cb_t read_cb, int fd, long long offset,
               const void *buffer, size_t count, void *user_data)
{
	struct l_aio_request *req = l_new(struct l_aio_request, 1);

	req->cb = read_cb;
	req->user_data = user_data;

	req->iocb = (struct iocb) {
		.aio_fildes = fd,
		.aio_lio_opcode = IOCB_CMD_PWRITE,
		.aio_reqprio = 0,
		.aio_buf = (intptr_t)buffer,
		.aio_nbytes = count,
		.aio_offset = offset,
		.aio_flags = IOCB_FLAG_RESFD,
		.aio_resfd = l_io_get_fd(aio->evfd),
		.aio_data = (intptr_t)req
	};

	struct iocb *iocbv[] = { &req->iocb };

	return io_submit(aio->ctx, 1, iocbv);
}
