#ifndef _SPIRAM_H_
#define _SPIRAM_H_

void spiRamInit();
void spiRamRead(int addr, char *buff, int len);
void spiRamWrite(int addr, char *buff, int len);
void spiRamTest();

#endif