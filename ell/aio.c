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
#include <aio.h>

#include <sys/eventfd.h>

#include "private.h"
#include "useful.h"
#include "queue.h"
#include "io.h"
#include "aio.h"


#define QUEUEID_SHIFT 28
#define QUEUEID_MASK  0x70000000
#define REQID_MASK    0x0fffffff


typedef enum {
	QUEUEID_INVALID = 0,
	QUEUEID_POLL,
	QUEUEID_WAIT,
	QUEUEID_LAST = QUEUEID_WAIT
} queueid_t;

typedef enum {
	CMD_READ,
	CMD_WRITE
} command_t;

struct suspend_t {
	uint32_t reqid;
	int64_t timeout;
};

struct entry {
	struct aiocb aiocb;
	l_aio_cb_t callback;
	void *user_data;
	ssize_t result;
	uint32_t reqid;
};

struct notifier {
	int eventfd;
};

struct l_aio {
	struct l_queue *poll_list;
	struct l_queue *wait_list;
	struct l_queue *ready_list;
	struct l_io *eventfd;
	uint32_t next_reqid;
	struct notifier notifier;
};

static queueid_t get_queueid(uint32_t reqid)
{
	uint32_t queueid = (reqid & QUEUEID_MASK) >> QUEUEID_SHIFT;
	return queueid <= QUEUEID_LAST ? (queueid_t) queueid : QUEUEID_INVALID;
}

static uint32_t new_reqid(struct l_aio *aio, queueid_t queueid)
{
	uint32_t requid = ((queueid << QUEUEID_SHIFT) & QUEUEID_MASK) | aio->next_reqid;
	aio->next_reqid = (aio->next_reqid + 1) & REQID_MASK;
	return requid;
}

static void notify(union sigval sigval)
{
	struct notifier * notifier = sigval.sival_ptr;
	uint64_t c = 1;
	int r = write(notifier->eventfd, &c, sizeof (c));
	if (r < 0) {}
}

static void delete_entry(struct entry* entry, ssize_t *result)
{
	if (result)
		*result = entry->result;

	l_free(entry);
}

static bool complete_entry(struct entry *entry)
{
	int r = aio_error(&entry->aiocb);

	switch (r) {
		case EINPROGRESS:
			return false;
		case ECANCELED:
			entry->result = -ECANCELED;
			return true;
		case 0:
			entry->result = aio_return(&entry->aiocb);
			return true;
		default:
			entry->result = -r;
			return true;
	}
}

static bool callback_entry(void *data, void *user_data)
{
	struct entry* entry = data;

	entry->callback(entry->result, entry->user_data);

	l_free(entry);

	return true;
}

static bool poll_entry(void *data, void *user_data)
{
	struct entry* entry = data;
	struct l_aio *aio = user_data;

	if (!complete_entry(entry))
		return false;

	l_queue_push_tail(aio->ready_list, entry);

	return true;
}

static bool suspend_entry(struct entry *entry, int64_t nanoseconds)
{
	if (nanoseconds == 0)
		return false;
		
	struct timespec timespec = nanoseconds > 0 ?
		(struct timespec) { .tv_sec = nanoseconds / 1000000000, .tv_nsec = nanoseconds % 1000000000 } :
		(struct timespec) {0};

	struct aiocb const *list[] = { &entry->aiocb };

	for (;;) {
		int r = aio_suspend(list, 1, nanoseconds < 0 ? NULL : &timespec);

		if ((r == -1) && (errno == EINTR))
			continue;

		if ((r == -1) && (errno == EAGAIN))
			return false;

		return complete_entry(entry);
	}
}

static bool cancel_entry(struct entry *entry)
{
	int r = aio_cancel(entry->aiocb.aio_fildes, &entry->aiocb);

	if (r == AIO_ALLDONE) {
		entry->result = aio_return(&entry->aiocb);
		return true;
	} else if (AIO_CANCELED) {
		entry->result = -ECANCELED;
		return true;
	} else {
		return false;
	}
}

static bool cancel_entry_if(const void *a, const void *b)
{
	struct entry *entry = (struct entry *)a;
	const uint32_t *id = b;

	if (entry->reqid != *id)
		return false;

	return cancel_entry(entry);
}

static bool suspend_entry_if(const void *a, const void *b)
{
	struct entry *entry = (struct entry *)a;
	const struct suspend_t *suspend = b;

	if (entry->reqid != suspend->reqid)
		return false;

	return suspend_entry(entry, suspend->timeout);
}

static bool match_entry(const void *a, const void *b)
{
	const struct entry *entry = a;
	const uint32_t *id = b;

	return entry->reqid == *id;
}

static bool event_callback(struct l_io *io, void *user_data)
{
	struct l_aio * aio = user_data;

	uint64_t c;
	int r = read(aio->notifier.eventfd, &c, sizeof(c));

	if (r < 0) {} //ignore error, we are polling anyway

	l_queue_foreach_remove(aio->wait_list, poll_entry, aio);
	l_queue_foreach_remove(aio->ready_list, callback_entry, NULL);

	return true;
}

static int32_t submit_command(struct l_aio *aio, command_t cmd, l_aio_cb_t callback, int fd, off_t offset,
                           void *buffer, size_t count, void *user_data)
{
	if (unlikely(!aio))
		return -1;

	struct entry *entry = l_new(struct entry, 1);

	*entry = (struct entry) {
		.callback = callback,
		.user_data = user_data,
		.aiocb.aio_fildes = fd,
		.aiocb.aio_offset = offset,
		.aiocb.aio_buf = buffer,
		.aiocb.aio_nbytes = count,
		.aiocb.aio_sigevent.sigev_notify = callback ? SIGEV_THREAD : SIGEV_NONE,
		.aiocb.aio_sigevent.sigev_notify_function = callback ? notify : NULL,
		.aiocb.aio_sigevent.sigev_value.sival_ptr = callback ? &aio->notifier : NULL,
	};

	int r;

	switch (cmd) {
		case CMD_READ:
			r = aio_read(&entry->aiocb);
			break;
		case CMD_WRITE:
			r = aio_write(&entry->aiocb);
			break;
	}

	if (r < 0)
		goto error_aio;

	if (callback) {
		entry->reqid = new_reqid(aio, QUEUEID_WAIT);
		l_queue_push_tail(aio->wait_list, entry);
	} else {
		entry->reqid = new_reqid(aio, QUEUEID_POLL);
		l_queue_push_tail(aio->poll_list, entry);
	}

	return entry->reqid;

error_aio:
	l_free(entry);
	return -1;
}

LIB_EXPORT struct l_aio * l_aio_create(void)
{
	struct l_aio *aio = l_new(struct l_aio, 1);

	aio->wait_list = l_queue_new();
	aio->poll_list = l_queue_new();
	aio->ready_list = l_queue_new();


	aio->notifier.eventfd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);

	if (aio->notifier.eventfd < 0)
		goto error_event;

	aio->eventfd = l_io_new(aio->notifier.eventfd);

	if (!aio->eventfd)
		goto error_io;

	if (!l_io_set_read_handler(aio->eventfd, event_callback, aio, NULL))
		goto error_handler;

	return aio;

error_handler:
	l_io_destroy(aio->eventfd);
error_io:
	close(aio->notifier.eventfd);
error_event:
	l_queue_destroy(aio->wait_list, NULL);
	l_queue_destroy(aio->ready_list, NULL);
	l_free(aio);
	return NULL;
}

LIB_EXPORT int l_aio_get_fd(struct l_aio *aio, uint32_t reqid)
{
	if (unlikely(!aio))
		return -1;

	queueid_t qid = get_queueid(reqid);

	struct entry *entry;

	switch (qid) {
		case QUEUEID_INVALID:
			entry = NULL;
			break;
		case QUEUEID_POLL:
			entry = l_queue_find(aio->poll_list, match_entry, &reqid);
			break;
		case QUEUEID_WAIT:
			entry = l_queue_find(aio->wait_list, match_entry, &reqid);
			break;
	}

	if (!entry)
		return -1;

	return entry->aiocb.aio_fildes;
}

LIB_EXPORT int32_t l_aio_read(struct l_aio *aio, l_aio_cb_t read_cb, int fd, off_t offset,
						  void *buffer, size_t count, void *user_data)
{
	return submit_command(aio, CMD_READ, read_cb, fd, offset, buffer, count, user_data);
}

LIB_EXPORT int32_t l_aio_write(struct l_aio *aio, l_aio_cb_t write_cb, int fd, off_t offset,
						   const void *buffer, size_t count, void *user_data)
{
	return submit_command(aio, CMD_WRITE, write_cb, fd, offset, (void *)buffer, count, user_data);
}

LIB_EXPORT bool l_aio_cancel(struct l_aio *aio, uint32_t reqid, ssize_t *result)
{
	if (unlikely(!aio))
		return false;

	queueid_t qid = get_queueid(reqid);

	struct entry *entry;

	switch (qid) {
		case QUEUEID_INVALID:
			entry = NULL;
			break;
		case QUEUEID_POLL:
			entry = l_queue_remove_if(aio->poll_list, cancel_entry_if, &reqid);
			break;
		case QUEUEID_WAIT:
			entry = l_queue_remove_if(aio->wait_list, cancel_entry_if, &reqid);
			break;
	}

	if (!entry)
		return false;

	delete_entry(entry, result);

	return true;
}

LIB_EXPORT bool l_aio_await(struct l_aio *aio, uint32_t reqid, int64_t nanoseconds, ssize_t *result)
{
	if (unlikely(!aio))
		return false;

	queueid_t qid = get_queueid(reqid);

	struct suspend_t suspend = { .reqid = reqid, .timeout = nanoseconds };

	struct entry *entry;

	switch (qid) {
		case QUEUEID_INVALID:
			entry = NULL;
			break;
		case QUEUEID_POLL:
			entry = l_queue_remove_if(aio->poll_list, suspend_entry_if, &suspend);
			break;
		case QUEUEID_WAIT:
			entry = l_queue_remove_if(aio->wait_list, suspend_entry_if, &suspend);
			break;
	}

	if (!entry)
		return false;

	delete_entry(entry, result);

	return true;
}

LIB_EXPORT void l_aio_destroy(struct l_aio *aio)
{
	l_io_destroy(aio->eventfd);

	//TODO cancel and suspend all pending operations
	l_queue_destroy(aio->poll_list, NULL);
	l_queue_destroy(aio->ready_list, NULL);
	l_queue_destroy(aio->wait_list, NULL);
	l_free(aio);
}

