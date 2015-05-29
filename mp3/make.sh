#!/bin/bash -e
#make COMPILE=gcc BOOT=none APP=0 SPI_SPEED=40 SPI_MODE=QIO SPI_SIZE=1024
make COMPILE=gcc BOOT=none APP=0 SPI_SPEED=80 SPI_MODE=QIO SPI_SIZE=1024
#xtensa-lx106-elf-size 
${ESPTOOL} --port ${ESPPORT} --baud ${ESPBAUD} write_flash 0x00000 ../bin/eagle.flash.bin 0xA0000 ../bin/eagle.irom0text.bin

