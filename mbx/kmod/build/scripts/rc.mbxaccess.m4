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

include(../scripts/common.m4)

###############################################################################
##  
## Diversion discipline
##
## 50	function header of load_mbxaccess function
## 51	body of load_mbxaccess function
## 52	function trailer of load_mbxaccess function
## 53	function header of unload_mbxaccess function
## 54	body of unload_mbxaccess function
## 55	function trailer of unload_mbxaccess function
## 200   main case statement for decoding arguments.
##  
###############################################################################

###############################################################################
##
## Diversion #50 - top of load_mbxaccess()
##
###############################################################################
pushdivert(50)dnl
load_mbxaccess()
{
popdivert


###############################################################################
##
## Diversion #52 - bottom of load_mbxaccess()
##
###############################################################################
pushdivert(52)dnl
	echo "Loaded mbxaccess driver."
	return 0;
}

popdivert


###############################################################################
##
## Diversion #53 - top of unload_mbxaccess()
##
###############################################################################
pushdivert(53)dnl
unload_mbxaccess()
{
popdivert


###############################################################################
##
## Diversion #55 - bottom of unload_mbxaccess()
##
###############################################################################
pushdivert(55)dnl
dnl	echo "Unloaded mbxaccess driver."
	echo "Unloaded modules."
	return 0;
}
popdivert

###############################################################################
##
## Diversion #56 - load_mbxdaemon()
##
###############################################################################
pushdivert(56)dnl
load_mbxdaemon()
{
	/usr/local/bin/mbxdaemon &
	echo "Loaded mbxdaemon."
	return 0;
}
popdivert

###############################################################################
##
## Diversion #57 - unload_mbxdaemon()
##
###############################################################################
pushdivert(57)dnl
unload_mbxdaemon()
{
		killall mbxdaemon	
		return 0;
	
}
popdivert


###############################################################################
##
## Diversion #200 -
## the main part of the script that does parsing of user invocation
##
###############################################################################
pushdivert(200)dnl
# Deal with the type of invocation we get.
#
case "$1" in
"start")
	load_mbxaccess
ifelse(LOAD_DAEMON,1,[[
	load_mbxdaemon
]])
	;;
stop)
ifelse(LOAD_DAEMON,1,[[
	if ! unload_mbxdaemon; then
		echo "mbxdaemon already unloaded" >&2;
		echo "Continuing to unload mbxaccess" >&2;
	fi
]])
	if ! unload_mbxaccess; then
		echo "Couldn't unload modules" >&2;
	fi
	;;
reload|restart)
ifelse(LOAD_DAEMON,1,[[
	if ! unload_mbxdaemon; then
		echo "Couldn't stop mbxdaemon" >&2;
		echo "Continuing to unload mbxaccess" >&2;
	fi
]])
	if ! unload_mbxaccess; then 
              echo "Continuing to load services." >&2; 
	fi
	load_mbxaccess
ifelse(LOAD_DAEMON,1,[[
	load_mbxdaemon
]])
	;;
*)
	echo "[[$]]0: unknown argument $[[1]]." >&2;
	;;
esac

popdivert

###############################################################################
##
## MAKEDEV_MBXACCESS - 
## Make /dev/MBXACCESS by getting the MBXACCESS major number from /proc/devices.
##
###############################################################################
define([[MAKEDEV_MBXACCESS]], [[dnl
pushdivert(51)dnl
	# Delete the device for MBXACCESS and recreate with the correct major number.
	#
	mbxaccess_maj=`grep "MBXACCESS_DEVICE$" /proc/devices | cut -b1,2,3`
	/bin/rm -f /dev/mbx
	/bin/mknod /dev/mbx c $mbxaccess_maj 0

popdivert()]])


###############################################################################
##
## MAKEDEV_DISPLAY_CONTROLLER - 
## Make /dev/... for the display controller by getting the appropriate major
## number from /proc/devices.
##
###############################################################################
define([[MAKEDEV_DISPLAY_CONTROLLER]], [[dnl
pushdivert(51)dnl
	# Delete the device for the display controller and recreate with the correct major number.
	#
	if [ -z $_DISPLAY_CONTROLLER_FAILED_ ]; then
		disp_maj=`grep "_DISPLAY_CONTROLLER_$" /proc/devices | cut -b1,2,3`
		/bin/rm -f /dev/_DISPLAY_CONTROLLER_
		/bin/mknod /dev/_DISPLAY_CONTROLLER_ c $disp_maj 0
	fi

popdivert()]])


###############################################################################
##
## MAKEDEV_BUFFER_CLASS_DEVICE - 
## Make /dev/... for a buffer class device by getting the appropriate major
## number from /proc/devices.
##
###############################################################################
define([[MAKEDEV_BUFFER_CLASS_DEVICE]], [[dnl
pushdivert(51)dnl
	# Delete the device for _BUFFER_CLASS_DEVICE_ and recreate with the
	# correct major number.
	#
	if [ -z $_BUFFER_CLASS_DEVICE_FAILED_ ]; then
		bcd_maj=`grep "_BUFFER_CLASS_DEVICE_$" /proc/devices | cut -b1,2,3`
		/bin/rm -f /dev/_BUFFER_CLASS_DEVICE_
		/bin/mknod /dev/_BUFFER_CLASS_DEVICE_ c $bcd_maj 0
	fi

popdivert()]])


###############################################################################
##
## LOAD_MODULE -
## Load a kernel loadable module.
##
###############################################################################
define([[LOAD_MODULE]],[[dnl
pushdivert(51)dnl
if ! /sbin/modprobe -q $1; then return; fi
popdivert()]])


###############################################################################
##
## LOAD_MODULE_OK_TO_FAIL -
## Load a kernel loadable module.
##
###############################################################################
define([[LOAD_MODULE_OK_TO_FAIL]],[[dnl
pushdivert(51)dnl
if ! /sbin/modprobe -q $1; then echo "Continuing to load PowerVR services"; $1_failed=1; fi
popdivert()]])


###############################################################################
##
## UNLOAD_MODULE -
## Unload a kernel loadable module.
##
###############################################################################
define([[UNLOAD_MODULE]],[[dnl
pushdivert(54)dnl
if /sbin/rmmod $1; then :; else return 1; fi
popdivert()]])


###############################################################################
##
## UNLOAD_MODULE_OK_TO_FAIL -
## Unload a kernel loadable module.
##
###############################################################################
define([[UNLOAD_MODULE_OK_TO_FAIL]],[[dnl
pushdivert(54)dnl
if /sbin/rmmod $1; then :; else echo "Warning: Could not unload $1"; fi
popdivert()]])


###############################################################################
##
## LOAD_MODULES -
## Load all required kernel modules.
##
###############################################################################
define([[LOAD_MODULES]],[[dnl
pushdivert(51)dnl
	# Load the modules required.
	#
	LOAD_MODULE(mbxaccess)dnl

	ifdef([[_DISPLAY_CONTROLLER_]], [[LOAD_MODULE_OK_TO_FAIL(_DISPLAY_CONTROLLER_)]])dnl
	ifdef([[_BUFFER_CLASS_DEVICE_]], [[LOAD_MODULE_OK_TO_FAIL(_BUFFER_CLASS_DEVICE_)]])dnl

popdivert()dnl
pushdivert(54)dnl
	# Unload the loaded modules.  This won't pick up cases where a further module
	# has been added which has a dependency on one of these modules.
	#
	ifdef([[_DISPLAY_CONTROLLER_]], [[UNLOAD_MODULE_OK_TO_FAIL(_DISPLAY_CONTROLLER_)]])dnl
	ifdef([[_BUFFER_CLASS_DEVICE_]], [[UNLOAD_MODULE_OK_TO_FAIL(_BUFFER_CLASS_DEVICE_)]])dnl
	UNLOAD_MODULE(mbxaccess)dnl
popdivert()]])
