// Copyright © Qualcomm Technologies, Inc. and/or its subsidiaries.
//
// SPDX-License-Identifier: BSD-3-Clause

#include <assert.h>
#include <hyptypes.h>

#include <hypcontainers.h>

#include <compiler.h>
#include <cpulocal.h>
#include <panic.h>
#include <platform_cpu.h>
#include <preempt.h>
#include <qcbor.h>
#include <scheduler.h>
#include <thread.h>
#include <util.h>
#include <vcpu.h>
#include <vic.h>
#include <virq.h>

#include <events/vcpu.h>

#include "event_handlers.h"

void
vcpu_handle_object_get_defaults_thread(thread_create_t *thread_create)
{
	uint32_t stack_size;

	assert(thread_create != NULL);

	// This may be 0, which will fall back to the global default
	stack_size = platform_cpu_stack_size();

#if defined(VCPU_MIN_STACK_SIZE)
	stack_size = util_max(stack_size, VCPU_MIN_STACK_SIZE);
#endif

	assert((stack_size == 0U) ||
	       util_is_baligned(stack_size, PGTABLE_HYP_PAGE_SIZE));
	assert(stack_size <= THREAD_STACK_MAX_SIZE);

	thread_create->stack_size = stack_size;
	thread_create->kind	  = THREAD_KIND_VCPU;
}

error_t
vcpu_handle_object_create_thread(thread_create_t thread_create)
{
	thread_t *thread = thread_create.thread;
	assert(thread != NULL);
	error_t ret;

	if (vcpu_is_vcpu(thread)) {
		scheduler_block_init(thread, SCHEDULER_BLOCK_VCPU_OFF);
	}

	if (thread_create.scheduler_priority_valid &&
	    (thread_create.scheduler_priority > VCPU_MAX_PRIORITY)) {
		ret = ERROR_DENIED;
	} else {
		ret = OK;
	}

	return ret;
}

// object_activate_thread 的 VCPU 侧处理：线程对象从“已配置”进入“可运行”
// 前的最后校验。vcpu.ev 里以 priority -100 订阅，尽量靠后执行。
//
// 非 VCPU 线程直接返回 OK。VCPU 必须已绑定 cspace，且 affinity 若是合法
// CPU 下标，对应物理核必须存在。随后清空 configure 阶段留下的
// vcpu_options，再触发 vcpu_activate_thread，让各模块重新写入经过检查的
// 选项；任一失败则返回 ERROR_OBJECT_CONFIG，激活失败并走 unwind。
error_t
vcpu_handle_object_activate_thread(thread_t *thread)
{
	error_t ret = OK;

	assert(thread != NULL);

	if (vcpu_is_vcpu(thread)) {
		// VCPU 通过 hypercall 操作对象，必须挂在某个 cspace 上
		if (thread->cspace_cspace == NULL) {
			ret = ERROR_OBJECT_CONFIG;
			goto out;
		}

		// affinity 已是合法 CPU 下标时，还要确认平台上真有这颗核
		if (cpulocal_index_valid(thread->scheduler_affinity) &&
		    !platform_cpu_exists(thread->scheduler_affinity)) {
			ret = ERROR_OBJECT_CONFIG;
			goto out;
		}

		// 重置线程的 vcpu_options。事件 handler 可以再次设置它们。
		// 这样可避免 configure 阶段未经检查的选项残留在线程选项里。
		vcpu_option_flags_t options = thread->vcpu_options;
		thread->vcpu_options	    = vcpu_option_flags_default();

		if (!trigger_vcpu_activate_thread_event(thread, options)) {
			ret = ERROR_OBJECT_CONFIG;
		}
	}

out:
	return ret;
}

void
vcpu_handle_thread_exited(void)
{
	thread_t *current = thread_get_self();
	assert(current != NULL);

	assert_preempt_disabled();

	if (vcpu_is_vcpu(current)) {
		if (vcpu_option_flags_get_critical(&current->vcpu_options)) {
			panic("Critical VCPU exited");
		}

		trigger_vcpu_stopped_event();
	}
}

void
vcpu_handle_thread_save_state(void)
{
	thread_t *thread = thread_get_self();

	if (compiler_expected(vcpu_is_vcpu(thread))) {
		trigger_vcpu_save_state_event();
	}
}

void
vcpu_handle_thread_load_state(void)
{
	thread_t *thread = thread_get_self();

	if (compiler_expected(vcpu_is_vcpu(thread))) {
		trigger_vcpu_load_state_event();
	} else {
		trigger_vcpu_disable_state_event();
	}
}

bool
vcpu_handle_vcpu_activate_thread(thread_t *thread, vcpu_option_flags_t options)
{
	bool ret = false;

	assert(thread != NULL);
	assert(vcpu_is_vcpu(thread));

	// Check that the partition has the right to mark the VCPU as critical.
	if (vcpu_option_flags_get_critical(&options) ||
	    vcpu_option_flags_get_hlos_vm(&options)) {
		if (!partition_option_flags_get_privileged(
			    &thread->header.partition->options)) {
			goto out;
		}

		vcpu_option_flags_set_critical(&thread->vcpu_options, true);
	}

	ret = true;

out:
	return ret;
}

void
vcpu_handle_object_deactivate_thread(thread_t *thread)
{
	if (vcpu_is_vcpu(thread)) {
		vic_unbind(&thread->vcpu_halt_virq_src);
	}
}

error_t
vcpu_bind_virq(thread_t *vcpu, vic_t *vic, virq_t virq,
	       vcpu_virq_type_t virq_type)
{
	return trigger_vcpu_bind_virq_event(virq_type, vcpu, vic, virq);
}

error_t
vcpu_unbind_virq(thread_t *vcpu, vcpu_virq_type_t virq_type)
{
	return trigger_vcpu_unbind_virq_event(virq_type, vcpu);
}

error_t
vcpu_handle_vcpu_bind_virq(thread_t *vcpu, vic_t *vic, virq_t virq)
{
	error_t err = vic_bind_shared(&vcpu->vcpu_halt_virq_src, vic, virq,
				      VIRQ_TRIGGER_VCPU_HALT);

	return err;
}

error_t
vcpu_handle_vcpu_unbind_virq(thread_t *vcpu)
{
	vic_unbind_sync(&vcpu->vcpu_halt_virq_src);

	return OK;
}

irq_trigger_result_t
vcpu_handle_virq_set_mode(void)
{
	return irq_trigger_result_ok(IRQ_TRIGGER_EDGE_RISING);
}
