#!/usr/bin/env python3
# SPDX-License-Identifier: MIT
"""
Put the MBX device into a QEMU 9.1 tree that has this repository's
patches for it applied: hw/display/mbx.c, the mbxemu core under
hw/display/mbxemu/, the build lines, and in hw/arm/omap2.c the device at
0x50000000 on INTC line 21 (OMAP_INT_24XX_GUFFAW_IRQ) plus the GFX reset
status the guest driver polls for.

    device/install.py <qemu tree>
"""
import os
import shutil
import sys

here = os.path.dirname(os.path.abspath(__file__))
emu = os.path.join(os.path.dirname(os.path.dirname(here)), "mbx")
tree = sys.argv[1]

os.makedirs(os.path.join(tree, "hw/display/mbxemu"), exist_ok=True)
shutil.copy(os.path.join(here, "mbx.c"), os.path.join(tree, "hw/display/mbx.c"))
for f in ("mbxemu.h", "emu.h", "core.c", "ta.c", "vgp.c", "render.c", "rcptab.c", "texture.c", "twod.c", "pb.c"):
    shutil.copy(os.path.join(emu, f), os.path.join(tree, "hw/display/mbxemu", f))


def edit(path, old, new):
    p = os.path.join(tree, path)
    s = open(p).read()
    if new in s:
        return
    if old not in s:
        sys.exit("%s: the text to change isn't there" % path)
    open(p, "w").write(s.replace(old, new, 1))


edit("hw/display/meson.build",
     "system_ss.add(when: 'CONFIG_OMAP', if_true: files('omap_lcdc.c'))\n",
     "system_ss.add(when: 'CONFIG_OMAP', if_true: files('omap_lcdc.c'))\n"
     "system_ss.add(when: 'CONFIG_OMAP', if_true: files('mbx.c', 'mbxemu/core.c',\n"
     "  'mbxemu/ta.c', 'mbxemu/vgp.c', 'mbxemu/render.c', 'mbxemu/rcptab.c', 'mbxemu/texture.c',\n"
     "  'mbxemu/twod.c', 'mbxemu/pb.c'))\n")
edit("hw/arm/omap2.c",
     """    case 0x350:	/* RM_RSTCTRL_GFX */
        s->rstctrl[0] = value & 1;
        /* TODO: reset */
        break;""",
     """    case 0x350:	/* RM_RSTCTRL_GFX */
        s->rstctrl[0] = value & 1;
        /* the GFX block's reset is done at once: GFX_SW_RST in RM_RSTST_GFX,
         * which the guest driver polls for after its pulse */
        if (value & 1) {
            s->rst[1] |= 0x10;
        }
        break;""")
edit("hw/arm/omap2.c",
     """    s->eac = omap_eac_init(omap_l4ta(s->l4, 32),""",
     """    /* the PowerVR MBX ("GUFFAW"): registers and slave ports, INTC line 21 */
    sysbus_create_simple("mbx", 0x50000000,
                         qdev_get_gpio_in(s->ih[0], OMAP_INT_24XX_GUFFAW_IRQ));

    s->eac = omap_eac_init(omap_l4ta(s->l4, 32),""")
print("MBX device installed in", tree)
