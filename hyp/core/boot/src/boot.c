// Copyright © Qualcomm Technologies, Inc. and/or its subsidiaries.
//
// SPDX-License-Identifier: BSD-3-Clause

#include <assert.h>
#include <hyptypes.h>

#include <hypversion.h>

#include <boot.h>
#include <compiler.h>
#include <log.h>
#include <memdb.h>
#include <panic.h>
#include <partition.h>
#include <prng.h>
#include <qcbor.h>
#include <thread_init.h>
#include <trace.h>
#include <util.h>

#include <events/boot.h>

#include "boot_init.h"
#include "event_handlers.h"

// Hypervisor 版本号字符串，格式为 "配置串-Git版本号 [质量标识]"。
const char hypervisor_version[] = HYP_CONF_STR "-" HYP_GIT_VERSION
#if defined(HYP_QUALITY)
					       " " HYP_QUALITY
#endif
	;
// Hypervisor 构建日期字符串
const char hypervisor_build_date[] = HYP_BUILD_DATE;

// 栈溢出保护（stack canary）全局变量。
// 在启用栈保护的构建中，编译器会在函数序言/尾声处插入对该值的校验代码。
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wreserved-identifier"
extern uintptr_t __stack_chk_guard;
uintptr_t	 __stack_chk_guard __attribute__((used, visibility("hidden")));
#pragma clang diagnostic pop

// 主 CPU 冷启动初始化入口
//
// 在主 CPU 早期引导阶段执行，完成栈保护值设置、各阶段引导事件触发以及
// 进入 idle 线程等关键流程。函数不会返回。
noreturn void
boot_cold_init(cpu_index_t cpu) LOCK_IMPL
{
	// 设置栈保护值：若保护值为全局变量则此处设置全局值，若为线程局部
	// 存储则仅设置 init 线程的值。该步骤无法放到事件处理函数中完成，
	// 因为事件处理函数在非内联（如 debug 构建）情况下可能触发栈检查失败。
	uint64_result_t guard_r = prng_get64();
	assert(guard_r.e == OK);
	__stack_chk_guard = (uintptr_t)guard_r.r;

	// 此时尚不能使用 trace/log，因为线程中的 CPU 索引与抢占计数尚未初始化

	// 以下三个函数为各个模块去做自己的初始化，每个函数可以理解为一个大捆绑包，里面
	// 包含了多个其他模块的初始化函数。根据不同的阶段，来执行不同的函数。
	trigger_boot_cpu_early_init_event();
	trigger_boot_cold_init_event(cpu);
	trigger_boot_cpu_cold_init_event(cpu);

	// 此时 CPU 索引与抢占计数已就绪，可以安全地使用 trace/log
	TRACE_AND_LOG(ERROR, WARN, "Hypervisor cold boot, version: {:s} ({:s})",
		      (register_t)hypervisor_version,
		      (register_t)hypervisor_build_date);

	TRACE(DEBUG, INFO, "boot_cpu_warm_init");
	trigger_boot_cpu_warm_init_event();
	TRACE(DEBUG, INFO, "boot_hypervisor_start");
	trigger_boot_hypervisor_start_event();
	TRACE(DEBUG, INFO, "boot_cpu_start");
	trigger_boot_cpu_start_event();
	TRACE(DEBUG, INFO, "entering idle");
	
	// 进入idle线程，等待事件触发
	// 这是冷启动结束后，把当前 CPU 从引导栈切到 idle 线程，让调度器开始真正跑系统
	thread_boot_set_idle();
}

#if defined(VERBOSE) && VERBOSE
// VERBOSE 调试构建下用于栈红区的填充字节与大小
#define STACK_GUARD_BYTE 0xb8
#define STACK_GUARD_SIZE 256U
#include <string.h>

#include <panic.h>
#endif

// AArch64 引导栈的底部地址（由链接脚本定义）
extern char aarch64_boot_stack[];

// boot_cold_init 事件回调：在 VERBOSE 构建下为引导栈添加红区
void
boot_handle_boot_cold_init(void)
{
#if defined(VERBOSE) && VERBOSE
	// 在引导栈底部填充红区字节，用于后续检测栈溢出
	errno_t err_mem = memset_s(aarch64_boot_stack, STACK_GUARD_SIZE,
				   STACK_GUARD_BYTE, STACK_GUARD_SIZE);
	if (err_mem != 0) {
		panic("Error in memset_s operation!");
	}
#endif
}

// idle 线程启动回调
//
// 在系统进入 idle 阶段时被调用：在 VERBOSE 构建下校验引导栈红区是否被破坏，
// 并将引导栈释放回 hypervisor 私有分区作为堆内存使用。
void
boot_handle_idle_start(void)
{
	char *stack_bottom = (char *)aarch64_boot_stack;

#if defined(VERBOSE) && VERBOSE
	// 校验引导栈红区字节是否仍保持原值，若被改写则说明发生了栈溢出
	for (index_t i = 0; i < STACK_GUARD_SIZE; i++) {
		if (stack_bottom[i] != (char)STACK_GUARD_BYTE) {
			panic("boot stack overflow!");
		}
	}
#endif

	partition_t *private = partition_get_private();

	size_t stack_size = BOOT_STACK_SIZE;

	// 释放引导栈，将其归还给 hypervisor 分区作为堆内存。
	// TODO: 寻找更合适的位置释放引导栈
	// FIXME: QC Gunyah issue #44
	error_t err = partition_add_heap(
		private, partition_image_virt_to_phys((uintptr_t)stack_bottom),
		stack_size);
	if (err != OK) {
		panic("Error freeing stack to hypervisor partition");
	}
}

// 副 CPU 冷启动初始化入口
//
// 由副 CPU 在被唤醒后执行，流程与主 CPU 类似但跳过部分仅主 CPU 需要的步骤。
// 函数不会返回。
noreturn void
boot_secondary_init(cpu_index_t cpu) LOCK_IMPL
{
	// 此时尚不能使用 trace/log，因为线程中的 CPU 索引与抢占计数尚未初始化

	trigger_boot_cpu_early_init_event();
	trigger_boot_cpu_cold_init_event(cpu);

	// 此时 CPU 索引与抢占计数已就绪，可以安全地使用 trace/log
	TRACE_AND_LOG(INFO, WARN, "secondary cpu ({:d}) cold boot",
		      (register_t)cpu);

	trigger_boot_cpu_warm_init_event();
	trigger_boot_cpu_start_event();

	TRACE_LOCAL(DEBUG, INFO, "cpu cold boot complete");
	thread_boot_set_idle();
}

// 任意 CPU 的热启动（第二次或之后的上电）入口。函数不会返回。
noreturn void
boot_warm_init(void) LOCK_IMPL
{
	trigger_boot_cpu_early_init_event();
	TRACE_LOCAL(INFO, INFO, "cpu warm boot start");
	trigger_boot_cpu_warm_init_event();
	trigger_boot_cpu_start_event();
	TRACE_LOCAL(DEBUG, INFO, "cpu warm boot complete");

	//注意：热启动结尾不是 `thread_boot_set_idle`，而是 `thread_boot_restore_frozen`
	// ——它恢复的是"被冻结的当前线程栈"（休眠前正在跑的那个线程），而不是 idle。因为热启动是从休眠中醒来，
	// 应该回到休眠前的地方继续跑，而不是回到 idle。
	thread_boot_restore_frozen();
}

// memdb_walk 的回调函数
//
// 将一段空闲物理内存区间的起始地址和大小以 CBOR 数组形式编码到输出上下文中。
static error_t
boot_do_memdb_walk(paddr_t base, size_t size, void *arg)
{
	qcbor_enc_ctxt_t *qcbor_enc_ctxt = (qcbor_enc_ctxt_t *)arg;

	if ((size == 0U) && (util_add_overflows(base, size - 1U))) {
		return ERROR_ARGUMENT_SIZE;
	}

	QCBOREncode_OpenArray(qcbor_enc_ctxt);

	QCBOREncode_AddUInt64(qcbor_enc_ctxt, base);
	QCBOREncode_AddUInt64(qcbor_enc_ctxt, size);

	QCBOREncode_CloseArray(qcbor_enc_ctxt);

	return OK;
}

// 将指定对象/类型对应的所有空闲内存区间编码到 CBOR 输出中
//
// 在 CBOR 输出上下文的 "free_ranges" 映射项下追加一个数组，数组中每个元素
// 形如 [base, size]，描述一段空闲物理内存区间。
error_t
boot_add_free_range(uintptr_t object, memdb_type_t type,
		    qcbor_enc_ctxt_t *qcbor_enc_ctxt)
{
	error_t ret;

	QCBOREncode_OpenArrayInMap(qcbor_enc_ctxt, "free_ranges");

	ret = memdb_walk(object, type, boot_do_memdb_walk,
			 (void *)qcbor_enc_ctxt);

	QCBOREncode_CloseArray(qcbor_enc_ctxt);

	return ret;
}

// 触发 hypervisor 启动移交事件
//
// 通常在引导流程末尾由主 CPU 调用，将控制权移交给上层启动逻辑。
void
boot_start_hypervisor_handover(void)
{
	trigger_boot_hypervisor_handover_event();
}
