/******************************************************************************
 * Copyright 2013-2015 Espressif Systems
 *
 * FileName: user_main.c
 *
 * Description: Driver for a 23LC1024 or similar chip connected to the SPI port.
 * The chip is connected to the same pins as the main flash chip except for the
 * /CS pin: that needs to be connected to IO0. The chip is driven in 1-bit SPI
 * mode: theoretically, we can move data faster by using double- or quad-SPI
 * mode but that is not implemented here. The chip also is used like a generic
 * SPI device, nothing memory-mapped like the main flash. Also: these routines
 * are not thread-safe; use mutexes around them if you access the SPI RAM from
 * different threads.
 *
 * Modification history:
 *     2015/06/01, v1.0 File created.
*******************************************************************************/
#include "esp_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "spiram.h"

#define SPI 			0
#define HSPI			1

//Initialize the SPI port to talk to the chip.
void ICACHE_FLASH_ATTR spiRamInit() {
	 //hspi overlap to spi, two spi masters on cspi
	//#define HOST_INF_SEL 0x3ff00028 
	SET_PERI_REG_MASK(0x3ff00028, BIT(7));
	//SET_PERI_REG_MASK(HOST_INF_SEL, PERI_IO_CSPI_OVERLAP);

	//set higher priority for spi than hspi
	SET_PERI_REG_MASK(SPI_EXT3(SPI), 0x1);
	SET_PERI_REG_MASK(SPI_EXT3(HSPI), 0x3);
	SET_PERI_REG_MASK(SPI_USER(HSPI), BIT(5));

	//select HSPI CS2 ,disable HSPI CS0 and CS1
	CLEAR_PERI_REG_MASK(SPI_PIN(HSPI), SPI_CS2_DIS);
	SET_PERI_REG_MASK(SPI_PIN(HSPI), SPI_CS0_DIS |SPI_CS1_DIS);

	//SET IO MUX FOR GPIO0 , SELECT PIN FUNC AS SPI CS2
	//IT WORK AS HSPI CS2 AFTER OVERLAP(THERE IS NO PIN OUT FOR NATIVE HSPI CS1/2)
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO0_U, FUNC_SPICS2);

	WRITE_PERI_REG(SPI_CLOCK(HSPI), 
					(((1)&SPI_CLKDIV_PRE)<<SPI_CLKDIV_PRE_S)|
					(((2)&SPI_CLKCNT_N)<<SPI_CLKCNT_N_S)|
					(((2)&SPI_CLKCNT_H)<<SPI_CLKCNT_H_S)|
					(((1)&SPI_CLKCNT_L)<<SPI_CLKCNT_L_S));

}

//Macro to quickly access the W-registers of the SPI peripherial
#define SPI_W(i, j)                   (REG_SPI_BASE(i) + 0x40 + ((j)*4))


//Read bytes from a memory location. The max amount of bytes that can be read is 64.
void spiRamRead(int addr, char *buff, int len) {
	int i;
	int *p=(int*)buff;
	int d;
	while(READ_PERI_REG(SPI_CMD(HSPI))&SPI_USR) ;
	SET_PERI_REG_MASK(SPI_USER(HSPI), SPI_CS_SETUP|SPI_CS_HOLD|SPI_USR_COMMAND|SPI_USR_ADDR|SPI_USR_MISO);
	CLEAR_PERI_REG_MASK(SPI_USER(HSPI), SPI_FLASH_MODE|SPI_USR_MOSI);
	WRITE_PERI_REG(SPI_USER1(HSPI), ((0&SPI_USR_MOSI_BITLEN)<<SPI_USR_MOSI_BITLEN_S)| //no data out
			((((8*len)-1)&SPI_USR_MISO_BITLEN)<<SPI_USR_MISO_BITLEN_S)| //len bits of data in
			((23&SPI_USR_ADDR_BITLEN)<<SPI_USR_ADDR_BITLEN_S)); //address is 24 bits A0-A23
	WRITE_PERI_REG(SPI_ADDR(HSPI), addr<<8); //write address
	WRITE_PERI_REG(SPI_USER2(HSPI), (((7&SPI_USR_COMMAND_BITLEN)<<SPI_USR_COMMAND_BITLEN_S) | 0x03));
	SET_PERI_REG_MASK(SPI_CMD(HSPI), SPI_USR);
	while(READ_PERI_REG(SPI_CMD(HSPI))&SPI_USR) ;
//	if ((((int)buff)&3)==0) {
	if (0) {
		//Aligned dest addr. Copy 32 bits at a time
		for (i=0; i<(len+3)/4; i++) {
			p[i]=READ_PERI_REG(SPI_W(HSPI, i));
		}
	} else {
		//Unaligned dest address. Copy 8bit at a time
		for (i=0; i<(len+3)/4; i++) {
			d=READ_PERI_REG(SPI_W(HSPI, i));
			buff[i*4+0]=(d>>0)&0xff;
			buff[i*4+1]=(d>>8)&0xff;
			buff[i*4+2]=(d>>16)&0xff;
			buff[i*4+3]=(d>>24)&0xff;
		}
	}
}

//Write bytes to a memory location. The max amount of bytes that can be written is 64.
void spiRamWrite(int addr, char *buff, int len) {
	int i;
	int *p=(int*)buff;
	int d;
	while(READ_PERI_REG(SPI_CMD(HSPI))&SPI_USR) ;
	SET_PERI_REG_MASK(SPI_USER(HSPI), SPI_CS_SETUP|SPI_CS_HOLD|SPI_USR_COMMAND|SPI_USR_ADDR|SPI_USR_MOSI);
	CLEAR_PERI_REG_MASK(SPI_USER(HSPI), SPI_FLASH_MODE|SPI_USR_MISO);
	WRITE_PERI_REG(SPI_USER1(HSPI), ((((8*len)-1)&SPI_USR_MOSI_BITLEN)<<SPI_USR_MOSI_BITLEN_S)| //len bitsbits of data out
			((0&SPI_USR_MISO_BITLEN)<<SPI_USR_MISO_BITLEN_S)| //no data in
			((23&SPI_USR_ADDR_BITLEN)<<SPI_USR_ADDR_BITLEN_S)); //address is 24 bits A0-A23
	WRITE_PERI_REG(SPI_ADDR(HSPI), addr<<8); //write address
	WRITE_PERI_REG(SPI_USER2(HSPI), (((7&SPI_USR_COMMAND_BITLEN)<<SPI_USR_COMMAND_BITLEN_S) | 0x02));
//	if ((((int)buff)&3)==0) {
	if (0) {
		//Aligned src address. Copy 32bit at a time
		for (i=0; i<(len+3)/4; i++) {
			WRITE_PERI_REG(SPI_W(HSPI, (i)), p[i]);
		}
	} else {
		//Unaligned src. Copy byte-wise.
		for (i=0; i<(len+3)/4; i++) {
			d=buff[i*4+0]<<0;
			d|=buff[i*4+1]<<8;
			d|=buff[i*4+2]<<16;
			d|=buff[i*4+3]<<24;
			WRITE_PERI_REG(SPI_W(HSPI, (i)), d);
		}
	}
	SET_PERI_REG_MASK(SPI_CMD(HSPI), SPI_USR);
}


//Simple routine to see if the SPI actually stores bytes. This is not a full memory test, but will tell
//you if the RAM chip is connected well.
int ICACHE_FLASH_ATTR spiRamTest() {
	int x;
	int err=0;
	char a[64];
	char b[64];
	char aa, bb;
	for (x=0; x<64; x++) {
		a[x]=x^(x<<2);
		b[x]=0xaa^x;
	}
	spiRamWrite(0x0, a, 64);
	spiRamWrite(0x100, b, 64);

	spiRamRead(0x0, a, 64);
	spiRamRead(0x100, b, 64);
	for (x=0; x<64; x++) {
		aa=x^(x<<2);
		bb=0xaa^x;
		if (aa!=a[x]) {
			err=1;
//			printf("aa: 0x%x != 0x%x\n", aa, a[x]);
		}
		if (bb!=b[x]) {
			err=1;
//			printf("bb: 0x%x != 0x%x\n", bb, b[x]);
		}
	}
	return !err;
}
