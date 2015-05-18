#include "esp_common.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

#include "lwip/sockets.h"
#include "lwip/dns.h"
#include "lwip/netdb.h"

#include "i2c_master.h"
#include "ft6x06.h"


//Gestures:
// - Flick up -> on
// - Flick down -> off
// - Slow up/down -> increase/decrease intensity
// - Slow left/right -> change saturation
// - Rotate left/right: Change hue

/*

void gestDetReset() {

}

void gestDetProcess(int x, int y, int dx, int dy) {

}

*/

void ICACHE_FLASH_ATTR tsktouch(void *param) {
	int x, y, t;
	i2c_master_gpio_init();
	ft6x06Init();
	ft6x06SetSens(80);
	while(1) {
		t=ft6x06GetTouch(&x, &y);
		printf("Touch %d, x=%d, y=%d\n", t, x, y);
	}
}


void ICACHE_FLASH_ATTR
user_init(void)
{
	UART_SetBaudrate(0, 115200);
	printf("Starting tasks...\n");
	xTaskCreate(tsktouch, "tsktouch", 200, NULL, 3, NULL);

}

