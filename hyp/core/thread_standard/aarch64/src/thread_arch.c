// Copyright © Qualcomm Technologies, Inc. and/or its subsidiaries.
//
// SPDX-License-Identifier: BSD-3-Clause

#include <assert.h>
#include <hyptypes.h>

#include <compiler.h>
#include <idle.h>
#include <object.h>
#include <panic.h>
#include <partition.h>
#include <pgtable.h>
#include <platform_timer.h>
#include <preempt.h>
#include <prng.h>
#include <scheduler.h>
#include <thread.h>
#include <trace.h>

#include <events/thread.h>

#include "event_handlers.h"
#include "thread_arch.h"

typedef register_t (*fptr_t)(register_t arg);
typedef void (*fptr_noreturn_t)(register_t arg);

const size_t thread_stack_min_align    = 16;
const size_t thread_stack_alloc_align  = PGTABLE_HYP_PAGE_SIZE;
const size_t thread_stack_size_default = PGTABLE_HYP_PAGE_SIZE;

static size_t
thread_get_tls_offset(void)
{
	size_t offset = 0;
	__asm__("add     %0, %0, :tprel_hi12:current_thread	;"
		"add     %0, %0, :tprel_lo12_nc:current_thread	;"
		: "+r"(offset));
	return offset;
}

static uintptr_t
thread_get_tls_base(thread_t *thread)
{
	return (uintptr_t)thread - thread_get_tls_offset();
}

static noreturn void
thread_arch_main(thread_t *prev, ticks_t schedtime) LOCK_IMPL
{
	thread_t *thread = thread_get_self();

	trigger_thread_start_event();

	trigger_thread_context_switch_post_event(prev, schedtime, (ticks_t)0UL);
	object_put_thread(prev);

	thread_func_t thread_func =
		trigger_thread_get_entry_fn_event(thread->kind);
	trigger_thread_load_state_event(true);

	if (thread_func != NULL) {
		preempt_enable();
		thread_func(thread->params);
	}

	thread_exit();
}

// AArch64 架构下的线程上下文切换函数。
//
// 功能：保存当前（旧）线程的 PC/SP/FP 到其 context 结构，加载下一个（新）
// 线程的 SP/FP/TLS 基址，然后通过寄存器间接跳转（BR）切换到新线程的 PC，
// 完成线程切换。这是 hypervisor 调度器执行线程切换的核心入口。
//
// 关键设计要点：
//   1. 旧线程的 PC/SP/FP 必须先保存到 old->context，以便下次切回该线程时
//      能从断点处继续执行；新线程的 PC/SP/FP 则来自 next_thread->context
//      （新线程首次运行时由 thread_arch_init_context() 设置）。
//   2. TPIDR_EL2 用作线程本地存储（TLS）基址指针，切换时一并更新，使
//      thread_get_self() 等基于 TLS 的访问能定位到正确的 thread_t 结构。
//   3. 新线程的 PC 放入 x16/x17，以兼容 ARMv8.5-BTI：BTI 要求间接跳转的
//      目标寄存器为 x16/x17 时，BR 才被视作合法的调用跳转板，允许跳转到
//      入口处的 BTI C 指令，否则会触发分支目标异常。
//   4. 通过内联汇编一次性完成"保存旧上下文 + 加载新上下文 + 跳转"，避免
//      中间状态被中断破坏；clobber 列表与硬绑定寄存器、显式保存的
//      x29/sp/pc 的并集必须覆盖全部整数寄存器状态，保证编译器不会误用。
//   5. 调度时间片（ticks）通过 x0/x1 在线程间传递：切出方把剩余时间写
//      入 x1，切入方（thread_arch_main）作为参数接收，本函数末尾再将其
//      回写到 *schedtime，供调度器统计。
//
// 返回值：返回被切出的旧线程指针（old），供调用方进行引用计数等清理。
thread_t *
thread_arch_switch_thread(thread_t *next_thread, ticks_t *schedtime)
{
	// 旧线程指针和调度时间片必须保存在 X0 和 X1 中，以确保
	// thread_arch_main() 在首次上下文切换时能将它们作为参数接收到。
	register thread_t *old __asm__("x0")   = thread_get_self();
	register ticks_t   ticks __asm__("x1") = *schedtime;

	// 此处其余硬绑定的寄存器只是为了保证下方 clobber 列表正确。
	// clobber 列表、硬绑定寄存器与显式保存的寄存器（x29、sp、pc）的并集
	// 必须覆盖全部整数寄存器状态。
	register register_t old_pc __asm__("x2");
	register register_t old_sp __asm__("x3");
	register register_t old_fp __asm__("x4");
	register uintptr_t  old_context __asm__("x5") =
		(uintptr_t)&old->context.pc;
	static_assert(offsetof(thread_t, context.sp) ==
			      (offsetof(thread_t, context.pc) +
			       sizeof(next_thread->context.pc)),
		      "PC and SP must be adjacent in context");
	static_assert(offsetof(thread_t, context.fp) ==
			      (offsetof(thread_t, context.sp) +
			       sizeof(next_thread->context.sp)),
		      "SP and FP must be adjacent in context");

	// 新线程的 PC 必须放在 x16 或 x17 中，这样 ARMv8.5-BTI 才会把下方的
	// BR 当作调用跳转板，从而允许它跳转到新线程入口点处的 BTI C 指令。
	register register_t new_pc __asm__("x16") = next_thread->context.pc;
	register register_t new_sp __asm__("x6")  = next_thread->context.sp;
	register register_t new_fp __asm__("x7")  = next_thread->context.fp;
	register uintptr_t  new_tls_base __asm__("x8") =
		thread_get_tls_base(next_thread);

	__asm__ volatile(
		"adr	%[old_pc], .Lthread_continue.%=		;" // 旧线程恢复点：切换回来时从这里继续
		"mov	%[old_sp], sp				;" // 保存当前 SP
		"mov	%[old_fp], x29				;" // 保存当前 FP（x29）
		"mov   sp, %[new_sp]				;" // 切换到新线程的 SP
		"mov   x29, %[new_fp]				;" // 切换到新线程的 FP
		"msr	TPIDR_EL2, %[new_tls_base]		;" // 切换 TLS 基址寄存器
		"stp	%[old_pc], %[old_sp], [%[old_context]]	;" // 保存旧线程的 PC、SP 到 context
		"str	%[old_fp], [%[old_context], 16]		;" // 保存旧线程的 FP 到 context
		"br	%[new_pc]				;" // 跳转到新线程的 PC（完成切换）
		".Lthread_continue.%=:				;" // 旧线程被切回时从此处继续执行
#if defined(ARCH_ARM_FEAT_BTI)
		"bti	j					;" // BTI 跳转指令，标记此处为合法的间接跳转目标
#endif
		: [old] "+r"(old), [old_pc] "=&r"(old_pc),
		  [old_sp] "=&r"(old_sp), [old_fp] "=&r"(old_fp),
		  [old_context] "+r"(old_context), [new_pc] "+r"(new_pc),
		  [new_sp] "+r"(new_sp), [new_fp] "+r"(new_fp),
		  [new_tls_base] "+r"(new_tls_base), [ticks] "+r"(ticks)
		: /* 此处不能有任何输入操作数 */
		: "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x17", "x18",
		  "x19", "x20", "x21", "x22", "x23", "x24", "x25", "x26", "x27",
		  "x28", "x30", "cc", "memory");

	// 从上一个线程传递过来的 tick 计数更新调度时间
	*schedtime = ticks;

	return old;
}

noreturn void
thread_arch_set_thread(thread_t *thread)
{
	// This should be called on the thread during power-up, which
	// should already be the current thread for TLS. It discards the current
	// execution state.
	assert(thread == thread_get_self());

	// The previous thread and the scheduling time must be kept in X0 and X1
	// to ensure that thread_arch_main() receives them as arguments on the
	// first context switch during CPU cold boot. The scheduling time is set
	// to 0 because we consider the idle thread to have been scheduled at
	// the epoch. These are unused on warm boot, which is always resuming a
	// thread_freeze() call.
	register thread_t *old __asm__("x0")   = thread;
	register ticks_t   ticks __asm__("x1") = (ticks_t)0U;

	// The new PC must be in x16 or x17 so ARMv8.5-BTI will treat the BR
	// below as a call trampoline, and thus allow it to jump to the BTI C
	// instruction at a new thread's entry point.
	register register_t new_pc __asm__("x16");
	new_pc		  = thread->context.pc;
	register_t new_sp = thread->context.sp;
	register_t new_fp = thread->context.fp;

	__asm__ volatile(
		"mov   sp, %[new_sp]			;"
		"mov   x29, %[new_fp]			;"
		"br	%[new_pc]			;"
		:
		: [old] "r"(old), [ticks] "r"(ticks), [new_pc] "r"(new_pc),
		  [new_sp] "r"(new_sp), [new_fp] "r"(new_fp)
		: "memory");
	__builtin_unreachable();
}

register_t
thread_freeze(fptr_t fn, register_t param, register_t resumed_result)
{
	TRACE(INFO, INFO, "thread_freeze start fn: {:#x} param: {:#x}",
	      (uintptr_t)fn, (uintptr_t)param);

	trigger_thread_save_state_event();

	thread_t *thread = thread_get_self();
	assert_safety(thread != NULL);

	// The parameter must be kept in X0 so the freeze function gets it as an
	// argument.
	register register_t x0 __asm__("x0") = param;

	// The remaining hard-coded registers here are only needed to
	// ensure a correct clobber list below. The union of the clobber
	// list, fixed output registers and explicitly saved registers
	// (x29, sp and pc) must be the entire integer register state.
	register register_t saved_pc __asm__("x1");
	register register_t saved_sp __asm__("x2");
	register uintptr_t  context __asm__("x3") =
		(uintptr_t)&thread->context.pc;
	register fptr_t fn_reg __asm__("x4") = fn;
	register bool	is_resuming __asm__("x5");

	static_assert(offsetof(thread_t, context.sp) ==
			      (offsetof(thread_t, context.pc) +
			       sizeof(thread->context.pc)),
		      "PC and SP must be adjacent in context");
	static_assert(offsetof(thread_t, context.fp) ==
			      (offsetof(thread_t, context.sp) +
			       sizeof(thread->context.sp)),
		      "SP and FP must be adjacent in context");

	__asm__ volatile(
		"adr	%[saved_pc], .Lthread_freeze.resumed.%=	;"
		"mov	%[saved_sp], sp				;"
		"stp	%[saved_pc], %[saved_sp], [%[context]]	;"
		"str	x29, [%[context], 16]			;"
		"blr	%[fn_reg]				;"
		"mov	%[is_resuming], 0			;"
		"b	.Lthread_freeze.done.%=			;"
		".Lthread_freeze.resumed.%=:			;"
#if defined(ARCH_ARM_FEAT_BTI)
		"bti	j					;"
#endif
		"mov	%[is_resuming], 1			;"
		".Lthread_freeze.done.%=:			;"
		: [is_resuming] "=%r"(is_resuming), [saved_pc] "=&r"(saved_pc),
		  [saved_sp] "=&r"(saved_sp), [context] "+r"(context),
		  [fn_reg] "+r"(fn_reg), "+r"(x0)
		: /* This must not have any inputs */
		: "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14",
		  "x15", "x16", "x17", "x18", "x19", "x20", "x21", "x22", "x23",
		  "x24", "x25", "x26", "x27", "x28", "x30", "cc", "memory");

	if (is_resuming) {
		x0 = resumed_result;
		trigger_thread_load_state_event(false);

		TRACE(INFO, INFO, "thread_freeze resumed: {:#x}", x0);
	} else {
		TRACE(INFO, INFO, "thread_freeze returned: {:#x}", x0);
	}

	return x0;
}

noreturn void
thread_reset_stack(fptr_noreturn_t fn, register_t param)
{
	thread_t	   *thread	     = thread_get_self();
	register register_t x0 __asm__("x0") = param;
	uintptr_t new_sp = (uintptr_t)thread->stack_base + thread->stack_size;

	__asm__ volatile("mov	sp, %[new_sp]	;"
			 "mov	x29, 0		;"
			 "blr	%[new_pc]	;"
			 :
			 : [new_pc] "r"(fn), [new_sp] "r"(new_sp), "r"(x0)
			 : "memory");
	panic("returned to thread_reset_stack()");
}

// 初始化新线程的 CPU 上下文（PC、SP、FP），使其在首次被调度切换到时，
// 能够正确跳转到线程入口并使用该线程专属的栈空间。
//
// - PC 设为 thread_arch_main：这是新线程第一次运行时实际执行的入口，
//   它会触发线程启动事件、加载线程状态，并调用线程真正的工作函数
//   （thread_func），工作函数返回后再执行 thread_exit() 结束线程。
//   因此本函数并不直接指向用户提供的 thread_func，而是经由
//   thread_arch_main 这一统一入口进行包装。
// - SP 设为栈顶（stack_base + stack_size）：AArch64 栈是满递减栈，
//   栈顶即栈内存的最高地址，首次压栈时向下生长。
// - FP 设为 0：表示该栈帧为初始帧（链的末尾），栈回溯到此为止。
//
// 注意：此处仅设置 PC/SP/FP 三个上下文寄存器，其余寄存器状态由
// thread_arch_switch_thread() 在首次上下文切换时通过内联汇编补齐。
void
thread_arch_init_context(thread_t *thread)
{
	assert(thread != NULL);

	thread->context.pc = (uintptr_t)thread_arch_main;
	thread->context.sp = (uintptr_t)thread->stack_base + thread->stack_size;
	thread->context.fp = (uintptr_t)0;
}

error_t
thread_arch_map_stack(thread_t *thread)
{
	error_t err;

	assert(thread != NULL);
	assert(thread->stack_base != 0U);

	partition_t *partition = thread->header.partition;
	paddr_t	     stack_phys =
		partition_virt_to_phys(partition, thread->stack_mem);

	pgtable_hyp_start();
	err = pgtable_hyp_map(partition, thread->stack_base, thread->stack_size,
			      stack_phys, PGTABLE_HYP_MEMTYPE_WRITEBACK,
			      PGTABLE_ACCESS_RW,
			      VMSA_SHAREABILITY_INNER_SHAREABLE);
	pgtable_hyp_commit();

	return err;
}

void
thread_arch_unmap_stack(thread_t *thread)
{
	pgtable_hyp_start();
	pgtable_hyp_unmap(thread->header.partition, thread->stack_base,
			  thread->stack_size, thread->stack_size);
	pgtable_hyp_commit();
}
