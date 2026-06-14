# Kastiusz System Manager

![C++20](https://img.shields.io/badge/C%2B%2B-20-blue)
![Terminal](https://img.shields.io/badge/UI-ANSI%20%2B%20termios-cyan)
![License](https://img.shields.io/badge/license-MIT-green)

Kastiusz System Manager, or **KSM**, is a terminal toolkit for Linux system management. It uses a blue ANSI interface, no ncurses, and small focused C++ tools.

## Features

- `ksm` wrapper for the main tools
- `khome` built-in help pages
- `kupgr` interactive updater from GitHub Releases
- `kuninstall` interactive uninstaller
- `kuseradd` interactive user creator
- `kuserdel` interactive user remover
- `kgroupadd` interactive group creator
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
khome -ui
kupgr
kuninstall
kuseradd
kuserdel
kgroupadd
kgroupdel
knetcfg
```

KSM wrapper alternatives:

```bash
ksm
ksm home
ksm home -ui
ksm upgrade
ksm upgrade -ex
ksm uninstall
ksm useradd
ksm userdel
ksm groupadd
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
  khome.cpp
  kupgr.cpp
  kuninstall.cpp
  kuseradd.cpp
  kuserdel.cpp
  kgroupadd.cpp
  kgroupdel.cpp
  knetcfg.cpp
  kinstall.cpp
  common/
```

## License

MIT
