/******************************************************************************
 * Copyright 2013-2014 Espressif Systems (Wuxi)
 *
 * FileName: user_main.c
 *
 * Description: entry file of user application
 *
 * Modification history:
 *     2014/12/1, v1.0 create this file.
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
#include "i2s_reg.h"
#include "slc_register.h"
#include "sdio_slv.h"
#include "spiram.h"

#define server_ip "192.168.12.1"
//#define server_ip "192.168.40.117"
//#define server_ip "192.168.1.4"
//#define server_ip "192.168.4.100"
#define server_port 1234


struct madPrivateData {
	int fd;
	xSemaphoreHandle muxBufferBusy;
	xSemaphoreHandle semNeedRead;
	int fifoLen;
	int fifoRaddr;
	int fifoWaddr;
};


#define SPIRAMSIZE (128*1024)
#define SPIREADSIZE 64 		//in bytes, needs to be multiple of 4
#define SPILOWMARK (127*1024) //don't start reading until spi ram is emptied till this mark


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


#define DMABUFLEN (32*4)
#define DMABUFCNT (10)

//unsigned int i2sBuf[2][DMABUFLEN];
unsigned int *i2sBuf[DMABUFCNT];
struct sdio_queue i2sBufDesc[DMABUFCNT];

/*
About the timings:
configTICK_RATE_HZ is 100Hz on an 80MHz device.
I think it's 200Hz here because we're running on a 160MHz clocked device, which should 
give us 5MS granularity.

Let's say the max output clock freq is 50KHz (48KHz plus some margin), that'd mean
to bridge one tick we need to keep (50000*0.005=)250 samples in the DMA buffer to
keep on truckin' if the CPU spends a tick in the task with a lower priority.
*/


#ifndef ETS_SLC_INUM
//for sdks without this info
#define ETS_SLC_INUM       1
//#define ETS_SLC_INTR_ATTACH(func, arg) \
//    ets_isr_attach(ETS_SLC_INUM, (func), (void *)(arg))
//#define ETS_SLC_INTR_ENABLE()\
//      ETS_INTR_ENABLE(ETS_SLC_INUM)
//#define ETS_SLC_INTR_DISABLE()\
//      ETS_INTR_DISABLE(ETS_SLC_INUM) 

//#define ETS_INTR_ENABLE(inum) \
//    ets_isr_unmask((1<<inum))


#endif


xQueueHandle dmaQueue;


LOCAL void slc_isr(void) {
	portBASE_TYPE HPTaskAwoken=0;
	struct sdio_queue *finishedDesc;
	uint32 slc_intr_status;
	int n;

	//Grab int status
	slc_intr_status = READ_PERI_REG(SLC_INT_STATUS);
	//clear all intrs
	WRITE_PERI_REG(SLC_INT_CLR, 0xffffffff);//slc_intr_status);
	if (slc_intr_status & SLC_RX_EOF_INT_ST) {
		//Seems DMA pushed the current block to the i2s subsystem. Push it on the queue because it can be re-filled now.
		finishedDesc=(struct sdio_queue*)READ_PERI_REG(SLC_RX_EOF_DES_ADDR);
		xQueueSendFromISR(dmaQueue, (void*)(&finishedDesc->buf_ptr), &HPTaskAwoken);
		//Hmm. The DMA should continue automatically, but does not. ToDo: check why :X
		SET_PERI_REG_MASK(SLC_TX_LINK, SLC_TXLINK_START);
		SET_PERI_REG_MASK(SLC_RX_LINK, SLC_RXLINK_START);
	}
	portEND_SWITCHING_ISR(HPTaskAwoken);
}


void ICACHE_FLASH_ATTR i2sInit() {
	int x;
	
	i2sBuf[0]=malloc(DMABUFLEN*4);
	i2sBuf[1]=malloc(DMABUFLEN*4);

	//Clear sample byffer. We don't want noise.
	for (x=0; x<DMABUFLEN; x++) {
		i2sBuf[0][x]=0;
		i2sBuf[1][x]=0;
	}

	//Reset DMA
	SET_PERI_REG_MASK(SLC_CONF0, SLC_RXLINK_RST|SLC_TXLINK_RST);
	CLEAR_PERI_REG_MASK(SLC_CONF0, SLC_RXLINK_RST|SLC_TXLINK_RST);

	//Clear DMA int flags
	SET_PERI_REG_MASK(SLC_INT_CLR,  0xffffffff);
	CLEAR_PERI_REG_MASK(SLC_INT_CLR,  0xffffffff);

	//Enable and configure DMA
	CLEAR_PERI_REG_MASK(SLC_CONF0, (SLC_MODE<<SLC_MODE_S));
	SET_PERI_REG_MASK(SLC_CONF0,(1<<SLC_MODE_S));
	SET_PERI_REG_MASK(SLC_RX_DSCR_CONF,SLC_INFOR_NO_REPLACE|SLC_TOKEN_NO_REPLACE);
	CLEAR_PERI_REG_MASK(SLC_RX_DSCR_CONF, SLC_RX_FILL_EN|SLC_RX_EOF_MODE | SLC_RX_FILL_MODE);


	//Initialize DMA buffer descriptors in such a way that they will form a circular
	//buffer.
	for (x=0; x<DMABUFCNT; x++) {
		i2sBufDesc[x].owner=1;
		i2sBufDesc[x].eof=1;
		i2sBufDesc[x].sub_sof=0;
		i2sBufDesc[x].datalen=DMABUFLEN*4;
		i2sBufDesc[x].blocksize=DMABUFLEN*4;
		i2sBufDesc[x].buf_ptr=(uint32_t)&i2sBuf[x][0];
		i2sBufDesc[x].unused=0;
		i2sBufDesc[x].next_link_ptr=(int)((x<(DMABUFCNT-1))?(&i2sBufDesc[x+1]):(&i2sBufDesc[x]));
		printf("Desc %x\n", &i2sBufDesc[x].buf_ptr);
	}
	
	//Feed dma the 1st buffer desc addr
	CLEAR_PERI_REG_MASK(SLC_TX_LINK,SLC_TXLINK_DESCADDR_MASK);
	SET_PERI_REG_MASK(SLC_TX_LINK, ((uint32)&i2sBufDesc[1]) & SLC_TXLINK_DESCADDR_MASK); //any random desc is OK, we don't use TX but it needs something valid
	CLEAR_PERI_REG_MASK(SLC_RX_LINK,SLC_RXLINK_DESCADDR_MASK);
	SET_PERI_REG_MASK(SLC_RX_LINK, ((uint32)&i2sBufDesc[0]) & SLC_RXLINK_DESCADDR_MASK);

	_xt_isr_attach(ETS_SLC_INUM, slc_isr);
	//enable sdio operation intr
	WRITE_PERI_REG(SLC_INT_ENA,  SLC_RX_EOF_INT_ENA);
	//clear any interrupt flags that are set
	WRITE_PERI_REG(SLC_INT_CLR, 0xffffffff);
	///enable sdio intr in cpu
	_xt_isr_unmask(1<<ETS_SLC_INUM);

	//We use a queue to keep track of the DMA buffers that are empty. The ISR will push buffers to the back of the queue,
	//the mp3 decode will pull them from the front and fill them. For ease, the queue will contain *pointers* to the DMA
	//buffers, not the data itself.
	dmaQueue=xQueueCreate(DMABUFCNT, sizeof(int*));

	//Start transmission
	SET_PERI_REG_MASK(SLC_TX_LINK, SLC_TXLINK_START);
	SET_PERI_REG_MASK(SLC_RX_LINK, SLC_RXLINK_START);

//----

	//Init pins to i2s functions
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_U0RXD_U, FUNC_I2SO_DATA);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_GPIO2_U, FUNC_I2SO_WS);
	PIN_FUNC_SELECT(PERIPHS_IO_MUX_MTDO_U, FUNC_I2SO_BCK);

	//Enable clock to i2s subsystem?
	i2c_writeReg_Mask_def(i2c_bbpll, i2c_bbpll_en_audio_clock_out, 1);

	//Reset I2S subsystem
	CLEAR_PERI_REG_MASK(I2SCONF,I2S_I2S_RESET_MASK);
	SET_PERI_REG_MASK(I2SCONF,I2S_I2S_RESET_MASK);
	CLEAR_PERI_REG_MASK(I2SCONF,I2S_I2S_RESET_MASK);

	//Select 16bits per channel (FIFO_MOD=0), no DMA access (FIFO only)
	CLEAR_PERI_REG_MASK(I2S_FIFO_CONF, I2S_I2S_DSCR_EN|(I2S_I2S_RX_FIFO_MOD<<I2S_I2S_RX_FIFO_MOD_S)|(I2S_I2S_TX_FIFO_MOD<<I2S_I2S_TX_FIFO_MOD_S));
	//Enable DMA in i2s subsystem
	SET_PERI_REG_MASK(I2S_FIFO_CONF, I2S_I2S_DSCR_EN);

	//tx/rx binaureal
	CLEAR_PERI_REG_MASK(I2SCONF_CHAN, (I2S_TX_CHAN_MOD<<I2S_TX_CHAN_MOD_S)|(I2S_RX_CHAN_MOD<<I2S_RX_CHAN_MOD_S));

	//?
	WRITE_PERI_REG(I2SRXEOF_NUM, 0x80);

	//Clear int
	SET_PERI_REG_MASK(I2SINT_CLR,   I2S_I2S_TX_REMPTY_INT_CLR|I2S_I2S_TX_WFULL_INT_CLR|
			I2S_I2S_RX_WFULL_INT_CLR|I2S_I2S_PUT_DATA_INT_CLR|I2S_I2S_TAKE_DATA_INT_CLR);
	CLEAR_PERI_REG_MASK(I2SINT_CLR, I2S_I2S_TX_REMPTY_INT_CLR|I2S_I2S_TX_WFULL_INT_CLR|
			I2S_I2S_RX_WFULL_INT_CLR|I2S_I2S_PUT_DATA_INT_CLR|I2S_I2S_TAKE_DATA_INT_CLR);

	//trans master&rece slave,MSB shift,right_first,msb right
	CLEAR_PERI_REG_MASK(I2SCONF, I2S_TRANS_SLAVE_MOD|
						(I2S_BITS_MOD<<I2S_BITS_MOD_S)|
						(I2S_BCK_DIV_NUM <<I2S_BCK_DIV_NUM_S)|
						(I2S_CLKM_DIV_NUM<<I2S_CLKM_DIV_NUM_S));
	SET_PERI_REG_MASK(I2SCONF, I2S_RIGHT_FIRST|I2S_MSB_RIGHT|I2S_RECE_SLAVE_MOD|
						I2S_RECE_MSB_SHIFT|I2S_TRANS_MSB_SHIFT|
						((16&I2S_BCK_DIV_NUM )<<I2S_BCK_DIV_NUM_S)|
						((7&I2S_CLKM_DIV_NUM)<<I2S_CLKM_DIV_NUM_S));


	//No idea if ints are needed...
	//clear int
	SET_PERI_REG_MASK(I2SINT_CLR,   I2S_I2S_TX_REMPTY_INT_CLR|I2S_I2S_TX_WFULL_INT_CLR|
			I2S_I2S_RX_WFULL_INT_CLR|I2S_I2S_PUT_DATA_INT_CLR|I2S_I2S_TAKE_DATA_INT_CLR);
	CLEAR_PERI_REG_MASK(I2SINT_CLR,   I2S_I2S_TX_REMPTY_INT_CLR|I2S_I2S_TX_WFULL_INT_CLR|
			I2S_I2S_RX_WFULL_INT_CLR|I2S_I2S_PUT_DATA_INT_CLR|I2S_I2S_TAKE_DATA_INT_CLR);
	//enable int
	SET_PERI_REG_MASK(I2SINT_ENA,   I2S_I2S_TX_REMPTY_INT_ENA|I2S_I2S_TX_WFULL_INT_ENA|
	I2S_I2S_RX_REMPTY_INT_ENA|I2S_I2S_TX_PUT_DATA_INT_ENA|I2S_I2S_RX_TAKE_DATA_INT_ENA);



	//Start transmit
	SET_PERI_REG_MASK(I2SCONF,I2S_I2S_TX_START);

/*
	while(1) {
		printf("SLC_INT_RAW %x %x\n", READ_PERI_REG(SLC_INT_RAW), READ_PERI_REG(SLC_TX_STATUS));
	}
*/
}


void i2sSetRate(int rate) {
/*
	CLEAR_PERI_REG_MASK(I2SCONF, I2S_TRANS_SLAVE_MOD|
						(I2S_BITS_MOD<<I2S_BITS_MOD_S)|
						(I2S_BCK_DIV_NUM <<I2S_BCK_DIV_NUM_S)|
						(I2S_CLKM_DIV_NUM<<I2S_CLKM_DIV_NUM_S));
	SET_PERI_REG_MASK(I2SCONF, I2S_RIGHT_FIRST|I2S_MSB_RIGHT|I2S_RECE_SLAVE_MOD|
						I2S_RECE_MSB_SHIFT|I2S_TRANS_MSB_SHIFT|
						((16&I2S_BCK_DIV_NUM )<<I2S_BCK_DIV_NUM_S)|
						((7&I2S_CLKM_DIV_NUM)<<I2S_CLKM_DIV_NUM_S));
*/
}

struct madPrivateData madParms;


unsigned int *currDMABuff=NULL;
int currDMABuffPos=0;

void render_sample_block(short *short_sample_buff, int no_samples) {
	int i, s;
	static int err=0;
	unsigned int samp;
	//short_sample_buff is signed integer

	if (currDMABuff==NULL || currDMABuffPos==DMABUFLEN) {
		//We need a new buffer. Pop one from the queue.
//		printf("Queue pop\n");
		xQueueReceive(dmaQueue, &currDMABuff, portMAX_DELAY);
//		printf("Queue pop %x\n", (int)currDMABuff);
		currDMABuffPos=0;
	}

	for (i=0; i<no_samples; i++) {
		samp=(short_sample_buff[i]);
		samp=(samp)&0xffff;
		samp=(samp<<16)|samp;
		currDMABuff[currDMABuffPos++]=short_sample_buff[i];
	}
}

void set_dac_sample_rate(int rate) {
//	printf("sr %d\n", rate);
}

//The mp3 read buffer. 2106 bytes should be enough for up to 48KHz mp3s according to the sox sources. Used by libmad.
#define READBUFSZ (2106+64)
static char readBuf[READBUFSZ]; 

void memcpyAligned(char *dst, char *src, int len) {
	int x;
	int w, b;
	if (((int)dst&3)==0 && ((int)src&3)==0) {
		memcpy(dst, src, len);
		return;
	}

	for (x=0; x<len; x++) {
		b=((int)src&3);
		w=*((int *)(src-b));
		if (b==0) *dst=(w>>0);
		if (b==1) *dst=(w>>8);
		if (b==2) *dst=(w>>16);
		if (b==3) *dst=(w>>24);
		dst++; src++;
	}
}


static enum  mad_flow ICACHE_FLASH_ATTR input(void *data, struct mad_stream *stream) {
	int n, i;
	int rem, fifoLen;
	char rbuf[SPIREADSIZE];
	struct madPrivateData *p = (struct madPrivateData*)data;
	//Shift remaining contents of buf to the front
	rem=stream->bufend-stream->next_frame;
	memmove(readBuf, stream->next_frame, rem);

	//Wait until there is enough data in the buffer. This only happens when the data feed rate is too low, and shouldn't normally be needed!
	do {
		xSemaphoreTake(p->muxBufferBusy, portMAX_DELAY);
		fifoLen=p->fifoLen;
		xSemaphoreGive(p->muxBufferBusy);
		if (fifoLen<(sizeof(readBuf)-rem)) {
			printf("Buf uflow %d < %d \n", (p->fifoLen), (sizeof(readBuf)-rem));
			vTaskDelay(100/portTICK_RATE_MS);
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

		//Move into place
		memcpyAligned(&readBuf[rem], rbuf, SPIREADSIZE);
		rem+=sizeof(rbuf);
	}

	//Let reader thread read more data if needed
	if (fifoLen<SPILOWMARK) xSemaphoreGive(p->semNeedRead);

	//Okay, decode the buffer.
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
			if (xTaskCreate(tskmad, "tskmad", 2060, NULL, 1, NULL)!=pdPASS) printf("ERROR! Couldn't create MAD task!\n");
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
	if (xTaskCreate(tskreader, "tskreader", 230, NULL, 2, NULL)!=pdPASS) printf("ERROR! Couldn't create reader task!\n");
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

