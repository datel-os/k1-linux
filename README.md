# Linux for the Creality K1

The kernel of [Datel OS](https://github.com/zales/datel-os) for the Creality K1 3D printer, whose mainboard carries an Ingenic X2000: two XBurst2 MIPS32 cores and 256 MB of RAM. It is kernel.org's stable Linux 6.18 plus the support the board needs, added through pull requests.

**State:** phase A runs on the printer. On 14 November 2026 a kernel built from this tree (`k1_defconfig`, the board's device tree built in) booted a K1 and passed phase A's checks, a test print excepted: both CPU cores at 816 MHz, Klipper with its three microcontrollers, Wi-Fi, the camera over WebRTC, the Helix codec's tests, 25 minutes of floating-point work on a hot printer without a wrong result, and a restart by the watchdog with the kernel log kept in pstore. Datel OS ships Ingenic's SDK kernel 6.6 until phase A below is complete.

The kernel's own introduction is in [README](README).

## Plan

Mainline has drivers for some of the X2000 (pin control, USB, SPI, Ethernet) but no clock driver, no device tree for the SoC and no support for its second core. Ingenic's SDK kernel has all of it, as code that follows few of the kernel's frameworks.

**Phase A** carries over from the SDK kernel only what the K1 needs, changed as little as 6.18 requires, so that the printer runs 6.18 soon. The source is `kernel/kernel-6.6` of `zales/k1-kernel` at commit `1dbe8469`: Linux 6.6.157-rt79 plus the vendor's changes, the tree NebulaOS keeps for the Ender-3 V3 KE. The display stack, the touch controller of another board, sleep and fast boot, and the changes that only serve PREEMPT_RT stay behind.

**Phase B** replaces the carried code one part at a time with code written against upstream's frameworks, and deletes what it replaces. Each part is verified on the printer before the next one starts.

| Part | Phase A: carried over | Phase B: rewritten |
|---|---|---|
| Platform (`arch/mips/xburst2`), boot image | carried over; runs on the printer | not yet |
| Clocks, timer, interrupt controller | carried over; runs on the printer | not yet |
| Pin control, serial | carried over; runs on the printer | not yet |
| MMC, DMA, I2C | carried over; MMC runs on the printer (the eMMC, Wi-Fi over SDIO); DMA and I2C start, and nothing on the K1 used them in the test | not yet |
| USB | transceiver carried over; upstream's `dwc2`, with its descriptor DMA path for isochronous endpoints corrected (the camera's stream stopped for good without that); runs on the printer (the hub, the camera) | not yet |
| TCU, watchdog, RTC, random number generator | carried over; runs on the printer | not yet |
| The K1 board: device tree, `k1_defconfig`, Helix codec driver, pstore | device tree cut down from the SDK's, board file written, Helix driver from Datel OS; runs on the printer | not yet |

The carried code keeps the SDK's paths, so a file can be compared with its origin: the platform in `arch/mips/xburst2`, the drivers in `module_drivers/`. What the SDK changes in the kernel's own files is tied to `CONFIG_MACH_XBURST2`. The device tree is where upstream keeps Ingenic's, `arch/mips/boot/dts/ingenic/`: `x2000.dtsi` and `x2000-pinctrl.dtsi` for the SoC, `creality-k1.dts` for the board.

## Branches

| Branch | What it holds |
|---|---|
| `vanilla` | One commit per upstream release. Its tree is the tree of the kernel.org tag: nothing added, nothing changed. |
| `main` | Starts at `vanilla`. Everything else reaches it through pull requests. |
| `feat/…`, `fix/…`, `chore/…`, `docs/…`, `refactor/…`, `test/…` | One topic each, named by the type of change and by what it does. |

`vanilla` does not carry upstream's history. Each of its commits is made from a depth 1 fetch of the release tag with `git commit-tree <tag>^{tree}`, the previous release as its parent. To check one against kernel.org:

```bash
git fetch --depth 1 https://git.kernel.org/pub/scm/linux/kernel/git/stable/linux.git tag v6.18.63
git rev-parse 'v6.18.63^{tree}' 'origin/vanilla^{tree}'   # the same hash twice
git tag -d v6.18.63
```

A new stable release is a new commit on `vanilla` and a pull request from `vanilla` into `main`.

Pull requests are merged with a merge commit, never squashed or rebased, for two reasons. `vanilla` has to stay an ancestor of `main`, or the next release would no longer merge as the difference between two releases. And a pull request may be stacked on another one that still waits for review; it then says so in its description, and the lower one is merged first.

## Pull requests

Each one says:

1. what it adds,
2. where the code comes from: a vendor file carried over and what had to change for 6.18, or code rewritten against which upstream framework,
3. how it was checked: the CI build, and which checks on the printer,
4. what it leaves for later.

## Building

Any GCC for little-endian MIPS will do for a build check; on Debian and Ubuntu it is the package `gcc-mipsel-linux-gnu`.

```bash
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- O=build k1_defconfig
make ARCH=mips CROSS_COMPILE=mipsel-linux-gnu- O=build -j"$(nproc)"
```

The image for the printer is `build/arch/mips/boot/uzImage.bin`: the self-extracting kernel in a U-Boot header, loaded at `0x80f00000`. The stock boot loader takes nothing else; it cannot unpack a kernel itself. `mkimage` comes from the package `u-boot-tools`.

A kernel meant for a printer needs three things more:

- `LOCALVERSION=` on both `make` lines. Built from a git checkout the release is otherwise `6.18.63+`, and the modules' directory has to carry the same name.
- The modules, `make … INSTALL_MOD_PATH=<root> modules_install`. The Wi-Fi driver `brcmfmac` is one: it reads its firmware from the root file system, which a built-in driver would ask for before that is mounted.
- The wireless regulatory database as built-in firmware, `CONFIG_EXTRA_FIRMWARE="regulatory.db regulatory.db.p7s"` with `CONFIG_EXTRA_FIRMWARE_DIR` set to where the files are. `cfg80211` is built in and loads it before the root file system is there. The files are not part of this tree, so `k1_defconfig` does not name them.

[.github/workflows/build.yml](.github/workflows/build.yml) runs this build for every pull request and for every push to `main`. It checks that the tree compiles; it does not run the kernel. The repository is private and its Actions minutes are limited, so the workflow is one job with a compiler cache, and changes to documentation alone do not start it.

## Cloning on macOS

The tree has 13 pairs of files whose names differ only by case (`xt_DSCP.c` and `xt_dscp.c` in netfilter, for example), and the default macOS file system cannot hold both of a pair. A sparse checkout that leaves those 26 files out keeps `git status` clean:

```bash
git clone --no-checkout git@github.com:zales/k1-linux.git && cd k1-linux
git ls-tree -r --name-only HEAD |
  awk '{ k = tolower($0); n[k]++; p[k] = p[k] "!/" $0 "\n" }
       END { print "/*"; for (k in n) if (n[k] > 1) printf "%s", p[k] }' |
  git sparse-checkout set --no-cone --stdin
git checkout main
```

Build on a case-sensitive file system, such as a Linux container's own.
