# ios-g0s

**Apple's iOS 16 running on a Samsung Galaxy S22+ (Exynos).**

This project ports Apple's open-source XNU kernel (xnu-8796, the iOS 16 generation) to the Samsung Galaxy S22+ (`g0s`, Exynos 2200). On top of it runs the **unmodified iOS 16.7.16 (20H392) userland of the iPhone X**: launchd, the system daemons, backboardd, SpringBoard and the stock apps.

As of **2026-10-28** the phone boots to the **iOS home screen**. You can unlock it, open Settings, pull down Control Center and the notification center, and use the touchscreen and the hardware buttons. Rendering runs entirely on the CPU; there is no GPU driver.

As of **2026-10-31** the phone shows up as a USB network card and `tools/sweet-usbnet.sh` is working.

> Experimental bring-up, not a usable phone OS. Many things are missing or stubbed (see below), and Apple's security model is mostly switched off. It installs into the `userdata` partition and so **erases Android's user data**.

## What works

| Area | State |
| --- | --- |
| Boot | Stock Samsung bootloader → uniLoader  flashed to the boot partition → shim execution → XNU. |
| CPU | All 8 cores (1x Cortex-X2; 3x Cortex-A710; 4x Cortex-A510) |
| Memory | About 5.98 GiB of the upper DDR bank |
| Storage | Own UFS host driver; the iOS root is a journaled HFS+ volume in `userdata`, read/write |
| iOS userland | Real iPhone X 16.7.16 root filesystem and shared cache; launchd and the system daemons run |
| Display | Kernel IOSurface + IOMobileFramebuffer (`AppleCLCD`) services; CoreAnimation renders in software; frames are copied into the bootloader's framebuffer. Presented to iOS as an iPhone X panel (1125×2436) |
| GUI | backboardd, SpringBoard, lock screen, home screen, Control Center, notification center |
| Apps | Settings, iMessage, Safari, Photos |
| Touch | STMicroelectronics FTS2BA61Y over SPI, delivered as HID digitizer events |
| Buttons | Power, volume up/down |
| Battery | IOPMPowerSource service (static 100 %, charging icon) |
| USB | USB gadget network interface and logs via `tools/sweet-usbnet.sh` |
| Device identity | iPhone X (`iPhone10,3` / `D22AP`): no Home button, gesture UI |
| Activation | Native "Hactivation" path of mobileactivationd, chosen through device-tree security status. No Apple servers involved |

## What does not work (yet)

- **No Wi-Fi, Bluetooth, cellular, audio, camera, sensors, GPS or SEP / Face ID.** Daemons for them start and fail or keep relaunching.
- **No GPU.** Everything is drawn by the CPU at roughly 20–30 fps.
- **Few apps.** The image has only what is on the system volume. There is no App Store, no Apple ID, and some Settings pages are empty.
- **Slow boot:** about 4 minutes on the Apple logo.
- **Security is off:** permissive code signing (AMFI) and sandbox policies, a stub AppleKeyStore, no data protection. Do not put anything personal on it.
- **Powering off the device:** kernel panics when trying to power off.

## How it boots

```
S-Boot (stock Samsung bootloader, unlocked, tripped Knox)
  -> boot partition image
     -> shim: own MMU setup, reads the live FDT, loads the payload from the ramdisk
        -> XNU Mach-O + generated Apple Device Tree (ADT) + boot-args
           -> XNU -> UFS driver -> HFS+ root in the userdata partition
              -> dyld + shared cache -> launchd -> daemons -> backboardd + SpringBoard
```

There is no exposed serial port. The kernel draws its console into the framebuffer that S-Boot leaves at `0xf6200000`, and keeps a persistent 4 MiB log ring at the end of `userdata`. You read that log from TWRP with `tools/sweet-log-pull.sh`, so you don't have to film the screen.

## How to do it yourself

You need a Galaxy S22+ (`g0s`, Exynos CPU variant, in my case SM-S906B) with an **unlocked bootloader**, a Linux PC, and a GitHub account for the kernel build. This is the exact path used for the screenshots above; other variants or firmware may need changes. **Everything on `userdata` is lost.** Bootloaders, recovery, modem and persist partitions are never touched.

### 0. Back up the phone

Flash TWRP (or any recovery with adb root) and pull
```
blcmd     debug   fmp_selftest  nad_refer   prism     spu       vbmeta
boot      dqmdbg  harx          omr         radio     steady    vbmeta_system
bota      dtbo    keyrefuge     optics      recovery  uhcfg     vendor_boot
cp_debug  efs     keystorage    param       reserved  uh
cpefs     epbl    misc          persistent  sec_efs   up_param
```
They contain per-device calibration data. Keep them off git; `.gitignore` already excludes `backup/`, `dtb/` and `recovery/`.

The shim needs the stock DTB. Unpack your phone's stock `vendor_boot.img`:

```sh
git clone https://github.com/LineageOS/android_system_tools_mkbootimg.git
python3 android_system_tools_mkbootimg/unpack_bootimg.py --format mkbootimg --boot_img backup/vendor_boot.img --out dtb/boot_unpacked
mv dtb/boot_unpacked/dtb dtb/boot_unpacked/dtb.original
python3 tools/g0s-dtb-extract.py dtb/boot_unpacked/dtb.original dtb/boot_unpacked/dtb
```

### 1. Build the kernel

The kernel builds on GitHub's macOS runners with [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build). The workflow fetches xnu-8796, applies `xnu/patches/` in order, and also stages the pinned open-source libpthread, HFS and BearSSL sources it needs. Fork the repo (public repos get free macOS minutes) and run:

```sh
gh workflow run xnu.yml -R YOU/ios-g0s
gh run download RUN_ID -R YOU/ios-g0s -n xnu-kernel -D xnu/out
```

To check a patch locally before spending a CI run, `tools/xnu-cc.py` replays the compile commands of a previous CI log for single files.

### 2. Build the iOS root image

Download the iPhone X (iPhone10,3) iOS 16.7.16 (20H392) IPSW from Apple. Then, on Linux, with `apfs-fuse`, `hfsprogs` and sudo:

```sh
# OS, SystemOS and AppOS disk images from the IPSW (see BuildManifest.plist for their names)
tools/mkiosroot.sh OS.dmg SystemOS.dmg AppOS.dmg ios16-root.img 16G
sudo fsck.hfsplus -fy ios16-root.img
sudo mount -t hfsplus -o loop,force ios16-root.img /mnt/iosroot
sudo python3 tools/sweet-root-tweaks.py /mnt/iosroot
sudo umount /mnt/iosroot
img2simg ios16-root.img ios16-root.simg
```

`mkiosroot.sh` copies the OS volume and both cryptexes into one case-sensitive journaled HFS+ volume. It adds the dyld cache links, the plain system keybag and the log directories the daemons expect. `sweet-root-tweaks.py` applies the bring-up changes: it skips Setup, seeds the activation state, removes the baseband daemon, disables the watchdogs that kill slow apps and the crash-looping media paths, and so on. The script's header explains each change.

The device tree builder copies `/product` from the real iPhone X device tree, which MobileGestalt needs. Extract it once, without downloading the whole IPSW:

```sh
python3 tools/remotezip.py IPSW_URL DeviceTree.d22ap.im4p research/ipsw-x/dt
ipsw img4 im4p extract research/ipsw-x/dt/Firmware/all_flash/DeviceTree.d22ap.im4p -o research/ipsw-x/d22ap.adt
```

### 3. Build the boot image

Needs `aarch64-linux-gnu-gcc` and `mkbootimg`.

```sh
python3 tools/mkadt.py xnu/out/adt.img --highmem
python3 tools/mkpayload.py xnu/out/kernel.development.sweet xnu/out/adt.img shim/out/payload.bin \
    --cmdline "debug=0xc serial=3 cpus=8 keepsyms=1 -enable_kprintf_spam -noprogress rd=ufsroot0 ufsroot=userdata ufswait=1 unentitled_ios_sim_launch=1 ignition_level=1 wdt_test=no_user_checkin ufsrw=1 rootrw=1 launchd_unsecure_cache=1 wdt=-1 amfi_get_out_of_my_way=1 cs_enforce=0 cs_enforcement_disable=1 sweetfb_native=0 sweetfb_noconsole=1 sweetfb_hz=30 sweetkeys=0 sweettouch=1 sweetusb=2 sweetbattery=0"

shim/build.sh ../shim/out/payload.bin
```

What the less obvious boot-args do:

| Boot-arg | Effect |
| --- | --- |
| `rd=ufsroot0 ufsrw=1 rootrw=1` | Root on the `userdata` partition, writable |
| `cpus=8` | Start all the 8 cores; `cpus=1` is the safe fallback |
| `launchd_unsecure_cache=1` | launchd accepts its job cache without AMFI |
| `wdt=-1 wdt_test=no_user_checkin` | No hardware watchdog |
| `sweetfb_noconsole=1` | Stop drawing kernel text over the GUI. It still goes to the persistent log; panics are drawn |
| `sweetfb_hz=30` | Panel refresh rate of the CPU copy |
| `sweettouch=0`, `sweetkeys=0` | Disable the touch or button driver |

```sh
python3 android_system_tools_mkbootimg/unpack_bootimg.py --boot_img backup/boot.img --format mkbootimg
# Example output:
# --header_version 4 --os_version 12.0.0 --os_patch_level 2025-09 --kernel out/kernel --ramdisk out/ramdisk --cmdline ''
```
Note this output somewhere and replace:
* `out/kernel` with `uniLoader`
* `out/ramdisk` with `empty_ramdisk`

Add this to the end:
`--out boot-uniLoader.img`

Example result:
```
--header_version 4 --os_version 12.0.0 --os_patch_level 2025-09 --kernel uniLoader --ramdisk empty_ramdisk --cmdline '' --out boot-uniLoader.img
```


```sh
git clone https://github.com/ivoszbg/uniLoader.git
cd uniLoader
cp ../shim/out/Image ./blob/Image
cp ../shim/out/ramdisk.bin ./blob/ramdisk
cp ../dtb/boot_unpacked/dtb ./blob/dtb

make ARCH=aarch64 CROSS_COMPILE=aarch64-linux-gnu- g0s_defconfig
make ARCH=aarch64 CROSS_COMPILE=aarch64-linux-gnu-

touch empty_ramdisk
../android_system_tools_mkbootimg/mkbootimg.py <--header_version 4 ...>
cd ..
```



### 4. Flash the root and boot (via TWRP or other recovery)

Boot into TWRP and press Reboot > Fastboot.

```sh
fastboot -S 128M flash userdata ios16-root.simg   # ~10 minutes; do not unplug
fastboot flash boot uniLoader/boot-uniLoader.img
```

The first boot takes about 4–5 minutes on the Apple logo, then the lock screen appears. Swipe up to unlock. To get back to recovery, hold Power + Volume down for ~7 seconds until the screen blanks, then immediately hold Power + Volume up (phone must be connected via USB cable to the PC).

### 5. Read the kernel log

Boot into TWRP or other recovery and run:

```sh
tools/sweet-log-pull.sh logs/boot.txt      # 4 MiB console ring from the end of userdata
```

The log contains crash reports with backtraces (`CRASH name ...`), IOKit traces of backboardd and SpringBoard, and the drivers' `SWEET ...` lines. `tools/sweet-crash.py` symbolicates them against the shared cache.

### QEMU

`tools/qemu-sweet.py` boots the same kernel on QEMU `virt` with a test RAM-disk root. It is useful for kernel-only changes; see `NOTES.md`.

## Repository layout

| Path | What |
| --- | --- |
| `shim/` | Bare-metal boot shim (C + asm) packed as an Android boot.img v2 |
| `xnu/patches/` | The XNU patch series (applied in order by CI), plus staging scripts for libpthread, HFS and BearSSL |
| `.github/workflows/xnu.yml` | Kernel CI build |
| `tools/mkadt.py` | Generates the Apple Device Tree from sweet's MMIO layout and the iPhone X identity |
| `tools/mkpayload.py` | Packs kernel + ADT + boot-args into the shim payload |
| `tools/mkiosroot.sh` | Builds the HFS+ iOS root from the IPSW images |
| `tools/sweet-root-tweaks.py` | Bring-up changes to the root (Setup skip, activation, daemons) |
| `tools/sweet-log-pull.sh`, `sweet-log.py` | Read the persistent kernel log from recovery |
| `tools/sweet-crash.py` | Symbolicate console crash reports |
| `tools/xnu-cc.py` | Local single-file compile check from a CI log |
| `tools/qemu-sweet.py` | QEMU test boots with framebuffer text decoding |
| `tools/adt.py`, `remotezip.py`, `unpack_bootv2.py`, `sparsediff.py` | Helpers |
| `NOTES.md` | Detailed engineering log: every boot, crash and decision, plus hardware facts |

Not in git: partition backups, live device trees, recovery images, Apple firmware and root images, and downloaded or generated build trees. They contain device serials and calibration data, or Apple's copyrighted files.

## Kernel patches

| # | What |
| --- | --- |
| 0001–0009 | Toolchain compatibility, SWEET machine config, non-PAC arm64, stubs for Apple-internal pieces |
| 0010–0018 | Early framebuffer console, physmap fix, PRNG without corecrypto, device-tree platform expert, timer PPI, boot CPU driver |
| 0019–0020 | Apple open-source libpthread kernel provider; BearSSL AES |
| 0021–0027 | RAM-disk root, mockfs, SHA-256, minimal AMFI, Apple open-source HFS+, MD5 |
| 0028–0034 | UFS host driver and root device, read/write root, DMA above 4 GiB |
| 0035–0045 | Entitlements, bindfs, AppleKeyStore stub, IOWatchdog, console crash reports |
| 0046 | IOSurface (IOCoreSurfaceRoot) and IOMobileFramebuffer (AppleCLCD) services |
| 0047–0055 | Diagnostics: persistent log, /dev/null tee, IOKit trace, heap strings and user stack sampler on crash; permissive Sandbox policy; HFS rename-swap; AppleKeyStore selectors |
| 0056–0057 | Throttled panel copy; no external identity resolver |
| 0058 | HID event services. Apple's IOHIDFamily-1915.140.3 user client and event queue, matching the iOS 16.7 userland |
| 0059 | iPhone X identity (`hw.product` / `hw.target` from the device tree) |
| 0060 | Power and volume keys over SPMI; touch over GENI I²C |
| 0061 | Present the display as 1125×2436 |
| 0062 | RPMh regulator vote for the touch rail; battery power source |
| 0063 | GENI FIFO packing fix and the Goodix GT9896 protocol |
| 0064 | Grant PRIV_WORK_INTERVAL (UIKit's CoreAnimation work interval) |
| 0065 | SMP: secondary A55 cores via PSCI CPU_ON |
| 0066–0067 | Fault-region crash info; console mute |
| 0068 | IOSurface lookup semantics for already-mapped surfaces |
| 0069 | Batched persistent-log flushes |

## Key hardware facts

- RAM: payloads use the upper bank, `0x880000000` + `0x17ec00000` (about 5.98 GiB), above Samsung's protected carve-outs.
- GICv3 (`0x10200000` / `0x10240000`) with security extensions: virtual timer INTID 27, 25.6 MHz timebase, ARMv8.2 without pointer authentication. MPIDR uses Aff1 for the core number.
- Panel: 1080×2340 video mode, continuous splash from S-Boot (`0xf6200000`), no refresh kick needed.
- Touch: STMicroelectronics FTS2BA61Y bit-banged over SPI2 on PERIC2 GPIO `GPP2` (`0x11c30000`), interrupt on ALIVE `GPA1-2` (`0x15850000`).
- Buttons: polled active-low on ALIVE GPIO `GPA0` (`0x15850000`): power on `GPA0-1`, volume up on `GPA0-2`, volume down on `GPA0-3`.

More in [`NOTES.md`](NOTES.md).

## Credits

Built on Apple's open-source releases (xnu, IOHIDFamily, libpthread, hfs), [BearSSL](https://bearssl.org), [blacktop/darwin-xnu-build](https://github.com/blacktop/darwin-xnu-build), [ipsw](https://github.com/blacktop/ipsw), Samsung/LineageOS published Linux kernel sources for the hardware details, and [TWRP](https://github.com/milxnaq/android_device_samsung_s5e9925). iOS itself is Apple's and is not distributed here.
