#!/bin/bash
#
# Script run on test machine to unload driver

set +e +x
ioclasscount > /tmp/before_aoe_unload.txt
sudo chown -R root:wheel build/Debug/AoE.kext
sudo chmod -R 755 build/Debug/AoE.kext
sudo kextload -t build/Debug/AoE.kext
