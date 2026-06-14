# Kastiusz System Manager

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Terminal](https://img.shields.io/badge/UI-ANSI%20%2B%20termios-cyan)
![License](https://img.shields.io/badge/license-MIT-green)

Kastiusz System Manager, or **KSM**, is a terminal toolkit for Linux system management. It uses a blue ANSI interface, no ncurses, and small focused C++ tools.

## Features

- `ksm` wrapper for the main tools
- `kcontrol` YaST-style KSM control center
- `khome` built-in help pages
- `kupgr` interactive updater from GitHub Releases
- `kuninstall` interactive uninstaller
- `ksysinfo` interactive system information dashboard
- `kserv` interactive systemd service manager
- `kperm` interactive permission and owner manager
- `kssh` interactive SSH daemon configuration helper
- `kfirewall` interactive ufw/firewalld helper
- `kuseradd` interactive user creator
- `kusermod` interactive user modifier
- `kuserdel` interactive user remover
- `kgroupadd` interactive group creator
- `kgroupmod` interactive group modifier
- `kgroupdel` interactive group remover
- `knetcfg` interactive network interface configuration
- ANSI + termios interface, no ncurses
- C++20 build through `src/build.sh`

## Install With Curl

```bash
curl -fsSL https://raw.githubusercontent.com/Zielina-Konrad-productions/KSM/main/INETINSTALL.sh | sudo bash
```

The internet installer downloads the latest GitHub Release tarball. If no release is available yet, it falls back to the `main` branch archive and starts the normal interactive installer.

## Local Install

```bash
git clone https://github.com/Zielina-Konrad-productions/KSM.git
cd KSM
sudo bash ./INSTALL.sh
```

The installer builds a temporary C++ panel, installs KSM to `/opt/KSM`, builds binaries into `/opt/KSM/bin`, and links commands into `/usr/bin`.

## Dependencies

The installer can install the needed packages automatically:

- Debian/Ubuntu: `g++ sudo coreutils nano passwd systemd systemd-resolved iproute2 procps util-linux openssh-server ufw`
- openSUSE: `gcc-c++ sudo coreutils nano shadow systemd iproute2 procps util-linux openssh firewalld`
- Fedora/RHEL: `gcc-c++ sudo coreutils nano shadow-utils systemd iproute procps-ng util-linux openssh-server firewalld`

## Uninstall

```bash
sudo kuninstall
```

The repository wrapper can also launch the installed uninstaller:

```bash
sudo bash ./UNINSTALL.sh
```

The uninstaller removes `/opt/KSM` and only removes `/usr/bin` command links that point to `/opt/KSM/bin`.

## Commands

```bash
khome
kcontrol
khome -ui
kupgr
kuninstall
ksysinfo
kserv
kperm
kssh
kfirewall
kuseradd
kusermod
kuserdel
kgroupadd
kgroupmod
kgroupdel
knetcfg
```

KSM wrapper alternatives:

```bash
ksm
ksm control
ksm menu
ksm home
ksm home -ui
ksm upgrade
ksm upgrade -ex
ksm uninstall
ksm sysinfo
ksm serv
ksm perm
ksm ssh
ksm firewall
ksm useradd
ksm usermod
ksm userdel
ksm groupadd
ksm groupmod
ksm groupdel
ksm netcfg
```

## Build

KSM requires GCC 10+ and C++20.

```bash
sudo bash ./INSTALL.sh
```

Or from an installed source tree:

```bash
sudo bash /opt/KSM/src/build.sh
```

`src/build.sh` compiles every `src/*.cpp` tool except `kinstall.cpp`, because `kinstall.cpp` exists only for `INSTALL.sh`.

## Project Layout

```text
INSTALL.sh
INETINSTALL.sh
UNINSTALL.sh
VERSION.txt
kastiusz.conf
src/
  build.sh
  KSM.cpp
  kcontrol.cpp
  khome.cpp
  kupgr.cpp
  kuninstall.cpp
  ksysinfo.cpp
  kserv.cpp
  kperm.cpp
  kssh.cpp
  kfirewall.cpp
  kuseradd.cpp
  kusermod.cpp
  kuserdel.cpp
  kgroupadd.cpp
  kgroupmod.cpp
  kgroupdel.cpp
  knetcfg.cpp
  kinstall.cpp
  common/
```

## License

MIT
