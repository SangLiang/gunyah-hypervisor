// Copyright © Qualcomm Technologies, Inc. and/or its subsidiaries.
//
// SPDX-License-Identifier: BSD-3-Clause

// 允许 rootvm_init() 调用 partition_get_root()。
#define ROOTVM_INIT 1

#include <assert.h>
#include <hyptypes.h>
#include <string.h>

#include <attributes.h>
#include <bitmap.h>
#include <cpulocal.h>
#include <cspace.h>
#include <memdb.h>
#include <object.h>
#include <panic.h>
#include <partition.h>
#include <partition_alloc.h>
#include <partition_init.h>
#include <platform_cpu.h>
#include <platform_mem.h>
#include <qcbor.h>
#include <scheduler.h>
#include <spinlock.h>
#include <thread.h>
#include <util.h>
#include <vcpu.h>

#if defined(PLATFORM_ENABLE_SYSTEM_SUSPEND) && PLATFORM_ENABLE_SYSTEM_SUSPEND
#include "vpm_base.h"
#endif // PLATFORM_ENABLE_SYSTEM_SUSPEND

#include <events/object.h>
#include <events/rootvm.h>

#include <asm/cache.h>
#include <asm/cpu.h>

#include "boot_init.h"
#include "event_handlers.h"

// FIXME: remove when we have a device tree where to read it from
// dummy value.
#define MAX_CAPS 4096

static void
copy_rm_env_data_to_rootvm_mem(hyp_env_data_t		hyp_env,
			       const rm_env_data_hdr_t *rm_env_data,
			       rt_env_data_t *crt_env, uint32_t env_data_size)
{
	paddr_t hyp_env_phys = hyp_env.env_ipa - hyp_env.me_ipa_base +
			       PLATFORM_ROOTVM_LMA_BASE;
	assert(util_is_baligned(hyp_env_phys, PGTABLE_VM_PAGE_SIZE));

	void *va = partition_phys_map(hyp_env_phys, env_data_size);
	partition_phys_access_enable(va);

	(void)memscpy(va, env_data_size, (void *)crt_env,
		      (rm_env_data->data_payload_size + sizeof(*rm_env_data) +
		       sizeof(*crt_env)));
	CACHE_CLEAN_RANGE((rm_env_data_hdr_t *)va,
			  (rm_env_data->data_payload_size +
			   sizeof(*rm_env_data) + sizeof(*crt_env)));

	partition_phys_access_disable(va);
	partition_phys_unmap(va, hyp_env_phys, env_data_size);
}

static void
rootvm_close_env_data(qcbor_enc_ctxt_t	*qcbor_enc_ctxt,
		      rm_env_data_hdr_t *rm_env_data)
{
	qcbor_err_t	    cb_err;
	const_useful_buff_t payload_out_buff;

	payload_out_buff.ptr = NULL;
	payload_out_buff.len = 0;

	cb_err = QCBOREncode_Finish(qcbor_enc_ctxt, &payload_out_buff);

	if (cb_err != QCBOR_SUCCESS) {
		panic("Env data encoding error, increase the buffer size");
	}

	rm_env_data->data_payload_size = (uint32_t)payload_out_buff.len;
}

typedef struct {
	hyp_env_data_t	   hyp_env;
	qcbor_enc_ctxt_t  *qcbor_enc_ctxt;
	rm_env_data_hdr_t *rm_env_data;
	rt_env_data_t	  *crt_env;
} rootvm_init_env_info;

static rootvm_init_env_info
rootvm_init_env_data(partition_t *root_partition, uint32_t env_data_size)
{
	void_ptr_result_t alloc_ret;
	hyp_env_data_t	  hyp_env; // Local on stack used as context
	qcbor_enc_ctxt_t *qcbor_enc_ctxt;

	rm_env_data_hdr_t *rm_env_data;
	rt_env_data_t	  *crt_env;
	size_t		   remaining_size;

	alloc_ret = partition_alloc(root_partition, env_data_size,
				    PGTABLE_VM_PAGE_SIZE);
	if (alloc_ret.e != OK) {
		panic("Allocate env_data failed");
	}
	crt_env = (rt_env_data_t *)alloc_ret.r;
	(void)memset_s(crt_env, env_data_size, 0, env_data_size);

	alloc_ret = partition_alloc(root_partition, sizeof(*qcbor_enc_ctxt),
				    alignof(*qcbor_enc_ctxt));
	if (alloc_ret.e != OK) {
		panic("Allocate cbor_ctxt failed");
	}

	qcbor_enc_ctxt = (qcbor_enc_ctxt_t *)alloc_ret.r;

	(void)memset_s(qcbor_enc_ctxt, sizeof(*qcbor_enc_ctxt), 0,
		       sizeof(*qcbor_enc_ctxt));

	(void)memset_s(&hyp_env, sizeof(hyp_env), 0, sizeof(hyp_env));

	hyp_env.env_data_size = env_data_size;
	remaining_size	      = env_data_size;

	crt_env->signature = ROOTVM_ENV_DATA_SIGNATURE;
	crt_env->version   = 1;

	size_t rm_config_offset =
		util_balign_up(sizeof(*crt_env), alignof(*rm_env_data));
	assert(remaining_size >= (rm_config_offset + sizeof(*rm_env_data)));

	remaining_size -= rm_config_offset;
	rm_env_data =
		(rm_env_data_hdr_t *)((uintptr_t)crt_env + rm_config_offset);

	crt_env->rm_config_offset = rm_config_offset;
	crt_env->rm_config_size	  = remaining_size;

	rm_env_data->signature		 = RM_ENV_DATA_SIGNATURE;
	rm_env_data->version		 = 1;
	rm_env_data->data_payload_offset = (uint32_t)sizeof(*rm_env_data);
	rm_env_data->data_payload_size	 = 0U;

	remaining_size -= sizeof(*rm_env_data);

	useful_buff_t qcbor_data_buff;
	qcbor_data_buff.ptr =
		(((uint8_t *)rm_env_data) + rm_env_data->data_payload_offset);
	qcbor_data_buff.len = remaining_size;

	QCBOREncode_Init(qcbor_enc_ctxt, qcbor_data_buff);

	return (rootvm_init_env_info){
		.crt_env	= crt_env,
		.hyp_env	= hyp_env,
		.qcbor_enc_ctxt = qcbor_enc_ctxt,
		.rm_env_data	= rm_env_data,
	};
}

// boot_hypervisor_start 阶段最后执行的 handler（priority last）。
//
// 职责：创建 RootVM 的 cspace、VCPU 线程与环境数据，广播 rootvm_init 让各
// 模块加载 GPKG / 建 Stage-2 等，最后 vcpu_poweron，把 RootVM 放进调度器。
// 本函数返回时 guest 尚未开始跑；要等 boot_cpu_start 解锁后 idle yield。
//
// 约束：
//   - 前面 RAM、idle、hyp 服务必须已齐（所以是 last）；
//   - 调用期间抢占关闭（rootvm.ev: require_preempt_disabled）。
//
// 步骤概览：
//   1. 选定 RootVM 绑定的物理核，给 root partition 补堆；
//   2. 创建并激活 root cspace；
//   3. 分配 root 线程并配成 VCPU，挂上 cspace；
//   4. 给 cspace / partition / thread 建 master cap，写入 CBOR 环境数据；
//   5. 走 memdb 收集 root partition 可用内存范围；
//   6. 广播 rootvm_init（加载 Runtime/RM、建地址空间等）；
//   7. 把环境数据拷进 RootVM 内存，activate 线程并 vcpu_poweron。
void NOINLINE
rootvm_init(void)
{
	static_assert(SCHEDULER_NUM_PRIORITIES >= (priority_t)3U,
		      "unexpected scheduler configuration");
	static_assert(ROOTVM_PRIORITY <= VCPU_MAX_PRIORITY,
		      "unexpected scheduler configuration");

	// 默认绑在当前核（冷启动时即 Boot CPU）。
	cpu_index_t boot_cpu_idx = cpulocal_get_index();

#if defined(PLATFORM_ROOTVM_AFFINITY) && !defined(ROOTVM_ALWAYS_ON_BOOT_CORE)
	// 平台指定了 RootVM 亲和性且该核可用时，改绑到那颗核。
	if (platform_cpu_functional(PLATFORM_ROOTVM_AFFINITY)) {
		boot_cpu_idx = PLATFORM_ROOTVM_AFFINITY;
	}
#endif

	thread_create_t params = {
		.scheduler_affinity	  = boot_cpu_idx,
		.scheduler_affinity_valid = true,
		.scheduler_priority	  = ROOTVM_PRIORITY,
		.scheduler_priority_valid = true,
	};

	partition_t *root_partition = partition_get_root();

	assert(root_partition != NULL);

	// 给 root partition 补堆（平台相关：额外 heap / trace 缓冲等）。
	platform_add_root_heap(root_partition);

	// 为 root partition 创建 capability 空间。
	cspace_create_t cs_params = { NULL };

	cspace_ptr_result_t cspace_ret =
		partition_allocate_cspace(root_partition, cs_params);
	if (cspace_ret.e != OK) {
		goto cspace_fail;
	}
	cspace_t *root_cspace = cspace_ret.r;

	spinlock_acquire_nopreempt(&root_cspace->header.lock);
	if (cspace_configure(root_cspace, MAX_CAPS) != OK) {
		spinlock_release_nopreempt(&root_cspace->header.lock);
		goto cspace_fail;
	}
	spinlock_release_nopreempt(&root_cspace->header.lock);

	if (object_activate_cspace(root_cspace) != OK) {
		goto cspace_fail;
	}

	// 让各模块补全线程创建默认参数（kind 等），再分配 root 线程。
	trigger_object_get_defaults_thread_event(&params);

	// 分配并配置 root 线程（此时还不能跑：LIFECYCLE / VCPU_OFF 仍挡着）。
	thread_ptr_result_t thd_ret =
		partition_allocate_thread(root_partition, params);
	if (thd_ret.e != OK) {
		panic("Error allocating root thread");
	}
	thread_t *root_thread = (thread_t *)thd_ret.r;

	vcpu_option_flags_t vcpu_options = vcpu_option_flags_default();

	vcpu_option_flags_set_critical(&vcpu_options, true);

	if (vcpu_configure(root_thread, vcpu_options) != OK) {
		panic("Error configuring vcpu");
	}

	// 把 root cspace 挂到 root 线程上（VCPU 走 hypercall 必须有 cspace）。
	if (cspace_attach_thread(root_cspace, root_thread) != OK) {
		panic("Error attaching cspace to root thread");
	}

	// 给 root cspace 发一张指向自己的 master cap，RM 侧用 CapID 引用它。
	object_ptr_t obj_ptr;

	obj_ptr.cspace		  = root_cspace;
	cap_id_result_t capid_ret = cspace_create_master_cap(
		root_cspace, obj_ptr, OBJECT_TYPE_CSPACE);
	if (capid_ret.e != OK) {
		goto cspace_fail;
	}

	uint32_t env_data_size = QCBOR_ENV_CONFIG_SIZE;

	rootvm_init_env_info info =
		rootvm_init_env_data(root_partition, env_data_size);

	hyp_env_data_t	  hyp_env	 = info.hyp_env;
	qcbor_enc_ctxt_t *qcbor_enc_ctxt = info.qcbor_enc_ctxt;

	rm_env_data_hdr_t *rm_env_data = info.rm_env_data;
	rt_env_data_t	  *crt_env     = info.crt_env;

	QCBOREncode_OpenMap(qcbor_enc_ctxt);

	QCBOREncode_AddUInt64ToMap(qcbor_enc_ctxt, "cspace_capid", capid_ret.r);

	// 多拿一次引用：删掉 master cap 时不要误把 partition 对象销毁。
	root_partition = object_get_partition_additional(root_partition);

	// 为 root partition 和 root 线程创建 master cap。
	obj_ptr.partition = root_partition;
	capid_ret	  = cspace_create_master_cap(root_cspace, obj_ptr,
						     OBJECT_TYPE_PARTITION);
	if (capid_ret.e != OK) {
		panic("Error creating root partition cap");
	}
	QCBOREncode_AddUInt64ToMap(qcbor_enc_ctxt, "partition_capid",
				   capid_ret.r);

	obj_ptr.thread = root_thread;
	capid_ret      = cspace_create_master_cap(root_cspace, obj_ptr,
						  OBJECT_TYPE_THREAD);
	if (capid_ret.e != OK) {
		panic("Error creating root partition cap");
	}
	QCBOREncode_AddUInt64ToMap(qcbor_enc_ctxt, "vcpu_capid", capid_ret.r);
	crt_env->vcpu_capid = capid_ret.r;

	// 遍历 memdb，把 root partition 的可用内存范围写入 rm_env_data。
	if (boot_add_free_range((uintptr_t)root_partition, MEMDB_TYPE_PARTITION,
				qcbor_enc_ctxt) != OK) {
		panic("Error doing the memory database walk");
	}

	// 广播 rootvm_init：加载 GPKG、建 Stage-2 地址空间、填 hyp_env 等。
	trigger_rootvm_init_event(root_partition, root_thread, root_cspace,
				  &hyp_env, qcbor_enc_ctxt);

	QCBOREncode_CloseMap(qcbor_enc_ctxt);

	rootvm_close_env_data(qcbor_enc_ctxt, rm_env_data);

	crt_env->runtime_ipa   = hyp_env.runtime_ipa;
	crt_env->app_ipa       = hyp_env.app_ipa;
	crt_env->app_heap_ipa  = hyp_env.app_heap_ipa;
	crt_env->app_heap_size = hyp_env.app_heap_size;
	crt_env->timer_freq    = hyp_env.timer_freq;
	crt_env->gicd_base     = hyp_env.gicd_base;
	crt_env->gicr_base     = hyp_env.gicr_base;

	// 把 rm_env_data 拷进 RootVM 内存（guest IPA 对应的物理页）。
	copy_rm_env_data_to_rootvm_mem(hyp_env, rm_env_data, crt_env,
				       env_data_size);

	// 激活 root 线程对象：清掉 LIFECYCLE，VCPU_OFF 仍在，还不能进就绪表。
	if (object_activate_thread(root_thread) != OK) {
		panic("Error activating root thread");
	}

	trigger_rootvm_init_late_event(root_partition, root_thread, root_cspace,
				       &hyp_env);

	vcpu_power_req_flags_t power_flags = vcpu_power_req_flags_default();

	scheduler_lock_nopreempt(root_thread);
	// FIXME: 最终应通过 DTB 传入；目前直接把 rm_env_data 的 IPA 放进 X0。
	bool_result_t power_ret =
		vcpu_poweron(root_thread, vmaddr_result_ok(hyp_env.entry_ipa),
			     register_result_ok(hyp_env.env_ipa), power_flags);
	if (power_ret.e != OK) {
		panic("Error vcpu poweron");
	}

	// 允许其他模块在 RootVM 创建完成后做清理。
	trigger_rootvm_started_event(root_thread);
	scheduler_unlock_nopreempt(root_thread);
	partition_free(root_partition, crt_env, env_data_size);
	rm_env_data = NULL;

	return;

cspace_fail:
	panic("Error creating root cspace cap");
}
