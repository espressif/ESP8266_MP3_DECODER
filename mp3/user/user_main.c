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

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "mad.h"
#include "i2s_reg.h"
#include "spiram.h"

/*
Mem usage:
layer3: 744 bytes rodata
*/


#define server_ip "192.168.40.117"
//#define server_ip "192.168.1.4"
//#define server_ip "192.168.4.100"
#define server_port 1234

//#define UART_AUDIO
#define I2S_AUDIO


struct madPrivateData {
	int fd;
	xSemaphoreHandle muxBufferBusy;
	xSemaphoreHandle semNeedRead;
	int fifoLen;
	int fifoRaddr;
	int fifoWaddr;
};

#ifdef UART_AUDIO
#define printf(a, ...) while(0)
#endif

#define SPIRAMSIZE (128*1024)
#define SPIREADSIZE 64 		//in bytes, needs to be multiple of 4

#ifdef I2S_AUDIO

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


void ICACHE_FLASH_ATTR i2sInit() {
	printf("i2sInit()\n");
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

	//Select 16bits per channel(FIFO_MOD=0), no DMA access (FIFO only)
	CLEAR_PERI_REG_MASK(I2S_FIFO_CONF, I2S_I2S_DSCR_EN|(I2S_I2S_RX_FIFO_MOD<<I2S_I2S_RX_FIFO_MOD_S)|(I2S_I2S_TX_FIFO_MOD<<I2S_I2S_TX_FIFO_MOD_S));

	//tx/rx binaureal
	CLEAR_PERI_REG_MASK(I2SCONF_CHAN, (I2S_TX_CHAN_MOD<<I2S_TX_CHAN_MOD_S)|(I2S_RX_CHAN_MOD<<I2S_RX_CHAN_MOD_S));

	//?
	WRITE_PERI_REG(I2SRXEOF_NUM, 0x80);

	//Clear int
	SET_PERI_REG_MASK(I2SINT_CLR,   I2S_I2S_TX_REMPTY_INT_CLR|I2S_I2S_TX_WFULL_INT_CLR|
			I2S_I2S_RX_WFULL_INT_CLR|I2S_I2S_PUT_DATA_INT_CLR|I2S_I2S_TAKE_DATA_INT_CLR);
	CLEAR_PERI_REG_MASK(I2SINT_CLR,   I2S_I2S_TX_REMPTY_INT_CLR|I2S_I2S_TX_WFULL_INT_CLR|
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

	//Start transmit
	SET_PERI_REG_MASK(I2SCONF,I2S_I2S_TX_START);
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


void i2sTxSamp(unsigned int samp) {
/*
	while(!(READ_PERI_REG(I2SINT_RAW) & I2S_I2S_TX_PUT_DATA_INT_RAW))  printf("w");

	SET_PERI_REG_MASK(I2SINT_CLR,   I2S_I2S_PUT_DATA_INT_CLR);
	CLEAR_PERI_REG_MASK(I2SINT_CLR,   I2S_I2S_PUT_DATA_INT_CLR);

	while(!(READ_PERI_REG(I2SINT_RAW) & I2S_I2S_TX_REMPTY_INT_RAW)) printf("w");

	SET_PERI_REG_MASK(I2SINT_CLR,   I2S_I2S_TX_REMPTY_INT_CLR);
	CLEAR_PERI_REG_MASK(I2SINT_CLR,   I2S_I2S_TX_REMPTY_INT_CLR);
*/
	WRITE_PERI_REG(I2STXFIFO, samp);

}

#endif


struct madPrivateData madParms;

void render_sample_block(short *short_sample_buff, int no_samples) {
	int i, s;
	static int err=0;
	unsigned int samp;
#ifdef UART_AUDIO
	char samp[]={0x00, 0x01, 0x11, 0x15, 0x55, 0x75, 0x77, 0xf7, 0xff};
	for (i=0; i<no_samples; i++) {
		s=short_sample_buff[i];
		s+=err;
		if (s>32867) s=32767;
		if (s<-32768) s=-32768;
		uart_tx_one_char(0, samp[(s >> 13)+4]);
		err=s-((s>>13)<<14);
	}
#endif
#ifdef I2S_AUDIO
	//short_sample_buff is signed integer
	
	for (i=0; i<no_samples; i++) {
		while(!(READ_PERI_REG(I2SINT_RAW) & I2S_I2S_TX_PUT_DATA_INT_RAW));//  printf("w");
//		printf(".");
/*
		samp=(short_sample_buff[i]);
		samp=(samp)&0xffff;
		samp=(samp<<16)|samp;
*/
		SET_PERI_REG_MASK(I2SINT_CLR,   I2S_I2S_PUT_DATA_INT_CLR);
		CLEAR_PERI_REG_MASK(I2SINT_CLR,   I2S_I2S_PUT_DATA_INT_CLR);
		WRITE_PERI_REG(I2STXFIFO, short_sample_buff[i]);
	}
#endif
//	printf("rsb %04x %04x\n", short_sample_buff[0], short_sample_buff[1]);
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
	int rem, canRead;
	char rbuf[SPIREADSIZE];
	struct madPrivateData *p = (struct madPrivateData*)data;
	//Shift remaining contents of buf to the front
	rem=stream->bufend-stream->next_frame;
	memmove(readBuf, stream->next_frame, rem);

	//Wait until there is enough data in the buffer. This only happens when the data feed rate is too low, and shouldn't normally be needed!
	do {
		xSemaphoreTake(p->muxBufferBusy, portMAX_DELAY);
		canRead=((p->fifoLen)>=(sizeof(readBuf)-rem));
		xSemaphoreGive(p->muxBufferBusy);
		if (!canRead) {
			printf("Buf uflow %d < %d \n", (p->fifoLen), (sizeof(readBuf)-rem));
			vTaskDelay(100/portTICK_RATE_MS);
		}
	} while (!canRead);

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

	//Let reader thread read more data.
	xSemaphoreGive(p->semNeedRead);

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


int ICACHE_FLASH_ATTR openConn() {
	while(1) {
		int n, i;
		struct sockaddr_in remote_ip;
		int sock=socket(PF_INET, SOCK_STREAM, 0);
		if (sock==-1) {
//			printf("Client socket create error\n");
			continue;
		}
		n=1;
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &n, sizeof(n));
		n=4096;
		setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n));
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


void ICACHE_FLASH_ATTR tskmad(void *pvParameters){
	struct mad_decoder decoder;
	struct madPrivateData *p=&madParms; //pvParameters;
	printf("MAD: Decoder start.\n");
	mad_decoder_init(&decoder, p, input, 0, 0 , output, error, 0);
	mad_decoder_run(&decoder, MAD_DECODER_MODE_SYNC);
	mad_decoder_finish(&decoder);
//	printf("MAD: Decode done.\n");
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
//		printf("Reading into SPI buffer...\n");
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
				if (n==0) break; //ToDo: Handle EOF better
			}
		} while (inBuf<(SPIRAMSIZE-64));
		if (!madRunning) {
			//tskMad seems to need at least 2450 bytes of RAM.
			if (xTaskCreate(tskmad, "tskmad", 2450, NULL, 2, NULL)!=pdPASS) printf("ERROR! Couldn't create MAD task!\n");
			madRunning=1;
		}
//		printf("Read done.\n");
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
	sprintf(config->ssid, "wifi-2");
	sprintf(config->password, "thesumof6+6=12");
//	sprintf(config->ssid, "Sprite");
//	sprintf(config->password, "pannenkoek");
	wifi_station_set_config(config);
	wifi_station_connect();
	free(config);
//	printf("Connection thread done.\n");

	if (xTaskCreate(tskreader, "tskreader", 230, NULL, 2, NULL)!=pdPASS) printf("ERROR! Couldn't create reader task!\n");
	vTaskDelete(NULL);
}

extern void os_update_cpu_frequency(int mhz);


void ICACHE_FLASH_ATTR
user_init(void)
{
	SET_PERI_REG_MASK(0x3ff00014, BIT(0));
	os_update_cpu_frequency(160);
	
#ifdef UART_AUDIO
	UART_SetBaudrate(0, 481000);
#else
	UART_SetBaudrate(0, 115200);
#endif
#ifdef I2S_AUDIO
	i2sInit();
#endif
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

