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
#include <time.h>
#include <sys/eventfd.h>
#include <sys/syscall.h>
#include <linux/aio_abi.h>

#include "private.h"
#include "useful.h"
#include "io.h"
#include "aio.h"


#define AIO_RING_MAGIC 0xa10a10a1

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

struct entry {
	struct iocb iocb;
	l_aio_cb_t callback;
	void *user_data;
	ssize_t result;
	enum {
		STATE_NONE = 0,
		STATE_WAIT,
		STATE_READY
	} state;
};

struct l_aio {
	aio_context_t context;
	struct entry *list;
	unsigned entries;
	unsigned index;
	struct l_io *eventfd;
	bool bycatch;
};

static bool io_ring_is_empty(aio_context_t context, struct timespec *timeout)
{
	struct aio_ring *ring = (struct aio_ring *)context;

	if (!ring || ring->magic != AIO_RING_MAGIC)
		return false;

	if (!timeout || timeout->tv_sec || timeout->tv_nsec)
		return false;

	if (ring->head != ring->tail)
		return false;

	return true;
}

static int io_setup(unsigned maxevents, aio_context_t *context)
{
	return syscall(__NR_io_setup, maxevents, context);
}

static int io_destroy(aio_context_t context)
{
	return syscall(__NR_io_destroy, context);
}

static int io_submit(aio_context_t context, long nr, struct iocb *ios[])
{
	return syscall(__NR_io_submit, context, nr, ios);
}

static int io_cancel(aio_context_t context, struct iocb *iocb, struct io_event *event)
{
	return syscall(__NR_io_cancel, context, iocb, event);
}

static int io_getevents(aio_context_t context, long min_nr, long nr, struct io_event *events, struct timespec *timeout)
{
	if (io_ring_is_empty(context, timeout))
		return 0;

	return syscall(__NR_io_getevents, context, min_nr, nr, events, timeout);
}

static ssize_t get_result(const struct io_event *event)
{
	ssize_t result = event->res2;

	if (result >= 0)
		result = event->res;

	return result;
}

struct entry* await_next_block(struct l_aio *aio, struct timespec *timeout)
{

	for (;;) {
		struct io_event event;

		int r = io_getevents(aio->context, 0, 1, &event, timeout);

		if (r == 1) {
			struct entry *entry = (struct entry*)event.data;

			if ((entry < &aio->list[0]) && (entry > &aio->list[aio->entries - 1]))
				continue;

			entry->result = get_result(&event);

			return entry;
		} else if ((r < 0) && (errno == EINTR)) {
			continue;
		} else {
			break;
		}
	}

	return NULL;
}

static bool event_callback(struct l_io *io, void *user_data)
{
	struct l_aio * aio = user_data;

	uint64_t c;
	int r = read(l_io_get_fd(aio->eventfd), &c, sizeof(c));

	if (r < 0) {} //ignore error, we are polling anyway

	if (aio->bycatch) {
		aio->bycatch = false;
		for (int i = 0; i < aio->entries; i++) {
			struct entry* entry = &aio->list[i];

			if (entry->state != STATE_READY)
				continue;

			if (!entry->callback)
				continue;

			entry->state = STATE_NONE;
			entry->callback(entry->result, entry->user_data);
		}
	}

	for (;;) {
		static struct timespec timeout = { 0 };

		struct entry* entry = await_next_block(aio, &timeout);

		if (!entry)
			break;

		if (entry->callback) {
			entry->state = STATE_NONE;
			entry->callback(entry->result, entry->user_data);
		} else {
			entry->state = STATE_READY;
		}
	}
	return true;
}

static int get_index(struct l_aio *aio)
{
	unsigned index = aio->index;

	do {
		if (aio->list[index].state == STATE_NONE)
			return index;

		index = (index + 1) % aio->entries;
	} while (index != aio->index);

	return -1;
}

LIB_EXPORT struct l_aio * l_aio_create(unsigned maxevents)
{
	if (unlikely(maxevents == 0))
		return NULL;

	struct l_aio *aio = l_new(struct l_aio, 1);

	aio->entries = maxevents;

	aio->list = l_new(struct entry, aio->entries);

	int r = io_setup(maxevents, &aio->context);

	if (r < 0)
		goto error_init;

	int efd = eventfd(0, O_NONBLOCK | O_CLOEXEC);

	if (efd < 0)
		goto error_event;

	aio->eventfd = l_io_new(efd);

	if (!l_io_set_read_handler(aio->eventfd, event_callback, aio, NULL))
		goto error_handler;

	return aio;

error_handler:
	close(efd);
	l_io_destroy(aio->eventfd);
error_event:
	io_destroy(aio->context);
error_init:
	l_free(aio->list);
	l_free(aio);
	return NULL;
}

LIB_EXPORT int l_aio_get_fd(struct l_aio *aio, unsigned reqid)
{
	if (unlikely(!aio))
		return -1;

	if (unlikely(reqid >= aio->entries))
		return -1;

	if (aio->list[reqid].state == STATE_NONE)
		return -1;

	return aio->list[reqid].iocb.aio_fildes;
}

LIB_EXPORT int l_aio_read(struct l_aio *aio, l_aio_cb_t read_cb, int fd, off_t offset,
						  void *buffer, size_t count, void *user_data)
{
	if (unlikely(!aio))
		return -1;

	int index = get_index(aio);

	if (index < 0)
		return -1;

	struct entry *entry = &aio->list[index];

	*entry = (struct entry) {
		.callback = read_cb,
		.user_data = user_data,

		.iocb.aio_fildes = fd,
		.iocb.aio_lio_opcode = IOCB_CMD_PREAD,
		.iocb.aio_reqprio = 0,
		.iocb.aio_buf = (intptr_t)buffer,
		.iocb.aio_nbytes = count,
		.iocb.aio_offset = offset,
		.iocb.aio_data = (intptr_t)entry
	};

	if (entry->callback) {
		entry->iocb.aio_flags |= IOCB_FLAG_RESFD;
		entry->iocb.aio_resfd = l_io_get_fd(aio->eventfd);
	}

	struct iocb *iocbv[] = { &entry->iocb };

	int r = io_submit(aio->context, 1, iocbv);

	if (r < 0)
		return -1;

	entry->state = STATE_WAIT;

	return index;
}

LIB_EXPORT int l_aio_write(struct l_aio *aio, l_aio_cb_t write_cb, int fd, off_t offset,
						   const void *buffer, size_t count, void *user_data)
{
	if (unlikely(!aio))
		return -1;

	int index = get_index(aio);

	if (index < 0)
		return -1;

	struct entry *entry = &aio->list[index];

	*entry = (struct entry) {
		.callback = write_cb,
		.user_data = user_data,

		.iocb.aio_fildes = fd,
		.iocb.aio_lio_opcode = IOCB_CMD_PWRITE,
		.iocb.aio_reqprio = 0,
		.iocb.aio_buf = (intptr_t)buffer,
		.iocb.aio_nbytes = count,
		.iocb.aio_offset = offset,
		.iocb.aio_data = (intptr_t)entry
	};

	if (entry->callback) {
		entry->iocb.aio_flags |= IOCB_FLAG_RESFD;
		entry->iocb.aio_resfd = l_io_get_fd(aio->eventfd);
	}

	struct iocb *iocbv[] = { &entry->iocb };

	int r = io_submit(aio->context, 1, iocbv);

	if (r < 0)
		return -1;

	entry->state = STATE_WAIT;

	return index;
}

LIB_EXPORT bool l_aio_cancel(struct l_aio *aio, unsigned reqid, ssize_t *result)
{
	if (unlikely(!aio))
		return false;

	if (unlikely(reqid >= aio->entries))
		return false;

	if (aio->list[reqid].state == STATE_READY) {
		aio->list[reqid].state = STATE_NONE;

		if (result)
			*result = aio->list[reqid].result;

		return true;
	} else if (aio->list[reqid].state != STATE_WAIT) {
		return false;
	}

	struct io_event event;
	int r = io_cancel(aio->context, &aio->list[reqid].iocb, &event);

	if (r >= 0) {
		aio->list[reqid].state = STATE_NONE;

		if (result)
			*result = get_result(&event);

		return true;
	} else {
		// maybe the operation was a already finished
		return l_aio_await(aio, reqid, 0, result);
	}
}

LIB_EXPORT bool l_aio_await(struct l_aio *aio, unsigned reqid, int64_t nanoseconds, ssize_t *result)
{
	if (unlikely(!aio))
		return false;

	if (unlikely(reqid >= aio->entries))
		return false;

	if (aio->list[reqid].state == STATE_READY) {
		aio->list[reqid].state = STATE_NONE;

		if (result)
			*result = aio->list[reqid].result;

		return true;
	} else if (aio->list[reqid].state != STATE_WAIT) {
		return false;
	}

	struct timespec ts = { .tv_nsec = 0, .tv_sec = 0 };

	if (nanoseconds > 0) {
		ts.tv_nsec = nanoseconds % 1000000000;
		ts.tv_sec = nanoseconds / 1000000000;
	}

	struct timespec *timeout = nanoseconds < 0 ? NULL : &ts;

	bool bycatch = aio->bycatch;

	for (;;) {
		struct entry* entry =  await_next_block(aio, timeout);

		if (!entry)
			return false;

		if (entry == &aio->list[reqid]) {
			entry->state = STATE_NONE;

			if (result)
				*result = entry->result;

			break;
		} else {
			entry->state = STATE_READY;

			if (entry->callback)
				bycatch = true;
		}
	}

	if (bycatch && !aio->bycatch) {
		aio->bycatch = true;
		uint64_t counter = 0;
		int r = write(l_io_get_fd(aio->eventfd), &counter, sizeof(counter));
		if (r < 0) {}
	}

	return true;
}

LIB_EXPORT void l_aio_destroy(struct l_aio *aio)
{
	l_io_destroy(aio->eventfd);
	io_destroy(aio->context);

	for (int i = 0; i < aio->entries; i++) {
		if (aio->list[i].state != STATE_WAIT)
			continue;

		if (aio->list[i].callback)
			aio->list[i].callback(-ECANCELED, aio->list[i].user_data);
	}

	l_free(aio->list);
	l_free(aio);
}

