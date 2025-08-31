# Playing on Linux

## Installing Dependencies

Cataclysm: The Last Generation depends on a few libraries that need to be installed before you can start playing with the tiles release. The commands to install these libraries will depend on your distribution.

NOTE: The curses release does not depend on these libraries. If you are playing with a curses release (e.g. `ctlg-linux-curses-x64-VERSION.tar.gz`) then you can skip this step.

### Debian

```bash
sudo apt install libsdl2-2.0-0 libsdl2-ttf-2.0-0 libsdl2-mixer-2.0-0 libsdl2-image-2.0-0
```

### Fedora

```bash
sudo dnf install SDL2
```

## Further Troubleshooting

If you are having trouble with the above steps, you can try the following:

### Ensure the system knows where the libraries are

```bash
# you may need to be root to run this command
ldconfig -p | grep libSDL2
```

If the output does not resemble:

```
libSDL2-2.0.so.0 (libc6,x86-64) => /lib/x86_64-linux-gnu/libSDL2-2.0.so.0
```

then the libraries may not be installed, may be installed in an unconventional location, or may be linked wrong in the shared library cache. Consult with the documentation for your distribution for further information.

### Ensure the library for the correct architecture is installed

Cataclysm depends on the *64-bit* version of the SDL libraries. You can check that the 32-bit version is not installed instead by running the `file` command on the right-hand side of the output from the `ldconfig` command given above. For example:

```bash
# your libSDL2 might be elsewhere, check the output of ldconfig from the command above
file /lib/x86_64-linux-gnu/libSDL2-2.0.so.0
```

If the output of the above command includes `x86-64` you have got the right library installed.

### Running the executable directly

You may also find success running `./cataclysm-tlg-tiles` directly rather than `./cataclysm-launcher`.
