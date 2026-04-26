#!/bin/bash

cd ../buildroot
BR2_EXTERNAL=../pifansrf/ make pifansrf_defconfig
make all
