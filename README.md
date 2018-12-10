Strata: A Cross Media File System
==================================

Strata is a research prototype file system, presented in SOSP 2017 ([Strata]).

Strata is developed and tested on Ubuntu 16.04 LTS, Linux kernel 4.8.12 and gcc
version 5.4.0.

This repository contains initial source code and tests. Benchmarks will be
released soon. As a research prototype, Strata has several limitations,
described in the [limitations section](#limitations).

To run NVM emulation, your machine should have enough DRAM for testing. Kernel
will reserve the DRAM for NVM emulation. Strata requires at least two
partitions of NVM: operation log (1 - 2 GB) and NVM shared area (It depends on
your test. I recommend to use more than 8 GB at least).

### Helper scripts ###
1. `ann.sh`: re-build strata and setup environment (including SPDK etc.)
2. `strata_reset.sh`: similar to `ann.sh` but does not re-build strata

### Building Strata ###
Assume current directory is a project root directory.

##### 1. Change memory configuration
~~~
./utils/change_dev_size.py [dax0.0] [SSD] [HDD] [dax1.0]
~~~
This script does the following:
1. Opens `libfs/src/storage/storage.h`
2. Modifies`dev_size` array values with each storage size (the same as in your
   grub conf, see the [running Strata](#runningstrata) section) in bytes.
    - `dev_size[0]`: could be always 0 (not used)
    - `dev_size[1]`: dax0.0 size
    - `dev_size[2]`: SSD size : just put 0 for now
    - `dev_size[3]`: HDD size : put 0 for now
    - `dev_size[4]`: dax1.0 size

##### 2. Build kernel
~~~
cd kernel/kbuild
make -f Makefile.setup .config
make -f Makefile.setup
make -j
sudo make modules_install ; sudo make install
~~~

This step requires reboot your machine after installing the new kernel.
##### 3. Build glibc

Building glibc might not be an easy task in some machines. We provide pre-built libc binaries in "shim/glibc-build".
If you keep failing to build glibc, I recommand to use the pre-built glibc for your testing.

~~~
cd shim
make
~~~
##### 4. Build dependent libraries (SPDK, NVML, JEMALLOC)
~~~
cd libfs/lib
git clone https://github.com/pmem/nvml
make

tar xvjf jemalloc-4.5.0.tar.bz2
cd jemalloc-4.5.0
./autogen
./configure
make
~~~

For SPDK build errors, please check a SPDK website (http://www.spdk.io/doc/getting_started.html)

For NVML build errors, please check a NVML repository (https://github.com/pmem/nvml/)
##### 5. Build Libfs
~~~
cd libfs
make
~~~
##### 6. Build KernelFS
~~~
cd kernfs
make
cd tests
make
~~~
##### 7. Build libshim
~~~
cd shim/libshim
make
~~~

### <a name="runningstrata"></a>Running Strata ###

##### 1. Setup 
~~~
sudo ./strata_reset.sh
~~~

##### 2. Run KernelFS
~~~
cd kernfs/tests
make
sudo ./run.sh kernfs
~~~

##### 3. Run testing problem
~~~
cd libfs/tests
make
sudo ./run.sh iotest sw 2G 4K 1 #sequential write, 2GB file with 4K IO and 1 thread
~~~

### Strata configuration ###
##### 1. LibFS configuration ######
In `libfs/Makefile`, search `MLFS_FLAGS` as keyword
~~~~
MLFS_FLAGS = -DLIBFS -DMLFS_INFO
#MLFS_FLAGS += -DCONCURRENT
MLFS_FLAGS += -DINVALIDATION
#MLFS_FLAGS += -DKLIB_HASH
MLFS_FLAGS += -DUSE_SSD
#MLFS_FLAGS += -DUSE_HDD
#MLFS_FLAGS += -DMLFS_LOG
~~~~

`DCONCURRENT` - allow parallelism in libfs <br/>
`DKLIB_HASH` - use klib hashing for log hash table <br/>
`DUSE_SSD`, `DUSE_HDD` - make LibFS to use SSD and HDD <br/>

###### 2. KernelFS configuration ######
~~~
#MLFS_FLAGS = -DKERNFS
MLFS_FLAGS += -DBALLOC
#MLFS_FLAGS += -DDIGEST_OPT
#MLFS_FLAGS += -DIOMERGE
#MLFS_FLAGS += -DCONCURRENT
#MLFS_FLAGS += -DFCONCURRENT
#MLFS_FLAGS += -DUSE_SSD
#MLFS_FLAGS += -DUSE_HDD
#MLFS_FLAGS += -DMIGRATION
#MLFS_FLAGS += -DEXPERIMENTAL
~~~

`DBALLOC` - use new block allocator (use it always) <br/>
`DIGEST_OPT` - use log coalescing <br/>
`DIOMERGE` - use io merging <br/>
`DCONCURRENT` - allow concurrent digest <br/>
`DMIGRATION` - allow data migration. It requires turning on `DUSE_SSD` <br/>

For debugging, DIGEST_OPT, DIOMERGE, DCONCURRENT is disabled for now

### Limitations ###

1. KernelFS is currently implmented in user-level.
2. Leases are not fully implemented.
3. A directory could contain up to 1000 files.
4. mmap is not supported yet.
5. Benchmarks are not fully tested in all configurations. Working
   configurations are described in our paper.
6. There are known bugs in fork.

[Strata]: http://www.cs.utexas.edu/~yjkwon/publication/strata/ "Strata project"
# This is migration project
