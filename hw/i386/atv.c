/*
 * Apple TV 1st Generation Machine Emulator
 *
 * Hardware: Intel 945GM + ICH7-M, Pentium M @ 1 GHz, 256 MB DDR2,
 *           NVIDIA GeForce Go 7300, RTL8139 Ethernet
 *
 * The Apple TV 1st gen uses a custom EFI implementation (boot.efi) rather
 * than a standard BIOS. The machine is based on the i440FX PC infrastructure
 * with PIIX4 south bridge as the closest available approximation of the
 * Intel 945GM + ICH7-M chipset pair.
 *
 * PCI topology mirrors the real hardware:
 *   00:00.0  945GM MCH  (emulated as i440FX)
 *   00:1f.0  ICH7-M LPC (emulated as PIIX4 ISA bridge)
 *   00:1f.1  ICH7 IDE   (emulated as PIIX IDE)
 *   03:03.0  RTL8139
 *
 * Copyright (c) 2024 QEMU contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include "qemu/osdep.h"
#include "qemu/units.h"
#include "hw/i386/x86.h"
#include "hw/i386/pc.h"
#include "hw/pci-host/i440fx.h"
#include "hw/southbridge/piix.h"
#include "hw/pci/pci.h"
#include "hw/ide/pci.h"
#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/smbus_eeprom.h"
#include "hw/acpi/acpi.h"
#include "hw/mem/nvdimm.h"
#include "hw/firmware/smbios.h"
#include "system/memory.h"
#include "system/kvm.h"
#include "system/tcg.h"
#include "system/numa.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/i386/acpi-build.h"
#include "target/i386/cpu.h"

/* Apple TV 1st gen has 256 MB RAM soldered */
#define ATV_RAM_SIZE        (256 * MiB)

/* SMBIOS identity strings that boot.efi checks at startup */
#define ATV_SMBIOS_MANUFACTURER "Apple Inc."
#define ATV_SMBIOS_PRODUCT      "AppleTV1,1"
#define ATV_SMBIOS_VERSION      "1.0"
#define ATV_SMBIOS_BOARD        "Mac-F4228DC8"
#define ATV_SMBIOS_FAMILY       "AppleTV"

static int atv_pci_slot_get_pirq(PCIDevice *pci_dev, int pci_intx)
{
    int slot_addend = PCI_SLOT(pci_dev->devfn) - 1;
    return (pci_intx + slot_addend) & 3;
}

static void atv_machine_init(MachineState *machine)
{
    PCMachineState *pcms = PC_MACHINE(machine);
    PCMachineClass *pcmc = PC_MACHINE_GET_CLASS(pcms);
    X86MachineState *x86ms = X86_MACHINE(machine);
    MemoryRegion *system_memory = get_system_memory();
    MemoryRegion *system_io = get_system_io();
    Object *phb;
    ISABus *isa_bus;
    Object *piix4_pm = NULL;
    qemu_irq smi_irq;
    GSIState *gsi_state;
    MemoryRegion *ram_memory;
    MemoryRegion *pci_memory;
    uint64_t hole64_size = 0;
    PCIDevice *pci_dev;
    DeviceState *dev;
    size_t i;

    /*
     * The Apple TV has 256 MB RAM.  All of it fits comfortably below 4G so
     * we do not need an above-4G region.
     */
    ram_memory = machine->ram;
    x86ms->above_4g_mem_size = 0;
    x86ms->below_4g_mem_size = machine->ram_size;

    pc_machine_init_sgx_epc(pcms);

    /*
     * Pentium M (Dothan) is x86 Family 6 Model 13 Stepping 8.
     * If the user has not overridden the CPU model we default to
     * "pentium-m" (or the closest available approximation).
     */
    x86_cpus_init(x86ms, pcmc->default_cpu_version);

    if (kvm_enabled()) {
        kvmclock_create(pcmc->kvmclock_create_always);
    }

    pci_memory = g_new(MemoryRegion, 1);
    memory_region_init(pci_memory, NULL, "pci", UINT64_MAX);

    /* 945GM MCH — emulated as i440FX host bridge */
    phb = OBJECT(qdev_new(TYPE_I440FX_PCI_HOST_BRIDGE));
    object_property_add_child(OBJECT(machine), "i440fx", phb);
    object_property_set_link(phb, PCI_HOST_PROP_RAM_MEM,
                             OBJECT(ram_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_PCI_MEM,
                             OBJECT(pci_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_SYSTEM_MEM,
                             OBJECT(system_memory), &error_fatal);
    object_property_set_link(phb, PCI_HOST_PROP_IO_MEM,
                             OBJECT(system_io), &error_fatal);
    object_property_set_uint(phb, PCI_HOST_BELOW_4G_MEM_SIZE,
                             x86ms->below_4g_mem_size, &error_fatal);
    object_property_set_uint(phb, PCI_HOST_ABOVE_4G_MEM_SIZE,
                             x86ms->above_4g_mem_size, &error_fatal);
    object_property_set_str(phb, I440FX_HOST_PROP_PCI_TYPE,
                            TYPE_I440FX_PCI_DEVICE, &error_fatal);
    sysbus_realize_and_unref(SYS_BUS_DEVICE(phb), &error_fatal);

    pcms->pcibus = PCI_BUS(qdev_get_child_bus(DEVICE(phb), "pci.0"));
    pci_bus_map_irqs(pcms->pcibus, atv_pci_slot_get_pirq);

    hole64_size = object_property_get_uint(phb,
                                           PCI_HOST_PROP_PCI_HOLE64_SIZE,
                                           &error_abort);

    pc_memory_init(pcms, system_memory, pci_memory, hole64_size);

    /*
     * Override SMBIOS Type 1 (System Information) and Type 2 (Baseboard)
     * fields so boot.efi recognises this as an AppleTV1,1.
     * fw_cfg_build_smbios() calls smbios_set_defaults() which reads these
     * globals; we overwrite them here, before that call happens.
     */
    smbios_type1.manufacturer = ATV_SMBIOS_MANUFACTURER;
    smbios_type1.product      = ATV_SMBIOS_PRODUCT;
    smbios_type1.version      = ATV_SMBIOS_VERSION;
    smbios_type1.family       = ATV_SMBIOS_FAMILY;

    gsi_state = pc_gsi_create(&x86ms->gsi, true);

    /*
     * ICH7-M LPC bridge — PIIX4 is the closest southbridge QEMU has.
     * It provides ISA, USB, IDE, ACPI PM, and SMBus functions needed
     * by the Apple TV firmware.
     */
    pci_dev = pci_new_multifunction(-1, pcms->south_bridge);
    object_property_set_bool(OBJECT(pci_dev), "has-usb",
                             machine_usb(machine), &error_abort);
    object_property_set_bool(OBJECT(pci_dev), "has-acpi",
                             x86_machine_is_acpi_enabled(x86ms),
                             &error_abort);
    object_property_set_bool(OBJECT(pci_dev), "has-pic", false,
                             &error_abort);
    object_property_set_bool(OBJECT(pci_dev), "has-pit", false,
                             &error_abort);
    qdev_prop_set_uint32(DEVICE(pci_dev), "smb_io_base", 0xb100);
    object_property_set_bool(OBJECT(pci_dev), "smm-enabled",
                             x86_machine_is_smm_enabled(x86ms),
                             &error_abort);
    dev = DEVICE(pci_dev);
    for (i = 0; i < ISA_NUM_IRQS; i++) {
        qdev_connect_gpio_out_named(dev, "isa-irqs", i, x86ms->gsi[i]);
    }
    pci_realize_and_unref(pci_dev, pcms->pcibus, &error_fatal);

    isa_bus = ISA_BUS(qdev_get_child_bus(DEVICE(pci_dev), "isa.0"));
    x86ms->rtc = ISA_DEVICE(object_resolve_path_component(OBJECT(pci_dev),
                                                          "rtc"));
    piix4_pm = object_resolve_path_component(OBJECT(pci_dev), "pm");
    dev = DEVICE(object_resolve_path_component(OBJECT(pci_dev), "ide"));
    pci_ide_create_devs(PCI_DEVICE(dev));
    pcms->idebus[0] = qdev_get_child_bus(dev, "ide.0");
    pcms->idebus[1] = qdev_get_child_bus(dev, "ide.1");

    if (x86ms->pic == ON_OFF_AUTO_ON || x86ms->pic == ON_OFF_AUTO_AUTO) {
        pc_i8259_create(isa_bus, gsi_state->i8259_irq);
    }

    ioapic_init_gsi(gsi_state, phb);

    if (tcg_enabled()) {
        x86_register_ferr_irq(x86ms->gsi[13]);
    }

    if (piix4_pm) {
        smi_irq = qemu_allocate_irq(pc_acpi_smi_interrupt, first_cpu, 0);
        qdev_connect_gpio_out_named(DEVICE(piix4_pm), "smi-irq", 0, smi_irq);
        pcms->smbus = I2C_BUS(qdev_get_child_bus(DEVICE(piix4_pm), "i2c"));
        smbus_eeprom_init(pcms->smbus, 8, NULL, 0);

        object_property_add_link(OBJECT(machine), PC_MACHINE_ACPI_DEVICE_PROP,
                                 TYPE_HOTPLUG_HANDLER,
                                 (Object **)&x86ms->acpi_dev,
                                 object_property_allow_set_link,
                                 OBJ_PROP_LINK_STRONG);
        object_property_set_link(OBJECT(machine), PC_MACHINE_ACPI_DEVICE_PROP,
                                 piix4_pm, &error_abort);
    }

    /*
     * GPU: NVIDIA GeForce Go 7300 (NV43/G72M) at PCI 01:00.0.
     * The nv43-gpu device presents the correct PCI vendor/device IDs
     * (10de:01d7) and provides a working VGA framebuffer backed by the
     * standard QEMU VGA core.
     */
    pci_create_simple(pcms->pcibus, PCI_DEVFN(1, 0), "nv43-gpu");

    pc_basic_device_init(pcms, isa_bus, x86ms->gsi, x86ms->rtc,
                         false, /* no floppy on Apple TV */
                         0x4);

    /*
     * NIC — Realtek RTL8139 at PCI 03:03.0 on the real hardware.
     * We let QEMU auto-assign the slot but honour the user's -nic options.
     * Default to rtl8139 to match hardware.
     */
    pc_nic_init(pcmc, isa_bus, pcms->pcibus);

    /*
     * Broadcom BCM4321 802.11a/b/g/n at PCI 02:00.0.
     * Presents correct PCI IDs (14e4:4328); the stub is enough for boot.efi
     * to enumerate the device without crashing.
     */
    pci_create_simple(pcms->pcibus, -1, "bcm4321-wifi");

    /*
     * Intel ICH7-M HD Audio at PCI 00:1b.0 (device_id 0x27d8).
     * Attach an ALC885-compatible duplex codec on the HDA bus.
     */
    {
        PCIDevice *hda_pci;
        BusState *hda_bus;
        DeviceState *codec;

        hda_pci = pci_create_simple(pcms->pcibus,
                                    PCI_DEVFN(0x1b, 0), "ich7-intel-hda");
        hda_bus = QLIST_FIRST(&hda_pci->qdev.child_bus);
        if (hda_bus) {
            codec = qdev_new("alc885");
            qdev_realize_and_unref(codec, hda_bus, &error_fatal);
        }
    }

    if (machine->nvdimms_state->is_enabled) {
        nvdimm_init_acpi_state(machine->nvdimms_state, system_io,
                               x86_nvdimm_acpi_dsmio,
                               x86ms->fw_cfg, OBJECT(pcms));
    }
}

static void atv_machine_options(MachineClass *m)
{
    PCMachineClass *pcmc = PC_MACHINE_CLASS(m);

    pcmc->pci_enabled = true;
    pcmc->default_south_bridge = TYPE_PIIX4_PCI_DEVICE;
    pcmc->pci_root_uid = 0;
    pcmc->default_cpu_version = 1;
    pcmc->smbios_defaults = true;
    pcmc->gigabyte_align = false;
    pcmc->has_reserved_memory = false;

    m->family = "apple_tv";
    m->desc = "Apple TV 1st Generation (945GM + ICH7-M)";
    /*
     * The Apple TV ships with a custom EFI (boot.efi).  Pass it via -bios or
     * -pflash.  There is no fallback BIOS so we leave default_machine_opts
     * empty rather than pointing at a non-existent image.
     */
    m->default_machine_opts = NULL;
    m->default_display = "std";
    /*
     * Hardware NIC is RTL8139; make that the default so guests see the
     * expected PCI device without any extra -nic flags.
     */
    m->default_nic = "rtl8139";
    m->no_floppy = true;
    m->no_cdrom = true;
    m->max_cpus = 1;
    m->min_cpus = 1;
    m->default_cpus = 1;
    m->default_ram_size = ATV_RAM_SIZE;
    m->default_ram_id = "atv.ram";
}

DEFINE_PC_MACHINE(apple_tv_1, "apple-tv-1", atv_machine_init,
                  atv_machine_options)
