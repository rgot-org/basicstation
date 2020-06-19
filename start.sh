#!/usr/bin/env bash

cd examples/live-s2.sm.tc
echo $TC_URI > tc.uri
curl https://letsencrypt.org/certs/trustid-x3-root.pem.txt -o tc.trust
RADIODEV=/dev/spidev0.0 ../../build-rpi-std/bin/station 

