#include "esp_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "spiram.h"

#define SPI 			0
#define HSPI			1



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

#define SPI_W(i, j)                   (REG_SPI_BASE(i) + 0x40 + ((j)*4))


//n=1..64
void spiRamRead(int addr, char *buff, int len) {
	int i;
	int *p=(int*)buff;
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
	for (i=0; i<(len+3)/4; i++) {
		p[i]=READ_PERI_REG(SPI_W(HSPI, i));
	}
}

//n=1..64
void spiRamWrite(int addr, char *buff, int len) {
	int i;
	int *p=(int*)buff;
	while(READ_PERI_REG(SPI_CMD(HSPI))&SPI_USR) ;
	SET_PERI_REG_MASK(SPI_USER(HSPI), SPI_CS_SETUP|SPI_CS_HOLD|SPI_USR_COMMAND|SPI_USR_ADDR|SPI_USR_MOSI);
	CLEAR_PERI_REG_MASK(SPI_USER(HSPI), SPI_FLASH_MODE|SPI_USR_MISO);
	WRITE_PERI_REG(SPI_USER1(HSPI), ((((8*len)-1)&SPI_USR_MOSI_BITLEN)<<SPI_USR_MOSI_BITLEN_S)| //len bitsbits of data out
			((0&SPI_USR_MISO_BITLEN)<<SPI_USR_MISO_BITLEN_S)| //no data in
			((23&SPI_USR_ADDR_BITLEN)<<SPI_USR_ADDR_BITLEN_S)); //address is 24 bits A0-A23
	WRITE_PERI_REG(SPI_ADDR(HSPI), addr<<8); //write address
	WRITE_PERI_REG(SPI_USER2(HSPI), (((7&SPI_USR_COMMAND_BITLEN)<<SPI_USR_COMMAND_BITLEN_S) | 0x02));
	for (i=0; i<(len+3)/4; i++) {
		WRITE_PERI_REG(SPI_W(HSPI, (i)), p[i]);
	}
	SET_PERI_REG_MASK(SPI_CMD(HSPI), SPI_USR);
}


void ICACHE_FLASH_ATTR spiRamTest() {
	int x;
	int err=0;
	char a[64];
	char b[64];
	char aa, bb;
	for (x=0; x<64; x++) {
		a[x]=x^(x<<2);
		b[x]=x;
	}
	spiRamWrite(0x0, a, 64);
	spiRamWrite(0x100, b, 64);

	spiRamRead(0x0, a, 64);
	spiRamRead(0x100, b, 64);
	for (x=0; x<64; x++) {
		aa=x^(x<<2);
		bb=x;
		if (aa!=a[x]) {
			err=1;
			printf("aa: 0x%x != 0x%x\n", aa, a[x]);
		}
		if (bb!=b[x]) {
			err=1;
			printf("bb: 0x%x != 0x%x\n", bb, b[x]);
		}
	}
	while(err);
}
