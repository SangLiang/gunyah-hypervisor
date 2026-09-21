# Gunyah EL2 层 virtio 实现细分析

> 调研对象：本地 `D:\work\gunyah-hypervisor`（Qualcomm Gunyah，BSD-3）
> 范围：`hyp/vm/virtio*` 在 EL2（hypervisor）层为 virtio 做的全部工作
>
> 证据来源（均已逐文件阅读）：
> - `hyp/vm/virtio/src/frontend.c`（617 行，virtio_t 状态机 + Frontend API）
> - `hyp/vm/virtio/src/backend.c`（Backend API，RM VM 经 hypercall 调）
> - `hyp/interfaces/virtio/include/virtio.h`（frontend/backend API 契约）
> - `hyp/vm/virtio_mmio/src/{virtio_mmio.c,vdevice.c,hypercalls.c}`
> - `hyp/vm/virtio_mmio/include/virtio_mmio.h`
> - `hyp/vm/virtio_pci/src/virtio_pci.c`（382 行，PCI transport）
> - `hyp/vm/virtio_virtq/src/virtio_virtq.c`（201 行，virtqueue 辅助）
> - `hyp/vm/virtio_backend/src/virtio_backend.c`（223 行，backend 桥对象）

---

## 1. 概述：EL2 virtio 是一套完整子系统，6 个部件

Gunyah 在 EL2 把 virtio 当**完整子系统**实现，不是"转发给用户态"。6 个部件分工：

```text
hyp/vm/
+--- virtio/             [1] virtio_t state machine (core)
|    +-- frontend.c      Frontend API: status / features / queue / config
|    +-- backend.c       Backend API: RM VM pushes device state via hypercall
+--- virtio_mmio/        [2] MMIO transport emulation
|    +-- virtio_mmio.c   register layout / IRQ / generation
|    +-- vdevice.c       guest MMIO write trap dispatch (trap entry)
|    +-- hypercalls.c    RM VM bind/unbind virq hypercall handlers
+--- virtio_pci/         [3] PCI transport emulation (BAR / cap / MSI-X)
+--- virtio_virtq/       [4] virtqueue descriptor chain helper
+--- virtio_backend/     [5] EL2-side backend object (frontend <-> RM VM bridge)

                         [6] Hypercall handlers (in each module's hypercalls.c)
                             RM VM -> EL2, capability-checked
```

**职责边界**：EL2 负责 virtio **协议 + transport + 状态 + 队列机制**；RM VM（EL1）只负责**设备逻辑**（拿到请求做什么 I/O）。本报告逐一分析 EL2 这 6 个部件。

---

## 2. virtio_t 状态机（`virtio/src/frontend.c` + `backend.c`）

EL2 为每个 virtio 设备持有 `virtio_t` 结构，是协议状态的中枢。

### 2.1 virtio_t 的字段（`virtio_configure()` 初始化）

```text
virtio_t {
  transport_type       MMIO or PCI
  backend_type          backend implementation type
  device_type           NETWORK / BLOCK / CONSOLE / BALLOON / MEMORY /
                       GPU / INPUT / SOCKET / IOMMU
  status               ACKNOWLEDGE / DRIVER / FEATURES_OK / DRIVER_OK /
                       FAILED / DEVICE_NEEDS_RESET
  dev_feat[]           device feature words (banked)
  drv_feat[]           driver feature words (banked)
  features_ok_set      features negotiation state
  vqs                   number of virtqueues
  queue_info[]         per-VQ: size / ready / desc / drv / dev ring addr
  queue_size_max[]     per-VQ max size
  config_offset/size   config space location
  config_cache_me       memextent backing the config cache
  config_range          virtual range in hyp_aspace for config cache
  config_cache          pointer to mapped config space
  config_gen            config generation counter (for non-atomic reads)
  config_update         config update in-progress flag
  reset_request         reset state
  per_queue_notify      per-VQ notify feature (auto-true if vqs==1)
  shutdown             shutdown flag
  status_lock          spinlock for status
  partition             owning partition
}
```

### 2.2 状态机（`virtio_write_status`，frontend.c:269-368）

EL2 严格实现 virtio 状态机，按规范递进：

```text
status write (guest -> EL2 frontend)
   |
   +-- write 0 => reset request
   |     set reset_request, set DEVICE_NEEDS_RESET
   |     clear all queue_info[].ready
   |     trigger reset_requested event to backend
   |
   +-- set ACKNOWLEDGE => OK (first step)
   |
   +-- set DRIVER => require ACKNOWLEDGE first, else ERROR_ARGUMENT_INVALID
   |
   +-- set FEATURES_OK => require DRIVER first, else reject
   |     set features_ok_set = true
   |
   +-- set DRIVER_OK => require FEATURES_OK first, else reject
   |     trigger driver_ok event to backend
   |
   +-- set FAILED => trigger failed event to backend
   |
   +-- set DEVICE_NEEDS_RESET (by frontend) => ERROR_DENIED (only backend sets)
```

复位是阻塞的——`virtio_read_status()` 会等复位落定再返回（dropping RCU section）。未知位一律拒绝（`virtio_status_is_clean`）。

### 2.3 Frontend API（guest 侧，经 MMIO/PCI trap 调用）

```text
virtio_write_status          status register write (state machine)
virtio_read_status          read status (may block on reset)
virtio_get_dev_features     read device features (banked by feature_sel)
virtio_set_drv_features     write driver features (banked by drv_feat_sel)
virtio_get_queue_size_max   read a queue's max size
virtio_get_queue_info_ptr   get queue config struct (size/ready/desc/drv/dev)
virtio_queue_notify         notify backend that a queue has buffers
virtio_device_config_read   read device config space (8/16/32-bit)
virtio_device_config_write  write device config space
virtio_device_config_read_cached  cached config read (from config_cache)
virtio_get_generation       read config generation counter
virtio_supports_per_queue_notify  query per-VQ notify feature
```

config 读写用 `memscpy` 按访问大小（8/16/32 位）安全拷贝，校验对齐和边界。`virtio_get_generation` 在 `config_update` 为真时自增（供 guest 检测非原子 config 读）。

### 2.4 Backend API（RM VM 侧，经 hypercall 调用）

`backend.c` 暴露给 RM VM 的接口，RM VM 用这些把设备侧状态推进 EL2：

```text
virtio_set_dev_features      set device feature words (requires pending reset)
virtio_set_queue_size_max    set per-VQ max size (requires pending reset)
virtio_get_drv_features      fetch driver-requested features
virtio_ack_features_ok       acknowledge features (trigger event)
virtio_get_queue_info        fetch queue info set by driver
virtio_config_update_begin   begin non-atomic config update (set update flag)
virtio_config_update_end     end config update (increment gen, notify frontend)
virtio_queue_ready           notify frontend a queue is ready
virtio_needs_reset           signal frontend that backend needs reset
virtio_reset_complete        complete a requested reset
```

`virtio_config_update_begin/end` 配合 generation 计数器：begin 时置 `config_update=true`（generation 读会自增），end 时 `config_gen++`、清 `config_update`、通知 frontend 触发 config 更新 IRQ。

---

## 3. MMIO transport 仿真（`virtio_mmio/`）

### 3.1 寄存器布局与初始化（`virtio_mmio.c`）

`virtio_mmio_handle_virtio_startup()`（virtio_mmio.c:27-93）：

- 校验 `config_offset ≥ offsetof(virtio_mmio_regs_t, device_config)`（即 ≥ 0x100），**这样能在前面放只读 mirror 的 common 寄存器**
- 校验 `config_size` 至少容下 device_config 区
- `virtio_mmio->regs` 指向 config cache（映射在 hyp_aspace）
- 写初始 `status`、`dev_id`（若 device_type 有效）
- 把 config cache memextent 注册成 `VDEVICE_TYPE_VIRTIO_MMIO` 设备（`vdevice_attach_phys`）——这样 guest 访问这个区会 trap 进 EL2

```text
virtio_mmio_regs layout (config cache, one page):
offset 0x000
  +-------------------------  read-only mirror of common regs (guest reads)
  |  magic / version / dev_id / vendor
  |  dev_feat / drv_feat / queue_sel / queue_num_max / queue_ready
  |  queue_desc/drv/dev low+high / queue_notify
  |  interrupt_status / interrupt_ack / status / config_gen
offset 0x100 (device_config)
  +-------------------------  device-specific config space
  |  (per device_type: blk config, net config, ...)
```

### 3.2 中断与 generation（`virtio_mmio.c`）

- `virtio_mmio_assert_irq()`（virtio_mmio.c:147-178）：原子 OR `interrupt_status` 位（queue_ready / config_update），非空就 `virq_assert`。平台不支持原子设备属性更新时退化为 `interrupt_lock` spinlock 保护
- `virtio_mmio_update_generation()`：config_update_begin/end 时更新 `config_gen` 寄存器
- `virtio_mmio_frontend_handle_virq_check_pending()`：`interrupt_status` 非空时 virq 保持拉起
- `virtio_mmio_frontend_bind/unbind_virq()`：把 frontend IRQ source 绑到 VIC，绑时若有 pending 立即拉起

### 3.3 guest trap 入口（`vdevice.c`，核心）

`virtio_mmio_handle_vdevice_access()`（vdevice.c:361-390）是 **guest MMIO 写 trap 的总入口**：

```text
guest MMIO access (Stage-2 trap)
   |
   +-- is it a read? => VCPU_TRAP_RESULT_UNHANDLED
   |     (reads go through the read-only mirror mapping, do NOT trap)
   |
   +-- is it a write?
   |     check access_allowed: word(32) always OK,
   |     byte only OK in device_config region
   |     |
   |     +-- access not allowed => VCPU_TRAP_RESULT_FAULT
   |     +-- virtio_mmio_vdevice_write(offset, val, size)
   |           |
   |           +-- OK => VCPU_TRAP_RESULT_EMULATED
   |           +-- error => VCPU_TRAP_RESULT_FAULT
```

**只 trap 写、读走只读 mirror**——这是性能优化，guest 读 common 寄存器不进 EL2。

`virtio_mmio_vdevice_write()`（vdevice.c:280-359）按 offset 大 switch 派发：

| 寄存器 | 处理 |
|---|---|
| `dev_feat_sel` | 选 feature 字，读 `dev_feat[sel]` 进 banked 寄存器 |
| `drv_feat_sel` | 选 driver feature 字 |
| `drv_feat` | 写 driver feature（`virtio_set_drv_features`） |
| `queue_sel` | 选队列，读 `queue_num_max`/`queue_ready` 进 banked 寄存器 |
| `queue_notify` | `virtio_queue_notify` 通知 backend |
| `interrupt_ack` | 清 `interrupt_status` 对应位 |
| `status` | `virtio_write_status`（状态机/复位，会阻塞，drop RCU） |
| `queue_num` | 写队列 size（min with max） |
| `queue_ready` | 置/清队列 ready |
| `queue_desc_low/high` | 写描述符环地址（低 32 / 高 32 位拼成 64 位） |
| `queue_drv_low/high` | 写 driver 环地址 |
| `queue_dev_low/high` | 写 device 环地址 |
| `device_config` 区 | `virtio_device_config_write` |

**banked 寄存器模型**：`queue_sel` / `dev_feat_sel` / `drv_feat_sel` 是选择器，guest 先写选择器再读写 banked 寄存器看到对应队列/feature 的值——这是 virtio-mmio 规范的语义。

### 3.4 RM VM 的 hypercall（`hypercalls.c`）

```text
hypercall_virtio_mmio_frontend_bind_virq(backend_cap, vic_cap, virq)
  - cspace_lookup_virtio_backend (CAP_RIGHTS_VIRTIO_BACKEND_BIND_MMIO_FRONTEND_VIRQ)
  - check transport_type == MMIO
  - cspace_lookup_vic (CAP_RIGHTS_VIC_BIND_SOURCE)
  - virtio_mmio_frontend_bind_virq

hypercall_virtio_mmio_frontend_unbind_virq(backend_cap)
  - same capability checks
  - virtio_mmio_frontend_unbind_virq
```

全部 capability 校验（`cspace_lookup_*`），transport_type 校验。

---

## 4. PCI transport 仿真（`virtio_pci/src/virtio_pci.c`）

`virtio_pci_handle_virtio_startup()`（virtio_pci.c:75-222）构造一个完整 virtio-pci 函数。

### 4.1 设备标识（`virtio_pci_set_device_id`，virtio_pci.c:25-73）

```text
vendor_id       = VIRTIO_PCI_VENDOR_ID
product_id      = device_type + VIRTIO_PCI_DEVICE_ID_BASE
device_revision = 1   (virtio 1.1 non-transitional)
subsys_vendor   = PCI_VENDOR_ID_QUALCOMM
subsys_product  = 0x6001  ("Gunyah Virtio PCI frontend")

device_class/subclass by device_type:
  NETWORK  -> CLASS_NETWORK / SUBCLASS_NETWORK_ETHERNET
  BLOCK    -> CLASS_MASS_STORAGE / SUBCLASS_OTHER
  CONSOLE  -> CLASS_CONSOLE / SUBCLASS_OTHER
  BALLOON  -> CLASS_MEMORY / SUBCLASS_MEMORY_RAM
  MEMORY   -> CLASS_MEMORY / SUBCLASS_MEMORY_RAM
  GPU      -> CLASS_DISPLAY / SUBCLASS_DISPLAY_3D
  INPUT    -> CLASS_INPUT / SUBCLASS_OTHER
  SOCKET   -> CLASS_NETWORK / SUBCLASS_NETWORK_OTHER
  IOMMU    -> CLASS_SYSTEM / SUBCLASS_SYSTEM_IOMMU
```

### 4.2 BAR 与 Capability

```text
BAR layout:
  VIRTIO_PCI_BAR_COMMON     common cfg + queue_notify + interrupt_status
                           (access_type = TRAPPED, 32-bit non-prefetch)
  VIRTIO_PCI_BAR_DEVCFG    device config space
                           (TRAPPED_W if config_cache present, else TRAPPED)
  VIRTIO_PCI_BAR_MSIX_TABLE  MSI-X Table (TRAPPED)
  VIRTIO_PCI_BAR_MSIX_PBA    MSI-X PBA (TRAPPED)

Capability list (vendor cap = PCI_CAPABILITY_ID_VENDOR):
  VIRTIO_PCI_CAP_COMMON_CFG   bar=COMMON,  offset=0,    len=0x38
  VIRTIO_PCI_CAP_NOTIFY_CFG   bar=COMMON,  offset=queue_notify, len=4
  VIRTIO_PCI_CAP_ISR_CFG      bar=COMMON,  offset=interrupt_status, len=1
  VIRTIO_PCI_CAP_DEVICE_CFG   bar=DEVCFG, offset=config_offset, len=config_size
  VIRTIO_PCI_CAP_PCI_CFG      (virtio 1.1 BAR-access cap, the only writable cap)

MSI-X capability (if per_queue_notify):
  vector_count = min(vqs + 1, 2048)
  table_bar = MSIX_TABLE, pba_bar = MSIX_PBA
  vpci_msix_init()
```

### 4.3 中断（`virtio_pci_assert_irq`）

```text
MSI-X enabled (per_queue_notify && vpci_msix_is_enabled):
  config_update_end -> vpci_msix_send(config_msix_vector)
  queue_ready       -> vpci_msix_send(queue_msix_vector[vq])

MSI-X not enabled:
  vpci_irq_assert (legacy INTx, IRQA pin)
  based on interrupt_status atomic union
```

### 4.4 绑定与未实现项

- `virtio_pci_handle_vpci_attach()`：把 virtio_pci 设备绑到 VPCI 总线（cap 查 `CAP_RIGHTS_VIRTIO_BACKEND_BIND_VPCI`，校验 transport_type == PCI，`vpci_bind_device`）
- `virtio_pci_handle_virtio_ack_features_ok()`：**返回 `ERROR_UNIMPLEMENTED`**（同步 features_ok 未做，TODO）
- `virtio_pci_handle_virtio_reset_complete()`：把所有队列 size 重置为 max

---

## 5. virtqueue 辅助（`virtio_virtq/src/virtio_virtq.c`）

这是给 virtio-iommu backend 用的队列操作辅助（不是通用 virtqueue，是 backend 处理描述符的工具）。

### 5.1 读描述符链（`virtio_virtq_read_desc_chain`，virtio_virtq.c:16-114）

```text
walk descriptor chain from first_desc_idx:
  for each desc:
    useraccess_copy_from_guest_ipa(addrspace, &desc_entry,
        q->desc + sizeof(desc)*idx, sizeof(desc), ...)   <- safe guest mem access

    if (!write flag && !found_writable):
        read-only request buffer:
        useraccess_copy_from_guest_ipa(... desc->addr, desc->len ...)
        accumulate req_size

    else if (write flag):
        write-only reply buffer:
        mark reply_start, accumulate reply_space

    else:
        read-only after first write-only => break (split VQ rule)

    follow next pointer if VIRTQ_DESC_F_NEXT
    stop at array limit or no NEXT
```

**EL2 用 `useraccess_copy_from/to_guest_ipa` 安全访问 guest 内存**，不直接解引用 guest 地址（防 Stage-2 翻译异常）。

### 5.2 写回复与 used 环（`virtio_virtq_write_reply`，virtio_virtq.c:116-200）

```text
write reply to write-only descs:
  for desc in [reply_start, count):
    useraccess_copy_to_guest_ipa(... desc->addr, desc->len, reply_buf+used ...)
    used_bytes += written

update used ring:
  uidx = curr_dev_idx % size
  curr_dev_idx++
  used_entry = { .id = first_desc_idx, .len = used_bytes }
  useraccess_copy_to_guest_ipa(... used ring @ uidx ...)

  atomic_thread_fence(release)   <- order used-buffer writes before used head

  used = { .idx = curr_dev_idx }
  useraccess_copy_to_guest_ipa(... used ring idx field ...)
```

**release fence 在更新 used head idx 之前**——匹配 frontend driver 读 used 环前的 acquire fence，保证 guest 看到数据后才看到 idx 前进。

---

## 6. virtio_backend 桥对象（`virtio_backend/src/virtio_backend.c`）

EL2 侧的**桥对象**，把 frontend 事件转成给 RM VM 的通知。

### 6.1 通知机制（`virtio_backend_notify`，virtio_backend.c:76-89）

```text
reason bitmap (atomic union, memory_order_release):
  new_buffer      (queue_notify: driver posted buffers)
  reset_request   (frontend requested reset)
  driver_ok       (driver set DRIVER_OK)
  failed          (driver set FAILED)

if any new reason bit set:
  virq_assert to RM VM
```

### 6.2 frontend→backend 事件转发

| frontend 事件 | backend 处理 | 通知 reason |
|---|---|---|
| `queue_notify` | 置 `vqs_bitmap` 对应位 | new_buffer |
| `reset_requested` | 通知 reset_request；非同步复位时立即 `virtio_reset_complete`（异步） | reset_request |
| `driver_ok` | 通知 | driver_ok |
| `failed` | 通知 | failed |
| `device_config_write` | 触发 backend config write 事件；`ignore_config_writes` 标志时静默丢弃 | （事件，非 reason） |

### 6.3 其它

- `virtio_backend_check_block_features`：隐藏 `VIRTIO_BLK_F_CONFIG_WCE`（需要同步 config 写，不安全）
- `virtio_backend_handle_virq_check_pending`：reason 非空时 virq 保持拉起
- 生命周期：`activate`/`deactivate`/`cleanup` 经 event 触发；`unwind_object_activate` 回滚

---

## 7. Hypercall 处理（各模块 `hypercalls.c`）

RM VM → EL2 的所有操作都经 hypercall，全部 **capability 校验**：

```text
RM VM -> hypercall (EL2)
  |
  +-- cspace_lookup_virtio_backend(cspace, cap, CAP_RIGHTS_*)
  |     (capability check, transport_type check)
  |
  +-- dispatch to virtio_mmio / virtio_pci / virtio_backend handler
  |
  +-- object_put_virtio_backend (release reference)
```

`virtio_mmio/hypercalls.c` 的两个：`bind_virq` / `unbind_virq`（带 `CAP_RIGHTS_VIRTIO_BACKEND_BIND_MMIO_FRONTEND_VIRQ`）。`virtio_pci` 的 `handle_vpci_attach`（带 `CAP_RIGHTS_VIRTIO_BACKEND_BIND_VPCI`）。

---

## 8. 完整数据流：一次 guest queue_notify 写

把 6 个部件串起来，guest 写 `queue_notify` 通知 backend 有新 buffer：

```text
Guest (EL1)              EL2 (Hypervisor)                    RM VM (EL1)
   |                        |                                   |
   | 1. write queue_notify  |                                   |
   |----------------------> virtio_mmio_handle_vdevice_access   |
   |   (Stage-2 trap,       | (vdevice.c, write-only)           |
   |    write only)         |                                   |
   |                        | 2. virtio_mmio_vdevice_write      |
   |                        |    -> virtio_queue_notify          |
   |                        |    -> trigger_virtio_queue_notify   |
   |                        |       _event (frontend.c)          |
   |                        | 3. virtio_backend_handle_          |
   |                        |    virtio_queue_notify (backend.c) |
   |                        |    set vqs_bitmap bit,             |
   |                        |    virtio_backend_notify           |
   |                        |    (reason = new_buffer)          |
   |                        | 4. virq_assert to RM VM           |
   |                        |---------------------------------->|
   |                        |                              5. RM VM receives IRQ,
   |                        |                                 calls Backend API
   |                        |                                 hypercall to fetch
   |                        |                                 queue_info / virtq
   |                        |<----------------------------------|
   |                        | 6. EL2 uses useraccess to copy    |
   |                        |    descriptors from guest IPA,    |
   |                        |    process, write reply + used ring
   |                        | 7. virtio_mmio_assert_irq         |
   |                        |    (set interrupt_status)          |
   |<--- 8. vIRQ injected --|                                   |
   |                        |                                   |
```

要点：
- guest 写 trap 进 EL2 vdevice handler（步骤 1-2）
- frontend 经 event 通知 backend 桥对象（步骤 3）
- backend 桥对象经 virq 通知 RM VM（步骤 4）
- RM VM 经 hypercall 取队列信息、用 useraccess 处理描述符（步骤 5-6）
- EL2 给 guest 注入完成中断（步骤 7-8）

**整条链路里 RM VM 不直接见 guest**，全经 EL2 中转（event + virq + useraccess）。

---

## 9. 结论

Gunyah 在 EL2 层为 virtio 实现的是**完整子系统**，6 个部件覆盖：

| 部件 | 职责 | 关键实现 |
|---|---|---|
| virtio_t 状态机 | 协议协商（status/features/queue/config） | 状态机递进校验、generation 计数器、status_lock + RCU |
| MMIO transport | virtio-mmio 寄存器仿真 + trap 入口 | 只 trap 写（读走只读 mirror）、banked 寄存器、大 switch 派发 |
| PCI transport | virtio-pci 函数仿真 | BAR/cap 链/MSI-X（per-VQ vector）、device class 映射 |
| virtqueue 辅助 | 描述符链读写 | useraccess 安全访问 guest IPA、split VQ 规则、release fence |
| virtio_backend 桥 | frontend↔RM VM 事件转发 | reason 位图 + virq 通知、异步复位 |
| Hypercall 处理 | RM VM→EL2 接口 | 全部 capability 校验 + transport_type 校验 |

**RM VM 只负责设备逻辑**（拿到请求做什么 I/O），**virtio 协议、transport、状态机、队列机制、guest 内存安全访问全在 EL2**。这正是 Gunyah"微kernel式分层"的体现——关键在 EL2，非关键（设备逻辑）委派给 RM VM。

> 注：本报告基于本地 C 版 Gunyah 源码逐文件阅读。`virtio_mmio_regs_t` 的完整字段定义、`virtio_t` 的完整结构定义在生成的容器头文件（`hypcontainers.h` 等）中，本报告字段表是从 `virtio_configure()` 初始化和各处字段访问反推的，非直接抄自结构体定义。
