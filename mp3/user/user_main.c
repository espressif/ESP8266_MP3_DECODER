/******************************************************************************
 * Copyright 2013-2015 Espressif Systems
 *
 * FileName: user_main.c
 *
 * Description: Main routines for MP3 decoder.
 *
 * Modification history:
 *     2015/06/01, v1.0 File created.
*******************************************************************************/
#include "esp_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "../mad/mad.h"
#include "../mad/stream.h"
#include "../mad/frame.h"
#include "../mad/synth.h"
#include "i2s_freertos.h"
#include "spiram.h"


//We need some defines that aren't in some RTOS SDK versions. Define them here if we can't find them.
#ifndef i2c_bbpll
#define i2c_bbpll                                 0x67
#define i2c_bbpll_en_audio_clock_out            4
#define i2c_bbpll_en_audio_clock_out_msb        7
#define i2c_bbpll_en_audio_clock_out_lsb        7
#define i2c_bbpll_hostid                           4

#define i2c_writeReg_Mask(block, host_id, reg_add, Msb, Lsb, indata)  rom_i2c_writeReg_Mask(block, host_id, reg_add, Msb, Lsb, indata)
#define i2c_readReg_Mask(block, host_id, reg_add, Msb, Lsb)  rom_i2c_readReg_Mask(block, host_id, reg_add, Msb, Lsb)
#define i2c_writeReg_Mask_def(block, reg_add, indata) \
      i2c_writeReg_Mask(block, block##_hostid,  reg_add,  reg_add##_msb,  reg_add##_lsb,  indata)
#define i2c_readReg_Mask_def(block, reg_add) \
      i2c_readReg_Mask(block, block##_hostid,  reg_add,  reg_add##_msb,  reg_add##_lsb)
#endif
#ifndef ETS_SLC_INUM
#define ETS_SLC_INUM       1
#endif


//The server to connect to
#define server_ip "192.168.12.1"
//#define server_ip "192.168.40.117"
//#define server_ip "192.168.1.4"
//#define server_ip "192.168.4.100"
#define server_port 1234



//TODO: REFACTOR!
struct madPrivateData {
	int fd;
	xSemaphoreHandle muxBufferBusy;
	xSemaphoreHandle semNeedRead;
	int fifoLen;
	int fifoRaddr;
	int fifoWaddr;
};


struct madPrivateData madParms;

//Parameters for the input buffer behaviour
#define SPIRAMSIZE (128*1024)	//Size of the SPI RAM used as an input buffer
#define SPIREADSIZE 64 			//Read burst size, in bytes, needs to be multiple of 4 and <=64
#define SPILOWMARK (92*1024)	//Don't start reading from network until spi ram is emptied below this mark

//Priorities of the reader and the decoder thread. Higher = higher prio.
#define PRIO_READER 2
#define PRIO_MAD 3

//The mp3 read buffer. 2106 bytes should be enough for up to 48KHz mp3s according to the sox sources. Used by libmad.
#define READBUFSZ (2106+64)
static char readBuf[READBUFSZ]; 

//This routine is called by the NXP modifications of libmad. It passes us (for the mono synth)
//32 16-bit samples.
void render_sample_block(short *short_sample_buff, int no_samples) {
	int i;
	unsigned int samp;

	for (i=0; i<no_samples; i++) {
		//Duplicate 16-bit sample to both the L and R channel
		samp=(short_sample_buff[i]);
		samp=(samp)&0xffff;
		samp=(samp<<16)|samp;
		//Send the sample.
		i2sPushSample(samp);
	}
}

//Called by the NXP modificationss of libmad. Sets the needed output sample rate.
void set_dac_sample_rate(int rate) {
//	printf("sr %d\n", rate);
}

static enum  mad_flow ICACHE_FLASH_ATTR input(void *data, struct mad_stream *stream) {
	int n, i;
	int rem, fifoLen;
	char rbuf[SPIREADSIZE];
	struct madPrivateData *p = (struct madPrivateData*)data;
	//Shift remaining contents of buf to the front
	rem=stream->bufend-stream->next_frame;
	memmove(readBuf, stream->next_frame, rem);

	//Wait until there is enough data in the buffer. This only happens when the data feed 
	//rate is too low, and shouldn't normally be needed!
	do {
		xSemaphoreTake(p->muxBufferBusy, portMAX_DELAY);
		fifoLen=p->fifoLen;
		xSemaphoreGive(p->muxBufferBusy);
		if (fifoLen<(sizeof(readBuf)-rem)) {
			printf("Buf uflow %d < %d \n", (p->fifoLen), (sizeof(readBuf)-rem));
			//We both silence the output as well as wait a while by pushing silent samples into the i2s system.
			//THis waits for about 100mS
			for (n=0; n<441; n++) i2sPushSample(0);
		}
	} while (fifoLen<(sizeof(readBuf)-rem));

	while (rem<=(READBUFSZ-64)) {
		//Grab 64 bytes of data from the SPI RAM fifo
		xSemaphoreTake(p->muxBufferBusy, portMAX_DELAY);
		spiRamRead(p->fifoRaddr, rbuf, SPIREADSIZE);
		p->fifoRaddr+=SPIREADSIZE;
		if (p->fifoRaddr>=SPIRAMSIZE) p->fifoRaddr-=SPIRAMSIZE;
		p->fifoLen-=SPIREADSIZE;
		xSemaphoreGive(p->muxBufferBusy);

		//Move data into place
		memcpy(&readBuf[rem], rbuf, SPIREADSIZE);
		rem+=sizeof(rbuf);
	}

	//Let reader thread read more data if needed
	if (fifoLen<SPILOWMARK) xSemaphoreGive(p->semNeedRead);

	//Okay, let MAD decode the buffer.
	mad_stream_buffer(stream, readBuf, rem);
	return MAD_FLOW_CONTINUE;
}

//Unused, the NXP implementation uses render_sample_block
static enum mad_flow ICACHE_FLASH_ATTR output(void *data, struct mad_header const *header, struct mad_pcm *pcm) {
	 return MAD_FLOW_CONTINUE;
}

static enum mad_flow ICACHE_FLASH_ATTR error(void *data, struct mad_stream *stream, struct mad_frame *frame) {
//	struct madPrivateData *p = (struct madPrivateData*)data;
	printf("dec err 0x%04x (%s)\n", stream->error, mad_stream_errorstr(stream));
	return MAD_FLOW_CONTINUE;
}


void ICACHE_FLASH_ATTR tskmad(void *pvParameters){
	int r;
	struct madPrivateData *p=&madParms; //pvParameters;
	struct mad_stream *stream;
	struct mad_frame *frame;
	struct mad_synth *synth;
	int t=0;

	stream=malloc(sizeof(struct mad_stream));
	frame=malloc(sizeof(struct mad_frame));
	synth=malloc(sizeof(struct mad_synth));

	printf("MAD: Decoder start.\n");
	mad_stream_init(stream);
	mad_frame_init(frame);
	mad_synth_init(synth);
	while(1) {
		input(p, stream); //calls mad_stream_buffer internally
//		printf("Read input stream\n");
		while(1) {
			r=mad_frame_decode(frame, stream);
			if (r==-1) {
	 			if (!MAD_RECOVERABLE(stream->error))
					break;
				error(NULL, stream, frame);
				continue;
			}
//			printf("Decoded\n");
			mad_synth_frame(synth, frame);
//			printf("Synth\n");
		}
//		if (malloc(16)!=NULL) t+=16;
//		printf("Free: %d\n", t);
	}
//	printf("MAD: Decode done.\n");
}



int ICACHE_FLASH_ATTR openConn() {
	while(1) {
		int n, i;
		struct sockaddr_in remote_ip;
		int sock=socket(PF_INET, SOCK_STREAM, 0);
		if (sock==-1) {
//			printf("Client socket create error\n");
			continue;
		}
		bzero(&remote_ip, sizeof(struct sockaddr_in));
		remote_ip.sin_family = AF_INET;
		remote_ip.sin_addr.s_addr = inet_addr(server_ip);
		remote_ip.sin_port = htons(server_port);
//		printf("Connecting to client...\n");
		if (connect(sock, (struct sockaddr *)(&remote_ip), sizeof(struct sockaddr))!=00) {
			close(sock);
			printf("Conn err.\n");
			vTaskDelay(1000/portTICK_RATE_MS);
			continue;
		}
		return sock;
	}
}



void ICACHE_FLASH_ATTR tskreader(void *pvParameters) {
	struct madPrivateData *p=&madParms;//pvParameters;
	fd_set fdsRead;
	int madRunning=0;
	char wbuf[SPIREADSIZE];
	int n, l, inBuf;
	p->fd=-1;
	while(1) {
		if (p->fd==-1) p->fd=openConn();
		printf("Reading into SPI buffer...\n");
		do {
			//Grab amount of buffer full-ness
			xSemaphoreTake(p->muxBufferBusy, portMAX_DELAY);
			inBuf=p->fifoLen;
			xSemaphoreGive(p->muxBufferBusy);
			if (inBuf<(SPIRAMSIZE-SPIREADSIZE)) {
//				printf("Fifo fill: %d\n", inBuf);
				//We can add some data. Read data from fd into buffer. Make sure we read 64 bytes.
				l=0;
				while (l!=SPIREADSIZE) {
					n=read(p->fd, &wbuf[l], SPIREADSIZE-l);
					l+=n;
					if (n==0) break;
				}
				//Write data into SPI fifo. We need to 
				xSemaphoreTake(p->muxBufferBusy, portMAX_DELAY);
				spiRamWrite(p->fifoWaddr, wbuf, SPIREADSIZE);
				p->fifoWaddr+=SPIREADSIZE;
				if (p->fifoWaddr>=SPIRAMSIZE) p->fifoWaddr-=SPIRAMSIZE;
				p->fifoLen+=SPIREADSIZE;
				xSemaphoreGive(p->muxBufferBusy);
				taskYIELD(); //don't starve the mp3 output thread
				if (n==0) break; //ToDo: Handle EOF better
			}
		} while (inBuf<(SPIRAMSIZE-64));
		if (!madRunning) {
			//tskMad seems to need at least 2060 bytes of RAM.
			//Max prio is 2; nore interferes with freertos.
			if (xTaskCreate(tskmad, "tskmad", 2060, NULL, PRIO_MAD, NULL)!=pdPASS) printf("ERROR! Couldn't create MAD task!\n");
			madRunning=1;
		}
		printf("Read done.\n");
		//Try to take the semaphore. This will wait until the mad task signals it needs more data.
		xSemaphoreTake(p->semNeedRead, portMAX_DELAY);
	}
}

void ICACHE_FLASH_ATTR tskconnect(void *pvParameters) {
	vTaskDelay(3000/portTICK_RATE_MS);
//	printf("Connecting to AP...\n");
	wifi_station_disconnect();
//	wifi_set_opmode(STATIONAP_MODE);
	if (wifi_get_opmode() != STATION_MODE) { 
		wifi_set_opmode(STATION_MODE);
	}
//	wifi_set_opmode(SOFTAP_MODE);

	struct station_config *config=malloc(sizeof(struct station_config));
	memset(config, 0x00, sizeof(struct station_config));
	sprintf(config->ssid, "testjmd");
	sprintf(config->password, "pannenkoek");
//	sprintf(config->ssid, "wifi-2");
//	sprintf(config->password, "thesumof6+6=12");
//	sprintf(config->ssid, "Sprite");
//	sprintf(config->password, "pannenkoek");
	wifi_station_set_config(config);
	wifi_station_connect();
	free(config);
//	printf("Connection thread done.\n");

	i2sInit();
	if (xTaskCreate(tskreader, "tskreader", 230, NULL, PRIO_READER, NULL)!=pdPASS) printf("ERROR! Couldn't create reader task!\n");
	vTaskDelete(NULL);
}

extern void os_update_cpu_frequency(int mhz);

void ICACHE_FLASH_ATTR
user_init(void)
{
	SET_PERI_REG_MASK(0x3ff00014, BIT(0));
	os_update_cpu_frequency(160);
	
	UART_SetBaudrate(0, 115200);
	spiRamInit();
	spiRamTest();

	madParms.fifoLen=0;
	madParms.fifoRaddr=0;
	madParms.fifoWaddr=0;
	madParms.muxBufferBusy=xSemaphoreCreateMutex();
	vSemaphoreCreateBinary(madParms.semNeedRead);
//	printf("Starting tasks...\n");
	xTaskCreate(tskconnect, "tskconnect", 200, NULL, 3, NULL);
}

