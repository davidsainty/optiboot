## XBee Series 2 API Bootloader for Arduino and Atmel AVR ##

This bootloader provides XBee Series 2 Over-The-Air firmware update capability to Atmel AVR devices, as well as supporting direct firmware update via the standard Optiboot protocol.

It provides the following features:

  * Bootloader fits within 1kB: This is larger than Optiboot's smallest build, but is still half the size of a standard Arduino bootlooader.
  * Reasonably fast Over-The-Air firware updates: Less than three minutes to write a 20kB firmware image.
  * Natively supports XBee API-mode protocol: No need to use AT-mode XBee firmware.
  * Over-The-Air updates are secure: XBee security models apply - so firmware updates are encrypted and authenticated with AES encryption if the Zigbee network is configured with encryption.
  * Over-The-Air updates are robust: The protocol uses direct addressing, so the other devices on the Zigbee network are unaffected.  You could even Over-The-Air update firmware on multiple devices concurrently.
  * Full access to Optiboot-supported bootloader facilities with a direct connection via standard avrdude software: The chip can still be firmware updated via a normal Arduino and unmodified avrdude software without replacing the bootloader.
  * Full access to Optiboot-supported bootloader facilities Over-The-Air via patched avrdude software.
