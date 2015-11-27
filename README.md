# I2S MP3 webradio streaming example

This is an example of how to use the I2S module inside the ESP8266 to output
sound. In this case, it is used to output decoded MP3 data (actually, more 
accurately: MPEG2 layer III data): the code described here basically is a 
webradio streamer which can connect to an Icecast server, take the MP3 data 
the server sends out, decode it and output it over the I2S bus to a DAC. The
MP3 decoder has been tested for bitrates up to 320KBit/s and sample
rates of up to 48KHz.

## Configuration options, building

All high-level options can be configured in mp3/user/playerconfig.h. Edit 
that file to set up your access point and a webradio stream or other source of
MP3 data served over HTTP.

To build the code, try running make.sh in the mp3/ directory. Alternatively,
the way to use 'make' to build this code is:
make COMPILE=gcc BOOT=none APP=0 SPI_SPEED=40 SPI_MODE=QIO SPI_SIZE=1024

The resulting binaries will be in the bin/ folder. Please disregard the message
that pops up at the end of the make process: the addresses it mentions are
wrong. The correct addresses to load the resulting files are:
```
bin/eagle.flash.bin     - 0x00000
bin/eagle.irom0text.bin - 0xA0000
```

## Needed hardware

If you want to have nice, high-quality buffered audio output, you will need to
connect two ICs to your ESP: a 128KByte SPI RAM and an I2S codec. Both ICs are 
optional, but you will get stuttering and low-quality sound if you leave them
off.

The SPI RAM is a Microchip 23LC1024 part and is used to buffer the incoming 
MP3 data. This guards against latency isuues that are present in all but the
most quiet networks and closest connections. It is connected to the same
bus as the SPI flash:

```
ESP pin   - 23LC1024 pin
------------------------
GPIO0     - /CS (1)
SD_D0     - SO/SI1 (2)
SD_D3     - SIO2 (3) *
gnd       - gnd (4)
SD_D1     - SI/SIO0 (5)
SD_CLK    - SCK (6)
SD_D2     - /HOLD/SIO3 (7) *
3.3V      - VCC (8)

*=optional, may also be connected to Vcc on 23LC1024 side.
```

One way to make these connections is to take the SSOIC version of the 23LC1024,
bend up pin 1 (/CS) and piggyback it on the SPI flash chip that already is on the
ESP module. Solder all the pins to the same pins on the SPI flash chip except
for the bent /CS pin; use a wire to connect that to GPIO0.

As Github user milkpirate correctly remarked, GPIO0 is also used to enter 
programming mode on the ESP8266 and will interfere with correct flashing if kept 
low. The correct way of flashing the module is to make GPIO0 low, reset the 
ESP8266 to enter programming mode, then make GPIO0 high again. Tools like
esptool.py generally follow this method to automatically enter programming
mode; if you manually enter programming mode you may have to adjust your
methodology.

For the I2S codec, pick whatever chip or board works for you; this code was 
written using a ES9023 chip, but other I2S boards and chips will probably
work as well. The connections to make here are:

```
ESP pin   - I2S signal
----------------------
GPIO2/TX1   - LRCK
GPIO3/RX0   - DATA
GPIO15      - BCLK
```

Also, don't forget to hook up any supply voltages and grounds needed.

## Running without the SPI RAM part

To not use the SPI RAM chip, please edit mp3/user/playerconfig.h and
define FAKE_SPI_BUFF. This will use a much smaller buffer in the main
memory of the ESP8266. Because the buffer is much smaller, the code
will be very sensitive to network latency; also, clock synchronization
with live streaming stations will not work. Expect the sound to cut
out a fair amount of times unless you have a quiet network and connect
to a server very close to you.

## Running without the I2S DAC

To not use an I2S DAC chip, please edit mp3/user/playerconfig.h and
define PWM_HACK. This uses some code to abuse the I2S module as a
5-bit PWM generator. You can now connect an amplifier to the I2S
data pin (GPIO3/RX0) of the ESP module. Connecting a speaker 
directly may also work but is not advised: the GPIOs of the ESP
are not meant to drive inductive loads directly.

## Sound quality

In the default configuration, this code will output 16-bit mono audio.
Stereo audio is possible but hasn't been implemented yet: a stereo
synth is available but has not been modified for ESP8266 use yet. In
PWM mode, the output is a dithered 5-bit PWM output. Furthermore, the
ESP can decode a pretty wide range of bitrates: 96KBit to 320KBit
MP3s have been tested and seem to work fine. Sound quality in general
is not bad, but the scaling and clock adaption algorithms can use
some improvement if the decoder needs to output real high-quality
sound: Patches are welcome.

## About the FreeRTOS SDK used

The MP3 example is a very memory-sensitive piece of code: the MP3 decoder
uses almost all the RAM and the needed buffers for input and output
buffering take up the rest: when using no external SPI RAM, only a few 
bytes of memory are left. The SDK libs that come with this example are
libraries that have been optimized for memory usage and are known to work.

## Technical details on this implementation

The biggest part of this code consists of a modified version of libmad,
a fixed-point mp3 decoder. The specific version we use here has already
been modified by NXP to use less memory 
(source: www.nxp.com/documents/application_note/AN10583.pdf) and has been
massaged by Espressif to store as much constants in flash as possible in
order to decrease RAM use even more. The MP3 decoder is fed from a FIFO
realized in the external 23LC1024 SPI RAM. This RAM is filled from a network
socket in a separate thread.

On the output side, the MP3 samples are fed into the I2S subsystem using 
DMA. The I2S DMA basically consists of a circular buffer consisting of
a number of smaller buffers. As soon as the DMA is done emptying one of
the smaller buffers into the I2S subsystem, it will fire an interrupt. This
interrupt will put the buffer address in a queue.

When the MP3 decoder has a bunch of samples ready, it will pop a buffer 
off this queue and put the samples in it until it is full, then take the
next buffer etc. The MP3 decoder generally is faster than the I2S output,
so at a certain moment there will be no free buffers left. The queue
system of FreeRTOS will suspend the mp3 decoding task when that
happens, allowing the ESP8266 to attend to other tasks.

While the ESP8266 is able to run at 160MHz, we're leaving it at its
default speed of 80MHz here: it seems that at that speed the ESP8266
is perfectly capable of decoding even 320KBit MP3 data.

