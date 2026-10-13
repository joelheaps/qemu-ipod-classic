# Docker tasks

The Ubuntu 26.04 images provide environments for the same mise tasks used on native Linux. `Dockerfile.build` installs build dependencies through `mise run setup`; `Dockerfile.dev` installs runtime/debugging tools through `mise run setup -- --runtime`. Both install mise and the project's contributor tools.

`mise run docker:build`, `docker:ipod:make-disk`, `docker:ipod:run`, `docker:ipod:gdb`, and `docker:check` invoke the shared tasks inside the appropriate image. Docker wrappers never run host dependency setup. Native builds use `build/native`; Docker uses `build/docker`.

Build contexts include only Dockerfiles, mise configuration and the setup script. Firmware, disk images and build artifacts are excluded. Image cache labels hash the copied setup/configuration inputs; changing setup or mise configuration rebuilds the image on the next Docker task. Execution tasks are read directly from the mounted checkout.

Validated on Ubuntu 26.04 ARM64: full emulator/qemu-img/plugin compilation, all shared checks (including synthetic disk integration), headless guest execution with both plugins, and the GDB wrapper. The build uses uv-managed Python 3.14.8 with a uv bootstrap environment and QEMU's bundled Meson 1.2.3. Real firmware boot, desktop display/audio, Ubuntu 24.04 and Fedora execution have not been exercised. See [the README](../README.md) for contributor commands and display/audio requirements.
