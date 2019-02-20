# Geckokapula
Silicon Labs EFR32 transceivers give access to filtered IQ samples on receive,
so it's possible to make an all-mode handheld receiver on them.

Currently it has a crude spectrogram display, FM, AM and DSB demodulators
and FM transmitter.

There's a simple user interface:
Rotate encoder to change the value under the cursor.
Push the encoder while rotating it to move the cursor.

# Construction
BRD4151A radio board (with EFR32MG1P232F256GM48)
is connected to a display and a speaker.

The display is something like this: https://www.ebay.com/itm/232327157750

Speaker is driven from a PWM output through a series capacitor and a resistor.

| Board pin | EFR32 pin | Connection   |
|-----------|-----------|--------------|
|   GND     |           | Ground plane |
| VMCU\_IN  |           | +3.3 V       |
|   P1      |   PC6     | Display SDA  |
|   P3      |   PC7     | Display CS   |
|   P5      |   PC8     | Display SCK  |
|   P7      |   PC9     | Display AO   |
|   P12     |   PC10    | Display LED  |
|   P9      |   PA0     | Debug UART TX (to RXD on USB-UART ) |
|   P11     |   PA1     | Debug UART RX (to TXD on USB-UART ) |
|   P34     |   PF6     | PTT or CW key (switch to GND) (optional) |
|   P4      |   PD10    | PWM Audio out |
|   P6      |   PD11    | Encoder A (switch to GND) |
|   P8      |   PD12    | Encoder B (switch to GND) |
|   P31/F18 |   PD13    | Encoder push button (switch to GND) |
|   F14     |   PD15    | Audio in, biased around 0.625 V |
|   P24     |   PF0     | J-link SWCLK |
|   P26     |   PF1     | J-link SWDIO |

VCC and RESET pins on display are connected to +3.3 V.

If you push wires straight to the radio board headers, reading pinout from the
datasheet is prone to mistakes as you are looking at it mirrored.
Here's a mirrored pinout to help: http://oh2eat.dy.fi/brd4151a-pinout.png

# Files in the project
The project should be opened in Simplicity Studio. If you find a way to import
it so that paths and compiler parameters are right, tell me how you did it!
Also try regenerating the RAIL configuration (click Generate in the isc file)
as it seems to fix (or in some cases, break) these settings.

The interesting code is under src/ and inc/
except for InitDevice.c which is generated.
Everything else is mostly silabs libraries and generated stuff.
The isc and hwconf files have the hardware parameters
that can be edited in Simplicity Studio GUI.


Font is from https://github.com/dhepper/font8x8/
