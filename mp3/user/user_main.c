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

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "mad.h"

//#define server_ip "192.168.40.115"
//#define server_ip "192.168.1.5"
#define server_ip "192.168.4.100"
#define server_port 1234

struct madPrivateData {
	int fd;
};


char readBuf[4096];
static enum  mad_flow ICACHE_FLASH_ATTR input(void *data, struct mad_stream *stream) {
	int n;
	struct madPrivateData *p = (struct madPrivateData*)data;
	printf("C > Read from sock\n");
	n=read(p->fd, readBuf, sizeof(readBuf));
	printf("C > Read %d bytes.\n", n);
	if (n==0) return MAD_FLOW_STOP;
	mad_stream_buffer(stream, readBuf, n);
	return MAD_FLOW_CONTINUE;
}

static inline signed int scale(mad_fixed_t sample) {
  /* round */
  sample += (1L << (MAD_F_FRACBITS - 16));

  /* clip */
  if (sample >= MAD_F_ONE)
    sample = MAD_F_ONE - 1;
  else if (sample < -MAD_F_ONE)
    sample = -MAD_F_ONE;

  /* quantize */
  return sample >> (MAD_F_FRACBITS + 1 - 16);
}

static enum mad_flow ICACHE_FLASH_ATTR output(void *data, struct mad_header const *header, struct mad_pcm *pcm) {
	unsigned int nchannels, nsamples;
	mad_fixed_t const *left_ch, *right_ch;

	/* pcm->samplerate contains the sampling frequency */

	nchannels = pcm->channels;
	nsamples  = pcm->length;
	left_ch   = pcm->samples[0];
	right_ch  = pcm->samples[1];

	printf("Output: %d channels %d samples\n", nchannels, nsamples);
/*
	while (nsamples--) {
		signed int sample;

		sample = scale(*left_ch++);
		putchar((sample >> 0) & 0xff);
		putchar((sample >> 8) & 0xff);

		if (nchannels == 2) {
			sample = scale(*right_ch++);
			putchar((sample >> 0) & 0xff);
			 putchar((sample >> 8) & 0xff);
		}
	}
*/
	 return MAD_FLOW_CONTINUE;
}

static enum mad_flow  ICACHE_FLASH_ATTR error(void *data, struct mad_stream *stream, struct mad_frame *frame) {
	struct madPrivateData *p = (struct madPrivateData*)data;
	printf("decoding error 0x%04x (%s)\n",
	  stream->error, mad_stream_errorstr(stream));

  /* return MAD_FLOW_BREAK here to stop decoding (and propagate an error) */

  return MAD_FLOW_CONTINUE;
}

#include "../mad/align.h"

static unsigned short const ICACHE_RODATA_ATTR tstp[]={0x1234, 0x5678};

void ICACHE_FLASH_ATTR tskmad(void *pvParameters)
{
	struct mad_decoder decoder;
	struct madPrivateData priv;

	printf("P: %x %x\n", unalShort(&tstp[0]), unalShort(&tstp[1]));
    while (1) {
        int recbytes;
        int sin_size;
        int str_len;
        int sta_socket;

        struct sockaddr_in local_ip;
        struct sockaddr_in remote_ip;

        sta_socket = socket(PF_INET, SOCK_STREAM, 0);

        if (-1 == sta_socket) {
            close(sta_socket);
            printf("C > socket fail!\n");
            continue;
        }

        printf("C > socket ok!\n");
        bzero(&remote_ip, sizeof(struct sockaddr_in));
        remote_ip.sin_family = AF_INET;
        remote_ip.sin_addr.s_addr = inet_addr(server_ip);
        remote_ip.sin_port = htons(server_port);

        if (0 != connect(sta_socket, (struct sockaddr *)(&remote_ip), sizeof(struct sockaddr))) {
            close(sta_socket);
            printf("C > connect fail!\n");
            vTaskDelay(4000 / portTICK_RATE_MS);
            continue;
        }


		printf("C > Decode start!\n");
		mad_decoder_init(&decoder, &priv,
			   input, 0 /* header */, 0 /* filter */, output,
			   error, 0 /* message */);
		mad_decoder_run(&decoder, MAD_DECODER_MODE_SYNC);
		mad_decoder_finish(&decoder);

//        char *recv_buf = (char *)zalloc(128);
  //      while ((recbytes = read(sta_socket , recv_buf, 128)) > 0) {

        if (recbytes <= 0) {
            printf("C > read data fail!\n");
        }
    }
}


void ICACHE_FLASH_ATTR
user_init(void)
{
	UART_SetBaudrate(0, 115200);
    printf("SDK version:%s\n", system_get_sdk_version());

    /* need to set opmode before you set config */
    wifi_set_opmode(STATIONAP_MODE);
//    wifi_set_opmode(STATION_MODE);

    {
        struct station_config *config = (struct station_config *)zalloc(sizeof(struct station_config));
        sprintf(config->ssid, "wifi-2");
        sprintf(config->password, "thesumof6+6=12");
//        sprintf(config->ssid, "Sprite");
//       sprintf(config->password, "pannenkoek");

        /* need to sure that you are in station mode first,
         * otherwise it will be failed. */
        wifi_station_set_config(config);
        free(config);
    }

    xTaskCreate(tskmad, "tskmad", 2048, NULL, 2, NULL);
}

