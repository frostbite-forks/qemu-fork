/*
 * Broadcom BCM4321 802.11a/b/g/n PCI stub
 *
 * The BCM4321 (PCI 14e4:4328) is the 802.11n WiFi card used in the
 * Apple TV 1st generation.  A full MAC/PHY emulation is not provided;
 * this stub presents the correct PCI identity so boot.efi and OS drivers
 * can enumerate the device without crashing.  All MMIO reads return 0
 * and writes are silently ignored, which is enough for an OS to probe
 * the card and then fail gracefully (or load the b43/wl driver which
 * will handle firmware loading separately).
 *
 * Copyright (c) 2024 QEMU contributors
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qom/object.h"

#define TYPE_BCM4321_WIFI   "bcm4321-wifi"
OBJECT_DECLARE_SIMPLE_TYPE(BCM4321State, BCM4321_WIFI)

#define PCI_VENDOR_ID_BROADCOM  0x14e4
#define BCM4321_DEVICE_ID       0x4328
#define BCM4321_REVISION        0x01   /* matches Apple TV hardware rev */

/* BCM4321 exposes two MMIO BARs on real hardware */
#define BCM4321_BAR0_SIZE   (16 * KiB)   /* chipset config registers */
#define BCM4321_BAR1_SIZE   (1 * MiB)    /* DMA / extended registers */

struct BCM4321State {
    PCIDevice   dev;
    MemoryRegion bar0;
    MemoryRegion bar1;
};

static uint64_t bcm4321_mmio_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void bcm4321_mmio_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    /* Stub: writes ignored */
}

static const MemoryRegionOps bcm4321_mmio_ops = {
    .read  = bcm4321_mmio_read,
    .write = bcm4321_mmio_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void bcm4321_realize(PCIDevice *pdev, Error **errp)
{
    BCM4321State *s = BCM4321_WIFI(pdev);

    /* Apple TV uses NVIDIA sub-system — leave sub-ids at zero for now */
    pci_set_word(pdev->config + PCI_SUBSYSTEM_VENDOR_ID, 0x106b); /* Apple */
    pci_set_word(pdev->config + PCI_SUBSYSTEM_ID,        0x004e);

    /* BAR 0: 64-bit MMIO config registers */
    memory_region_init_io(&s->bar0, OBJECT(pdev), &bcm4321_mmio_ops, s,
                          "bcm4321.bar0", BCM4321_BAR0_SIZE);
    pci_register_bar(pdev, 0,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->bar0);

    /* BAR 2: 64-bit prefetchable extended register space */
    memory_region_init_io(&s->bar1, OBJECT(pdev), &bcm4321_mmio_ops, s,
                          "bcm4321.bar1", BCM4321_BAR1_SIZE);
    pci_register_bar(pdev, 2,
                     PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->bar1);
}

static const VMStateDescription vmstate_bcm4321 = {
    .name = "bcm4321-wifi",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, BCM4321State),
        VMSTATE_END_OF_LIST()
    },
};

static void bcm4321_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass    *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k  = PCI_DEVICE_CLASS(klass);

    k->realize   = bcm4321_realize;
    k->vendor_id = PCI_VENDOR_ID_BROADCOM;
    k->device_id = BCM4321_DEVICE_ID;
    k->revision  = BCM4321_REVISION;
    k->class_id  = PCI_CLASS_NETWORK_OTHER;   /* 0x0280 — network controller */

    dc->desc = "Broadcom BCM4321 802.11a/b/g/n (stub)";
    dc->vmsd = &vmstate_bcm4321;
    set_bit(DEVICE_CATEGORY_NETWORK, dc->categories);
}

static const TypeInfo bcm4321_info = {
    .name          = TYPE_BCM4321_WIFI,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(BCM4321State),
    .class_init    = bcm4321_class_init,
    .interfaces    = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void bcm4321_register_types(void)
{
    type_register_static(&bcm4321_info);
}

type_init(bcm4321_register_types)
