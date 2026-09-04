## Buildroot for BeagleV-Fire


### Getting Buildroot

Clone the repository from GitHub...
```sh
$ git clone -b buildroot https://github.com/RohmSemiconductor/linux-poc.git --recurse-submodules
$ cd linux-poc
```

...and configure Buildroot for BeagleV-Fire boards:
```sh
$ cd buildroot
$ make beaglev_fire_defconfig
```

Make sure the kernel that used is `linux4microchip+fpga-2025.10`. The actual
kernel version is `6.12.48`.

The version can be found at top of the kernel Makefile at
`output/build/linux-custom/Makefile`. The buildroot config option
`BR2_LINUX_KERNEL_CUSTOM_TARBALL_LOCATION` should also have the kernel name.


### Configuring Buildroot

Continuing in the `buildroot` directory, include the custom `sampler` and
`sampler-libiio` packages:
```sh
$ make BR2_EXTERNAL="$(pwd)/../sampler*" menuconfig
```

Setting the base `BR2_PACKAGE_SAMPLER` option selects the child packages as well.
- The `sampler` package sets up the required network and SSH configurations in the target filesystem.
- The `sampler-libiio` package contains the 1.0 version of LibIIO for the target system.

With `make menuconfig`, include the `BR2_PACKAGE_OPENSSH` OpenSSH package, and
set a root password using the `BR2_TARGET_GENERIC_ROOT_PASSWD` option. The
password appears in plaintext, so don't use any important ones.


### Patching and configuring the kernel

With `make menuconfig`, include patches using the `BR2_LINUX_KERNEL_PATCH`
config option. The value should be a space-separated list of URLs, local file
paths or directories.

The patches for the DMA sampler and IIO files can be found at:
- https://raw.githubusercontent.com/RohmSemiconductor/linux-poc/refs/heads/patches/dma-sampler-for-6.12.patch

Disable the `BR2_DOWNLOAD_FORCE_CHECK_HASHES` option, so Buildroot can download
the patches.

With `make linux-menuconfig`, enable the following Linux kernel config options:
- `CONFIG_IIO_BUFFER=y`
- `CONFIG_IIO_BUFFER_DMA=m`


### Building Buildroot

To build Buildroot, including the tooling, kernel and packages, simply run:
```sh
$ make
```

If the build fails because of any missing dependencies, install them and
run `make` again; it will continue from where it left off.


### Installing the image

To prepare the BeagleV-Fire for receiving the image via USB, stop the boot
process by pressing the user button on the board. A tool like `minicom` can be
used to visualise the process.

From the host machine, write the image data using `dd`:
```sh
$ dd if=output/images/sdcard.img of=/dev/sda
```

> Make sure the image and device paths are correct for your setup!

When the writing has finished, reboot the BeagleV-Fire.


### Updating the gateware

Add the `LinuxProgramming` directory to the target filesystem, either under
`board/beagleboard/beaglev_fire/rootfs-overlay/` or directory to `build/target/`.

On the BeagleV-Fire, update the gateware:
```sh
$ /usr/share/microchip/update-gateware.sh <path-to-LinuxProgramming-directory>
```

The BeagleV-Fire will restart itself after updating.


### Running the UI

To run the server:
```sh
$ cd ui
$ sudo python3 adc_server.py
```

The server requires the `aiohttp` Python package, and for the `sampler`
Buildroot package to be built (for the LibIIO wrapper library).

To run the frontend:
```sh
$ cd ui/react-frontend-webgl2
$ npm install
$ npm run dev
```
