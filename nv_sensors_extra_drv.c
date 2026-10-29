/* SPDX-License-Identifier: GPL-2.0-only */

/*
 * nv_sensors_extra_drv.c - hwmon driver for NVIDIA GDDR6/GDDR6X memory, GPU
 * hotspot and GPU core temperatures
 *
 * Sensor values are read directly from PCI BAR0 of the GPU. This driver
 * does NOT register a PCI driver and does NOT take ownership of the
 * device; the GPU belongs to the proprietary nvidia driver, which must be
 * loaded first (see MODULE_SOFTDEP). For every matching GPU we take a PCI
 * device reference, map BAR0 and register a standalone hwmon device.
 */

#include <linux/err.h>
#include <linux/hwmon.h>
#include <linux/io.h>
#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>

#include "nv_sensors_extra.h"

#define NV_SENSORS_EXTRA_MAX_DEVICES	32
#define NV_SENSORS_EXTRA_HWMON_NAME	"nv_sensors_extra"

/*
 * Device table: offsets of the sensor registers in BAR0.
 *
 * The GPU hotspot and GPU core sensors sit at the same offsets for all
 * supported chips. The VRAM sensor lives at 0x0000E2A8 on every chip
 * except the GA104-based RTX 3070 cards, which use 0x0000EE50.
 */
static const struct nv_sensors_extra_entry nv_sensors_extra_dev_table[] = {
	{ 0x2684, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 4090", "GDDR6X", "AD102" },
	{ 0x2685, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 4090 D", "GDDR6X", "AD102" },
	{ 0x2702, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 4080 Super", "GDDR6X", "AD103" },
	{ 0x2704, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 4080", "GDDR6X", "AD103" },
	{ 0x2705, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 4070 Ti Super", "GDDR6X", "AD103" },
	{ 0x2782, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 4070 Ti", "GDDR6X", "AD104" },
	{ 0x2783, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 4070 Super", "GDDR6X", "AD104" },
	{ 0x2786, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 4070", "GDDR6X", "AD104" },
	{ 0x2860, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 4070 Max-Q / Mobile", "GDDR6", "AD106" },
	{ 0x28e0, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 4060 Max-Q / Mobile", "GDDR6", "AD107" },
	{ 0x28a0, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 4060 Laptop", "GDDR6", "AD107" },
	{ 0x2203, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 3090 Ti", "GDDR6X", "GA102" },
	{ 0x2204, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 3090", "GDDR6X", "GA102" },
	{ 0x2208, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 3080 Ti", "GDDR6X", "GA102" },
	{ 0x2206, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 3080", "GDDR6X", "GA102" },
	{ 0x2216, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 3080 LHR", "GDDR6X", "GA102" },
	{ 0x2484, 0x0000EE50, 0x0002046C, 0x00020460, "RTX 3070", "GDDR6", "GA104" },
	{ 0x2488, 0x0000EE50, 0x0002046C, 0x00020460, "RTX 3070 LHR", "GDDR6", "GA104" },
	{ 0x2531, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX A2000", "GDDR6", "GA106" },
	{ 0x2571, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX A2000", "GDDR6", "GA106" },
	{ 0x24b0, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX A4000", "GDDR6", "GA104" },
	{ 0x2232, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX A4500", "GDDR6", "GA102" },
	{ 0x2231, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX A5000", "GDDR6", "GA102" },
	{ 0x26B1, 0x0000E2A8, 0x0002046C, 0x00020460, "RTX 6000 Ada Generation", "GDDR6", "AD102" },
	{ 0x27B8, 0x0000E2A8, 0x0002046C, 0x00020460, "L4", "GDDR6", "AD104" },
	{ 0x26B9, 0x0000E2A8, 0x0002046C, 0x00020460, "L40S", "GDDR6", "AD102" },
	{ 0x2236, 0x0000E2A8, 0x0002046C, 0x00020460, "A10", "GDDR6", "GA102" },
};

/*
 * Per-GPU driver state. The struct lives in a static array so it is not
 * attached to any device's devres (the device is owned by the nvidia
 * driver); module_exit() tears everything down manually.
 */
struct nv_sensors_extra_priv {
	struct pci_dev *pdev;
	void __iomem *base;
	const struct nv_sensors_extra_entry *entry;
	struct device *hwdev;
	char label1[64];
	char label2[64];
	char label3[64];
};

static struct nv_sensors_extra_priv nv_sensors_extra_privs[NV_SENSORS_EXTRA_MAX_DEVICES];
static int nv_sensors_extra_num_privs;

const struct nv_sensors_extra_entry *nv_sensors_extra_match(u16 device_id)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(nv_sensors_extra_dev_table); i++) {
		if (nv_sensors_extra_dev_table[i].device_id == device_id)
			return &nv_sensors_extra_dev_table[i];
	}

	return NULL;
}

long nv_sensors_extra_vram_temp(u32 raw)
{
	if (raw == 0x00000000U || raw == 0xFFFFFFFFU)
		return -ENODATA;

	/*
	 * VRAM temperature in degrees Celsius is encoded in the low 12
	 * bits of the register with 1/32 degree resolution.
	 */
	return ((long)(raw & 0x00000FFFU) * 1000) / 32;
}

long nv_sensors_extra_hotspot_temp(u32 raw)
{
	if (raw == 0x00000000U || raw == 0xFFFFFFFFU)
		return -ENODATA;

	/*
	 * 16-bit fixed point: bits [15:8] are integer degrees Celsius and the
	 * low byte is the fraction (1/256 degree Celsius per LSB). Available
	 * if the primary sensor flag (bit 29) or the shadow sensor flag
	 * (bit 30) is set; under the proprietary driver (GSP-RM) the
	 * firmware populates the shadow sensors, so bit 29 stays clear.
	 */
	if (!(raw & (0x20000000U | 0x40000000U)))
		return -ENODATA;

	return (long)((raw & 0x0001FFF8U) >> 8) * 1000 +
	       (long)((raw & 0xFFU) * 1000) / 256;
}

long nv_sensors_extra_core_temp(u32 raw)
{
	if (raw == 0x00000000U || raw == 0xFFFFFFFFU)
		return -ENODATA;

	/*
	 * The reading is available if the primary sensor flag (bit 29) or the
	 * shadow sensor flag (bit 30) is set. Under the proprietary driver
	 * (GSP-RM) the firmware populates the shadow sensors, so bit 29 stays
	 * clear.
	 *
	 * 16-bit fixed point: bits [15:8] are integer degrees Celsius and the
	 * low byte is the fraction (1/256 degree Celsius per LSB). No
	 * calibration is applied here: the raw reading runs a couple of
	 * degrees above the NVML value, see the README for adjusting it
	 * through the lm-sensors offset.
	 */
	if (!(raw & (0x20000000U | 0x40000000U)))
		return -ENODATA;

	return (long)((raw & 0x0001FFF8U) >> 8) * 1000 +
	       (long)((raw & 0xFFU) * 1000) / 256;
}

bool nv_sensors_extra_bound_to_nvidia(const struct pci_dev *pdev)
{
	return pdev->driver && !strcmp(pdev->driver->driver.name, "nvidia");
}

static umode_t nv_sensors_extra_hwmon_visible(const void *drvdata,
					  enum hwmon_sensor_types type,
					  u32 attr, int channel)
{
	/*
	 * attr is the plain attribute enum value (not a BIT mask) and
	 * channel is a 0-based index; sysfs names are channel + 1.
	 */
	if (type == hwmon_temp &&
	    (attr == hwmon_temp_input || attr == hwmon_temp_label) &&
	    channel >= 0 && channel <= 2)
		return 0444;

	return 0;
}

static int nv_sensors_extra_hwmon_read(struct device *dev,
				  enum hwmon_sensor_types type, u32 attr,
				  int channel, long *val)
{
	struct nv_sensors_extra_priv *priv = dev_get_drvdata(dev);
	long temp;
	u32 raw;

	if (type != hwmon_temp || attr != hwmon_temp_input)
		return -EOPNOTSUPP;

	switch (channel) {
	case 0:
		raw = readl(priv->base + priv->entry->core_offset);
		temp = nv_sensors_extra_core_temp(raw);
		break;
	case 1:
		raw = readl(priv->base + priv->entry->hotspot_offset);
		temp = nv_sensors_extra_hotspot_temp(raw);
		break;
	case 2:
		raw = readl(priv->base + priv->entry->vram_offset);
		temp = nv_sensors_extra_vram_temp(raw);
		break;
	default:
		return -EOPNOTSUPP;
	}

	if (temp < 0)
		return (int)temp;

	*val = temp;
	return 0;
}

static int nv_sensors_extra_hwmon_read_string(struct device *dev,
					 enum hwmon_sensor_types type,
					 u32 attr, int channel,
					 const char **str)
{
	struct nv_sensors_extra_priv *priv = dev_get_drvdata(dev);

	if (type != hwmon_temp || attr != hwmon_temp_label)
		return -EOPNOTSUPP;

	switch (channel) {
	case 0:
		*str = priv->label1;
		break;
	case 1:
		*str = priv->label2;
		break;
	case 2:
		*str = priv->label3;
		break;
	default:
		return -EOPNOTSUPP;
	}

	return 0;
}

/*
 * Three temperature channels, each with an input and a label:
 *   channel 1 - GPU core temperature
 *   channel 2 - GPU hotspot temperature
 *   channel 3 - memory junction temperature
 *
 * Every element of config[] is the attribute mask of one channel (the
 * array is terminated by 0) and channel numbering restarts for every
 * hwmon_channel_info entry, so all channels must be described inside a
 * single HWMON_CHANNEL_INFO() entry.
 */
static const struct hwmon_channel_info * const nv_sensors_extra_info[] = {
	HWMON_CHANNEL_INFO(temp, HWMON_T_INPUT | HWMON_T_LABEL,
			       HWMON_T_INPUT | HWMON_T_LABEL,
			       HWMON_T_INPUT | HWMON_T_LABEL),
	NULL,
};

static const struct hwmon_ops nv_sensors_extra_hwmon_ops = {
	.is_visible = nv_sensors_extra_hwmon_visible,
	.read = nv_sensors_extra_hwmon_read,
	.read_string = nv_sensors_extra_hwmon_read_string,
};

static const struct hwmon_chip_info nv_sensors_extra_chip_info = {
	.ops = &nv_sensors_extra_hwmon_ops,
	.info = nv_sensors_extra_info,
};

/*
 * nv_sensors_extra_add_gpu() - register one supported GPU as an hwmon device
 *
 * Takes our own PCI reference (independent of the reference held by the
 * scanning loop), maps BAR0, builds the channel labels and registers the
 * hwmon device.
 */
static int nv_sensors_extra_add_gpu(struct pci_dev *pdev,
			    const struct nv_sensors_extra_entry *entry)
{
	struct nv_sensors_extra_priv *priv = &nv_sensors_extra_privs[nv_sensors_extra_num_privs];
	void __iomem *base;
	struct device *hwdev;
	int err;

	memset(priv, 0, sizeof(*priv));

	priv->pdev = pci_dev_get(pdev);
	if (!priv->pdev)
		return -ENODEV;

	base = pci_iomap(pdev, 0, 0);
	if (!base) {
		err = -ENOMEM;
		goto iomap_fail;
	}
	priv->base = base;
	priv->entry = entry;

	snprintf(priv->label1, sizeof(priv->label1),
		 "%s GPU core temperature", entry->name);
	snprintf(priv->label2, sizeof(priv->label2),
		 "%s GPU hotspot temperature", entry->name);
	snprintf(priv->label3, sizeof(priv->label3),
		 "%s memory junction temperature", entry->name);

	hwdev = hwmon_device_register_with_info(&pdev->dev,
						NV_SENSORS_EXTRA_HWMON_NAME, priv,
						&nv_sensors_extra_chip_info,
						NULL);
	if (IS_ERR(hwdev)) {
		err = PTR_ERR(hwdev);
		goto register_fail;
	}
	priv->hwdev = hwdev;
	nv_sensors_extra_num_privs++;

	pr_info("%s: %s (%s, %s) registered: "
		"temp1 = GPU core temperature, "
		"temp2 = GPU hotspot temperature, "
		"temp3 = memory junction temperature\n",
		pci_name(pdev), entry->name, entry->arch, entry->vram);

	return 0;

register_fail:
	pci_iounmap(pdev, base);
iomap_fail:
	pci_dev_put(priv->pdev);
	priv->pdev = NULL;
	priv->base = NULL;
	priv->entry = NULL;
	return err;
}

/*
 * nv_sensors_extra_unwind_devices() - undo all registrations made so far
 *
 * Used both on module_init() error paths and from module_exit().
 */
static void nv_sensors_extra_unwind_devices(void)
{
	int i;

	for (i = nv_sensors_extra_num_privs - 1; i >= 0; i--) {
		struct nv_sensors_extra_priv *priv = &nv_sensors_extra_privs[i];

		hwmon_device_unregister(priv->hwdev);
		pci_iounmap(priv->pdev, priv->base);
		pci_dev_put(priv->pdev);
		priv->hwdev = NULL;
		priv->base = NULL;
		priv->entry = NULL;
		priv->pdev = NULL;
	}

	nv_sensors_extra_num_privs = 0;
}

static int __init nv_sensors_extra_init(void)
{
	struct pci_dev *pdev = NULL;
	int err;

	/*
	 * Scan the PCI bus. pci_get_device() (behind for_each_pci_dev)
	 * holds exactly one reference at a time: every call drops the
	 * reference of the previous device (passed as @from) and takes
	 * one on the next, so no references are outstanding when the
	 * loop terminates. Do not add pci_dev_put() calls to the loop
	 * body. Devices we claim get a reference of our own through
	 * pci_dev_get() in nv_sensors_extra_add_gpu().
	 */
	for_each_pci_dev(pdev) {
		const struct nv_sensors_extra_entry *entry;

		if (pdev->vendor != PCI_VENDOR_ID_NVIDIA ||
		    !(entry = nv_sensors_extra_match(pdev->device)) ||
		    !nv_sensors_extra_bound_to_nvidia(pdev))
			continue;

		if (nv_sensors_extra_num_privs >= ARRAY_SIZE(nv_sensors_extra_privs)) {
			pr_warn("skipping %s: maximum of %d supported GPUs "
				"reached\n",
				pci_name(pdev),
				(int)ARRAY_SIZE(nv_sensors_extra_privs));
			continue;
		}

		err = nv_sensors_extra_add_gpu(pdev, entry);
		if (err) {
			pr_err("%s: failed to register device (%d)\n",
			       pci_name(pdev), err);
			nv_sensors_extra_unwind_devices();
			/*
			 * Leaving the loop early: the next
			 * pci_get_device() call that would normally
			 * drop the scan reference does not happen.
			 */
			pci_dev_put(pdev);
			return err;
		}
	}

	if (!nv_sensors_extra_num_privs) {
		pr_err("no supported NVIDIA GPU bound to the nvidia driver "
		       "found\n");
		return -ENODEV;
	}

	return 0;
}

static void __exit nv_sensors_extra_exit(void)
{
	/*
	 * devm_ helpers cannot be used here: the devres list belongs to the
	 * device owned by the nvidia driver and would never run when this
	 * module is removed.
	 */
	nv_sensors_extra_unwind_devices();
}

module_init(nv_sensors_extra_init);
module_exit(nv_sensors_extra_exit);

/* The GPU must be owned by the proprietary nvidia driver first. */
MODULE_SOFTDEP("pre: nvidia");
MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION("NVIDIA GDDR6/GDDR6X memory, GPU hotspot and GPU core temperatures");
