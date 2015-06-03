#ifndef _PLAYER_CONFIG_H_
#define _PLAYER_CONFIG_H_

/*
Define the access point name and its password here.
*/
#define AP_NAME "testjmd"
#define AP_PASS "pannenkoek"

/* Define stream URL here. For example, the URL to the MP3 stream of a certain Dutch radio station
is http://icecast.omroep.nl/3fm-sb-mp3 . This translates of a server name of "icecast.omroep.nl"
and a path of "/3fm-sb-mp3". The port usually is 80 (the standard HTTP port) */
//#define PLAY_SERVER "icecast.omroep.nl"
//#define PLAY_PATH "/3fm-sb-mp3"
//#define PLAY_PORT 80

/* You can use something like this to connect to a local mpd server which has a configured 
mp3 output: */
#define PLAY_SERVER "192.168.33.128"
#define PLAY_PATH "/"
#define PLAY_PORT 8000

/*Playing a real-time MP3 stream has the added complication of clock differences: if the sample
clock of the server is a bit faster than our sample clock, it will send out mp3 data faster
than we process it and our buffer will fill up. Conversely, if the server clock is slower, we'll
eat up samples quicker than the server provides them and we end up with an empty buffer.
To fix this, the mp3 logic can insert/delete some samples to modify the speed of playback.
If our buffers are filling up too fast (presumably due to a quick sample clock on the other side)
we will increase our playout speed; if our buffers empty too quickly, we will decrease it a bit.
Unfortunately, adding or deleting samples isn't very good for the audio quality. If you
want better quality, feel free to implement a better algorithm.
WARNING: Don't use this define if you play non-stream files. It will presume the sample clock
on the server side is waaay too fast and will default to playing back the stream too fast.*/
#define ADD_DEL_SAMPLES

/*While connecting an I2S codec to the I2S port of the ESP is obviously the best way to get nice
44KHz 16-bit sounds out of the ESP, it is possible to run this code without the codec. For
this to work, instead of outputting a 2x16bit PCM sample the DAC can decode, we use the I2S
port as a makeshift 5-bit PWM generator. To do this, we map every mp3 sound sample to a
value that has an amount of 1's set that's linearily related to the sound samples value and
then output that value on the I2S port. The net result is that the average analog value on the 
I2S data pin corresponds to the value of the MP3 sample we're trying to output. Needless to
say, a hacked 5-bit PWM output is going to sound a lot worse than a real I2S codec.*/
//#define PWM_HACK

/*While a large (tens to hundreds of K) buffer is necessary for Internet streams, on a
quiet network and with a direct connection to the stream server, you can get away with
a much smaller buffer. Enabling the following switch will disable accesses to the 
23LC1024 code and use a much smaller but internal buffer instead. You want to enable
this if you don't have a 23LC1024 chip connected to the ESP but still want to try
the MP3 decoder. Be warned, if your network isn't 100% quiet and latency-free and/or
the server isn't very close to your ESP, this _will_ lead to stutters in the played 
MP3 stream! */
//#define FAKE_SPI_BUFF

//Can't have those two combined.
#ifdef FAKE_SPI_BUFF
#undef ADD_DEL_SAMPLES
#endif


#endif