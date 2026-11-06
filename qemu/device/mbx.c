/*
 * PowerVR MBX1 (the OMAP2420's graphics core, as on the Nokia N800/N810)
 *
 * A thin QEMU device around mbxemu: one 64 KB
 * MMIO window at 0x50000000 (registers at 0, the TA, 2D and TA control
 * slave ports at 0x8000, 0xa000, 0xc000), one interrupt line (the INTC's
 * line 21), and the MBX's memory accesses as a bus master through
 * address_space_memory, after the MBX's own MMU (which the core models).
 *
 * Everything runs in the vCPU's MMIO access: a render kick rasterizes the
 * scene before the write returns, and the completion interrupt is raised
 * then. No migration state: a snapshot loses the scene being built.
 *
 * Builds in QEMU 9.1 (the last with the n800/n810 machines), with
 * qemu-n8x0's patches.
 *
 * SPDX-License-Identifier: MIT
 */
#include "qemu/osdep.h"
#include "hw/sysbus.h"
#include "exec/address-spaces.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/irq.h"
#include "hw/qdev-properties.h"
#include "qemu/module.h"
#include "mbxemu/mbxemu.h"

#define TYPE_MBX "mbx"
#define MBX(obj) OBJECT_CHECK(MBXState, (obj), TYPE_MBX)

typedef struct MBXState {
    SysBusDevice parent_obj;

    MemoryRegion iomem;
    qemu_irq irq;
    struct mbxemu *emu;
    uint32_t trace;
} MBXState;

static void mbx_mem_read(void *opaque, uint32_t phys, void *buf, uint32_t len)
{
    address_space_read(&address_space_memory, phys, MEMTXATTRS_UNSPECIFIED,
                       buf, len);
}

static void mbx_mem_write(void *opaque, uint32_t phys, const void *buf, uint32_t len)
{
    address_space_write(&address_space_memory, phys, MEMTXATTRS_UNSPECIFIED,
                        buf, len);
}

static void mbx_irq(void *opaque, int level)
{
    MBXState *s = opaque;

    qemu_set_irq(s->irq, level);
}

static void mbx_log(void *opaque, const char *line)
{
    fprintf(stderr, "mbx: %s\n", line);
}

static uint64_t mbx_read(void *opaque, hwaddr off, unsigned size)
{
    MBXState *s = opaque;

    return mbxemu_read(s->emu, off);
}

static void mbx_write(void *opaque, hwaddr off, uint64_t v, unsigned size)
{
    MBXState *s = opaque;

    mbxemu_write(s->emu, off, v);
}

static const MemoryRegionOps mbx_ops = {
    .read = mbx_read,
    .write = mbx_write,
    .endianness = DEVICE_NATIVE_ENDIAN,
    .valid = {
        .min_access_size = 4,
        .max_access_size = 4,
    },
};

static void mbx_reset(DeviceState *dev)
{
    MBXState *s = MBX(dev);

    mbxemu_reset(s->emu);
}

static void mbx_realize(DeviceState *dev, Error **errp)
{
    SysBusDevice *sbd = SYS_BUS_DEVICE(dev);
    MBXState *s = MBX(sbd);
    struct mbxemu_host host = {
        .opaque = s,
        .mem_read = mbx_mem_read,
        .mem_write = mbx_mem_write,
        .irq = mbx_irq,
        .log = mbx_log,
    };

    memory_region_init_io(&s->iomem, OBJECT(s), &mbx_ops, s, "mbx", MBXEMU_SIZE);
    sysbus_init_mmio(sbd, &s->iomem);
    sysbus_init_irq(sbd, &s->irq);
    s->emu = mbxemu_new(&host);
    if (!s->emu) {
        error_setg(errp, "mbx: cannot create the emulator");
        return;
    }
    mbxemu_set_trace(s->emu, s->trace);
}

static Property mbx_properties[] = {
    DEFINE_PROP_UINT32("trace", MBXState, trace, 0),
    DEFINE_PROP_END_OF_LIST(),
};

static void mbx_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->realize = mbx_realize;
    device_class_set_props(dc, mbx_properties);
    dc->reset = mbx_reset;
    dc->desc = "PowerVR MBX1 (functional model, mbxemu)";
}

static const TypeInfo mbx_info = {
    .name          = TYPE_MBX,
    .parent        = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MBXState),
    .class_init    = mbx_class_init,
};

static void mbx_register_types(void)
{
    type_register_static(&mbx_info);
}

type_init(mbx_register_types)
