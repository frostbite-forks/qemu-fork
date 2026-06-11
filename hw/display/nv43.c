/*
 * NVIDIA GeForce Go 7300 (NV43 / G72M) PCI stub with VGA framebuffer
 *
 * The G72M (device_id 0x01d7) is an NV43-family mobile GPU used in the
 * Apple TV 1st generation.  A full NV43 3D engine is not emulated here;
 * instead the device presents the correct PCI identity and delegates all
 * display work to the standard QEMU VGA core, which is sufficient for
 * boot.efi framebuffer output and VGA-compatible OS drivers.
 *
 * PCI BARs mirror the real hardware layout:
 *   BAR 0 (32-bit MEM, 16 MB)  — NV register space   (stub: unassigned)
 *   BAR 1 (64-bit PMEM, 256 MB) — VRAM aperture       (VGA VRAM)
 *   BAR 2 (64-bit MEM, 16 MB)  — RAMIN / FIFO space   (stub: unassigned)
 *   BAR 6 (32-bit PMEM)        — ROM                  (VGA BIOS)
 *
 * Copyright (c) 2024 QEMU contributors
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/pci/pci_device.h"
#include "hw/pci/pci_ids.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "vga_int.h"
#include "ui/console.h"
#include "qapi/error.h"
#include "qom/object.h"

#define TYPE_NV43_GPU "nv43-gpu"
OBJECT_DECLARE_SIMPLE_TYPE(NV43State, NV43_GPU)

/* NV43 G72M — GeForce Go 7300 */
#define NV43_VENDOR_ID      PCI_VENDOR_ID_NVIDIA   /* 0x10de */
#define NV43_DEVICE_ID      0x01d7
#define NV43_REVISION       0xa1                   /* matches real hardware */
#define NV43_SUBSYS_ID      0x01d710de             /* NVIDIA reference sub-id */

/* 64 MB VRAM (128 MB on some units; use the common 64 MB default) */
#define NV43_VRAM_SIZE_MB   64

struct NV43State {
    PCIDevice   dev;
    VGACommonState vga;
    MemoryRegion bar0;   /* NV register space (16 MB stub) */
    MemoryRegion bar2;   /* RAMIN / FIFO (16 MB stub) */
};

/* BAR 0: 16 MB NV register window — reads return 0, writes ignored */
static uint64_t nv43_regs_read(void *opaque, hwaddr addr, unsigned size)
{
    return 0;
}

static void nv43_regs_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    /* NV register writes ignored in stub */
}

static const MemoryRegionOps nv43_regs_ops = {
    .read  = nv43_regs_read,
    .write = nv43_regs_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void nv43_realize(PCIDevice *pdev, Error **errp)
{
    NV43State *s = NV43_GPU(pdev);
    VGACommonState *vga = &s->vga;

    /* Set PCI subsystem ID to match Apple TV board */
    pci_set_word(pdev->config + PCI_SUBSYSTEM_VENDOR_ID, 0x10de);
    pci_set_word(pdev->config + PCI_SUBSYSTEM_ID, 0x01d7);

    /* VGA core — provides the actual display output */
    if (!vga_common_init(vga, OBJECT(pdev), errp)) {
        return;
    }
    vga->vram_size_mb = NV43_VRAM_SIZE_MB;
    vga_init(vga, OBJECT(pdev),
             pci_address_space(pdev),
             pci_address_space_io(pdev),
             true);
    vga->con = qemu_graphic_console_create(DEVICE(pdev), 0, vga->hw_ops, vga);

    /*
     * BAR 1 (64-bit prefetchable) — VRAM aperture.
     * We expose the VGA VRAM region here; the real GPU maps 256 MB but
     * guests only touch the first vram_size bytes.
     */
    pci_register_bar(pdev, 1,
                     PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &vga->vram);

    /* BAR 0 — 16 MB NV register stub */
    memory_region_init_io(&s->bar0, OBJECT(pdev), &nv43_regs_ops, s,
                          "nv43.regs", 16 * MiB);
    pci_register_bar(pdev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar0);

    /* BAR 2 — 16 MB RAMIN/FIFO stub (64-bit) */
    memory_region_init_io(&s->bar2, OBJECT(pdev), &nv43_regs_ops, s,
                          "nv43.ramin", 16 * MiB);
    pci_register_bar(pdev, 2,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &s->bar2);
}

static void nv43_exit(PCIDevice *pdev)
{
    NV43State *s = NV43_GPU(pdev);
    qemu_graphic_console_close(s->vga.con);
}

static const VMStateDescription vmstate_nv43 = {
    .name = "nv43-gpu",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(dev, NV43State),
        VMSTATE_STRUCT(vga, NV43State, 0, vmstate_vga_common, VGACommonState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property nv43_properties[] = {
    DEFINE_PROP_UINT32("vgamem_mb", NV43State, vga.vram_size_mb,
                       NV43_VRAM_SIZE_MB),
};

static void nv43_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass  *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *k = PCI_DEVICE_CLASS(klass);

    k->realize   = nv43_realize;
    k->exit      = nv43_exit;
    k->vendor_id = NV43_VENDOR_ID;
    k->device_id = NV43_DEVICE_ID;
    k->revision  = NV43_REVISION;
    k->class_id  = PCI_CLASS_DISPLAY_VGA;
    k->romfile   = "vgabios-stdvga.bin";

    dc->desc  = "NVIDIA GeForce Go 7300 (NV43/G72M)";
    dc->vmsd  = &vmstate_nv43;
    dc->hotpluggable = false;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
    device_class_set_props(dc, nv43_properties);
}

static const TypeInfo nv43_info = {
    .name          = TYPE_NV43_GPU,
    .parent        = TYPE_PCI_DEVICE,
    .instance_size = sizeof(NV43State),
    .class_init    = nv43_class_init,
    .interfaces    = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void nv43_register_types(void)
{
    type_register_static(&nv43_info);
}

type_init(nv43_register_types)
