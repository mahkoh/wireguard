/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Copyright (C) 2015-2018 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#ifndef IPC_H
#define IPC_H

#include <stdbool.h>

#include "containers.h"

struct wgdevice;

int ipc_set_device(struct wgnetns *dev_netns, struct wgdevice *dev);
int ipc_get_device(struct wgnetns *dev_netns, struct wgdevice **dev,
		const char *interface);
char *ipc_list_devices(void);

#endif
