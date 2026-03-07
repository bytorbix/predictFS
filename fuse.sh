#!/bin/bash

MNT=/tmp/mnt

fusermount -u $MNT 2>/dev/null

make || exit 1

mkdir -p $MNT

./pfs_fuse $MNT

echo "Mounted at $MNT"
