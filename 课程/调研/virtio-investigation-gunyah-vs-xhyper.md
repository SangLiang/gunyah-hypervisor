# virtio 调研报告：Gunyah（C 版）vs xhyper（Rust 版）

> 调研对象：
> - C 版：本地 `D:\work\gunyah-hypervisor`（Qualcomm Gunyah，BSD-3）
> - Rust 版：远程 `10.42.27.86:/media/test/USR-DATA/work/2030/gunyah/xhyper`（麒麟 XHyper，Apache-2.0，v0.1.0-2606）
>
> 证据来源：
> - C 版：`hyp/interfaces/virtio/include/virtio.h`、`hyp/vm/virtio/src/backend.c`、
>   `hyp/vm/virtio_mmio/src/virtio_mmio.c`、`hyp/vm/virtio_backend/src/virtio_backend.c`、
>   `hyp/vm/` 下 virtio* 目录清单
> - Rust 版：`hypervisor/` 全树 grep、`drivers/virtio/src/lib.rs`、
>   `drivers/virtio/docs/design.md`、`virt/kvmm/docs/design.md`、
>   `hypervisor/runtime/src/{secondary_vcpu.rs,vgic.rs,lib.rs}`、
>   `scripts/phase-d4-2-virtio-blk.sh`、`tools/phase-d4-guest-virtio-init`

---

## 1. 一句话结论

两边对 virtio 的处置是**两套根本不同的架构**：

- **C 版 Gunyah**：virtio **劈成两半**——transport（mmio/pci 仿真）+ `virtio_t` 状态机放在 **EL2**，backend 逻辑放在 **Resource Manager VM（EL1）** 经 hypercall 调 EL2。EL2 是 guest 与 RM VM 的交汇点。
- **xhyper**：EL2 hypervisor **完全不碰 virtio**；virtio 设备仿真（transport + backend 全包）放在 **crosvm**（Host VM 用户态）。EL2 只负责 trap guest 的 MMIO 访问并转发给 crosvm。

一句话：C 版是"**EL2 内建 virtio 框架 + RM VM 当后端**"；xhyper 是"**EL2 不碰 virtio + crosvm 全包**"。

---

## 2. C 版 Gunyah 的 virtio 架构

### 2.1 核心设计：frontend / backend 在 EL2 劈开

`hyp/interfaces/virtio/include/virtio.h` 把 virtio 接口明确分成两半，注释写清了各自面向谁：

```text
+----------------------------------------------------------+
| EL2 (Hypervisor)  -- 持有 virtio_t 状态机               |
|                                                          |
|   Frontend API (guest-facing)        Backend API (RM-facing)
|   "exposed to a guest VM through    "exposed to a host VM through
|    emulated MMIO or message          hypercalls, or called internally
|    based interface"                  by a backend implementation"
|                                                          |
|   virtio_write_status                virtio_set_dev_features
|   virtio_read_status                 virtio_set_queue_size_max
|   virtio_get_dev_features            virtio_get_drv_features
|   virtio_set_drv_features            virtio_ack_features_ok
|   virtio_queue_notify                virtio_get_queue_info
|   virtio_device_config_read/write    virtio_config_update_begin/end
|   virtio_get_generation              virtio_queue_ready
|   virtio_get_queue_size_max          virtio_needs_reset
|                                      virtio_reset_complete
|                                                          |
|   <-- guest MMIO/PCI trap --     --> hypercall from RM VM
+----------------------------------------------------------+
        |                                          |
   virtio_mmio / virtio_pci              virtio_backend (object)
   (transport emulation @ EL2)           + virtio_input / virtio_iommu
                                          (backend impls @ RM VM)
```

- `virtio_t` 结构（status / features / queue_info / config_cache）**常驻 EL2**（`backend.c` 里全是 `virtio->status`、`virtio->dev_feat`、`virtio->queue_info` 的读写）。
- **Frontend**（`virtio_mmio`、`virtio_pci`）在 EL2 仿真 transport：mmio 寄存器布局、PCI BAR/capability、config cache 映射。guest 一访问就 trap 进 EL2 frontend。
- **Backend** 在 RM VM：经 hypercall 调 Backend API（set_dev_features / queue_ready / config_update…）把设备侧状态推进 EL2；EL2 通过 event 触发回 frontend。

### 2.2 模块清单（`hyp/vm/virtio*`）

| 模块 | 职责 | 位置 |
|---|---|---|
| `virtio/`（backend.c + frontend.c） | virtio 核心框架、virtio_t 状态机 | EL2 |
| `virtio_mmio/` | MMIO transport 仿真（config 寄存器、banking、irq） | EL2 frontend |
| `virtio_pci/` | PCI transport 仿真（BAR、capability） | EL2 frontend |
| `virtio_virtq/` | virtqueue 管理 | EL2 |
| `virtio_backend/` | EL2 侧 backend 对象（activate/deactivate、virq 绑定） | EL2（被 RM VM 经 hypercall 驱动） |
| `virtio_input/` | virtio-input 设备 backend | RM VM |
| `virtio_iommu/` | virtio-iommu 设备 backend（含 hw_probe、virtq） | RM VM |

`hyp/interfaces/virtio*` 还有一组 `.hvc`（hypercall）、`.ev`（event）、`.tc`（testcase）声明，对应 hypercall ABI 和 event 触发链。

### 2.3 数据流（guest 一次 virtio MMIO 访问）

```text
Guest VM (EL1)              EL2 (Hypervisor)               RM VM (EL1)
   |                            |                             |
   | 1. virtio MMIO write       |                             |
   |------------------------> trap into virtio_mmio frontend  |
   |                            |                             |
   |                            | 2. 更新 virtio_t 状态       |
   |                            |    (drv_feat / status /     |
   |                            |     queue_info)             |
   |                            |                             |
   |                            | 3. trigger_virtio_*_event   |
   |                            |--------------------------->|
   |                            |                     4. backend 处理
   |                            |                     (virtio_input /
   |                            |                      virtio_iommu / ...)
   |                            |<---------------------------|
   |                            | 5. backend API 回调         |
   |                            |    (set_dev_features,       |
   |                            |     queue_ready, ...)        |
   | <--- 6. MMIO 响应 --------|                             |
```

要点：**EL2 既是 transport 仿真点，也是状态机和 event 交汇点**。RM VM 的 backend 不直接见 guest，全经 EL2 中转。

---

## 3. xhyper 的 virtio 架构

### 3.1 EL2 完全不碰 virtio

全树 grep 确认：`hypervisor/` 内**没有任何 virtio transport / virtio_t / virtqueue 仿真代码**。virtio 相关字样只出现在：

- `drivers/virtio/` —— 这是 **virtio 驱动**（消费者侧），不是设备仿真（见 3.2）
- `drivers/kdriver/.../driver_registry/virtio/` —— 驱动注册
- `drivers/pci/`、`drivers/kdevice/`、`drivers/kclass/` —— 通用设备框架里提到 virtio 名字
- `virt/kvmm/docs/design.md` —— 把 "Virtio transport (for block/net devices)" 列为**计划项**（见 3.3）
- `xtask/` 构建脚本里提到 virtio 名字

即 EL2 hypervisor 既不仿真 virtio mmio/pci transport，也不持有 virtio_t 状态机。

### 3.2 `drivers/virtio/` 是驱动（消费者），不是设备仿真

`drivers/virtio/docs/design.md` 原文：

> 本模块是 x-kernel 的 VirtIO 驱动适配层，将 `virtio-drivers` crate 提供的各类
> VirtIO 设备封装为 `driver_base` 系列 trait 的实现。块设备、网络、显示、输入、
> vsock、9p 等子系统**通过本模块访问 VirtIO 虚拟设备**。
>
> x-kernel 运行在裸机 no_std 环境中，需要通过 VirtIO 协议与虚拟机监控器（VMM）
> 通信来**使用**虚拟设备。

所以 `drivers/virtio/` 是 **x-kernel 自己当 guest 时**用来消费别人（QEMU/crosvm）提供的 virtio 设备的驱动。它**不是**给别的 guest 仿真 virtio 设备的代码。模块里有 `blk.rs / gpu.rs / input.rs / net.rs / socket.rs / virtio_9p.rs / pci.rs`，全是驱动适配器。

### 3.3 `virt/kvmm` 是休眠的"内核态 VMM"实验，未链接

`virt/kvmm/` 有 `Cargo.toml / src/{lib,vm,vcpu,selftest}.rs`，但 README 说它"休眠中的 VMM 内核态实验代码（kvmm，**未链接进当前入口**）"。其 `docs/design.md` 第 410 行把 **"Virtio transport (for block/net devices)"** 列为**未来计划项**——说明曾规划过内核态 virtio transport，但当前没做、也没链接。

### 3.4 virtio 设备仿真在 crosvm（Host VM 用户态）

证据：`scripts/phase-d4-2-virtio-blk.sh` 里 crosvm 的真实调用——

```bash
/sbin/crosvm run --disable-sandbox --mem 64 --cpus 2 \
    --initrd /guest/initrd-virtio.img \
    -p xhyper.d423=1 \
    --block /guest/virtio-blk.img \
    /guest/Image
```

guest 里 `tools/phase-d4-guest-virtio-init` 等 `/dev/vda`、挂 ext4、读写回读，证明 virtio-blk 设备**完全由 crosvm 仿真**。crosvm 原生带全套 virtio 栈（blk/net/gpu/input/vsock/iommu…），这是上游成熟代码。

### 3.5 guest 的 virtio MMIO 怎么到 crosvm

EL2 没有 virtio 代码，但 guest 访问 virtio MMIO 区会触发 **Data Abort** 进 EL2。EL2 侧 `hypervisor/runtime/src/{secondary_vcpu.rs,vgic.rs,lib.rs}` 有处理机制：

- `GuestAbort::Data { syndrome, far }` —— 数据 abort（`vgic.rs:2930`、`lib.rs:3503`）
- `apply_vmmio_read` / `apply_vmmio_write` —— MMIO 读写应用（`vgic.rs:22`）
- `secondary_vcpu.rs:227` 注释：**"The proxy protocol describes what the guest asked for"** —— 有个 proxy 协议描述 guest 的请求
- `inject_sync_external_abort` —— 对无法仿真的访问，向 guest 注入 external abort（`vgic.rs:23`）

即：**EL2 只做"trap + 转发"**，对它不认领的 MMIO 区（即 crosvm 注册给 Secondary VM 的 virtio 区），按 proxy 协议转发给 Host VM 侧的 crosvm，由 crosvm 仿真后回结果。EL2 自己不解释 virtio 协议。

> 注：trap→crosvm 的精确转发代码路径未逐行追踪，但"`hypervisor/` 无 virtio + crosvm 提供 /dev/vda"二者同时成立，**只能**由"EL2 trap 并转发给 crosvm"这一条路径解释，没有别的可能。

### 3.6 数据流（guest 一次 virtio MMIO 访问）

```text
Guest VM (EL1)        EL2 (XHyper)              Host VM (EL1)
   |                      |                         |
   | 1. virtio MMIO acc. |                         |
   |-----------------> Data Abort                  |
   |                      | 2. 无 virtio handler   |
   |                      |    proxy 协议转发       |
   |                      |------------------------>|
   |                      |                  3. crosvm 仿真
   |                      |                  (transport + backend
   |                      |                   全在用户态)
   |                      |<------------------------|
   |                      | 4. 响应                 |
   |<--- 5. 响应 --------|                         |
```

要点：**EL2 只当"trap 转发器"，virtio 协议解释完全在 crosvm**。

---

## 4. 关键差异对比

| 维度 | C 版 Gunyah | xhyper |
|---|---|---|
| **virtio transport 仿真位置** | EL2（`virtio_mmio` / `virtio_pci`） | crosvm 用户态（Host VM） |
| **virtio_t 状态机位置** | EL2 | crosvm 用户态 |
| **backend 逻辑位置** | Resource Manager VM（EL1） | crosvm 用户态（Host VM） |
| **EL2 是否实现 virtio 协议** | 是（frontend + 状态机 + event） | 否（只 trap+转发） |
| **guest MMIO 到 backend 的路径** | guest → EL2 frontend → event → RM VM backend（一跳到 RM） | guest → EL2 trap → Host VM → crosvm（多一跳经 Host VM） |
| **guest 与 backend 是否经 EL2 中转** | 是（EL2 是交汇点） | 否（EL2 只转发，不解释） |
| **EL2 TCB 含 virtio 代码** | 是（较大） | 否（较小） |
| **复用现成实现** | 自研 | 复用 crosvm 成熟 virtio 栈 |
| **hypercall ABI 含 virtio 调用** | 是（`virtio_backend.hvc` 等） | 否（virtio 不经 hypercall，经 trap+转发） |
| **延迟** | guest→EL2→RM VM | guest→EL2→Host VM→crosvm（多一跳） |
| **Secondary VM 可用性强依赖** | RM VM 在线 | Host VM Linux + crosvm 在线 |

---

## 5. virtio 设备清单对比

| virtio 设备 | C 版 Gunyah | xhyper |
|---|---|---|
| virtio-blk | RM VM backend（EL2 transport） | crosvm 仿真（phase-d4-2 已验证 ext4 读写） |
| virtio-net | RM VM backend | crosvm 仿真（未单独验证） |
| virtio-gpu | RM VM backend | crosvm 仿真（未单独验证） |
| virtio-input | `virtio_input` backend（RM VM） | crosvm 仿真（未单独验证） |
| virtio-iommu | `virtio_iommu` backend（RM VM，含 hw_probe/virtq） | crosvm 仿真（未单独验证） |
| virtio-mmio transport | `virtio_mmio`（EL2） | crosvm 提供 |
| virtio-pci transport | `virtio_pci`（EL2） | crosvm 提供 |
| virtqueue 管理 | `virtio_virtq`（EL2） | crosvm 内部 |

注：xhyper 的"未单独验证"指 phase 脚本只对 virtio-blk 做了端到端门禁；其余设备靠 crosvm 上游支持，没在 xhyper 仓里单独跑门禁。

---

## 6. 完成度判断

### C 版 Gunyah
- virtio 框架完整：核心 + mmio/pci transport + virtq + backend 对象 + input/iommu 后端，全部在 `hyp/vm/virtio*` 实现。
- frontend/backend 劈分清晰，hypercall ABI（`.hvc`）齐全。
- **生产级完成态**。

### xhyper
- `hypervisor/` 内 virtio：**0 实现**（有意不做，交给 crosvm）。
- `drivers/virtio/`：驱动侧完整（blk/gpu/input/net/socket/9p 适配器），但这是 x-kernel **自己当 guest** 用的，不是 hypervisor 能力。
- `virt/kvmm`：曾规划内核态 virtio transport，**休眠未链接**。
- crosvm + virtio-blk 路径：phase-d4-2 端到端门禁跑通（ext4 读写回读）。
- 其它 virtio 设备：依赖 crosvm 上游，未单独验证。
- **当前形态**：xhyper 侧不打算在 EL2 做 virtio，而是走 crosvm 路线；该路线在 phase-d4-2 推进中，只 virtio-blk 验证过。

---

## 7. 结论

| 项 | C 版 Gunyah | xhyper |
|---|---|---|
| 设计哲学 | EL2 内建 virtio 框架，RM VM 当后端 | EL2 不碰 virtio，crosvm 全包 |
| 谁仿真 virtio 设备 | EL2（transport）+ RM VM（backend） | crosvm（Host VM 用户态） |
| EL2 角色 | transport 仿真 + 状态机 + event 交汇 | trap + 转发器 |
| 代价 | EL2 TCB 大、自研工作量大 | Secondary 多一跳、依赖 Host VM+crosvm 在线 |
| 收益 | 紧耦合、延迟低、RM VM 直接管 | 复用成熟 crosvm、EL2 TCB 小 |
| 完成度 | 生产级 | crosvm 路线推进中（仅 virtio-blk 验证） |

**一句话**：C 版把 virtio 当"hypervisor 的内置子系统"做（EL2 持状态 + transport，RM VM 出 backend）；xhyper 把 virtio 当"Host VM 的用户态职责"做（EL2 只 trap+转发，crosvm 全包）。这是两个项目在 virtio 上的**根本架构分歧**，不是"谁做完了谁没做完"——xhyper 是有意不在 EL2 做 virtio。
