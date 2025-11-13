#!/bin/bash

set -e

rm -rf build.paho
mkdir build.paho
cd build.paho
echo "travis build dir $TRAVIS_BUILD_DIR pwd $PWD with OpenSSL root $OPENSSL_ROOT_DIR"
cmake -DPAHO_BUILD_STATIC=$PAHO_BUILD_STATIC -DPAHO_BUILD_SHARED=$PAHO_BUILD_SHARED -DCMAKE_BUILD_TYPE=Debug -DPAHO_WITH_SSL=TRUE -DOPENSSL_ROOT_DIR=$OPENSSL_ROOT_DIR -DPAHO_BUILD_DOCUMENTATION=FALSE -DPAHO_BUILD_SAMPLES=TRUE -DPAHO_HIGH_PERFORMANCE=$PAHO_HIGH_PERFORMANCE -DPAHO_USE_SELECT=$PAHO_USE_SELECT ..
cmake --build .
python3 ../test/mqttsas.py &
ctest -VV --timeout 600
killall python3 || true
sleep 3 # allow broker time to terminate and report
#killall mosquitto
cpack --verbose

