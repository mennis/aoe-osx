#!/bin/bash
#

set +e +x
sudo chown -R root:wheel com.aoed.plist
sudo chmod -R 755 com.aoed.plist
sudo launchctl load com.aoed.plist
