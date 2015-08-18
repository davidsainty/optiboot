## XBee Series 2 API Bootloader for Arduino and Atmel AVR ##

This bootloader provides XBee Series 2 Over-The-Air firmware update capability to Atmel AVR devices, as well as supporting direct firmware update via the standard Optiboot protocol.

It provides the following features:

  * Bootloader fits within 1kB: This is larger than Optiboot's smallest build, but is still half the size of a standard Arduino bootloader.
  * Reasonably fast Over-The-Air firmware updates: Less than three minutes to write a 20kB firmware image.
  * Natively supports XBee API-mode protocol: No need to use AT-mode XBee firmware.
  * Over-The-Air updates are secure: XBee security models apply - so firmware updates are encrypted and authenticated with AES encryption if the Zigbee network is configured with encryption.
  * Over-The-Air updates are robust: The protocol uses direct addressing, so the other devices on the Zigbee network are unaffected.  You could even Over-The-Air update firmware on multiple devices concurrently.
  * Full access to Optiboot-supported bootloader facilities with a direct connection via standard avrdude software: The chip can still be firmware updated via a normal Arduino and unmodified avrdude software without replacing the bootloader.
  * Full access to Optiboot-supported bootloader facilities Over-The-Air via patched avrdude software.


#### Does it work with XBee modules in endpoint mode? ####

Yes it does, although it is of course a little slower getting going if the
remote device is sleeping.  Once it is going it appears to run something like
25% slower than a normal router mode device, E.g. a little over three minutes
for a 20kB update, rather than a little under three minutes.

#### Does it work with XBee modules in AT mode? ####

Not intentionally.  Because the AT firmware isn't really usable in any
environment with more than two XBee devices, I haven't looked at AT mode very
carefully.

#### What hardware support does it need? ####

The Atmega device needs to be able to communicate serially with the XBee.
This requires connecting the XBee DOUT (pin 2) to the Atmega RXD (Atmega328P
pin 2), and connecting the XBee DIN (pin 3) to the Atmega TXD (Atmega328P pin
3).  You've probably already done that.

Apart from the serial link, there is only one additional connection required:
We need a mechanism to hard-reset the Atmega to enter the bootloader.  This is
supported by connecting the XBee DIO3 pin (pin 17) to the Atmega RESET pin
(Atmega328P pin 1).  Optionally, a 0.1uF capacitor can be included in the
reset pin connection - this isn't mandatory, but including the capacitor does
eliminate the possibility of the XBee accidentally holding the Atmega in a
permanent state of reset.

This is intentionally identical circuitry to [SparkFun's
tutorial](https://www.sparkfun.com/tutorials/122) for use with XBee Series 1
devices.
