#!/bin/bash
#
# Script run on test machine to save debug symbols after a panic

set +e +x
sudo chown -R root:wheel build/Debug/AoE.kext
sudo chmod -R 755 build/Debug/AoE.kext
sudo kextload -s /tmp -tn build/Debug/AoE.kext
