//Gestures:
// - Flick up -> on
// - Flick down -> off
// - Slow up/down -> increase/decrease intensity
// - Slow left/right -> change saturation
// - Rotate left/right: Change hue

#include "esp_common.h"
#include "math.h"
#include "gesture.h"


//Clear a gesture struct for detecting a new gesture
void ICACHE_FLASH_ATTR gestDetReset(GestDetState *st) {
	memset(st, 0, sizeof(GestDetState));
}

//Find a square root according to a modified Newton-Rhapson method.
//http://ww1.microchip.com/downloads/en/AppNotes/91040a.pdf
int ICACHE_FLASH_ATTR mySqrt(int v) {
	int guess=0;
	int bit=0x8000;
	while(bit!=0) {
		if (v>((guess|bit)*(guess|bit))) {
			guess|=bit;
		}
		bit>>=1;
	}
	return guess;
}


//Quick and dirty absolute number macro
#define ABS(x) ((x>0)?(x):(-(x)))


//Discr_ratio are actually is 1/n, as in we accept a gesture when good>bad*DISCR_TRESH
#define RAD_DIST_CENTER 40			//Distance from center inside which we don't detect circular motion
#define STRAIGHT_DIST_CENTER 70		//Distance from center outside which we don't detect hor/vert motion
#define DISCR_TRESH 40				//Minimum distance to move before we detect H/V movement
#define DISCR_RATIO 3

#define DISCR_TRESH_CIRC 30
#define DISCR_RATIO_CIRC 1

#define MAXSTEP 60

#define MAX_SAMP_FLICK 10
#define MIN_MOVE_FLICK 100

//Feed normalized coordinates into this, that is: x,y in [-128, 127] and dx,dy scaled accordingly.
int ICACHE_FLASH_ATTR gestDetProcess(GestDetState *st, int x, int y, int dx, int dy, int *gestVal) {
	int rad, tang, t;

	//We have an extra sample.
	st->noSamples++;
	//Movement in X and Y directions is easy; rotation in radial/tangential direction not so much.
	//Figure it out by getting the angle of (x,y), then rotating (dx,dy) as much in the opposite direction.
	if (ABS(x)>RAD_DIST_CENTER || ABS(y)>RAD_DIST_CENTER) {
		//We would want to do this:
		//angle=atan2(y, x);
		//rad=x*cos(-angle)-y*sin(-angle);
		//tang=x*sin(-angle)+y*cos(-angle);
		//but that'd involve linking the entire angle-part of the math lib...
		//We can do it by stating that sin(angle)=y/veclen(x,y) and cos(angle)=x/veclen(x,y).
		float myCos, mySin;
		float vecLen;
		vecLen=mySqrt(x*x+y*y); //ToDo: Find a less precise but simpler equivalent of this?
		myCos=(float)x/vecLen; mySin=(float)y/vecLen;
		//We want to multiply by cos(-angle) etc, not cos(angle).
		//Cos(-angle)==cos(angle), sin(angle)=-sin(angle) so mySin is negated to accomplish this.
		rad=(float)dx*myCos-(float)dy*(-mySin);
		tang=(float)dx*(-mySin)+(float)dy*myCos;
		//ToDo: This can be made integer-only by substituting variables in an interesting way and doing int math.
	} else {
		//Too close to center. Don't move.
		rad=0; tang=0;
	}

	//If the amount of movement is too high, it's a fluke or (in the case of circular movement) we've gone through the center.
	//If that's not the case, update the positions and cumulatives.
	if (ABS(dx)<MAXSTEP && ABS(y)<STRAIGHT_DIST_CENTER) {
		st->xPos+=dx;
		st->cumX+=ABS(dx);
	}
	if (ABS(dy)<MAXSTEP && ABS(x)<STRAIGHT_DIST_CENTER) {
		st->yPos+=dy;
		st->cumY+=ABS(dy);
	}
	if (ABS(tang)<MAXSTEP) {
		st->tangentialPos+=tang;
		st->cumRad+=ABS(rad);
		st->cumTang+=ABS(tang);
	}
	//See if we can detect something. The trick is that for each possible gesture, we calculate the ratio of
	//'good' movement vs 'bad' movement. If the first is way more than the second one, that's probably the
	//gesture we're seeing.
	printf("                         cR=%d cT=%d\n", st->cumRad, st->cumTang);
	if (st->gestDetected==0 && st->noSamples>=MAX_SAMP_FLICK) {
		if (st->cumX>DISCR_TRESH && ABS(st->cumX)>st->cumY*DISCR_RATIO) {
			st->gestDetected=GEST_HOR;
		} else if (st->cumY>DISCR_TRESH && ABS(st->cumY)>st->cumX*DISCR_RATIO) {
			st->gestDetected=GEST_VER;
		} else if (st->cumTang>DISCR_TRESH_CIRC && ABS(st->cumTang)>st->cumRad*DISCR_RATIO_CIRC) {
			st->gestDetected=GEST_CIRCLE;
		}
	}
	//If we indeed detected a gesture, return its value.
	if (st->gestDetected==GEST_HOR) *gestVal=st->xPos;
	if (st->gestDetected==GEST_VER) *gestVal=st->yPos;
	if (st->gestDetected==GEST_CIRCLE) *gestVal=st->tangentialPos;
	return st->gestDetected;
}

int ICACHE_FLASH_ATTR gestDetFinish(GestDetState *st) {
	//The gesture is done. We can detect flicks now.
	if (st->noSamples<MAX_SAMP_FLICK) {
		if (st->cumX>DISCR_TRESH && ((float)ABS(st->cumX)/(float)st->cumY)>DISCR_RATIO) {
			return (st->xPos<0)?GEST_FLICKLEFT:GEST_FLICKRIGHT;
		} else if (st->cumY>DISCR_TRESH && ((float)ABS(st->cumY)/(float)st->cumX)>DISCR_RATIO) {
			return (st->yPos<0)?GEST_FLICKUP:GEST_FLICKDOWN;
		}
	}
	return GEST_NONE;
}
