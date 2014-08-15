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

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <assert.h>

#include <ell/ell.h>
#include "ell/dbus-private.h"

static void do_debug(const char *str, void *user_data)
{
	const char *prefix = user_data;

	l_info("%s%s", prefix, str);
}

static void signal_handler(struct l_signal *signal, uint32_t signo,
							void *user_data)
{
	switch (signo) {
	case SIGINT:
	case SIGTERM:
		l_info("Terminate");
		l_main_quit();
		break;
	}
}

static void ready_callback(void *user_data)
{
	struct l_dbus *dbus = user_data;

	l_info("ready");
}

int main(int argc, char *argv[])
{
	struct l_dbus *dbus;
	char bus_name[16];
	char bus_address[64];
	int bus_fd;
	struct l_signal *signal;
	sigset_t mask;

	sigemptyset(&mask);
	sigaddset(&mask, SIGINT);
	sigaddset(&mask, SIGTERM);

	signal = l_signal_create(&mask, signal_handler, NULL, NULL);

	l_log_set_stderr();

	snprintf(bus_name, sizeof(bus_name), "%u-ell-test", getuid());

	bus_fd = _dbus_kernel_create_bus(bus_name);
	if (bus_fd < 0) {
		l_warn("kdbus not available");
		return EXIT_SUCCESS;
	}

	snprintf(bus_address, sizeof(bus_address),
				"kernel:path=/dev/kdbus/%s/bus", bus_name);

	dbus = l_dbus_new(bus_address);

	assert(dbus);

	l_dbus_set_debug(dbus, do_debug, "[DBUS] ", NULL);

	l_dbus_set_ready_handler(dbus, ready_callback, dbus, NULL);

	l_main_run();

	l_dbus_destroy(dbus);
	close(bus_fd);

	l_signal_remove(signal);

	return EXIT_SUCCESS;
}
