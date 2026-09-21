# virtio 设计动机与 crosvm 缺口分析：Gunyah vs xhyper

> 本文聚焦三个问题：
> 1. xhyper-mgr（Root VM）调研——发现休眠的 Gunhaus 式 virtio backend 管理代码
> 2. Root VM 在 crosvm 路径里的真实角色，以及 crosvm 完整运行的缺口清单
> 3. 为什么 Gunyah 选择在 EL2 + RM VM 里实现 virtio（设计哲学）
>
> 证据来源：
> - xhyper-mgr：`crates/xhyper-abi/src/operation.rs`、`crates/xhyper-mgr-core/src/domains/{virtual_device,platform,secondary_configuration}.rs`
> - xhyper：`hypervisor/runtime/src/{vcpu_run,secondary_vcpu,vgic}.rs`
> - Gunyah README（本地 `D:\work\gunyah-hypervisor\README.md`）

---

## 1. 概述：两条 virtio 路径，不要混淆

xhyper 里其实存在**两条独立的 virtio 路径**，一条活跃（crosvm），一条休眠。先把全景画清楚：

```text
路径 A: Gunhaus-style (dormant, NOT active)
+-------------------+      hypercall       +-------------------+
| xhyper-mgr        | -------------------> | EL2               |
| (Root VM, EL1)    |  partition_create_   | virtio backend    |
| virtio backend    |  virtio_backend /    | object +          | <-- NOT implemented
| management code   |  configure /         | transport         |     (grep empty)
| (dormant)         |  bind_virq /         | emulation         |
| default:          |  activate            +-------------------+
| Unavailable       |
+-------------------+
  ABI 继承自 Gunyah，代码在 mgr 里，但 EL2 不实现、mgr 默认关

路径 B: crosvm (active, verified by phase-d4-2)
+-----------+  /dev/xhyper  +-----------+  hypercall  +-----+
| crosvm    | -------------> | xhyper-mgr| ----------> | EL2 |
| (Host VM  |   ioctl        | (Root VM) |             |     |
| userspace)|                | VM/mem/   |             | syn |
| full      | <------------- | IRQ       | <---------- | c   |
| virtio    |   result       | resource  |   result    | MMIO|
| emulation |                | scheduling|             | exit|
+-----------+                +-----------+             +-----+
  crosvm 自带全套 virtio 栈，Root VM 只做资源调度，EL2 只做同步 MMIO exit
```

**关键认知**：路径 A 的休眠代码**不是**路径 B（crosvm）的前置条件。让 crosvm 完整运行要补的是路径 B 上的缺口，不是把路径 A 启用。

---

## 2. xhyper-mgr 调研：休眠的 Gunhaus 式 virtio backend 管理

xhyper-mgr 是 Root VM 里的 Rust 全局资源管理器，等价于 C 版的 `gunyah-resource-manager`。单个静态 PIE ELF，打包成 `rootvm.gpkg` 给 XHyper 启动时嵌入，跑在 EL1。

### 2.1 它保留着 Gunhaus 式 virtio backend 管理脚手架

`crates/xhyper-abi/src/operation.rs` 里有一整套 Rust 封装，直接发对应 immediate：

| hypercall（ABI） | operation.rs 方法 |
|---|---|
| `PARTITION_CREATE_VIRTIO_BACKEND` | `partition_create_virtio_backend()` |
| `VIRTIO_BACKEND_CONFIGURE`（mmio/pci） | `virtio_backend_configure_mmio/pci()` |
| `VIRTIO_BACKEND_BIND_VIRQ` / `UNBIND_VIRQ` | `virtio_backend_bind/unbind_virq()` |
| `OBJECT_ACTIVATE`（virtio backend） | `object_activate_virtio_backend()` |
| `cspace_{delete,copy}_virtio_backend_from` | 同名方法 |

这套 immediate 和 C 版 Gunyah 的 `virtio_backend.hvc` 是**同一个 ABI**——印证 README 的"兼容 hypercall"。

`crates/xhyper-mgr-core/src/domains/virtual_device.rs` 有 `VirtioMmioRecord { frontend, backend, ... }`、`BackendPage`、`VirtioBackendResourceProjection`；`secondary_configuration.rs` 有 `VirtioMmio`/`VirtioPci`/`VirtioIommu` 配置项 + `VirtioNodePlacement`。这是**在 manager 侧管理 virtio backend 对象的脚手架**，模型和 C 版 Gunyah 的 RM 一致。

### 2.2 但 EL2 不实现，mgr 也默认关掉

- xhyper 的 `hypervisor/` 全树 grep `virtio_backend|VirtioBackendCap|VirtioDeviceType` = **零命中**。EL2 **没有实现** virtio backend 对象的 create/configure/activate 逻辑。
- `crates/xhyper-mgr-core/src/domains/platform.rs` 默认：`virtio_mmio: Unavailable`、`virtio_pci: Unavailable`、`iommu: QemuUnavailable`。路径 A **没开**。

### 2.3 准确的能力边界表述

若只看 `hypervisor/`（EL2），会得出"xhyper 仓没有任何 virtio 设备侧实现"的结论——这话对 EL2 成立，但**漏了 xhyper-mgr 这套休眠代码**。准确表述：

> xhyper 的 EL2 不实现 virtio；但 xhyper-mgr（Root VM）保留了从 Gunyah ABI 继承的 virtio backend **管理脚手架**（create/configure/bind_virq/activate 的 hypercall 封装 + 配置/投影领域代码），当前默认 `Unavailable`、休眠；EL2 侧也未实现对应 hypercall。实际生效的 virtio 路径仍是 crosvm（路径 B）。

这说明 xhyper **曾打算走 Gunyah 那条"RM 管 virtio backend 对象"的路**（脚手架都搭了），只是当前没启用、EL2 也没实现，转而用 crosvm。

---

## 3. Root VM 在 crosvm 路径里的真实角色：不是空壳，但也不做仿真

crosvm 不是"全包"到 Root VM 当甩手掌柜。架构链路 `crosvm → /dev/xhyper → xhyper-mgr → hypercall → EL2` 里，xhyper-mgr 是**中继 + 策略层**，crosvm **没法绕过它**。即便 virtio 设备仿真在 crosvm，这些事 Root VM 必须做：

- **VM 生命周期**：Secondary VM 的 `VM_CREATE` / `VCPU_CREATE` / `VM_START`
- **内存映射策略**：virtio-blk 的 backing memory、virtio MMIO 区段映进 Secondary 的 Stage-2
- **vIRQ 路由策略**：virtio 设备的中断绑到 Secondary 的 vGIC

但要分清职责边界：

| 职责 | 谁干 | 现在（crosvm 路径） | 休眠的 Gunhaus 式路径（若启用） |
|---|---|---|---|
| virtio 设备仿真（virtqueue、config 空间） | crosvm / 后端 VM | crosvm | 后端逻辑在 RM VM 或专用 backend |
| virtio backend **对象管理**（create/configure/activate） | Root VM + EL2 | 不走（休眠） | Root VM 经 hypercall 管 EL2 对象 |
| VM 容器/内存/中断资源调度 | Root VM | ✅ 在做 | ✅ 在做 |

**Root VM 做的是资源管理/对象管理/策略，不是 virtio 协议仿真本身**。即便启用休眠那条 Gunhaus 式路径，virtio 的**设备行为**还是要在某个后端跑，Root VM 管的是"backend 对象在 EL2 怎么创建、绑中断、激活"。

---

## 4. crosvm 完整运行缺口清单（要补什么）

### 4.1 当前已通的链路（为什么 virtio-blk 能跑）

`hypervisor/runtime/src/vcpu_run.rs` 揭示机制：crosvm 调 `HYPERCALL_VCPU_RUN`（经 /dev/xhyper → xhyper-mgr → EL2），guest 访问 virtio MMIO 区时，EL2 让 vCPU run 带 **`VCPU_RUN_STATE_ADDRSPACE_VMMIO_READ/WRITE`** 状态退出，把 MMIO 访问信息返给 crosvm，crosvm 仿真后 resume。这是**同步 MMIO exit 模型**（像最基础的 KVM）。

```text
Guest vCPU runs (VCPU_RUN hypercall from crosvm)
   |
   |  guest accesses virtio MMIO region (Stage-2 fault)
   v
EL2: VCPU_RUN exits with VCPU_RUN_STATE_ADDRSPACE_VMMIO_READ/WRITE
   |  (MMIO access info returned to crosvm via /dev/xhyper)
   v
crosvm: emulates virtio access (virtqueue, config space, ...)
   |  (crosvm resumes VCPU_RUN with the result)
   v
Guest vCPU continues
```

配合 `memextent`/`addrspace` 映射、`vic`/`bind_virq` 注入 IRQ，virtio-blk 跑通了 phase-d4-2（ext4 读/写/回读）。

### 4.2 缺口清单（按优先级）

| # | 缺口 | 性质 | 现状 | 在哪补 |
|---|---|---|---|---|
| 1 | **异步通知 ioeventfd/irqfd** | 性能（最关键） | hypervisor/ grep `ioeventfd/irqfd/eventfd` = **零命中**，全是同步 MMIO exit | EL2 + /dev/xhyper UAPI |
| 2 | **VPCI（虚拟 PCI）** | virtio-pci 支持 | xhyper-abi 有 `vpci_*` 一套 hypercall；`hypervisor/` 里 `find -iname '*pci*'` = **空**，EL2 没实现 | EL2（ABI 有，实现缺） |
| 3 | **多 virtio 设备并发 + 多 IRQ** | 覆盖度 | phase-d4-2 只验了单 virtio-blk | 门禁 + 可能的容量调整 |
| 4 | **长时运行 vCPU + 完整 SMP** | 稳定性 | phase-d 只跑短 create→boot→poweroff 周期 | EL2 runtime |
| 5 | **/dev/xhyper UAPI 完整性** | 接口层 | 在 Host VM Linux 内核里（打补丁的 Linux），不在 xhyper/xhyper-mgr 仓 | Host Linux 内核（另查） |

### 4.3 各缺口的解释

**① 异步通知机制——最大的性能缺口**
- 现状：每次 virtio 访问都是同步 trap→exit→crosvm→resume。virtio-blk 功能测试（低吞吐）能过，但**真负载**（网络收包、磁盘高 IOPS）同步 exit 扛不住。
- KVM 正是靠 ioeventfd（注册 MMIO 区，命中异步通知 VMM，不退出 vCPU）+ irqfd（VMM 异步注入 IRQ）才扛得住 virtio 性能。xhyper 没有。
- 要做：在 EL2 + /dev/xhyper UAPI 里加"注册 MMIO 区→命中异步通知 VMM"和"VMM 异步注入 IRQ"两条机制。

**② VPCI——用 virtio-pci 的前提**
- 现状：xhyper-abi 有 `partition_create_vpci`/`vpci_configure`/`vpci_attach`/`vpci_bind_legacy_virq` 一整套，但 EL2 里没有任何 vpci 实现。crosvm 只能用 **virtio-mmio**（phase-d4-2 就是 mmio）。
- 要用 virtio-pci（多设备、MSI-X、热插拔、PCIe）就得 EL2 实现 VPCI 仿真（config 空间、BAR、MSI-X 表）。这跟 virtio_backend 是平行的另一块"ABI 有、EL2 没"。

**③ 多设备并发 + ④ 长运行 SMP**：机制同 virtio-blk，但没验证过多设备并发和长时运行稳定性。

**⑤ /dev/xhyper UAPI**：crosvm 直接调的 ioctl 接口在 Host VM Linux 内核里，不在 xhyper / xhyper-mgr 仓。是否齐了（VCPU_RUN 带 MMIO 状态、内存 map、IRQ inject、未来的 ioeventfd/irqfd）得去 Host Linux 那个仓看。

### 4.4 一句话

**跟那套休眠的 `virtio_backend` 代码无关**——那套是"EL2 自己仿真 virtio + RM 管对象"的另一条路（路径 A）。让 crosvm 完整跑（路径 B），补的是**异步通知 + VPCI + 多设备 + 长运行**这套通用基础设施。

---

## 5. 为什么 Gunyah 在 EL2 + RM VM 里实现 virtio（设计哲学）

这不是"Gunyah 顺手做了 virtio"，而是**它的设计哲学决定的**。Gunyah README 原话：

### 5.1 Type-1 纯粹性 + 最小 TCB

> Gunyah is a Type-1 hypervisor ... It does not depend on any lower-privileged OS kernel/code for its core functionality. This increases its security and can support a much smaller trusted computing base than a Type-2 like hosted-hypervisors.

Type-1 的核心信条是**不依赖任何高特权 OS 内核**。把 virtio 甩给 crosvm（像 xhyper 那样）就得有一个 Host VM Linux + crosvm 用户态进程——等于把一个完整 Linux 内核 + VMM 塞进 TCB。Gunyah 把 virtio transport 放 EL2、backend 放 RM VM（精简专用 VM），整个 virtio 链路都在"hypervisor + RM"受信边界内，**不引入一个完整 Linux 当依赖**。

### 5.2 微kernel式分层：关键在 EL2，非关键甩出去

> Gunyah's design principle is not dissimilar to a traditional microkernel ... delegates the provision of non-critical services to non-privileged (or less-privileged) processes.

对上 `virtio.h` 的劈分：
- **关键（transport + virtio_t 状态机）在 EL2**：`virtio_mmio`/`virtio_pci` 仿真 config 空间、virtqueue、status/feature 协商——virtio 协议核心，放 EL2 保证性能和审计
- **非关键（backend 设备逻辑）甩给 RM VM**：`virtio_input`/`virtio_iommu` 的实际设备行为跑在 RM VM（EL1，低特权）

微kernel的典型套路：内核态留最关键的，设备驱动逻辑下放到用户态（RM VM）。

### 5.3 场景决定：实时/安全/移动，不要一个 Host OS

README 列的场景：移动支付、安全 UI、电池设备、实时。共同点：
- 不能容忍一个完整 Host VM Linux 当依赖（太大、太重、攻击面大、启动慢）
- 延迟敏感（xhyper 那条 guest→EL2→Host VM→crosvm 多一跳，实时场景不接受）
- TCB 要小到能审计（Linux + crosvm 没法做安全审计）

所以 Gunyah 用一个**精简的 RM VM**跑 backend，既满足微kernel分层，又不引入完整 OS。

### 5.4 ARM EL2 的天然契合

ARM 虚拟化里 guest 访问 virtio MMIO 本来就 trap 进 EL2（Stage-2 fault）。Gunyah 用 VHE 模式跑 EL2，在 EL2 直接处理 trap、仿真 transport，是**硬件特性决定的自然落点**。xhyper 选"EL2 只转发不解释"是有意多一跳换复用 crosvm；Gunyah 选"EL2 直接解释"是用硬件给的便利换低延迟。

---

## 6. 两种权衡对比

| | Gunyah（EL2 + RM VM） | xhyper（crosvm） |
|---|---|---|
| TCB | 小（hypervisor + 精简 RM VM） | 大（+ Host VM Linux + crosvm） |
| 依赖一个 Host OS | 不依赖 | 依赖 |
| 延迟 | 低（guest→EL2→RM） | 高（guest→EL2→Host→crosvm） |
| 实现成本 | 自研全套 virtio | 复用成熟 crosvm |
| 场景定位 | 实时/安全/移动 | 快速搭起能力、对齐功能面 |
| 启动复杂度 | RM VM 精简，启动链短 | 要等 Host Linux + crosvm 起来 |
| virtio 实现位置 | EL2 transport + RM VM backend | crosvm 用户态（Host VM） |
| EL2 是否实现 virtio 协议 | 是 | 否（只同步 MMIO exit + 转发） |

两边各自合理：Gunyah 是 Qualcomm 在悉尼为移动/安全场景做的 Type-1，定位决定了"virtio 必须在受信边界内自研"；xhyper 是麒麟用 Rust 复刻 Gunyah，优先级是**快速搭起能力面**，所以用 crosvm 省掉自研 virtio 的巨量工作。

---

## 7. 结论

1. **xhyper 存在两条 virtio 路径**：路径 A（Gunhaus 式，休眠，mgr 有脚手架 + ABI，EL2 没实现，默认 Unavailable）；路径 B（crosvm，活跃，phase-d4-2 验证）。两者独立，不要混淆。

2. **Root VM 不是空壳**：即便 crosvm 路径，Root VM 也做 VM/内存/中断资源调度；休眠代码显示曾打算让它还管 virtio backend 对象。但"管理" ≠ "仿真"。

3. **crosvm 完整运行要补的是路径 B 上的缺口**：异步通知（ioeventfd/irqfd，性能）、VPCI（virtio-pci）、多设备、长运行 SMP——**跟那套休眠的 virtio_backend 代码无关**。

4. **Gunyah 在 EL2+RM VM 实现 virtio 是设计哲学决定的**：Type-1 纯粹性 + 最小 TCB + 微kernel分层 + 实时/安全场景，注定了 virtio 不能甩给一个用户态 VMM + Host OS。xhyper 用 crosvm 是另一种权衡，不是对错。

> 注：xhyper-mgr 的 `VirtioBackendCap` 能力边界、"代理转发到 crosvm 的精确代码路径"、/dev/xhyper UAPI 的完整 ioctl 清单，这几项未逐行追踪（部分在 Host Linux 内核仓，不在这俩仓内）。本报告的判断基于已验证的 ABI 清单 + grep 结果 + phase 脚本验证标记 + README 设计原则。
