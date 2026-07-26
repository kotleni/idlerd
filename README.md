# idlerd

Configless idle management daemon for Wayland compositors that support the `ext-idle-notifier-v1` protocol.

## Usage

```
idlerd --timeout <seconds> <command> [--resume <command>]
```

You need to run `idlerd` in the background. For example, in `Sway`, you can add this to your configuration:
```sh
exec idlerd \
  --timeout 400 "swaylock" \
  --timeout 900 "swaymsg 'output * power off'"
  --timeout 1400 "systemctl sleep"
  --resume "swaymsg 'output * power on'"
```

## Options

| Option | Description |
|---|---|
| `-t`, `--timeout <seconds> <command>` | Run `command` after `<seconds>` of inactivity. Can be specified multiple times. |
| `-r`, `--resume <command>` | Run `command` when the user becomes active again. |
| `-h`, `--help` | Print usage information. |

## Installing dependencies

```bash
# Debian/Ubuntu
sudo apt install cmake pkg-config libwayland-dev wayland-protocols

# Arch
sudo pacman -S cmake pkg-config wayland wayland-protocols

# Fedora
sudo dnf install cmake pkgconf wayland-devel wayland-protocols-devel
```

## Building

```bash
cmake -B build
cmake --build build
```

## Installing

```bash
cmake --install build
```

This installs the `idlerd` binary to `/usr/local/bin/` by default. You can change the prefix:

```bash
cmake --install build --prefix ~/.local
```

Or install it from the <a href="https://aur.archlinux.org/packages/idlerd-git">AUR</a>:

```sh
yay -S panium-git
```

