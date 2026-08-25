#!/bin/sh
# xfce4-asusd-battery - autogen.sh

set -e

autoreconf -vfi

echo "Running ./configure $@"
./configure "$@"
