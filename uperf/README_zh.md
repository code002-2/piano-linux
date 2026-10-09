# uperf-linux

> Linux ARM64 设备的用户态性能调度器，专为游戏优化。

## 概述

`uperf-linux` 是一个由 systemd 管理的守护进程，附带 GTK4/libadwaita 图形界面，为 Linux ARM64 游戏设备提供精细的 CPU/GPU 调度。采用 JSON 驱动的配置文件、场景化状态机、触摸感知调速和功耗模型优化。

- **JSON 配置驱动** — 直接写入 sysfs 节点（cpufreq、devfreq、uClamp）
- **场景化状态机** — idle → touch → trigger → gesture → junk → switch → boost
- **触摸感知调速** — 从触摸屏读取滑动/手势事件，即时提升性能
- **功耗模型** — 基于 P-F 曲线寻找每个簇的"甜点频率"
- **重载检测** — 持续高负载时自动切换到激进调度策略
- **任务亲和性管理** — 通过 cgroup v2 将游戏线程绑定到性能核心

## 架构

```
┌──────────────────────────────────────────────────────────────┐
│                    systemd (PID 1)                           │
│  uperf-linux.service  (After=multi-user.target)              │
└──────────────────────────┬───────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────┐
│                    uperf-linux 守护进程 (root)                 │
│                                                              │
│  ConfigParser ←→ JSON 配置（inotify 热重载）                 │
│  InputMonitor ←→ /dev/input/event*（evdev, epoll）           │
│  StateEngine  ←→ FSM 状态机（timerfd 超时）                  │
│  SysfsWriter  ←→ 批量去重写入 sysfs                          │
│  CgroupMgr    ←→ cgroup v2 切片 + uClamp                     │
│  HeavyLoad    ←→ /proc/stat 轮询                             │
│  GameScanner  ←→ /proc/*/comm + /proc/*/cmdline 匹配         │
│  DBusManager  ←→ org.uperflinux.Daemon（system bus）         │
└──────────────────────────────────────────────────────────────┘
```

## GUI（GTK4/libadwaita）

平板友好的 GTK4/libadwaita 图形控制器，通过 **DBus** 与守护进程双向通信：

```bash
# 启动 GUI
uperf-gui
```

### 功能
- **仪表盘** — 电源模式按钮、实时 CPU 频率显示、负载仪表、场景指示器
- **游戏列表** — 已检测到的游戏进程，支持按应用分配模式
- **设置** — 阈值输入（HeavyLoad 阈值、采样时间、余量、突发强度、功耗预算、热管理）
- **日志** — 守护进程实时日志查看器
- **频率覆盖** — 手动锁定 CPU/GPU 频率

### DBus 接口
守护进程在 system bus 上暴露 `org.uperflinux.Daemon`：
```bash
# 查询当前模式
dbus-send --system --dest=org.uperflinux.Daemon --print-reply \
  /org/uperflinux/Daemon org.freedesktop.DBus.Properties.Get \
  string:'org.uperflinux.Daemon' string:'CurrentMode'

# 切换模式
dbus-send --system --dest=org.uperflinux.Daemon --print-reply \
  /org/uperflinux/Daemon org.uperflinux.Daemon.SetMode string:'performance'
```

## 支持平台

| 平台 | SoC | cpufreq 驱动 | devfreq | 状态 |
|------|-----|-------------|---------|------|
| 小米平板 6S Pro | SM8550 (骁龙 8 Gen 2) | cpufreq-dt | msm_dvfs | ✅ 主要目标 |
| 其他骁龙平台 | 各种型号 | cpufreq-dt | 视情况而定 | ⚠️ 需要配置 |

## 快速开始

### 前置依赖

```bash
# Debian/Ubuntu
sudo apt install cmake pkg-config libjson-c-dev libglib2.0-dev libsystemd-dev \
    libgtk-4-dev libadwaita-1-dev

# Arch Linux
sudo pacman -S cmake json-c systemd-libs glib2 gtk4 libadwaita

# Fedora
sudo dnf install cmake pkg-config json-c-devel glib2-devel systemd-devel \
    gtk4-devel libadwaita-devel
```

### 编译

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### 安装

```bash
# 以 root 身份执行
sudo cp uperf-linux /usr/local/bin/
sudo cp uperfctl /usr/local/bin/
sudo mkdir -p /etc/uperf-linux
sudo cp ../config/sm8550.json /etc/uperf-linux/config.json
sudo cp ../config/perapp_powermode /etc/uperf-linux/
sudo mkdir -p /run/uperf-linux
sudo cp ../systemd/uperf-linux.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now uperf-linux.service

# GUI
sudo cp uperf-gui /usr/local/bin/
sudo desktop-file-install gui/uperf-gui.desktop
```

### 从 deb 安装

```bash
sudo dpkg -i uperf-linux-gui_0.1.0_arm64.deb
sudo systemctl enable --now uperf-linux
```

### 命令行用法

```bash
# 查看状态
sudo uperfctl status

# 切换电源模式
sudo uperfctl mode performance    # 全速模式
sudo uperfctl mode balance        # 均衡模式（默认）
sudo uperfctl mode powersave      # 省电模式

# 列出检测到的游戏进程
sudo uperfctl game-list

# 查看日志
journalctl -u uperf-linux -f
```

## 配置文件

主配置文件位于 `/etc/uperf-linux/config.json`。关键配置段：

### 功耗模型 (Power Model)

定义每个 CPU 簇的性能特征：

```json
"powerModel": [
  {
    "efficiency": 350,        /* 相对 IPC 分数（Cortex-A53@1GHz = 100）*/
    "nr": 1,                  /* 该簇的核心数量 */
    "typicalPower": 1.2,      /* 典型频率下单核功耗（瓦）*/
    "typicalFreq": 2400,      /* 正常工作频率（MHz）*/
    "sweetFreq": 1800,        /* 能效最佳频率（MHz）*/
    "plainFreq": 1400,        /* 线性区域边界（MHz）*/
    "freeFreq": 600           /* 最低有效频率（MHz）*/
  }
]
```

### Sysfs 节点

将节点名称映射到 sysfs 路径。`%d` 会按 CPU 编号展开：

```json
"knob": {
  "cpufreqMax": "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq",
  "gpuMaxFreq": "/sys/class/devfreq/soc\\:qcom\\:gpu/max_freq"
}
```

### 预设 (Presets)

三种电源模式，每种包含逐场景覆盖：

```json
"presets": {
  "balance": {
    "*": { "cpu.margin": 0.2 },
    "idle": { "cpu.baseSampleTime": 0.04 },
    "touch": { "cpu.margin": 0.4 }
  }
}
```

## 许可证

本项目采用 **GNU GPL v3** 许可证。详见 [LICENSE](LICENSE) 文件。

允许自由使用、修改和分发，但衍生作品也必须以相同的许可证开源。

## 开发路线图

- [x] 项目脚手架 + CMake 构建系统
- [x] 日志子系统（journald + 文件 + 标准错误输出）
- [x] JSON 配置解析器（含验证 + 热重载）
- [x] Sysfs 写入器（批量写入 + 去重）
- [x] 功耗模型（P-F 曲线、甜点频率选择）
- [x] 状态机（7 种场景、基于 timerfd 的转换）
- [x] 输入监控（evdev 触摸事件解析）
- [x] cgroup v2 管理器（切片、uClamp、CPU 亲和绑定）
- [x] 重载检测（/proc/stat 轮询）
- [x] 游戏进程扫描器（/proc 扫描）
- [x] SM8550 默认配置
- [x] CLI 工具（uperfctl）
- [x] systemd 服务单元
- [x] DBus 接口（org.uperflinux.Daemon）
- [x] GTK4/libadwaita GUI（仪表盘、游戏列表、设置、日志、频率覆盖）
- [x] deb 打包（dpkg-deb）
- [x] 单元测试（扩展覆盖率）
- [x] 热管理（读取 /sys/class/thermal/，频率限制，DBus 暴露）
- [x] 按应用电源模式自动切换（perapp_powermode 解析器，游戏扫描器集成，GUI 接线）
- [x] SoC 配置向导（uperf-wizard detect，uperfctl detect）
- [x] 手动 CPU/GPU 频率覆盖（CLI + GUI + DBus）
