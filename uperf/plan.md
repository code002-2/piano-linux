# Uperf-Linux 调度器 — SM8550 (骁龙 8 Gen 2) 移植计划

## Context

Uperf-Game-Turbo 是一个 Android Magisk 模块，提供用户态 CPU/GPU 性能调度。其核心优势在于：JSON 驱动的 sysfs 调优、场景状态机、触摸感知、功耗模型、任务亲和性管理。

但该项目有两个致命限制使其无法直接在标准 Linux ARM 上运行：
1. **`uperf` 二进制**是 NDK r24 构建的 Android ELF（Bionic libc + `/system/bin/linker64`），且源码未公开
2. **Shell 脚本层**深度绑定 Android 服务、`getprop`、Magisk 生命周期、Android cpuset 命名

小米平板 6S Pro (SM8550) 运行标准 Linux ARM64 时，有完整的 cpufreq-dt、devfreq(msm_dvfs)、uClamp、cgroup v2、evdev 输入子系统可用——这些正是 uperf 的核心调优接口。因此我们不是"移植二进制"，而是**借鉴其设计思想和配置驱动架构，用标准 Linux 工具链从零实现**。

---

## 架构总览

### 进程模型：单一 systemd 守护进程

```
systemd (uperf-linux.service, root)
├── ConfigParser    → 加载/验证 JSON，inotify 热重载
├── InputMonitor    → epoll 监听 /dev/input/event* (evdev)
├── StateEngine     → 场景状态机 (idle/touch/trigger/gesture/junk/switch/boost)
├── SysfsWriter     → 批量去重写入 cpufreq/devfreq/sysfs
├── CgroupManager   → cgroup v2 层级管理 + uClamp 分配
├── HeavyLoadDetector → /proc/stat 轮询 + 负载预测
├── PowerModel      → P-F 曲线评估 + 甜点频率选择
└── GameScanner     → /proc 扫描发现游戏进程
```

### 与 uperf 的模块映射

| uperf 模块 | Linux 等价物 | 改动幅度 |
|-----------|-------------|---------|
| switcher | 相同逻辑，`switchInode` → `/run/uperf-linux/cur_powermode` | 小 |
| input | 完全兼容 Linux evdev (`/dev/input/event*`) | 无 |
| cpu (powerModel) | 完全平台无关，需 SM8750 校准 | 无 |
| sysfs | cpufreq-dt 替代 msm_cpufreq；devfreq 替代 kgsl | 中 |
| sched (affinity/prio) | `pthread_setaffinity_np()` + cgroup v2 替代 cpuset | 大 |
| sfanalysis | **移除**（Android SurfaceFlinger 注入，Linux 无等价物） | 删除 |
| cgroup 管理 | cgroup v2 层级替代 `/dev/cpuset/` | 大 |
| 内核 boost 禁用 | 禁用 OEM 竞争服务（MIUI/OPLUS/samsung） | 中 |

---

## 实现计划

### Phase 1: 基础框架

**1.1 项目结构初始化**
```
uperf-linux/
├── CMakeLists.txt
├── CMakeToolchain-aarch64.cmake
├── src/
│   ├── main.c              # 入口、事件循环(epoll)、信号处理
│   ├── config_parser.c     # JSON 解析(json-c)、验证、热重载
│   ├── state_machine.c     # FSM 状态机、timerfd 超时
│   ├── input_monitor.c     # evdev 触摸事件解析
│   ├── sysfs_writer.c      # 批量去重 sysfs 写入
│   ├── cgroup_manager.c    # cgroup v2 层级创建、任务分配
│   ├── heavyload_detector.c # /proc/stat 采样、负载计算
│   ├── power_model.c       # P-F 曲线、甜点频率选择
│   ├── game_scanner.c      # /proc 扫描、游戏进程匹配
│   ├── log.c               # 结构化日志
│   └── include/
│       ├── config.h
│       ├── state_machine.h
│       ├── input_monitor.h
│       ├── sysfs_writer.h
│       ├── cgroup_manager.h
│       ├── heavyload_detector.h
│       ├── power_model.h
│       ├── game_scanner.h
│       └── log.h
├── config/
│   ├── sm8550.json         # SM8550 默认配置
│   ├── perapp_powermode    # 按应用分配电源模式
│   └── templates/
│       ├── balance.json
│       ├── powersave.json
│       └── performance.json
├── systemd/
│   └── uperf-linux.service
├── tests/
│   ├── test_config_parser.c
│   ├── test_state_machine.c
│   ├── test_power_model.c
│   └── test_sysfs_writer.c
└── README.md
```

**1.2 CMakeLists.txt**
- 依赖: `json-c` (JSON 解析), `pthread`, `rt` (timerfd/clock_gettime)
- C17 标准, GCC 11+/Clang 14+
- ARM64 原生构建 + x86_64 交叉编译工具链

**1.3 日志系统 (`log.c`)**
- 分级: DEBUG/INFO/WARN/ERROR/FATAL
- 输出: journald (systemd) + 可选文件日志 `/var/log/uperf-linux/`
- SIGHUP 触发日志级别动态调整

### Phase 2: 核心引擎

**2.1 配置系统 (`config_parser.c`)**
- 加载 JSON 配置，验证 schema 完整性
- 检查所有 sysfs 路径是否存在且可写
- inotify 监听配置文件变更 → 子进程验证 → 原子替换配置指针
- 错误时回退到上一份有效配置

**2.2 状态机 (`state_machine.c`)**
- 状态: `IDLE → TOUCH → TRIGGER → GESTURE → JUNK → SWITCH → BOOST`
- 转换触发: 输入事件、timerfd 超时、负载检测、cgroup 变化
- 每个状态关联一组 `ActionParams`（见 Phase 3）
- 防抖: `requestBurstSlack` 冷却期防止快速振荡

**2.3 输入监控 (`input_monitor.c`)**
- 枚举 `/dev/input/event*`，通过 ioctl 识别触摸屏设备
  - `EVIOCGBIT(EV_KEY)` → `BTN_TOUCH`
  - `EVIOCGBIT(EV_ABS)` → `ABS_MT_POSITION_X/Y`, `ABS_PRESSURE`
- epoll 多路复用所有触摸屏事件
- 触摸跟踪器: 计算滑动距离、速度、手势阈值判断
  - `swipeThd`: 滑动判定阈值
  - `gestureThdX/Y`: 边缘手势判定
  - `holdEnterTime`: 长按判定

**2.4 Sysfs 写入器 (`sysfs_writer.c`)**
- 批量写入: 收集变更 → 1ms 窗口 → 去重 → 统一写入
- 去重: 比较 last_written_value，跳过无变化写入
- 容错: `ENOENT` (路径不存在) → WARN 跳过; `EACCES` → FATAL 退出
- 支持的路径类型:
  - `PERCPU`: `/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq`
  - `PERCLUSTER`: 同簇内所有 CPU 同步写
  - `DEVFREQ`: `/sys/class/devfreq/*/max_freq`
  - `UCLAMP`: `/sys/fs/cgroup/*/cpu.uclamp.min`
  - `STRING`: 字符串写入 (如 `performance`/`schedutil`)

### Phase 3: 调度与资源管理

**3.1 功耗模型 (`power_model.c`)**
- 每个 CPU 簇定义: `efficiency`, `nr_cores`, `typicalPower`, `sweetFreq`, `plainFreq`, `freeFreq`
- 负载计算: `Σ efficiency[i] × (load_pct[i]/100) × (freq_MHz[i]/1000)`
- 甜点频率选择: 在满足目标性能前提下找到最低功耗频率点
- 功率预算: `slowLimitPower` / `fastLimitPower` 控制响应策略

**3.2 重度负载检测 (`heavyload_detector.c`)**
- 每 `baseSampleTime` (默认 10ms) 采样 `/proc/stat`
- 计算系统负载，与 `heavyLoad` 阈值比较
- 进入 BOOST 状态: 负载持续高于阈值 → 激进调频
- 退出条件: 负载低于 `idleLoad` 持续 1 秒 + `requestBurstSlack` 冷却

**3.3 cgroup 管理器 (`cgroup_manager.c`)**
- 创建 cgroup v2 层级: `/sys/fs/cgroup/uperf-linux.slice/`
- 子切片: `game.slice`, `system.slice`, `background.slice`
- 每个切片配置:
  - `cpuset.cpus`: CPU 亲和掩码 (集群绑定)
  - `cpu.weight`: 公平共享权重
  - `cpu.uclamp.min/max`: 利用率钳制
- 游戏进程自动分配到 `game.slice`
- 降级策略: cgroup 不可用时 → `prctl(PR_SET_UCLAMP_THRESHOLD)` 每线程 uClamp

**3.4 游戏扫描器 (`game_scanner.c`)**
- 启动时扫描 `/proc/*/comm` + `/proc/*/cmdline`
- 正则匹配游戏进程: UnityMain, GameThread, RenderThread, Wine, Proton, Dolphin, RetroArch 等
- 运行时持续监控 cgroup.procs 变化 → 新进程检测
- 按进程名匹配 `perapp_powermode` 规则

### Phase 4: SM8550 配置

**4.1 SM8550 硬件特征**
- 8 核 ARM big.LITTLE: **1+2+5** 三簇
  - **Prime** (cpu0): 1x Cortex-X3 @ **3.2 GHz**
  - **Performance** (cpu1-2): 2x Cortex-A715 @ **2.8 GHz**
  - **Efficiency** (cpu3-7): 5x Cortex-A510 @ **2.0 GHz**
- cpufreq driver: `cpufreq-dt` (主线路径，Linux 6.3+ 稳定支持)
- devfreq: `gpu`, `cpu-mem`, `soc` (通过 `msm_dvfs` + `qcom-cpumem`)
- uClamp: 完整支持 (schedutil governor)
- APCS GLB clock: 簇间通信同步电压/频率

**4.2 sysfs 路径定义**
```
# CPU 频率 (cpufreq-dt, 所有 CPU)
/sys/devices/system/cpu/cpu{0-7}/cpufreq/scaling_{min,max,cur}_freq
/sys/devices/system/cpu/cpu{0-7}/cpufreq/scaling_available_frequencies
/sys/devices/system/cpu/cpu{0-7}/cpufreq/scaling_governor
/sys/devices/system/cpu/cpu{0-7}/cpufreq/scaling_driver  # → cpufreq-dt

# schedutil 调参 (部分内核版本支持)
/sys/devices/system/cpu/cpu0/cpufreq/schedutil/up_rate_limit_us
/sys/devices/system/cpu/cpu0/cpufreq/schedutil/down_rate_limit_us
/sys/devices/system/cpu/cpu0/cpufreq/schedutil/hispeed_freq

# GPU devfreq (msm_dvfs)
/sys/class/devfreq/soc\:qcom\:gpu/available_frequencies
/sys/class/devfreq/soc\:qcom\:gpu/min_freq
/sys/class/devfreq/soc\:qcom\:gpu/max_freq
/sys/class/devfreq/soc\:qcom\:gpu/governor
/sys/class/devfreq/soc\:qcom\:gpu/governor_params/target_level

# CPU-MEM devfreq (qcom-cpumem, 基于 L3 cache 统计调 DDR 频率)
/sys/class/devfreq/soc\:qcom\:cpu-cpu-llcc-bw/{min,max}_freq
/sys/class/devfreq/soc\:qcom\:cpu-llcc-ddr-bw/{min,max}_freq

# SOC devfreq (片上互连带宽)
/sys/class/devfreq/soc\:qcom\:soc/{min,max}_freq
```

**4.3 SM8550 JSON 配置 (`config/sm8550.json`)**
- 完整的 3 簇 powerModel (Prime/Performance/Efficiency)
- 三种预设: balance, powersave, performance
- sched.rules: 桌面 WM、游戏引擎、浏览器合成器、后台守护进程的亲和性/优先级规则
- 初始值: 锁定 cpufreq max 到最高频率，devfreq 全开

**4.4 电源模式切换**
- 文件触发: `echo "performance" > /run/uperf-linux/cur_powermode`
- 可选: DBus 接口供 CLI 工具 `uperfctl set-mode performance` 调用

### Phase 5: 部署与系统集成

**5.1 systemd service**
```ini
[Service]
ExecStart=/usr/local/bin/uperf-linux --config /etc/uperf-linux/config.json
CapabilityBoundingSet=CAP_SYS_NICE CAP_SYS_RESOURCE CAP_DAC_OVERRIDE CAP_BLOCK_SUSPEND
Delegate=yes
Slice=uperf-linux.slice
```

**5.2 竞品服务禁用**
- 检测并停止: `thermald` (过度节流时可调优而非禁用), `auto-cpufreq`, `cpupower` 后台服务
- OEM 特定: MIUI migt sysfs 覆盖, OPLUS scheduler boost
- 设置 cpufreq governor 为 `schedutil` (而非 `performance` 常驻满频)

**5.3 CLI 工具 (`uperfctl`)**
- `uperfctl status` — 当前状态和负载
- `uperfctl mode <balance|powersave|performance>` — 切换电源模式
- `uperfctl game-list` — 已识别的游戏进程
- `uperfctl log-level <debug|info|warn>` — 动态调整日志

---

## 关键技术决策

| 决策 | 选择 | 理由 |
|------|------|------|
| 语言 | C17 | 与 uperf 一致，零开销，内核接口直接访问 |
| JSON 库 | json-c | 轻量、标准 Linux 包、无需额外依赖 |
| 构建 | CMake 3.20+ | 跨平台、pkg-config 集成、systemd 安装 |
| 进程模型 | 单二进制守护进程 | 简化部署，避免 shell 胶水代码 |
| 事件循环 | epoll + timerfd | 高效多路复用，替代 Android 的 Looper |
| 配置热重载 | inotify + 子进程验证 | 安全验证后原子替换，不影响主循环 |
| 任务调度 | cgroup v2 + prctl | 现代 Linux 标准，降级到每线程 uClamp |
| 触摸输入 | evdev ioctl | 标准 Linux 输入子系统，与 uperf 逻辑一致 |

---

## 验证方法

1. **单元测试**: config_parser, state_machine, power_model, sysfs_writer 独立测试
2. **系统诊断脚本**: 自动检测 SoC、CPU 拓扑、cpufreq driver、devfreq 设备、触摸屏
3. **手动测试**:
   - `echo "performance" > /run/uperf-linux/cur_powermode` → 验证频率锁定
   - 运行游戏 → 验证进程被自动识别并分配到 game.slice
   - 触摸屏幕 → 验证状态机从 idle→touch→trigger 正确转换
   - 负载测试 (stress-ng) → 验证 heavyLoad 检测和 boost 切换
4. **对比测试**: 开启/关闭 uperf-linux 运行同一游戏，记录帧率波动和功耗

---

## 风险与缓解

| 风险 | 影响 | 缓解 |
|------|------|------|
| SM8550 cpufreq-dt 在主线路径完全支持 (Linux 6.3+) | 极低 | 主线路径成熟，风险很小 |
| devfreq 节点路径因内核版本不同而异 | 中 | 启动时自动探测 devfreq 设备，未找到则跳过 |
| cgroup v2 在某些发行版未启用 | 中 | 降级到 per-thread `prctl(PR_SET_UCLAMP_*)` |
| 触摸事件解析在非 Android Linux 格式差异 | 低 | 提供 `input.gestureThdX/Y` 可配置阈值 |
| 功耗模型参数需要实测校准 | 中 | 提供保守默认值，用户可通过配置调整 |
