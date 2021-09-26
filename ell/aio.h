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

#ifndef __ELL_AIO_H
#define __ELL_AIO_H

#include <stdbool.h>
#include <stdint.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

struct l_aio;

typedef void (*l_aio_cb_t) (ssize_t result, void *user_data);

struct l_aio *l_aio_create(void);

int l_aio_get_fd(struct l_aio *aio, uint32_t reqid);

int32_t l_aio_read(struct l_aio *aio, l_aio_cb_t read_cb, int fd, off_t offset,
               void *buffer, size_t count, void *user_data);


int32_t l_aio_write(struct l_aio *aio, l_aio_cb_t write_cb, int fd, off_t offset,
               const void *buffer, size_t count, void *user_data);

bool l_aio_cancel(struct l_aio *aio, uint32_t reqid, ssize_t *result);

bool l_aio_await(struct l_aio *aio, uint32_t reqid, int64_t nanoseconds, ssize_t *result);

void l_aio_destroy(struct l_aio *aio);

#ifdef __cplusplus
}
#endif

#endif /* __ELL_AIO_H */
