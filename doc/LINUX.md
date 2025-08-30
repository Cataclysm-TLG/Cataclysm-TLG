# Playing on Linux

## Installing Dependencies

Cataclysm: The Last Generation depends on a few libraries that need to be installed before you can start playing with the tiles release. The commands to install these libraries will depend on your distribution.

NOTE: The curses release does not depend on these libraries. If you are playing with the curses release (e.g. `ctlg-linux-curses-x64-VERSION.tar.gz`) then you can skip this step.

### Debian

```bash
sudo apt install libsdl2-2.0-0 libsdl2-ttf-2.0-0 libsdl2-mixer-2.0-0 libsdl2-image-2.0-0
```

### Fedora

```bash
sudo dnf install SDL2.x86_64
```
