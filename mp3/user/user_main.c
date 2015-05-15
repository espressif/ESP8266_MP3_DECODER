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

#define server_ip "192.168.40.115"
//#define server_ip "192.168.1.5"
//#define server_ip "192.168.4.100"
#define server_port 1234

struct madPrivateData {
	int fd;
	xSemaphoreHandle muxBufferBusy;
	xSemaphoreHandle semNeedRead;
	char buff[2016];
	int buffPos;
};

//#define printf(a, ...) while(0)

struct madPrivateData madParms;

void render_sample_block(short *short_sample_buff, int no_samples) {
	int i, s;
//	char samp[]={0x00, 0x01, 0x11, 0x15, 0x55, 0x75, 0x77, 0xf7, 0xff};
	for (i=0; i<no_samples; i++) {
		s=short_sample_buff[i];
//		putchar((s >> 0) & 0xff);
//		putchar((s >> 8) & 0xff);
//		uart_tx_one_char(0, (s >> 8) & 0xff);
//		uart_tx_one_char(0, (s >> 0) & 0xff);
//		printf("%04X", s);
	}
	printf("render_sample_block %04x %04x\n", short_sample_buff[0], short_sample_buff[1]);
}

void set_dac_sample_rate(int rate) {
	printf("set_dac_sample_rate %d\n", rate);
}

#define READBUFSZ 2016
static char readBuf[READBUFSZ]; //The mp3 read buffer. 2106 bytes should be enough for up to 48KHz mp3s according to the sox sources. Used by libmad.

static enum  mad_flow ICACHE_FLASH_ATTR input(void *data, struct mad_stream *stream) {
	int n, i;
	int rem, canRead;
	struct madPrivateData *p = (struct madPrivateData*)data;
	//Shift remaining contents of buf to the front
	rem=stream->bufend-stream->next_frame;
	memmove(readBuf, stream->next_frame, rem);

	//Wait until there is enough data in the buffer. This only happens when the data feed rate is too low, and shouldn't normally be needed!
	do {
		xSemaphoreTake(p->muxBufferBusy, portMAX_DELAY);
		canRead=(p->buffPos>=(sizeof(readBuf)-rem));
		xSemaphoreGive(p->muxBufferBusy);
		if (!canRead) {
			printf("Out of mp3 data! Waiting for more...\n");
			vTaskDelay(300/portTICK_RATE_MS);
		}
	} while (!canRead);


	//Read in bytes from buffer
	xSemaphoreTake(p->muxBufferBusy, portMAX_DELAY);
	n=sizeof(readBuf)-rem; //amount of bytes to re-fill the buffer with
	memcpy(&readBuf[rem], p->buff, n);
	//Shift remaining contents of readBuff to the front
	memmove(p->buff, &p->buff[n], sizeof(p->buff)-n);
	p->buffPos-=n;
	xSemaphoreGive(p->muxBufferBusy);

	for (i=0; i<16; i++) printf("%02X ", readBuf[i]);

	//Let reader thread read more data.
	xSemaphoreGive(p->semNeedRead);
	mad_stream_buffer(stream, readBuf, sizeof(readBuf));
	return MAD_FLOW_CONTINUE;
}

//Unused, the NXP implementation uses render_sample_block
static enum mad_flow ICACHE_FLASH_ATTR output(void *data, struct mad_header const *header, struct mad_pcm *pcm) {
	 return MAD_FLOW_CONTINUE;
}

static enum mad_flow ICACHE_FLASH_ATTR error(void *data, struct mad_stream *stream, struct mad_frame *frame) {
//	struct madPrivateData *p = (struct madPrivateData*)data;
	printf("decoding error 0x%04x (%s)\n", stream->error, mad_stream_errorstr(stream));
	return MAD_FLOW_CONTINUE;
}


//#define FAKE_MAD
#ifndef FAKE_MAD

void ICACHE_FLASH_ATTR tskmad(void *pvParameters){
	struct mad_decoder decoder;
	struct madPrivateData *p=&madParms; //pvParameters;
	printf("MAD: Decoder init.\n");
	mad_decoder_init(&decoder, p, input, 0, 0 , output, error, 0);
	mad_decoder_run(&decoder, MAD_DECODER_MODE_SYNC);
	mad_decoder_finish(&decoder);
	printf("MAD: Decode done.\n");
}

#else
void ICACHE_FLASH_ATTR tskmad(void *pvParameters){
	int n, i=0;
	int rem, canRead;
	struct madPrivateData *p = &madParms;
	while(1) {
		//Wait until there is enough data in the buffer. This only happens when the data feed rate is too low, and shouldn't normally be needed!
		do {
			xSemaphoreTake(p->muxBufferBusy, portMAX_DELAY);
			canRead=(p->buffPos>=(sizeof(readBuf)));
			xSemaphoreGive(p->muxBufferBusy);
			if (!canRead) {
				printf("Out of mp3 data! Waiting for more...\n");
				vTaskDelay(300/portTICK_RATE_MS);
			}
		} while (!canRead);
		//Read in bytes from buffer
		xSemaphoreTake(p->muxBufferBusy, portMAX_DELAY);
		p->buffPos=0;
		xSemaphoreGive(p->muxBufferBusy);

		//Let reader thread read more data.
		xSemaphoreGive(p->semNeedRead);


		vTaskDelay(100/portTICK_RATE_MS);
	}
}
#endif

/*
THIS IS TOO SLOW! Incoming packets arrive, but with big hickups.
Things that don't help:
- AP/STA
- More free memory
- Killing muxes
- Doing a write before the select
*/

void ICACHE_FLASH_ATTR tskreader(void *pvParameters) {
	struct madPrivateData *p=&madParms;//pvParameters;
	while(1) {
		int n, i;
		struct sockaddr_in remote_ip;
		fd_set fdsRead;
		int sock=socket(PF_INET, SOCK_STREAM, 0);
		if (sock==-1) {
			printf("Client socket create error\n");
			continue;
		}
		n=1;
		setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &n, sizeof(n));
		n=1024;
		setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n));
		bzero(&remote_ip, sizeof(struct sockaddr_in));
		remote_ip.sin_family = AF_INET;
		remote_ip.sin_addr.s_addr = inet_addr(server_ip);
		remote_ip.sin_port = htons(server_port);
		printf("Connecting to client...\n");
		if (connect(sock, (struct sockaddr *)(&remote_ip), sizeof(struct sockaddr))!=00) {
			close(sock);
			printf("Client connect fail.\n");
			vTaskDelay(1000/portTICK_RATE_MS);
			continue;
		}
		
		while(1) {
			while (p->buffPos!=sizeof(p->buff)) {
				//Wait for more data
				printf("Doing select...\n");
				FD_ZERO(&fdsRead);
				FD_SET(p->fd, &fdsRead);
				lwip_select(p->fd+1, &fdsRead, NULL, NULL, NULL);
				//Take buffer mux and read data.
				xSemaphoreTake(p->muxBufferBusy, portMAX_DELAY);
				n=read(p->fd, &p->buff[p->buffPos], sizeof(p->buff)-p->buffPos);
				p->buffPos+=n;
				xSemaphoreGive(p->muxBufferBusy);
				printf("%d bytes of data read from socket.\n", n);
				if (n==0) break; //ToDo: handle eof better
			}
			printf("Buffer full. Waiting for mad to empty it.\n");
			//Try to take the semaphore. This will wait until the mad task signals it needs more data.
			xSemaphoreTake(p->semNeedRead, portMAX_DELAY);
		}
	}
}

void ICACHE_FLASH_ATTR tskconnect(void *pvParameters) {
	vTaskDelay(3000/portTICK_RATE_MS);
	printf("Connecting to AP...\n");
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
	printf("Connection thread done.\n");

	xTaskCreate(tskreader, "tskreader", 250, NULL, 11, NULL);
	xTaskCreate(tskmad, "tskmad", 2200, NULL, 3, NULL);
	vTaskDelete(NULL);
}



void ICACHE_FLASH_ATTR
user_init(void)
{
	UART_SetBaudrate(0, 115200);
//	UART_SetBaudrate(0, 441000);
	printf("SDK version:%s\n", system_get_sdk_version());

	madParms.muxBufferBusy=xSemaphoreCreateMutex();
	vSemaphoreCreateBinary(madParms.semNeedRead);
	printf("Starting tasks...\n");
	xTaskCreate(tskconnect, "tskconnect", 200, NULL, 3, NULL);

}

