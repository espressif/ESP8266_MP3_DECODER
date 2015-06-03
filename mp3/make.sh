#!/bin/bash -e
make COMPILE=gcc BOOT=none APP=0 SPI_SPEED=40 SPI_MODE=QIO SPI_SIZE=1024
${ESPTOOL} --port ${ESPPORT} --baud ${ESPBAUD} write_flash 0x00000 ../bin/eagle.flash.bin 0xA0000 ../bin/eagle.irom0text.bin

