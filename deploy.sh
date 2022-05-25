#!/bin/sh

gzip -fk www/index.html
idf.py build
scp firmware/water-controller/build/water-controller.bin 192.168.11.15:/home/tomek/apps/ota-server/builds/
