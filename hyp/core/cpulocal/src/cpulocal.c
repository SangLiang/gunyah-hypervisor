// Copyright © Qualcomm Technologies, Inc. and/or its subsidiaries.
//
// SPDX-License-Identifier: BSD-3-Clause

//cpulocal.c 负责维护 “当前线程跑在哪颗 CPU 上”，
//给hypervisor 的 per-CPU 数据（CPULOCAL(...)）提供索引。

#include <assert.h>
#include <hyptypes.h>

#include <compiler.h>
#include <cpulocal.h>
#include <idle.h>
#include <thread.h>
#include <trace.h>

#include "event_handlers.h"

// 判断 CPU 索引是否合法
//
// 检查给定的索引是否在平台支持的最大核数范围内。
//
// 参数:
//   index - 待校验的 CPU 索引
//
// 返回值:
//   true  - 索引合法
//   false - 索引越界
bool
cpulocal_index_valid(cpu_index_t index)
{
	return index < (cpu_index_t)PLATFORM_MAX_CORES;
}

// 校验并返回 CPU 索引
//
// 在调试构建中会断言索引合法；非调试构建下直接返回该索引。
cpu_index_t
cpulocal_check_index(cpu_index_t index)
{
	assert_debug(cpulocal_index_valid(index));
	return index;
}

// 将指针差值校验后转换为 CPU 索引
//
// 用于把基于 percpu 基址的偏移量转换回 CPU 索引。
cpu_index_t
cpulocal_ptr_check_index(ptrdiff_t index)
{
	assert_safety(index >= (ptrdiff_t)0);
	assert_safety(cpulocal_index_valid((cpu_index_t)index));
	return (cpu_index_t)index;
}

// 获取指定线程当前所在的 CPU 索引
//
// 注意：调用方需保证传入的线程指针非空，且线程当前确实在某个 CPU 上运行。
cpu_index_t
cpulocal_get_index_for_thread(const thread_t *thread)
{
	assert_safety(thread != NULL);
	return thread->cpulocal_current_cpu;
}

// 获取当前线程所在 CPU 的索引（非安全版本）
//
// 直接读取当前线程中保存的 CPU 索引，不做额外同步检查。
cpu_index_t
cpulocal_get_index_unsafe(void)
{
	const thread_t *self = thread_get_self();
	return cpulocal_get_index_for_thread(self);
}

// CPU 冷启动初始化回调
//
// 在 CPU 早期引导阶段被调用，用于在主 idle 线程上尽早设置其所属的 CPU 索引，
// 以便后续 percpu 相关逻辑能够正确定位当前 CPU。
void
cpulocal_handle_boot_cpu_cold_init(cpu_index_t cpu)
{
	thread_t *self = thread_get_self();
	assert_safety(self != NULL);

	// 尽早在主 idle 线程上设置 CPU 索引
	self->cpulocal_current_cpu = cpulocal_check_index(cpu);

	// 此处是能够调用 TRACE() 的最早时机，借此通知调试器当前 CPU 已上线
	TRACE_LOCAL(DEBUG, INFO, "CPU {:d} coming online", cpu);
}

// 线程对象创建回调
//
// 主 idle 线程会在自身创建时调用本函数，此时其 CPU 索引已在
// boot_cpu_cold_init 回调中设置完毕；因此需要避免覆盖当前线程已设置好的索引。
error_t
cpulocal_handle_object_create_thread(thread_create_t thread_create)
{
	// 主 idle 线程会调用自身，并且已在 boot_cpu_cold_init 处理函数中
	// 设置好了 CPU 索引；这里需要检查，避免误覆盖当前线程的索引
	if (thread_get_self() != thread_create.thread) {
		thread_create.thread->cpulocal_current_cpu = CPU_INDEX_INVALID;
	}

	return OK;
}

// 线程上下文切换完成后的回调
//
// 在调度器完成从 prev 线程切换到当前线程之后调用，用于更新两个线程各自
// 记录的“当前所在 CPU”字段，保证 percpu 语义正确。
void
cpulocal_handle_thread_context_switch_post(thread_t *prev)
{
	thread_t *self = thread_get_self();
	assert_safety(self != NULL);
	cpu_index_t this_cpu = CPU_INDEX_INVALID;

#if SCHEDULER_CAN_MIGRATE
	// 当调度器允许线程迁移时，需要区分两种情况：
	//   1) prev == self：表示 idle 线程被重新调度回来，此时以调度亲和性为准；
	//   2) prev != self：从 prev 线程继承其 CPU 索引，并校验 idle 线程的亲和性一致性。
	if (compiler_unexpected(prev == self)) {
		assert_safety(idle_thread() == prev);
		this_cpu = self->scheduler_affinity;
	} else {
		assert_safety(prev != NULL);
		this_cpu = cpulocal_check_index(prev->cpulocal_current_cpu);
		assert_debug(!thread_is_kind(self, THREAD_KIND_IDLE) ||
			     (this_cpu == self->scheduler_affinity));
	}
#else
	// 调度器不允许迁移时，线程始终绑定在其亲和性指定的 CPU 上
	this_cpu = self->scheduler_affinity;
#endif

	assert_safety(this_cpu != CPU_INDEX_INVALID);
	// prev 线程即将离开当前 CPU，将其索引置为无效
	prev->cpulocal_current_cpu = CPU_INDEX_INVALID;
	// 当前线程接管该 CPU
	self->cpulocal_current_cpu = this_cpu;
}
