#!/bin/bash

CURR_DIR=$(pwd)
MONTAGE_HOME="${CURR_DIR}/../../"
MONTAGE_LDFLAGS="-L${MONTAGE_HOME}/lib -L${MONTAGE_HOME}/ext/ralloc"
MONTAGE_LIBS="-lstdc++ -lm -ljemalloc -lpthread -lhwloc -lMontage -lralloc -lgomp -latomic "
MONTAGE_CPPFLAGS="-I${MONTAGE_HOME}/src/persist/api -I${MONTAGE_HOME}/src/persist -I${MONTAGE_HOME}/src  -I${MONTAGE_HOME}/src/utils  -I${MONTAGE_HOME}/ext/ralloc/src"

LDFLAGS=$MONTAGE_LDFLAGS LIBS=$MONTAGE_LIBS CPPFLAGS=$MONTAGE_CPPFLAGS ./configure

make clean; make