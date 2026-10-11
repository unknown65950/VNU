#!/bin/vash
# A script carried in the initial ramdisk. Run it with `vash
# /initrd/bin/hello.sh` or, thanks to the `#!' line and the executable
# bit the archive preserves, just `/initrd/bin/hello.sh`.
echo "hello from the initrd"
