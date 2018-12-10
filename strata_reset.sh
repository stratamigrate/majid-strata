#!/bin/sh


cd /home/stratamigrate/majid/strata/utils
sudo ./use_dax.sh unbind
sudo ./use_dax.sh bind

sudo ./uio_setup.sh linux reset
sudo ./uio_setup.sh linux config

cd /home/stratamigrate/majid/strata/libfs
sudo ./bin/mkfs.mlfs 1
sudo ./bin/mkfs.mlfs 2
sudo ./bin/mkfs.mlfs 4

