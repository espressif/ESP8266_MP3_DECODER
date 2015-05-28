#ifndef GESTURE_H
#define GESTURE_H

#define GEST_NONE 0
#define GEST_HOR 1
#define GEST_VER 2
#define GEST_CIRCLE 3
#define GEST_FLICKUP 4
#define GEST_FLICKDOWN 5
#define GEST_FLICKLEFT 6
#define GEST_FLICKRIGHT 7

typedef struct {
	int tangentialPos;
	int xPos;
	int yPos;
	int cumX, cumY;
	int cumRad;
	int cumTang;
	int gestDetected;
	int noSamples;
}  GestDetState;



void ICACHE_FLASH_ATTR gestDetReset(GestDetState *st);
int ICACHE_FLASH_ATTR gestDetProcess(GestDetState *st, int x, int y, int dx, int dy, int *gestVal);

#endif