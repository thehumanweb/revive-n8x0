#
# Copyright(c) 2008 Imagination Technologies Ltd. All rights reserved.
# 
# This program is free software; you can redistribute it and/or modify it
# under the terms and conditions of the GNU General Public License,
# version 2, as published by the Free Software Foundation.
# 
# This program is distributed in the hope it will be useful but, except 
# as otherwise stated in writing, without any warranty; without even the 
# implied warranty of merchantability or fitness for a particular purpose. 
# See the GNU General Public License for more details.
# 
# You should have received a copy of the GNU General Public License along with
# this program; if not, write to the Free Software Foundation, Inc.,
# 51 Franklin St - Fifth Floor, Boston, MA 02110-1301 USA.
# 
# The full GNU General Public License is included in this distribution in
# the file called "COPYING".
#
# Contact Information:
# Imagination Technologies Ltd. <gpl-support@imgtec.com>
# Home Park Estate, Kings Langley, Herts, WD4 8LZ, UK 
# 
#
##

##	Set up variables.
##
DISPLAY_CONTROLLER(omaplcd)

ifelse(SUPPORT_BUFFER_CLASS,1,[[BUFFER_CLASS_DEVICE(swcamera)]])

TARGET_HAS_DEPMOD(FALSE)
TARGET_RUNS_DEPMOD_AT_BOOT(FALSE)

## Configure how to create rc.pvr
##
LOAD_MODULES
ifelse(PDUMP2,1,
[[MAKEDEV_DBGDRV]])


ifelse(MBX_GPL,1,[[ifelse(KBUILD,1,[[MAKEDEV_MBXACCESS 1]])]],[[MAKEDEV_PVRSRV 1]])

MAKEDEV_DISPLAY_CONTROLLER
ifelse(SUPPORT_BUFFER_CLASS,1,[[
MAKEDEV_BUFFER_CLASS_DEVICE
]])

## Configure how to create install.sh
##

ifelse(MBX_GPL,1,[[ifelse(KBUILD,1,[[STANDARD_SCRIPTS]])]],[[STANDARD_SCRIPTS]])

ifelse(MBX_GPL,1,[[ifelse(KBUILD,1,[[STANDARD_KERNEL_MODULES]])]],[[STANDARD_KERNEL_MODULES]])

ifelse(MBX_GPL,1,[[ifelse(UBUILD,1,[[STANDARD_LIBRARIES]])]],[[STANDARD_LIBRARIES]])
ifelse(MBX_GPL,1,[[ifelse(UBUILD,1,[[ifelse(SUPPORT_UNITTESTS,1,[[STANDARD_UNITTESTS]])]])]],[[ifelse(SUPPORT_UNITTESTS,1,[[STANDARD_UNITTESTS]])]])


## Configure our X11 window system support.
##

ifelse(MBX_GPL,1,[[ifelse(UBUILD,1,[[ifelse(SUPPORT_XWS,1,[[
FREEDESKTOP(/usr/X11R6)
FREEDESKTOP_ALL
]])
]])]],[[ifelse(SUPPORT_XWS,1,[[
FREEDESKTOP(/usr/X11R6)
FREEDESKTOP_ALL
]])
]])
