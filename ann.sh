#!/bin/sh

/home/stratamigrate/majid/strata/libfs
make clean
make

cd /home/stratamigrate/majid/strata/libfs/tests
make clean
make


cd /home/stratamigrate/majid/strata/kernfs
make clean
make


cd /home/stratamigrate/majid/strata/kernfs/tests
make clean
make 




cd /home/stratamigrate/majid/strata/utils
sudo ./use_dax.sh unbind
sudo ./use_dax.sh bind


#sudo ./spdk_setup.sh reset
#sudo HUGEMEM=2048 ./spdk_setup.sh config

sudo ./uio_setup.sh linux reset
sudo ./uio_setup.sh linux config

cd /home/stratamigrate/majid/strata/libfs
sudo ./bin/mkfs.mlfs 1
sudo ./bin/mkfs.mlfs 2
sudo ./bin/mkfs.mlfs 4

cd /home/stratamigrate/majid/strata/kernfs/tests
make


