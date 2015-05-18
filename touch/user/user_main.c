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

#include "i2c_master.h"

#include "ft6x06.h"

int ft6x06ReadRegs(int regStart, unsigned char* ret, int n) {
	int i;
	int ack=0;
	i2c_master_start();
	i2c_master_writeByte((FT6206_ADDR<<1));
	ack=i2c_master_getAck();
	i2c_master_writeByte(regStart);
	ack|=i2c_master_getAck();
	i2c_master_start();
	i2c_master_writeByte((FT6206_ADDR<<1)|1);
	ack|=i2c_master_getAck();
	for (i=0; i<n; i++) {
		ret[i]=i2c_master_readByte();
		if (i==n-1) i2c_master_send_nack(); else i2c_master_send_ack();
	}
	i2c_master_stop();
	return ack;
}

unsigned char ft6x06ReadReg(int reg) {
	unsigned char ret;
	ft6x06ReadRegs(reg, &ret, 1);
	return ret;
}

int ft6x06WriteReg(int reg, unsigned char val) {
	int ack=0;
	i2c_master_start();
	i2c_master_writeByte(FT6206_ADDR<<1);
	ack=i2c_master_getAck();
	i2c_master_writeByte(reg);
	ack|=i2c_master_getAck();
	i2c_master_writeByte(val);
	ack|=i2c_master_getAck();
	i2c_master_stop();
	return ack;
}


int ft6x06Init() {
	int vend, chip;
	vend=ft6x06ReadReg(FT6206_REG_VENDID);
	chip=ft6x06ReadReg(FT6206_REG_CHIPID);
	if (vend!=0x11 || chip!=0x6) {
		printf("FT6x06: VID 0x%X (should be 0x11) cid 0x%X (should be 0x6)\n", vend, chip);
		return 0;
	}
	return 1;
}

void ft6x06SetSens(int sens) {
	ft6x06WriteReg(FT6206_REG_TH_GROUP, sens);
}

int ft6x06GetTouchSlot(int slot, int *x, int *y) {
	unsigned char buff[4];
	int f;
	int reg;
	if (slot==1) reg=FT6206_REG_P1_XH;
	if (slot==2) reg=FT6206_REG_P2_XH;
	
	f=ft6x06ReadReg(reg)>>6;
	if (f!=0x2) return 0; //no contact event? Exit.
	ft6x06ReadRegs(reg, buff, sizeof(buff));
	*x=((buff[0]&0xf)<<8)|(buff[1]);
	*y=((buff[2]&0xf)<<8)|(buff[3]);
	return 1;
}

int ft6x06GetTouch(int *x, int *y) {
	int r;
	r=ft6x06GetTouchSlot(1, x, y);
	if (r) return r;
	r=ft6x06GetTouchSlot(2, x, y);
	return r;
}


//Gestures:
// - Flick up -> on
// - Flick down -> off
// - Slow up/down -> increase/decrease intensity
// - Slow left/right -> change saturation
// - Rotate left/right: Change hue








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

