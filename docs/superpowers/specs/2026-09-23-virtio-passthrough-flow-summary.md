# Virtio / 直通 / 驱动：完整流程复习

> 整理自 2026-09-23 讨论，便于细读。  
> 依据：本机 `gunyah-hypervisor`、86 上 `gunyah-src/resource-manager`、xhyper 设计稿与仓库现状。  
> 配套概念笔记：`2026-09-23-virtio-iommu-study-notes.md`

---

## 0. 先记住三句话

1. **多 VM 同时跑很常见**；和「一台物理设备通常只给一个 VM」不矛盾。  
2. **大家都要上网/用盘** → 主流用 **virtio 仿真**（软件复用 Host 上的真网卡/磁盘）。  
3. **直通** → 少数 VM 独占真设备 + 原厂驱动；不是每人一张物理卡的解法。

---

## 1. 系统里有谁

| 角色 | 干什么 |
|------|--------|
| **EL2 Hypervisor** | 管 VM、trap、SMMU、virtio 框架（状态机/MMIO/通知桥） |
| **Root VM = Resource Manager (RM)** | 管家：创建 HLOS/secondary、划内存、创建 virtio 对象、执行直通分配 |
| **HLOS (primary)** | 主业务 Linux，常先「看见」整机设备 |
| **Secondary Guest** | 其它 Linux VM |
| **Host 用户态 VMM/backend**（如 crosvm） | 实现 virtio-blk/net 等，接到 Host 文件/网络 |

你们产品线：**xhyper**（EL2）+ **xhyper-mgr**（对标 RM）+ **xhyper-abi**（合同）；对标 Gunyah，进度不同。

---

## 2. 两条路（不要混）

### 路 A：Virtio 仿真（假设备）

```text
Guest 标准 virtio 驱动
  → virtio 协议（MMIO + virtqueue）
  → Frontend/框架（EL2 或 exit 到 VMM）
  → Backend（软件）真正读盘/收发包
  → Host 上的文件、tap、真网卡驱动等
```

- Guest **不需要** NVMe/网卡原厂驱动  
- Guest 用发行版自带的 `virtio-blk` / `virtio-net`  
- 多 guest 可同时用「各自的虚盘/虚网卡」

### 路 B：设备直通（真设备）

```text
RM 把设备从 HLOS 撕下 → 划给某一个 secondary
  → 该 Guest 加载原厂驱动
  → 硬件自己 DMA
  → （通常）virtio-iommu + SMMUv3 做 DMA 隔离
```

- Guest **需要** 原厂驱动  
- **通常不用** virtio-blk/net  
- virtio 若出现，多半是 **virtio-iommu（控制面）**

---

## 3. 概念速查（驱动相关）

| 词 | 含义 | 谁写/谁用 |
|----|------|-----------|
| **virtio** | guest↔hypervisor 的标准设备协议 | 规范；guest 有标准驱动 |
| **backend** | 协议背后真正干活的实现 | crosvm / Host 进程 / 少数在 EL2（如 input） |
| **frontend** | guest 侧看到的设备 + hyp 内状态机/MMIO | EL2 框架或 VMM |
| **DMA** | 设备自己搬内存 | 硬件或 backend 按地址访问 guest 内存 |
| **IOMMU/SMMU** | 设备端 MMU：IOVA→PA + 权限 | 硬件 + 驱动/EL2 编程 |
| **MMIO** | CPU 用访存方式访问设备寄存器 | 配置、kick 队列 |
| **直通** | 真设备独占给某个 VM | RM 改映射归属 |

**virtio 定格式；backend 定怎么完成（软件或接到真硬件能力）。**

---

## 4. 流程一：一块 virtio 虚盘（所有 guest 都能用盘）

### 目标
每个 guest 看到自己的 virtio-blk，读写镜像文件或 Host 块设备；**不是**每人直通一块物理盘。

### 谁提供驱动
| 位置 | 驱动 |
|------|------|
| Guest | Linux **自带** `virtio-blk`（不用你写） |
| Backend | crosvm / QEMU / 自研 Host backend（实现 virtio-blk 设备模型） |
| 物理盘（若 backend 接到真盘） | **Host/HLOS** 上的 NVMe/SCSI 驱动 |

### 数据怎么走（逻辑）

```text
1. Guest virtio-blk 驱动：在内存摆好请求描述符，写 queue_notify
2. 通路：
   - 路径 B（xhyper 现状）：MMIO trap → exit 到 crosvm
   - 路径 A（设计中）：MMIO → EL2 框架 → 通知 Host backend（ABI）
3. Backend：读描述符 → 读/写镜像或 Host 块设备 → 写 used ring → 注入中断
4. Guest：收中断，完成 I/O
```

### 多 guest
每个 guest 一个（或多个）virtio-blk 实例；可共用同一 Host 磁盘上的不同镜像文件，或不同分区。  
**Backend 用软件复用**，不要求「一 guest 一物理盘」。

### Gunyah OSS 依据（边界）
- EL2 有框架 + HYPERCALL 桥，**无** virtio-blk backend 实现  
- 生产上 blk 的 backend 在桥对面（HLOS/用户态）；RM 提供创建对象和 hypercall 封装  

### xhyper 现状
- **路径 B 已验证** virtio-blk（crosvm）  
- **路径 A**：设计为 Stage 3 Host 精简 blk backend + 数据面 ABI；**不要**在 EL2/mgr 里写存储驱动栈  

---

## 5. 流程二：所有 guest 都要上网（virtio-net）

### 目标
大家都上网，但通常 **只有一张（几张）物理网卡**。

### 正确做法（默认）

```text
物理网卡 ← Host 网卡驱动（Host/HLOS 上，不用你为 guest 写）
     ↑
 Host 网桥 / NAT / tap
     ↑
 GuestA virtio-net | GuestB virtio-net | GuestC virtio-net
 （各用 Linux 自带 virtio-net 驱动）
```

「软件复制成多份 virtio-net」= **给每人一张假网卡**，由 backend + Host 网络栈转发到真网卡。  
**不是**复制物理网卡硬件，**不是**为每个 guest 写原厂网卡驱动。

### Backend 够不够？
**主体是 virtio-net backend**，但还要：

- Frontend/框架通路（kick 能到 backend）  
- 给 VM 挂上设备（配置）  
- Host 侧 tap/网桥/NAT  
- 中断/通知通路  

路径 B：crosvm 已具备这类能力，多为配置问题。  
路径 A：若做 net，同样倾向 **Host backend**，不是 EL2 里写网卡驱动。

### 进阶
- 要接近直通性能且网卡支持 → **SR-IOV**（一卡多 VF，每 VF 直通给一个 VM）  
- 某一 VM 必须独享整卡 → 直通给它；其它 VM 仍走 virtio-net  

---

## 6. 流程三：设备直通（真网卡/真盘进某一个 guest）

### 「RM 从 HLOS 撕下划给 secondary」什么意思
不是拔硬件，是 **改地址空间归属**：

1. 配置：设备 X 的 MMIO 段 + IRQ → 某个 secondary 的 VMID  
2. RM：从 **HLOS** 的 IO 映射里 **unmap** 这些范围（HLOS 不能再碰）  
3. 再映射进 **该 secondary**  
4. 该 guest 加载 **原厂驱动**，当本地设备用  

**一台物理设备通常同时只属于一个 VM**（这个 VM 可以是 HLOS 或某一个 guest）。  
多个 guest 可以各自拿 **不同的** 设备；不能默认「同一套寄存器给两个 guest 同时开驱动」。

### 和 virtio-iommu 的关系

```text
Guest 原厂驱动发 DMA（描述符里常是 IOVA）
  → 物理设备 DMA
  → SMMUv3 翻译/检查
  → 打到该 guest 内存

控制面：
Guest virtio-iommu 驱动 ATTACH/MAP/…
  → EL2 virtio_iommu
  → smmuv3_attach_stream / TLBI / sync
```

- 数据面：硬件 DMA，EL2 不逐包搬  
- 控制面：virtio-iommu 管映射  

### 多 VM 会不会冲突？
- 多 VM 同时跑：**不会**因为直通规则而禁止  
- 冲突来自：**同一物理设备被多个 VM 同时独占编程**  
- 解决：不同设备分给不同 VM；或 SR-IOV；或多数 VM 走 virtio  

### 可行性（依据仓库）
| 环境 | 直通 |
|------|------|
| 仅本机 `gunyah-hypervisor` | 有 EL2 iommu/SMMU/vPCI 积木；缺 RM 编排 |
| 86 `gunyah-src`（hyp + RM + n90-acpi-passthrough） | Root VM、passthrough 配置、virtio-iommu 创建、N90 资源表（含 SMMU/PCI 等）**有** |
| QEMU virt / 无 SMMU 平台 | 一般 **不是** 直通验证面 |
| xhyper 设计 | virtio-iommu 等有真 SMMU 再评估；当前主线是 virtio 仿真路径 |

---

## 7. 「要不要写驱动」总表

| 场景 | Guest 驱动 | EL2 | Host/RM |
|------|------------|-----|---------|
| virtio 虚盘/虚网 | 自带 virtio-blk/net | 框架或 exit；**不写** NVMe/网卡驱动 | backend（crosvm 等）+ 可选 Host 块/网驱动 |
| 直通网卡/盘 | **原厂**驱动 | SMMU/iommu/vPCI 等 | RM 做归属；Host 不再占用该设备 |
| virtio-iommu | 自带 virtio-iommu | **有** backend（Gunyah） | RM 创建/配置对象 |
| EL2 内 console/input 类 | 标准 virtio | 可以 EL2 内 backend | 配置/激活 |

**结论：多 guest 共享上网用盘 → 几乎不用写新驱动，要的是 backend + 配置；直通 → guest 用原厂驱动，RM/hyp 管归属和 IOMMU。**

---

## 8. xhyper 两条路径（现状）

| | 路径 B（活跃） | 路径 A（设计复活中） |
|--|----------------|----------------------|
| Virtio 谁实现 | crosvm 全套 | EL2 框架 +（blk）Host backend |
| 现状 | phase-d4-2 等已验 blk | ABI 已齐；EL2 handler 未落地 |
| 和直通 | 不是主路径 | 非当前 Stage 目标 |

设计选择：**blk 不在 EL2/mgr 里写存储栈**；console 可放 EL2（对标 Gunyah input）。

---

## 9. 一张总图（放在脑子里）

```text
                    ┌─────────────────────────────────────┐
                    │            多 VM 同时运行            │
                    └─────────────────────────────────────┘
                                      │
          ┌───────────────────────────┴───────────────────────────┐
          ▼                                                       ▼
 ┌─────────────────────┐                             ┌─────────────────────┐
 │  路：Virtio 仿真     │                             │  路：设备直通         │
 │  （默认，可一对多）   │                             │  （独占，一对一）     │
 ├─────────────────────┤                             ├─────────────────────┤
 │ Guest: virtio 驱动  │                             │ Guest: 原厂驱动      │
 │ Backend: 软件       │                             │ RM: 从 HLOS 撕下设备 │
 │ Host: 真网卡/镜像   │                             │ DMA: SMMU + 常配     │
 │                     │                             │      virtio-iommu    │
 └─────────────────────┘                             └─────────────────────┘
```

---

## 10. 细读时可用的自问清单

1. 我现在说的是 **仿真** 还是 **直通**？  
2. 驱动在 **guest / EL2 / Host** 哪一层？要不要新写？  
3. 多 guest 靠的是 **软件复用（virtio）** 还是 **硬件切分（SR-IOV）** 还是 **不同设备各分一台**？  
4. 这条在 **Gunyah OSS / RM / xhyper** 哪一层已经有代码？  

---

*文档结束。*
