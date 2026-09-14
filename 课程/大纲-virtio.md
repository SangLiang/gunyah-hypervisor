# Gunyah VirtIO 大纲（10 课）

接在 [大纲.md](大纲.md) 第 27 课之后。面向已经走完 L0–L5 的读者。原则不变：**每课 1–2 个核心知识点**；只跟一条路径。

| 项 | 值 |
|----|-----|
| 课数 | 10（预备 1 + 主线 8 + 串讲 1） |
| 主路径 | `virtio_backend` 对象 + **MMIO** 前端 |
| 正文目录 | 建议 `课程/virtio/`（不要和前 27 课课号撞车） |
| 完成标准 | 能口述：Guest 写 QueueNotify → hyp trap → 后端 vIRQ → 后端 notify → 前端 vIRQ |
| 前置 | [第 20–21 课](第20课.md) memextent / 两套地址空间；[第 23–24 课](第23课.md) `virq_assert` / WFI；[第 25–27 课](第25课.md) HVC / CapID / doorbell |

**本阶段不铺开**：PCI 传输、virtio-iommu / SMMU、`virtio_input` 设备实现、hyp 里解析 descriptor chain、网卡/块设备后端逻辑。方向写在第 10 课。

---

## 先对齐：Gunyah 的 VirtIO 是什么

仓库里的 VirtIO **不是** hyp 实现一张网卡。它是：

```text
Frontend VM（Guest 驱动）
    ↕  MMIO 寄存器窗口（memextent + vdevice trap）
Gunyah EL2（virtio_t 核心 + 两边 vIRQ）
    ↕  hypercall（CapID）
Backend VM（通常是 RM；真正处理队列数据）
```

队列缓冲区在 **共享物理页** 里（第 18–21 课的权证 / Stage-2）。hyp 传的是「铃」和「控制面状态」，一般 **不拷描述符里的 payload**。

`hyp/vm/virtio_virtq/` 是给 **virtio-iommu** 用的，不是这条主路径。

---

## 预备 · V0（第 1 课）

不跟 Gunyah 源码深读。目标：进第 2 课之前，能分开「传输」和「设备类型」。

| 课 | 核心知识点（≤2） | 本课不讲 | 回指 / 入口 |
|----|------------------|----------|-------------|
| [1](virtio/第1课.md) | ① VirtIO = 前端驱动 + 后端设备 ② 传输（MMIO）≠ 设备类型（net/block/…） | 规范全文、PCI、打包格式 | 第 27 课 L6 点过方向 |

---

## V1 · 对象与创建（第 2–3 课）

一条链：hyp 里是什么对象 → 怎样用 CapID 造出来并 configure。

| 课 | 核心知识点（≤2） | 本课不讲 | 入口 |
|----|------------------|----------|------|
| 2 | ① `virtio_t` 是核心状态机 ② `virtio_backend` 是对外对象；MMIO 是它的一种 frontend | PCI frontend、input 设备 | `hyp/interfaces/virtio/include/virtio.h`；`hyp/vm/virtio/src/frontend.c` |
| 3 | ① `partition_create_virtio_backend` ② `virtio_backend_configure`：memextent + vqs + MMIO | 全部 backend hypercall | `hyp/interfaces/virtio_backend/virtio_backend.hvc`；`hyp/vm/virtio_backend/src/hypercalls.c` |

configure 时那张 memextent：MMIO 要求 offset **正好 256**（`0x100`）。前 256 字节给公共寄存器，后面给 device config。这和第 19–20 课的权证是同一张。

---

## V2 · Guest 怎样碰到设备（第 4–5 课）

一条链：窗口怎么 map 进 Guest → 写寄存器怎样陷入 EL2。

| 课 | 核心知识点（≤2） | 本课不讲 | 入口 |
|----|------------------|----------|------|
| 4 | ① config cache 挂在 memextent 上，hyp `attach` 一份自己看 ② Guest 侧只读 map + `vdevice_attach_phys` | BAR / PCI capability | `hyp/vm/virtio_mmio/src/virtio_mmio.c`；回指第 21 课 hyp_aspace ≠ guest addrspace |
| 5 | ① Guest 写 `QueueNotify` / `Status` 走 vdevice trap ② 落到 `virtio_queue_notify` / `virtio_write_status` | 每个 MMIO 偏移的清单 | `hyp/vm/virtio_mmio/src/vdevice.c` |

第 5 课只要跟 **QueueNotify** 和 **Status** 两格。Features / QueueSel 银行寄存器可以当「同类 trap」一句带过。

---

## V3 · 通知闭环（第 6–7 课）

一条链：前端按铃 → 后端被注入；后端按铃 → 前端被注入。复用第 23、27 课，不是第三套中断。

| 课 | 核心知识点（≤2） | 本课不讲 | 入口 |
|----|------------------|----------|------|
| 6 | ① `virtio_queue_notify` 广播 `virtio_queue_notify` 事件 ② backend 订阅后 `virq_assert` 自己的 source | per-queue MSI-X | `frontend.c` `virtio_queue_notify`；`virtio_backend.c` `handle_virtio_queue_notify` |
| 7 | ① `virtio_backend_notify` hypercall 注入 **前端** vIRQ ② 两根 bind：`virtio_mmio_frontend_bind_virq` vs `virtio_backend_bind_virq` | 所有 notify flags | `virtio_mmio.hvc` `0x4a`；`virtio_backend.hvc` `0x4c` / `0x4e` |

和 doorbell 对照一句即可：**同一 hyp 对象、两边各持 cap、铃是 vIRQ**。差别是中间多了 MMIO trap 和控制面。

---

## V4 · 控制面与队列地址（第 8–9 课）

一条链：status/features 握手 → 队列三地址交给后端。hyp 仍然不读 ring 里的包。

| 课 | 核心知识点（≤2） | 本课不讲 | 入口 |
|----|------------------|----------|------|
| 8 | ① `virtio_status` 握手（ACK → DRIVER → FEATURES_OK → DRIVER_OK） ② reset 靠 `device_needs_reset`，后端 `acknowledge_reset` | 同步 reset 全部分支 | `virtio.tc` 的 `virtio_status`；`virtio_backend_*` features / reset |
| 9 | ① Guest 写下 desc / drv / dev **物理地址** ② 后端 `virtio_backend_get_queue_info` 读走；hyp **不**解析 virtqueue | `virtio_virtq.c`、IOMMU 翻译 | `virtio_queue_info`；`virtio_backend.hvc` `0x52` |

第 9 课要钉死：payload 在两边都 map 过的那页 RAM 上（第 21 课 7 步）。hyp 只当「地址本」。

---

## V5 · 串讲与边界（第 10 课）

| 课 | 核心知识点（≤2） | 本课不讲 | 入口 |
|----|------------------|----------|------|
| 10 | ① 把第 2–9 课串成一条端到端 ② L6 其余方向只点名（PCI / IOMMU / input） | 写完 PCI 或 SMMU | 见下表 |

建议记住的 9 步：

```text
[1] partition_create_virtio_backend → master cap
[2] virtio_backend_configure(me_cap, vqs, MMIO, offset=0x100)
[3] activate → virtio_activate → MMIO startup（vdevice 挂上）
[4] 把同一张 memextent map 进 Frontend VM（只读窗口）
[5] frontend_bind_virq（Guest 的 VIC）
    backend_bind_virq（Backend VM 的 VIC）
[6] 后端 set_dev_features / set_queue_size_max
[7] Guest 驱动走 status 握手，写下三地址
[8] Guest 写 QueueNotify
      → vdevice trap → virtio_queue_notify
      → backend virq_assert
[9] 后端处理共享队列后 virtio_backend_notify
      → frontend virq_assert → Guest 驱动收 used
```

第 10 课末尾只点方向，不要开新课：

| 方向 | 入口 | 和主路径的关系 |
|------|------|----------------|
| PCI 传输 | `hyp/vm/virtio_pci/` | 同一套 `virtio_t`，trap 在 BAR，不是 MMIO `0x100` |
| virtio-iommu | `hyp/vm/virtio_iommu/` + `virtio_virtq/` | hyp **自己当后端**，会读队列；还牵 SMMU |
| virtio-input | `hyp/vm/virtio_input/` | 一种 device_type 的 hyp 侧实现，不是框架 |
| SMMU | `hyp/platform/smmuv3/` | DMA 隔离，另开一条，不要和本大纲缠在一起 |

---

## 和前 27 课怎么接

| 前课 | 本大纲用在哪 |
|------|----------------|
| [第 7–8 课](第7课.md) 事件总线 | `virtio_queue_notify` 是 `trigger_*`，backend / iommu 各自 subscribe |
| [第 20–21 课](第20课.md) | config cache、队列页都是 memextent；hyp attach ≠ Guest map |
| [第 23–24 课](第23课.md) | 两边 notify 的终点都是 `virq_assert`；后端睡着靠 WFI |
| [第 25–26 课](第25课.md) | 全部 `virtio_*` 走 `hvc #0x6000+`，参数是 CapID |
| [第 27 课](第27课.md) doorbell | 同一对象、多份 cap；VirtIO 多了 MMIO 窗口 |

---

## 写法约定（拆正文时用）

和 [大纲.md](大纲.md) 相同：

1. 标题只点本课那 1–2 个知识点。
2. 开头对齐上一课，不超过一小节。
3. 结尾三句话必须对上本表「核心知识点」列。
4. 小练习两题。
5. 需要预备概念时，只回指前 27 课课号，或本大纲第 1 课；不要在代码课里重开一门 VirtIO 规范。

旧 API 名（`virtio_mmio_configure`、`virtio_mmio_backend_assert_virq` 等）在 `docs/api/gunyah_api.md` 里标了 renamed。课文用 **现在的** `virtio_backend_*` / `virtio_mmio_frontend_*`。
