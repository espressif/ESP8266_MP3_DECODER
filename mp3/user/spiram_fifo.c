/******************************************************************************
 * Copyright 2013-2015 Espressif Systems
 *
 * FileName: user_main.c
 *
 * Description: Routines to use a SPI RAM chip as a big FIFO buffer. Multi-
 * thread-aware: the reading and writing can happen in different threads and
 * will block if the fifo is empty and full, respectively.
 *
 * Modification history:
 *     2015/06/02, v1.0 File created.
*******************************************************************************/
#include "esp_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "spiram_fifo.h"
#include "spiram.h"
#include "playerconfig.h"

#define SPIREADSIZE 64

static int fifoRpos;
static int fifoWpos;
static int fifoFill;
static xSemaphoreHandle semCanRead;
static xSemaphoreHandle semCanWrite;
static xSemaphoreHandle mux;
static long fifoOvfCnt, fifoUdrCnt;

//Low watermark where we restart the reader thread.
#define FIFO_LOWMARK (112*1024)

#ifdef FAKE_SPI_BUFF
//Re-define a bunch of things so we use the internal buffer
#undef SPIRAMSIZE
//allocate enough for about one mp3 frame
#define SPIRAMSIZE 1850 
static char fakespiram[SPIRAMSIZE];
#define spiRamInit() while(0)
#define spiRamTest() 1
#define spiRamWrite(pos, buf, n) memcpy(&fakespiram[pos], buf, n)
#define spiRamRead(pos, buf, n) memcpy(buf, &fakespiram[pos], n)
#endif

//Initialize the FIFO
int ICACHE_FLASH_ATTR spiRamFifoInit() {
	fifoRpos=0;
	fifoWpos=0;
	fifoFill=0;
	fifoOvfCnt=0;
	fifoUdrCnt=0;
	vSemaphoreCreateBinary(semCanRead);
	vSemaphoreCreateBinary(semCanWrite);
	mux=xSemaphoreCreateMutex();
	spiRamInit();
	return (spiRamTest());
}

//Read bytes from the FIFO
void ICACHE_FLASH_ATTR spiRamFifoRead(char *buff, int len) {
	int n;
	while (len>0) {
		n=len;
		if (n>SPIREADSIZE) n=SPIREADSIZE;			//don't read more than SPIREADSIZE
		if (n>(SPIRAMSIZE-fifoRpos)) n=SPIRAMSIZE-fifoRpos; //don't read past end of buffer
		xSemaphoreTake(mux, portMAX_DELAY);
		if (fifoFill<n) {
//			printf("FIFO empty.\n");
			//Drat, not enough data in FIFO. Wait till there's some written and try again.
			fifoUdrCnt++;
			xSemaphoreGive(mux);
			if (fifoFill<FIFO_LOWMARK) xSemaphoreTake(semCanRead, portMAX_DELAY);
		} else {
			//Read the data.
			spiRamRead(fifoRpos, buff, n);
			buff+=n;
			len-=n;
			fifoFill-=n;
			fifoRpos+=n;
			if (fifoRpos>=SPIRAMSIZE) fifoRpos=0;
			xSemaphoreGive(mux);
			xSemaphoreGive(semCanWrite); //Indicate writer thread there's some free room in the fifo
		}
	}
}

//Write bytes to the FIFO
void ICACHE_FLASH_ATTR spiRamFifoWrite(char *buff, int len) {
	int n;
	while (len>0) {
		n=len;
		if (n>SPIREADSIZE) n=SPIREADSIZE;		//don't read more than SPIREADSIZE
		if (n>(SPIRAMSIZE-fifoWpos)) n=SPIRAMSIZE-fifoWpos; //don't read past end of buffer

		xSemaphoreTake(mux, portMAX_DELAY);
		if ((SPIRAMSIZE-fifoFill)<n) {
//			printf("FIFO full.\n");
			//Drat, not enough free room in FIFO. Wait till there's some read and try again.
			fifoOvfCnt++;
			xSemaphoreGive(mux);
			xSemaphoreTake(semCanWrite, portMAX_DELAY);
		} else {
			//Write the data.
			spiRamWrite(fifoWpos, buff, n);
			buff+=n;
			len-=n;
			fifoFill+=n;
			fifoWpos+=n;
			if (fifoWpos>=SPIRAMSIZE) fifoWpos=0;
			xSemaphoreGive(mux);
			xSemaphoreGive(semCanRead); //Tell reader thread there's some data in the fifo.
		}
	}
}

//Get amount of bytes in use
int ICACHE_FLASH_ATTR spiRamFifoFill() {
	int ret;
	xSemaphoreTake(mux, portMAX_DELAY);
	ret=fifoFill;
	xSemaphoreGive(mux);
	return ret;
}

int ICACHE_FLASH_ATTR spiRamFifoFree() {
	return (SPIRAMSIZE-spiRamFifoFill());
}

int ICACHE_FLASH_ATTR spiRamFifoLen() {
	return SPIRAMSIZE;
}

long ICACHE_FLASH_ATTR spiRamGetOverrunCt() {
	long ret;
	xSemaphoreTake(mux, portMAX_DELAY);
	ret=fifoOvfCnt;
	xSemaphoreGive(mux);
	return ret;
}

long ICACHE_FLASH_ATTR spiRamGetUnderrunCt() {
	long ret;
	xSemaphoreTake(mux, portMAX_DELAY);
	ret=fifoUdrCnt;
	xSemaphoreGive(mux);
	return ret;
}

