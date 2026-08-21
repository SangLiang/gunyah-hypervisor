// Copyright © Qualcomm Technologies, Inc. and/or its subsidiaries.
//
// SPDX-License-Identifier: BSD-3-Clause

#include <assert.h>
#include <hyptypes.h>

#include <idle.h>
#include <object.h>
#include <panic.h>
#include <partition.h>
#include <refcount.h>
#include <thread.h>
#include <thread_init.h>

#include <events/object.h>

#include "event_handlers.h"
#include "thread_arch.h"

extern void
thread_switch_boot_thread(thread_t *new_thread);

void
thread_standard_handle_boot_runtime_warm_init(thread_t *boot_thread)
{
	// This must be the last operation in boot_runtime_warm_init.
	thread_switch_boot_thread(boot_thread);
}

// init函数执行完后停留在这里，等待调度器调度
noreturn void
thread_boot_set_idle(void)
{
	thread_t *thread = thread_get_self();
	assert(thread == idle_thread());

	thread_arch_set_thread(thread);
}

noreturn void
thread_boot_restore_frozen(void)
{
	thread_t *thread = thread_get_self();

	thread_arch_set_thread(thread);
}
