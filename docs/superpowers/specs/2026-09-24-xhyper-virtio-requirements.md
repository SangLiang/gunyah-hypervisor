# XHyper EL2 Virtio 框架需求文档

> **本文档独立交付，不依赖其他文档。** 定义"做成什么样子"（需求与约束），
> 实现计划（`docs/superpowers/plans/2026-09-24-xhyper-virtio-implementation.md`）
> 定义"怎么做成这个样子"（任务与步骤）。需求文档后续尽量不调整；实现计划
> 随实施进展可变。

---

## 1. 背景与动机

### 1.1 为什么要在 xhyper EL2 内做 virtio

xhyper 当前的 virtio 路径是"EL2 只做 trap+转发，crosvm 在 Host VM 用户态全套仿真"
（路径 B，phase-d4-2 验证了 virtio-blk）。这条路能交付但有两个根本问题：

- **TCB 大**：依赖一个完整 VMM（crosvm）+ 一个 Host Linux 内核——等于把一整套 OS
  塞进可信计算基，与 Type-1 hypervisor 的最小 TCB 信条冲突
- **延迟高**：每次 guest MMIO 访问都 `trap → exit → crosvm → resume`（4 跳），
  无异步通知快路径，真负载扛不住

Gunyah（Qualcomm 的 C 版 Type-1 hypervisor）选择了另一条路：**EL2 内建 virtio
安全核心**（transport + 状态机 + 桥对象 + virtq + capability 校验，5645 行），
backend 逻辑放 HLOS 用户态进程走 hypercall 合同。TCB 不含完整 VMM，通知走异步 virq。
这条路叫**路径 A**。

本需求即在 xhyper（Rust）里实现路径 A 的 EL2 部分，与路径 B 以 feature-gate 并存。

### 1.2 三段式分层

Gunyah 的 virtio 架构是经典微内核三层在 virtio 场景的投影：

| 层 | 职责 | xhyper 对应 | 本需求覆盖 |
|---|---|---|---|
| **EL2（安全核心）** | transport、状态机、桥对象、virtq、capability 校验 | `hypervisor/virtio/*` 新建 | ✅ Stage 0–1 |
| **Root VM（编排）** | backend 对象生命周期编排：create/configure/bind_virq/activate/cleanup、DT 生成、capability 路由 | xhyper-mgr `virtual_device` 域（休眠待唤醒） | ✅ Stage 2 |
| **HLOS（执行）** | backend 任务执行：拉请求、处理 virtqueue、读写 guest 内存、对接真实驱动栈 | Host 用户态进程 | ❌ Stage 3（后续） |

**关键约束**：Root VM 的"调度"是生命周期编排，不是 runtime CPU 调度——backend 真跑
起来后由 HLOS 自己的内核调度器排。这也是 backend 不能塞进 Root VM 的根本原因
（mgr 是 no_std 裸机，没有驱动栈）。

### 1.3 xhyper 团队的移植路线（组织信号）

xhyper-abi 仓在逐块导入 Gunyah ABI 面（`import futlab vRTC hypercalls`、
`add the virtual ITS hypercalls`），virtio_backend 全家已在快照里；EL2 已相继立起
vRTC、vITS、vSMMUv2 对象——团队路线就是逐对象对标移植 Gunyah，virtio 是名单上的
下一块。

---

## 2. 目标

**Stage 0–2 闭环**：Secondary guest 的 `/dev/hvc0` 双向可交互，全链路不依赖 crosvm、
不依赖 Host Linux 内核改动。

| 阶段 | 交付 | 验收 |
|---|---|---|
| Stage 0 | EL2 MMIO 派发框架泛化（多设备注册表 + 只读 mirror） | `phase-e0-vdevice` PASS + `phase-d4-verify` 零回归 |
| Stage 1 | virtio 框架四部件（model/mmio/backend/virtq）+ 管理面 hypercall | `phase-e1-probe` PASS（guest 标准 virtio_mmio 驱动 probe 到 DRIVER_OK） |
| Stage 2 | virtio-console EL2 内 backend + mgr 编排域唤醒 | `phase-e2-console` PASS（/dev/hvc0 双向交互，不依赖 crosvm） |

---

## 3. 非目标

- **VPCI / virtio-pci**：xhyper-abi 有 `vpci_*` 但 EL2 零实现；Gunyah 自己 pci 路径的
  `virtio_ack_features_ok` 都是 `ERROR_UNIMPLEMENTED`。最大单体工程，有真实多设备/MSI-X
  需求再做。
- **virtio-iommu**：硬依赖 `platform/smmuv3` 物理 SMMU；QEMU virt 无 SMMUv3，无验证
  载体。Gunyah 侧 1600+ 行（自带请求引擎），是 EL2 backend 复杂度上限参照。N80/N90
  Phytium SMMU 或 T11206 展锐 SMMU 后再评估。
- **mgr 内嵌 backend**（方案 C）：no_std 裸机里写块设备/网络栈是重量级错误方向，已否决。
- **替换或禁用路径 B**：crosvm 路径以 `CONFIG_HYPERVISOR_VIRTIO` feature-gate 并存，
  feature off 时既有门禁零变化。

---

## 4. xhyper 现状（构建基座）

### 4.1 EL2：virtio 零实现，但"派发框架"并非没有

- `hypervisor/` 全树无 virtio transport/状态机/backend
- 但 EL2 已有多个 MMIO 三态派发先例（`Unhandled`/`Fault`/`Emulated`，与 Gunyah
  vdevice 合同同构）：vGIC 的 GICD/GICR aperture、vITS 的 vdevice 窗口、vRTC
  （PL031/Phytium，DT 与 ACPI Host 均已跑通）
- 范围注册经 `ADDRSPACE_CONFIGURE_RANGE(ADD_VMMIO)`
- `hypervisor/device/` 当前只有 `vrtc`——Stage 0 把它的固定地址派发泛化为多设备
  动态注册表

### 4.2 ABI：三层现状

- **定义层完整**：xhyper-abi 冻结快照（`generated/hypercalls.rs`）已含全部
  11 条 `virtio_backend_*`（0x49–0x55），编号与 Gunyah 逐条一致；位域类型
  （`notify_reason` 的 new_buffer/reset_request/driver_ok/failed 等）一比一移植。
  EL2 的 `hypervisor/abi` include 的正是这份快照——**派发常量已可用**。
- **消费层**：xhyper-mgr 内嵌 `operation.rs` 只封装管理面；数据面封装缺（mgr
  正迁移共享 codegen 输出）。
- **实现层**：EL2 runtime 分发表无 `VIRTIO_BACKEND_*` handler——要填的全部

### 4.3 mgr：休眠编排脚手架

`virtual_device.rs`（2000+ 行）有完整的 realize/compensate/cleanup 事务模型
（失败补偿、回滚）；vRTC 域是**活的**（phase-d4-probe 验证）；virtio_mmio/pci/iommu
默认 `Unavailable`（platform 域有 builder API `with_virtio_mmio(Available)` 可显式
开启，但开了 EL2 也没实现）。另有 `PhysicalStreamBinding`/`bind_physical_streams`
（对标 Gunyah viommu_bind_streams，透传路线）。

### 4.4 其他基座

- `hypervisor/memory/useraccess`：guest IPA 安全访问（virtq 描述符遍历的底座）✅
- `hypervisor/memory/address_space`：Stage-2 映射 + `FaultKind::Vmmio`/`ResumeRequest`
  三态模型 ✅
- `hypervisor/object/model` + capability/cspace 两级模型 ✅
- `hypervisor/irq/vic` + `virq`：虚拟中断注入 ✅
- `hypervisor/rcu` + `preempt` + `spinlock` ✅

---

## 5. ABI 合同（已冻结）

Gunyah `hyp/interfaces/virtio_backend/virtio_backend.hvc`（xhyper-abi 快照逐条一致）：

| call_num | hypercall | 方向 | 作用 |
|---|---|---|---|
| — | `partition_create_virtio_backend` | RM→EL2 | 在 partition cspace 里创建对象 |
| 0x49 | `virtio_backend_configure` | RM→EL2 | transport 类型/memextent/vqs_num/flags |
| 0x4c | `virtio_backend_bind_virq` | RM→EL2 | frontend vIRQ 绑 VIC |
| 0x4d | `virtio_backend_unbind_virq` | RM→EL2 | 解绑 |
| 0x4e | `virtio_backend_notify` | backend→EL2 | 置 interrupt_status/触发 config 更新 |
| 0x4f | `virtio_backend_set_dev_features` | backend→EL2 | 设备 feature 字（须待复位） |
| 0x50 | `virtio_backend_set_queue_size_max` | backend→EL2 | 每 VQ 最大长度（须待复位） |
| 0x51 | `virtio_backend_get_drv_features` | backend→EL2 | 取驱动协商的 feature |
| 0x52 | `virtio_backend_get_queue_info` | backend→EL2 | 取 VQ size/ready/desc/drv/dev |
| 0x53 | `virtio_backend_get_notification` | backend→EL2 | 取 vqs_bitmap + reason（拉模型） |
| 0x54 | `virtio_backend_acknowledge_reset` | backend→EL2 | 确认复位完成 |
| 0x55 | `virtio_backend_update_status` | backend→EL2 | 后端改 status（如 NEEDS_RESET） |

`reason` 位域：`new_buffer` / `reset_request` / `driver_ok` / `failed`；
`notify_flags`：`per_queue` / `config_update`。

EL2 实现必须逐条匹配 Gunyah 语义（capability rights、错误码、复位时序）。

---

## 6. 架构概览

### 6.1 新建 crate 布局

```text
hypervisor/virtio/
├── model/      # 纯状态机：VirtioStatus / FeatureNegotiation / QueueState / ConfigUpdate（无 unsafe、无硬件依赖、穷尽单测）
├── mmio/       # MMIO transport：寄存器布局 + 写派发 + 读 mirror 维护
├── backend/    # 桥对象：ObjectKind::VirtioBackend + reason 位图 + virq 通知 + RAII 生命周期
├── virtq/      # 描述符链遍历 + used 回写（基于 useraccess）
└── console/    # console backend（Stage 2）：rx/tx 队列消费 + guest_uart 出口
```

### 6.2 Gunyah C 移植源对照

| Rust 目标 | Gunyah C 源 |
|---|---|
| `virtio/model` | `hyp/vm/virtio/src/frontend.c`（状态机，617 行）+ `backend.c` |
| `virtio/mmio` | `hyp/vm/virtio_mmio/src/{virtio_mmio.c,vdevice.c}` |
| `virtio/backend` | `hyp/vm/virtio_backend/src/virtio_backend.c`（223 行） |
| `virtio/virtq` | `hyp/vm/virtio_virtq/src/virtio_virtq.c`（201 行） |

Gunyah C 源位于本地 `D:\work\gunyah-hypervisor`（或服务器
`10.42.27.86:/media/test/USR-DATA/work/2030/gunyah/gunyah-src/hyp`）。

### 6.3 关键机制决策

| 机制 | 决策 |
|---|---|
| 通知 reason 位图 | `AtomicU64::fetch_or(Release)`，拉取侧 `Acquire`（对照 Gunyah `virtio_backend_notify`） |
| used ring 更新顺序 | 数据写 → `fence(Release)` → 写 head idx（对照 `virtio_virtq.c`） |
| status 互斥 | per-device spinlock（xhyper `spinlock_ticket`） |
| 阻塞复位 | guest vCPU 线程挂起等 backend `acknowledge_reset`；接 xhyper 线程模型；对象销毁先唤醒后回收 |
| 共享 config cache 页 | guest Stage-2 RO 映射（单映射，读不 trap、写 permission fault）；EL2 hyp 映射 RW（唯一写者）；mgr 经 memextent 捐赠 + hypercall 驱动（无直接映射） |
| 生命周期回滚 | RAII guard（Drop 时按已完成步骤逆序回滚，对照 Gunyah 手工 `unwind_object_activate`） |
| feature-gate | `CONFIG_HYPERVISOR_VIRTIO`（默认 off，Kconfig） |

---

## 7. 约束

1. **路径 B 零回归**：`CONFIG_HYPERVISOR_VIRTIO=off` 时 `phase-d4-verify` 全套不变绿转红
2. **Rust 规范**（xhyper 硬性）：每个 `unsafe` 块配 `SAFETY:` 注释；提交 `子系统: 祈使句` +
   `git commit -s`（DCO）；提交前 `cargo fmt --all && make clippy && make unittest`
3. **model 层先行**：纯逻辑、穷尽单测、红绿循环；架构相关实现收敛在适配器 crate；
   `runtime` 是唯一组合根
4. **基线提交**：xhyper `main@d67e6f5e`、xhyper-mgr `main@0a39089`、xhyper-abi `main@cba464c`

---

## 8. 验收标准

| 里程碑 | 门禁 | 标准 |
|---|---|---|
| Stage 0 | `make phase-e0-vdevice` | trivial 测试设备在 guest 里可读写 + `phase-d4-verify` PASS |
| Stage 1 | `make phase-e1-probe` | guest 标准 virtio_mmio 驱动 probe 到 DRIVER_OK、队列 ready（stub backend） |
| **Stage 2（闭环）** | `make phase-e2-console` | Secondary guest `/dev/hvc0` 双向交互，**不依赖 crosvm、不依赖 Host Linux 内核改动** |

每阶段还需：`make clippy` 无 warning + `cargo fmt --all --check` + 既有门禁零回归。

---

## 9. 开放问题（影响 Stage 3 范围，不影响 Stage 0–2）

1. ~~xhyper-abi 数据面 immediate 编号~~ —— **已关闭**（0x49–0x55 与 Gunyah 一致）
2. **blk backend 进程宿主形态**：独立新进程 vs 扩展 `xhyper-vmm`（倾向独立进程）
3. **阻塞复位的等待落点**：等 mgr 的哪条路径（hypercall 上下文 vs 独立线程）
4. **Host Linux `/dev/xhyper` 驱动数据面 ABI 现状**（Stage 3 前置）：是否已暴露
   0x4e–0x55 ioctl + backend virq 投递 + memextent 捐赠映射。未核实，决定 Stage 3
   是"写 daemon"还是"改内核 + 写 daemon"
