#include "esp_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "i2c_master.h"
#include "ft6x06.h"
#include "gesture.h"

#define ABS(x) ((x>0)?(x):(-(x)))

#define DETDIST 5

typedef struct {
	volatile int h;
	volatile int s;
	volatile int v;
} Hsv;

Hsv ledHsv;
volatile int inhibit=0;

//ToDo: muxes around this and ledHsv.
volatile int needUpdate=0;
volatile int needUpdateFinal=0;

#define server_ip "192.168.40.118"
//#define server_ip "192.168.4.1"
#define server_port 80

int openConn() {
	int n, i;
	struct sockaddr_in remote_ip;
	int sock=socket(PF_INET, SOCK_STREAM, 0);
	if (sock==-1) {
		printf("Client socket create error\n");
		return -1;
	}
	n=1;
	setsockopt(sock, IPPROTO_TCP, TCP_NODELAY, &n, sizeof(n));
	n=4096;
	setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &n, sizeof(n));
	bzero(&remote_ip, sizeof(struct sockaddr_in));
	remote_ip.sin_family = AF_INET;
	remote_ip.sin_addr.s_addr = inet_addr(server_ip);
	remote_ip.sin_port = htons(server_port);
	printf("Connecting to client %s\n", server_ip);
	if (connect(sock, (struct sockaddr *)(&remote_ip), sizeof(struct sockaddr))!=00) {
		close(sock);
		printf("Conn err.\n");
		return -1;
	}
	return sock;
}

//http://www.cs.rit.edu/~ncs/color/t_convert.html
//H: 0..(256*6), S: 0..255, V: 0..255
void hsvToRgb(int h, int s, int v, int *r, int *g, int *b) {
	unsigned char region, fpart, p, q, t;
	if(s == 0) {
		/* color is grayscale */
		*r = *g = *b = v;
	return;
	}
	region=(h>>8);
	fpart=h&255;

	/* calculate temp vars, doing integer multiplication */
	p = (v * (255 - s)) >> 8;
	q = (v * (255 - ((s * fpart) >> 8))) >> 8;
	t = (v * (255 - ((s * (255 - fpart)) >> 8))) >> 8;

	/* assign temp vars based on color cone region */
	switch(region) {
		case 0:
			*r = v; *g = t; *b = p; break;
		case 1:
			*r = q; *g = v; *b = p; break;
		case 2:
			*r = p; *g = v; *b = t; break;
		case 3:
			*r = p; *g = q; *b = v; break;
		case 4:
			*r = t; *g = p; *b = v; break;
		default:
			*r = v; *g = p; *b = q; break;
	}
	return;
}


typedef struct __attribute__((packed)) {
	char identifier[8];
	uint16_t opcode;
	uint16_t protoVersion;
	uint8_t sequence;
	uint8_t physical;
	uint16_t universe;
	uint16_t length;
	unsigned char data[512];
} ArtNetPacket;

extern struct pwm_param pwm;

#define SWAPSH(x) ((x>>8)|((x&0xff)<<8))

//Calculate an universe/address according to the current IP address. This means the address and
//universes are 'fixed'
void calcUniverseAddr(int *uni, int *adr, unsigned char ip3, unsigned char ip4) {
	int tmp=((ip3<<8)+ip4)*4;
	*adr=tmp&511;
	*uni=(tmp>>9)&1;
}

void ICACHE_FLASH_ATTR sendUdp(int r, int g, int b) {
	int n, i, len;
	struct sockaddr_in remote_ip;
	static uint8_t seqNo=1;
	ArtNetPacket ap;
	int sock=socket(PF_INET, SOCK_DGRAM, 0);
	if (sock==-1) {
		printf("UDP socket create error\n");
		return;
	}
	bzero(&remote_ip, sizeof(struct sockaddr_in));
	remote_ip.sin_family = AF_INET;
	remote_ip.sin_addr.s_addr = inet_addr(server_ip);
	remote_ip.sin_port = htons(0x1936);
	memcpy(ap.identifier, "Art-Net", 8);
	ap.opcode=0x5000;
	ap.protoVersion=SWAPSH(14);
	ap.sequence=seqNo++;
	if (seqNo==0) seqNo=1;
	ap.physical=0;
	ap.universe=SWAPSH(0x7ffe); //private universe for each lamp
	ap.length=SWAPSH(4);
	ap.data[0]=r;
	ap.data[1]=g;
	ap.data[2]=b;
	len=sizeof(ap)-(512-ap.length);
	n=sendto(sock, &ap, len, 0, (struct sockaddr *)&remote_ip, sizeof(remote_ip));
	if (n!=len) {
		printf("UDP: Short write\n");
	}
	close(sock);
}


void ICACHE_FLASH_ATTR sendJson(int r, int g, int b) {
	char buff[1024];
	char json[1024];
	int fd=openConn();
	if (fd>=0) {
		printf("Connected.\n");
		sprintf(json, "{\"freq\": 8192, \"inhibit\": %d, \"rgb\": {\"red\": %d, \"green\": %d, \"blue\": %d}}", inhibit, r*800, g*800, b*800);
		sprintf(buff, "POST /config?command=light HTTP/1.1\r\n" \
						"Host: 0.0.0.0:80\r\n" \
						"Connection: close\r\n" \
						"User-Agent: hack\r\n" \
						"Content-Type: application/json\r\n" \
						"Content-Length: %d\r\n" \
						"\r\n" \
						"%s", strlen(json), json);
		write(fd, buff, strlen(buff));
		vTaskDelay(300/portTICK_RATE_MS); //...yeah, not elegant.
		printf("Update done. New needUpdate %d.\n", needUpdate);
		close(fd);
	}
}




void ICACHE_FLASH_ATTR tsksend(void *param) {
	int updateDone;
	int r, g, b;
	while(1) {
		if (!needUpdate && !needUpdateFinal) {
			//Eeuw. Use semaphores here.
			vTaskDelay(50/portTICK_RATE_MS);
		} else {
			hsvToRgb(ledHsv.h, ledHsv.s, ledHsv.v, &r, &g, &b);
			printf("Updating to hsv %d %d %d rgb %d %d %d\n", ledHsv.h, ledHsv.s, ledHsv.v, r, g, b);
			if (needUpdate) {
				updateDone=needUpdate;
				sendUdp(r, g, b);
				needUpdate-=updateDone;
			}
			if (needUpdateFinal) {
				updateDone=needUpdateFinal;
				sendJson(r, g, b);
				needUpdateFinal-=updateDone;
			}
		}
	}
}

void ICACHE_FLASH_ATTR tsktouch(void *param) {
	int x, y, oldx, oldy, t, r, gestVal;
	Hsv oldHsv;
	GestDetState gest;
	i2c_master_gpio_init();
	while (!ft6x06Init()) {
		vTaskDelay(500/portTICK_RATE_MS);
	}
	ft6x06SetSens(128);
	printf("Touch: main thread\n");
	while(1) {
		//Wait for touch
		do {
			vTaskDelay(20/portTICK_RATE_MS);
			t=ft6x06GetTouch(&oldx, &oldy);
		} while (t==0);
		//Touch detected! Init gesture rec and hsv stuff.
		memcpy(&oldHsv, &ledHsv, sizeof(Hsv));
		gestDetReset(&gest);

		printf("Gesture start\n");
		do {
			t=ft6x06GetTouch(&x, &y);
			if ((ABS(x-oldx)>DETDIST || ABS(y-oldy)>DETDIST) && x<255 && y<255 && t==1) {
				vTaskDelay(10/portTICK_RATE_MS);
				r=gestDetProcess(&gest, x-128, y-128, x-oldx, y-oldy, &gestVal);
				if (r!=0) {
					if (r==GEST_HOR) ledHsv.s=oldHsv.s-gestVal;
					if (r==GEST_VER) ledHsv.v=oldHsv.v-gestVal;
					if (r==GEST_CIRCLE) ledHsv.h=oldHsv.h-gestVal;
					while (ledHsv.h<0) ledHsv.h+=6*256;
					if (ledHsv.s<0) ledHsv.s=0;
					if (ledHsv.v<0) ledHsv.v=0;
					while (ledHsv.h>=(6*256)) ledHsv.h-=(6*256);
					if (ledHsv.s>=256) ledHsv.s=255;
					if (ledHsv.v>=256) ledHsv.v=255;
					printf("%03d %03d: Gest %d: %05d hsv %03d %03d %03d\n", x, y, r, gestVal, ledHsv.h, ledHsv.s, ledHsv.v);
					inhibit=0;
					needUpdate++;
				} else {
					printf("%03d %03d: No gest.\n", x, y);
				}
				oldx=x; oldy=y;
			} else if (t==0) {
				//Gesture ended.
				r=gestDetFinish(&gest);
				printf("Finalized gest: %d\n", r);
				if (r==GEST_FLICKUP) {
					inhibit=0;
				} else if (r==GEST_FLICKDOWN) {
					inhibit=1;
				}
				needUpdateFinal++;
			}
		} while (t==1);
		printf("Gesture end.\n");
	}
}


void ICACHE_FLASH_ATTR tskconnect(void *pvParameters) {
	vTaskDelay(2000/portTICK_RATE_MS);
	wifi_station_disconnect();
	if (wifi_get_opmode() != STATION_MODE) { 
		wifi_set_opmode(STATION_MODE);
	}

	struct station_config *config=malloc(sizeof(struct station_config));
	memset(config, 0x00, sizeof(struct station_config));
	sprintf(config->ssid, "wifi-2");
	sprintf(config->password, "thesumof6+6=12");
//	sprintf(config->ssid, "ESP_98040D");
//	sprintf(config->password, "");
//	sprintf(config->ssid, "Sprite");
//	sprintf(config->password, "pannenkoek");
	wifi_station_set_config(config);
	wifi_station_connect();
	free(config);
	printf("Connection thread done.\n");

	vTaskDelay(4000/portTICK_RATE_MS);
	vTaskDelete(NULL);
}



void ICACHE_FLASH_ATTR
user_init(void)
{
	UART_SetBaudrate(0, 115200);
	printf("Starting tasks...\n");
	xTaskCreate(tskconnect, "tskconnect", 1800, NULL, 12, NULL);
	xTaskCreate(tsksend, "tsksend", 3000, NULL, 1, NULL);
	xTaskCreate(tsktouch, "tsktouch", 2000, NULL, 1, NULL);
}

