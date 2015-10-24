#!/bin/bash
#
# Script run on test machine to unload driver

set +e +x

if [ $USER == root ]; then
	echo ERROR: DO NOT RUN THIS SCRIPT AS ROOT
	exit
fi

sudo kextunload build/Debug/AoE.kext
sleep 1
sudo kextunload build/Debug/AoE.kext
sudo chown -R $USER:$GROUPS build/Debug/AoE.kext
sudo chmod -R 777 build/Debug/AoE.kext

if [ -f /tmp/before_aoe_unload.txt ]; then
	ioclasscount > /tmp/after_aoe_unload.txt
	diff /tmp/before_aoe_unload.txt /tmp/after_aoe_unload.txt > /tmp/aoe_class_diff.txt
fi