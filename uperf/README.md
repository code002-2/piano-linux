# uperf-linux

> Userspace performance scheduler for Linux ARM64 gaming devices.

[中文文档](README_zh.md) | [License](LICENSE) | [GitHub](https://github.com/code002-2/uperf-linux)

## Overview

`uperf-linux` is a systemd-managed daemon with a GTK4/libadwaita GUI that provides CPU/GPU frequency control
for gaming on Linux ARM64 devices. It uses a JSON-driven configuration approach with scene-based state machines,
touch-aware pacing, and power model optimization.

- **JSON-config-driven** cpufreq policy control with validated hardware limits
- **Scene-based state machine** (idle → touch → trigger → gesture → junk → switch → boost)
- **Touch-aware pacing** — reacts to swipe/gesture events from the touchscreen
- **Power model** — finds the "sweet spot" on the P-F curve for each cluster
- **HeavyLoad detection** — automatic boost mode when sustained load exceeds threshold
- **Per-app modes** — applies configured power modes when matching games start and stop

## Architecture

```
┌──────────────────────────────────────────────────────────────┐
│                    systemd (PID 1)                           │
│  uperf-linux.service  (Type=dbus, system bus)                │
└──────────────────────────┬───────────────────────────────────┘
                           │
                           ▼
┌──────────────────────────────────────────────────────────────┐
│                    uperf-linux daemon (root)                 │
│                                                              │
│  ConfigParser ←→ JSON config (SIGHUP/DBus reload)            │
│  InputMonitor ←→ /dev/input/event* (evdev, epoll)            │
│  StateEngine  ←→ FSM with monotonic timeouts                 │
│  FreqControl  ←→ cpufreq policies + power model              │
│  HeavyLoad    ←→ /proc/stat polling                          │
│  GameScanner  ←→ /proc/*/comm + /proc/*/cmdline matching     │
│  DBusManager  ←→ org.uperflinux.Daemon (system bus)          │
└──────────────────────────────────────────────────────────────┘
```

## GUI (GTK4/libadwaita)

A tablet-friendly GTK4/libadwaita graphical controller communicates with the daemon over **DBus**:

```bash
# Launch the GUI
uperf-gui
```

### Features
- **Dashboard**: Power mode buttons, real-time CPU frequency display, load meters, scene indicator
- **Games**: Detected game process list with per-app mode assignment
- **Configuration**: Reload `/etc/uperf-linux/config.json` without restarting the daemon
- **Logs**: Latest 200 service journal entries
- **Frequency Override**: Manual CPU/GPU frequency locking

### DBus Interface
The daemon exposes `org.uperflinux.Daemon` on the system bus:
```bash
# Query current mode
dbus-send --system --dest=org.uperflinux.Daemon --print-reply \
  /org/uperflinux/Daemon org.freedesktop.DBus.Properties.Get \
  string:'org.uperflinux.Daemon' string:'CurrentMode'

# Set mode
dbus-send --system --dest=org.uperflinux.Daemon --print-reply \
  /org/uperflinux/Daemon org.uperflinux.Daemon.SetMode string:'performance'
```

## Supported Platforms

| Platform | SoC | cpufreq driver | devfreq | Status |
|----------|-----|----------------|---------|--------|
| Xiaomi Pad 6S Pro | SM8550 (SD 8 Gen 2) | cpufreq-dt | msm_dvfs | ✅ Primary target |
| Other Snapdragon | Various | cpufreq-dt | varies | ⚠️ Config required |

## Quick Start

### Prerequisites

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

### Build

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
make -j$(nproc)
```

### Install

```bash
# As root
sudo cp uperf-linux /usr/bin/
sudo cp uperfctl /usr/bin/
sudo mkdir -p /etc/uperf-linux
sudo cp ../config/sm8550.json /etc/uperf-linux/config.json
sudo cp ../config/perapp_powermode /etc/uperf-linux/
sudo mkdir -p /run/uperf-linux
sudo cp ../systemd/uperf-linux.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable --now uperf-linux.service

# GUI
sudo cp uperf-gui /usr/bin/
sudo desktop-file-install gui/uperf-gui.desktop
```

### Install from deb

```bash
sudo dpkg -i uperf-linux-gui_0.1.0_arm64.deb
sudo systemctl enable --now uperf-linux
```

### Usage

```bash
# Check status
uperfctl status

# Switch power mode
uperfctl mode performance    # Full throttle
uperfctl mode balance        # Balanced (default)
uperfctl mode powersave      # Battery saving

# List detected game processes
uperfctl game-list

# View logs
journalctl -u uperf-linux -f
```

## Configuration

The main config file is `/etc/uperf-linux/config.json`. Key sections:

### Power Model

Defines per-cluster performance characteristics:

```json
"powerModel": [
  {
    "efficiency": 350,        /* Relative IPC score (Cortex-A53@1GHz = 100) */
    "nr": 1,                  /* Number of cores in this cluster */
    "cpus": [7],              /* Exact CPUs in this cpufreq policy */
    "typicalPower": 1.2,      /* Single-core power at typicalFreq (W) */
    "typicalFreq": 2400,      /* Normal operating frequency (MHz) */
    "sweetFreq": 1800,        /* Most energy-efficient frequency (MHz) */
    "plainFreq": 1400,        /* Linear region boundary (MHz) */
    "freeFreq": 600           /* Minimum efficient frequency (MHz) */
  }
]
```

Each enabled CPU model must identify its policy explicitly. Use a direct
`"cpus": [...]` array as above, or `"cpumask": "prime"` to reference a
named array under `modules.sched.cpumask`. The daemon matches this mask to
the kernel policy's `related_cpus`; power-model array order is not used to
infer topology.

### Sysfs Knobs

Maps knob names to sysfs paths. `%d` is expanded per-CPU:

```json
"knob": {
  "cpufreqMax": "/sys/devices/system/cpu/cpu%d/cpufreq/scaling_max_freq",
  "gpuMaxFreq": "/sys/class/devfreq/soc\\:qcom\\:gpu/max_freq"
}
```

### Presets

Three power modes, each with per-scene overrides:

```json
"presets": {
  "balance": {
    "*": { "cpu.margin": 0.2 },
    "idle": { "cpu.baseSampleTime": 0.04 },
    "touch": { "cpu.margin": 0.4 }
  }
}
```

## License

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 3 of the License, or
(at your option) any later version.  See [LICENSE](LICENSE) for details.

## Development Roadmap

- [x] Project scaffolding + CMake build system
- [x] Logging subsystem (journald + file + stderr)
- [x] JSON config parser with validation + SIGHUP/DBus hot-reload
- [x] Sysfs writer with batching + deduplication
- [x] Power model (P-F curve, sweet spot selection)
- [x] State machine (7 scenes, timer-based transitions)
- [x] Input monitor (evdev touch event parsing)
- [ ] Per-task affinity scheduling (helpers exist; runtime policy is not enabled)
- [x] HeavyLoad detector (/proc/stat polling)
- [x] Game scanner (/proc scanning)
- [x] SM8550 config
- [x] CLI tool (uperfctl)
- [x] systemd service unit
- [x] DBus interface (org.uperflinux.Daemon)
- [x] GTK4/libadwaita GUI (Dashboard, Games, Config reload, Logs, Frequency Override)
- [x] deb packaging (dpkg-deb)
- [x] Unit and integration tests (CTest, ASan and UBSan)
- [x] Thermal awareness (read /sys/class/thermal/, frequency capping, DBus exposure)
- [x] Per-app power mode auto-switching (perapp_powermode parser, game scanner integration, GUI wiring)
- [x] Config generation wizard for new SoCs
- [x] Manual CPU/GPU frequency override (CLI + GUI + DBus) (uperf-wizard detect, uperfctl detect)
