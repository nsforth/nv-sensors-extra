# nv-sensors-extra — extra GPU sensors as a kernel hwmon driver

A Linux kernel module that reads the **GDDR6/GDDR6X memory junction
temperature**, the **GPU hotspot temperature** and the **GPU core
temperature** from supported NVIDIA GPUs and exports them
through the standard `hwmon` interface, so any tool that reads hwmon
(`sensors`, libhwmon, `lm-sensors`, monitoring daemons, etc.) can use
them.

Built and installed with **DKMS**, so it automatically rebuilds on kernel
upgrades.

Naming note: the module file is `nv-sensors-extra.ko` and the DKMS package
is `nv-sensors-extra`, while the kernel-normalized module name (what you
see in `modinfo`, `/sys/module/`) is `nv_sensors_extra` — the kernel build
system converts hyphens in module names to underscores. Both forms work
with `modprobe`.

## Attribution

**This project is a derivative work of the original userspace
[gddr6](https://github.com/olealgoritme/gddr6) project by
[olealgoritme](https://github.com/olealgoritme).**

The register offsets, the GPU device table, and the temperature decoding in
this driver come directly from the original project's reverse engineering of
the NVIDIA GPU Linux driver. Thanks to the original author for the reverse
engineering work and for publishing the results openly — this driver exists
only because of that work.

This port covers the same Ada/Ampere-era devices as the original device
table (plus a few additions from the upstream `master`), with one
deliberate difference: this driver also reads the **GPU hotspot** sensor
(upstream `master` dropped it), and the Blackwell/GDDR7 support from
upstream `master` is intentionally not ported.

## How it works

- The module **does not register or claim any PCI device**. It scans the
  PCI bus and only works with GPUs that are already bound to the
  proprietary `nvidia` driver (declared as a soft dependency:
  `MODULE_SOFTDEP("pre: nvidia")`). It coexists with `nvidia.ko` without
  conflicting with it.
- For each detected card it maps the BAR0 register space of the GPU and
  reads the memory-temperature register directly, decoding it to Celsius.
- Multi-GPU: every supported card gets its own hwmon device named
  `nv_sensors_extra` with three sensors, all reported in millidegrees
  Celsius:
  - `temp1` — GPU core temperature (the "GPU current temperature" that
    nvidia-smi reports), 1/256 °C resolution
  - `temp2` — GPU hotspot temperature, 1/256 °C resolution
  - `temp3` — memory junction (GDDR6 memory) temperature, 1/32 °C
    resolution (in practice the firmware quantizes the value to whole
    even degrees)
- The `temp*_label` files carry the GPU name and the sensor name, e.g.
  `RTX 3090 GPU core temperature`,
  `RTX 3090 GPU hotspot temperature` and
  `RTX 3090 memory junction temperature`.

The only supported scenario is running **alongside** the proprietary
NVIDIA driver: a GPU that is not bound to `nvidia` is ignored.

### Calibration (no offset is applied by the module)

The module reports **raw sensor values** and deliberately applies no
calibration. Measured against `nvidia-smi` on a GA102 (RTX 3090), the raw
`temp1` reading sits about **+2 °C** above the NVML `temperature.gpu`
value over the 27..42 °C range. The exact offset is per-die /
per-architecture — measure it on your own card (see `RESEARCH.md` for the
method) and apply it in userspace, not in the module.

The standard way is the lm-sensors `compute` statement. Create
`/etc/sensors.d/nv-sensors-extra`:

```
chip "nv_sensors_extra-*"
compute temp1 @ - 2, @ + 2
compute temp2 @ - x, @ + x
compute temp3 @ - y, @ + y
```

Two syntax details that are easy to get wrong (both fail silently, leaving
the reading uncalibrated):

- A `chip` name must be what `sensors` prints, i.e.
  `<hwmon name>-<bus id>` (`nv_sensors_extra-pci-0100`), or a pattern.
  A bare `nv_sensors_extra` is rejected with
  `Error: File /etc/sensors.d/..., line 1: Parse error in chip name` and
  the whole block is ignored. `nv_sensors_extra-*` matches every card.
- There is **no `offset` statement** in lm-sensors — the valid statements
  are `chip`, `label`, `ignore`, `compute`, `set` and `bus`; anything else
  gives `Error: Line N: Invalid keyword`. The second expression of
  `compute` is the inverse used when writing limits back, so keep both.

Check that it took effect — parse errors go to stderr, so don't discard
them:

```sh
sensors 2>&1 | grep -A4 nv_sensors_extra
nvidia-smi --query-gpu=index,temperature.gpu --format=csv
```

If the cards in a multi-GPU box drift differently, give each card its own
block with the exact chip name instead of the pattern:

```
chip "nv_sensors_extra-pci-0100"
compute temp1 @ - 2, @ + 2
chip "nv_sensors_extra-pci-0600"
compute temp1 @ - 1, @ + 1
```

`compute` only affects readers that go through libsensors (`sensors`,
lm-sensors based daemons); raw `sysfs` reads and `nvidia-smi` keep
reporting uncalibrated values. The expression works on the converted
value, so `2` means 2 °C, not 2000 millidegrees. Apply the same form to
`temp2`/`temp3` if you ever need it for them.

## Requirements (Debian/Ubuntu)

- The proprietary NVIDIA driver is installed and its `nvidia.ko` module is
  loadable/loaded (this module reads registers that only make sense with
  the GPU owned by `nvidia`).
- `dkms`
- Kernel headers matching the running kernel
- `libpci-dev`
- A C toolchain (`build-essential`)

```sh
sudo apt install dkms build-essential libpci-dev linux-headers-$(uname -r)
```

Notes:

- If you install DKMS for a kernel that is not currently running, install
  that kernel's headers instead (DKMS builds per installed kernel).
- **Secure Boot**: DKMS signs the module with a locally generated key.
  If Secure Boot is enabled, you need to enroll that key into MOK
  (`mokutil --enroll`), or disable Secure Boot. Without either, the kernel
  refuses to load unsigned modules.

## Build and install

### Using the bundled scripts

```sh
git clone <this-repo>
cd <repo-dir>

sudo ./nv-sensors-extra-dkms-build.sh    # dkms add + dkms build
sudo ./nv-sensors-extra-dkms-install.sh  # dkms install
```

(The scripts use their own directory as the DKMS source; override with
`SRC_DIR=/path/to/source`.)

### DKMS manually

```sh
sudo dkms add /path/to/this/repo
sudo dkms build -m nv-sensors-extra -v 1.2
sudo dkms install -m nv-sensors-extra -v 1.2
```

### Load the module

```sh
sudo modprobe nv-sensors-extra
```

DKMS places the module in
`/lib/modules/$(uname -r)/updates/dkms/`; it is **not** loaded
automatically after `dkms install`, so run `modprobe` once. To auto-load on
every boot:

```sh
echo nv-sensors-extra | sudo tee /etc/modules-load.d/nv-sensors-extra.conf
```

## Verification

```sh
sensors                          # should show an nv_sensors_extra section
# or, directly:
cat /sys/class/hwmon/hwmon*/temp1_input   # GPU core, millidegrees C
cat /sys/class/hwmon/hwmon*/temp2_input   # GPU hotspot, millidegrees C
cat /sys/class/hwmon/hwmon*/temp3_input   # memory junction, millidegrees C
cat /sys/class/hwmon/hwmon*/temp1_label
```

If nothing appears, check that `nvidia` is loaded (`lsmod | grep nvidia`),
that the GPU is bound to it (`lspci -k`), and `dmesg` for driver messages.

## Uninstall

```sh
sudo rmmod nv-sensors-extra
sudo dkms remove nv-sensors-extra/1.2
sudo rm -f /etc/modules-load.d/nv-sensors-extra.conf
```

## Supported devices

RTX 4090, RTX 4090 D, RTX 4080 Super, RTX 4080, RTX 4070 Ti Super,
RTX 4070 Ti, RTX 4070 Super, RTX 4070, RTX 4070 Max-Q / Mobile,
RTX 4060 Max-Q / Mobile, RTX 4060 Laptop, RTX 3090 Ti, RTX 3090,
RTX 3080 Ti, RTX 3080, RTX 3080 LHR, RTX 3070, RTX 3070 LHR,
RTX A2000 (2x), RTX A4000, RTX A4500, RTX A5000, RTX 6000 Ada Generation,
L4, L40S, A10

The table can be extended in `nv_sensors_extra_drv.c`
(`nv_sensors_extra_dev_table`) for any Ada/Ampere card whose VRAM/hotspot/
core register offsets are known.

## Versioning

`PACKAGE_VERSION` in `dkms.conf` is the release identifier DKMS keys on —
bump it for new releases (`1.1` → `1.2`, …).
