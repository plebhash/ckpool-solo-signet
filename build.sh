#!/bin/sh

# note: we assume the following packages are available on your system:
# - gcc
# - autotools
# - yasm
# - libzmq3-dev

# build ckpool-solo
./autogen.sh
./configure
make

# ckpool is a confusing name
# let's rename it to stratum-server
# and place it on the root dir
mv src/ckpool stratum-server
