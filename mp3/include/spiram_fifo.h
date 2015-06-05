#ifndef _SPIRAM_FIFO_H_
#define _SPIRAM_FIFO_H_

int ICACHE_FLASH_ATTR spiRamFifoInit();
void ICACHE_FLASH_ATTR spiRamFifoRead(char *buff, int len);
void ICACHE_FLASH_ATTR spiRamFifoWrite(char *buff, int len);
int ICACHE_FLASH_ATTR spiRamFifoFill();
int ICACHE_FLASH_ATTR spiRamFifoFree();
long ICACHE_FLASH_ATTR spiRamGetOverrunCt();
long ICACHE_FLASH_ATTR spiRamGetUnderrunCt();

#endif