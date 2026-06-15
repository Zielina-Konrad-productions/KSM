# Kastiusz System Manager

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Terminal](https://img.shields.io/badge/UI-ANSI%2Btermios%20%2B%20FTXUI-cyan)
![License](https://img.shields.io/badge/license-MIT-green)

Kastiusz System Manager, or **KSM**, is a terminal system manager for Linux. It uses one blue FTXUI control center opened with `sudo ksm`.

## Features

- one public command: `sudo ksm`
- FTXUI based YaST-style control center
- native panels for users, groups, installed services, Samba, Apache, DNS, DHCP, FTP, power actions, permissions, network, SSH, firewall, extensions, update, uninstall, and system info
- updater from GitHub Releases with a step progress bar and clear green completion screen
- Extensions panel can install ZPM, then `/opt/ZPM` enables a separate ZPM tab with GUI forms for ZPM functions
- internal helper binaries stay in `/opt/KSM/bin` and are not linked as public commands
- C++20 build through `src/build.sh`

Everything is operated from the panel. After install, use only:

```bash
sudo ksm
```

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

The installer builds a temporary C++ panel, installs KSM to `/opt/KSM`, builds internal binaries into `/opt/KSM/bin`, and links only `/usr/bin/ksm`.

## Dependencies

The installer can install the needed packages automatically:

- Debian/Ubuntu: `g++ sudo coreutils curl tar nano passwd systemd systemd-resolved iproute2 procps util-linux openssh-server ufw pkg-config libftxui-dev`
- openSUSE: `gcc-c++ sudo coreutils curl tar nano shadow systemd iproute2 procps util-linux openssh firewalld pkgconf-pkg-config ftxui-devel`
- Fedora/RHEL: `gcc-c++ sudo coreutils curl tar nano shadow-utils systemd iproute procps-ng util-linux openssh-server firewalld pkgconf-pkg-config ftxui-devel`

Optional service extensions can be installed from `Extensions` / `Supported Services` inside `sudo ksm`. Installed services then appear in the `Services` category:

- Samba: `samba`
- Apache: `apache2` or `httpd`
- DNS: `bind9` or `bind`
- DHCP: `isc-dhcp-server` or `dhcp-server`
- FTP: `vsftpd`

Service panels include common fields plus `Extra ... directives` fields for native config lines separated with semicolons.

## Uninstall

```bash
sudo bash ./UNINSTALL.sh
```

You can also uninstall from inside `sudo ksm`. The uninstaller removes `/opt/KSM` and only removes `/usr/bin` command links that point to `/opt/KSM/bin`.

## Usage

```bash
sudo ksm
```

## ZPM Extension

Open `Extensions` in the control center to install ZPM from `Zielina-Konrad-productions/ZPM`. When `/opt/ZPM` exists, KSM shows a separate `ZPM` tab with panel forms for `zinst`, `zrm`, `zsearch`, `zinfo`, `zlist`, `zrun`, `zupd`, `zupgr`, `zclean`, `zhome`, `ztui`, and `zuninstall`.

## Build

KSM requires GCC 10+ and C++20.

```bash
sudo bash ./INSTALL.sh
```

Or from an installed source tree:

```bash
sudo bash /opt/KSM/src/build.sh
```

`src/build.sh` builds the public `ksm` entrypoint plus the internal `kcontrol` and `kupgr` helpers. Legacy source files remain in the tree, but they are not public commands.

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
