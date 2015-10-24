#!/bin/bash
#
# Script to test raw aoe read/write handling
set +e +x

# constants
aoe_disk="/dev/disk2"
tmp="/var/tmp"
count=$1

[ $# == 0 ] && echo "Invalid input arguments" && exit;

# create a random file
if [ ! -f $tmp/random_write$count.hex ]; then
	echo create random data 
	sudo dd if=/dev/random of=$tmp/random_write$count.hex bs=512 count=$count
fi
echo saving data to "$aoe_disk"
sudo dd if=$tmp/random_write$count.hex of=$aoe_disk bs=512 count=$count
echo reading data from "$aoe_disk"
sudo dd if=$aoe_disk of=$tmp/random_read$count.hex bs=512 count=$count

echo comparing read/write files
diff $tmp/random_write$count.hex $tmp/random_read$count.hex

if [ $? == 0 ]; then
	echo Files are the same
else
	# write as text files for comparison
	hexdump $tmp/random_read$count.hex > $tmp/random_read$count.txt
	hexdump $tmp/random_write$count.hex > $tmp/random_write$count.txt
fi
