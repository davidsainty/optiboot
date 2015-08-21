## XBeeBoot: XBee Series 2 API Bootloader for Arduino and Atmel AVR ##

The XBeeBoot bootloader provides XBee Series 2 Over-The-Air firmware update capability to Atmel AVR devices, as well as supporting direct firmware update via the standard Optiboot protocol.

It provides the following features:

  * Bootloader fits within 1kB: This is larger than [Optiboot's]
    (https://github.com/Optiboot/optiboot) smallest build, but is still half
    the size of a standard Arduino bootloader.

  * Reasonably fast Over-The-Air firmware updates: Less than three minutes to
    write a 20kB firmware image.

  * Natively supports XBee API-mode protocol: No need to use AT-mode XBee
    firmware.

  * Over-The-Air updates are secure: XBee security models apply - so firmware
    updates are encrypted and authenticated with AES encryption if the Zigbee
    network is configured with encryption.

  * Over-The-Air updates are robust: The protocol uses direct addressing, so
    the other devices on the Zigbee network are unaffected.  You could even
    Over-The-Air update firmware on multiple devices concurrently.

  * Full access to Optiboot-supported bootloader facilities with a direct
    connection via standard [avrdude] (http://www.nongnu.org/avrdude/)
    software: This bootloader detects an attempt to program via the standard
    Arduino/avrdude firmware update protocol and automatically switches to the
    standard Optiboot protocol.

  * Full access to Optiboot-supported bootloader facilities Over-The-Air via
    [patched avrdude software]
    (https://savannah.nongnu.org/patch/index.php?8719).


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

This is intentionally identical circuitry to [SparkFun's tutorial]
(https://www.sparkfun.com/tutorials/122) for use with XBee Series 1 devices.


#### Are there any limits on which XBee can bootload which XBee? ####

No.  In particular, it doesn't matter if the coordinator node is a separate
and unrelated node.  Any XBee address can bootload to any other
Zigbee-networked XBee-hosted AVR device, so long as they share encryption
keys.


#### Does it work with XBee modules in endpoint mode? ####

Yes it does, although it is of course a little slower getting going if the
remote device is sleeping.  Once it is going it appears to run something like
25% slower than a normal router mode device, E.g. a little over three minutes
for a 20kB update, rather than a little under three minutes.


#### Does it work with XBee modules in AT mode? ####

Not intentionally.  Because the AT firmware isn't really usable in any
environment with more than two XBee devices, I haven't looked at AT mode very
carefully.


#### Are there any limits on the number of Zigbee nodes in the network? ####

There are no known limits.  I currently have eight active XBee devices on the
same Zigbee network, meshed over some fairly complicated terrain, with a mix
of router and endpoint devices, and can update the firmware successfully and
reliably across the network.


#### Can this bootloader be used without any XBee devices? ####

Yes it can.  In two ways, in fact:

  1. You can still use it as if it were a normal Optiboot/Arduino bootloader,
     with the caveat that the default serial baud rate is 9600 baud, to match
     the XBee default.

  1. You can also bootload, with the avrdude xbee programmer directly, by not
     giving a Zigbee address.  This is useful for diagnosis and proof of
     concept testing.  It may also be useful for bootloading over unreliable
     serial links (E.g. AT mode XBee links), as the XBee bootloader protocol
     includes checksums, is packet-based and can recover from lost or
     corrupted packets in the serial data stream.


#### It sounds like XBeeBoot is perfect!  Is it? ####

  * XBeeBoot uses the hardware watchdog to guarantee that the bootloader
    doesn't hang the chip, and does eventually exit.  The hardware watchdog
    time-out can be set to a maximum of eight seconds on AVR devices.  A
    time-out of eight seconds is a little on the short side for Over-The-Air
    updates, so if your mesh network is prone to packet loss it may be
    necessary to restart the firmware update because the eight second time-out
    was tripped.

  * XBee traffic on an encrypted network is already digitally authenticated
    via AES encryption, which is "pretty good".  Realistically, standard XBee
    security will normally be good enough.  Perfection would be if XBeeBoot
    supported an extra level of digital signature on firmware updates as an
    additional security measure.  But then XBeeBoot wouldn't fit into 1kB.

  * By necessity, XBeeBoot needs to be talked to from programmer software that
    also knows how to talk to an XBee device in API mode.  Currently avrdude
    doesn't have that feature as standard, so you will need to apply the
    [patch] (https://savannah.nongnu.org/patch/index.php?8719) yourself and
    rebuild avrdude to be able to perform Over-The-Air firmware updates.


----

> XBeeBoot is an amalgamation of an Over-The-Air protocol by David Sainty, and a core bootloader based directly on Optiboot.
>
> Optiboot builds on the original work of Jason P. Kyle (stk500boot.c), [Arduino group (bootloader)](http://arduino.cc), [Spiff (1K bootloader)](http://spiffie.org/know/arduino_1k_bootloader/bootloader.shtml), [AVR-Libc group](http://nongnu.org/avr-libc), [Ladyada (Adaboot)](http://www.ladyada.net/library/arduino/bootloader.html), Peter Knight (aka Cathedrow) and [Bill Westfield (aka WestfW)] (https://github.com/Optiboot/optiboot).
