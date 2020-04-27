/*
 * avrdude - A Downloader/Uploader for AVR device programmers
 * Copyright (C) 2015-2020 David Sainty
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/* $Id: xbee.c 11682 2015-08-19 03:07:56Z dave $ */

/*
 * avrdude interface for AVR devices Over-The-Air programmable via an
 * XBee Series 2 device.
 *
 * The XBee programmer is STK500v1 (optiboot) encapsulated in the XBee
 * API protocol.  The bootloader supporting this protocol is available at:
 *
 * https://github.com/davidsainty/xbeeboot
 */

#include "ac_cfg.h"

#include <sys/time.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "avrdude.h"
#include "libavrdude.h"
#include "stk500_private.h"
#include "stk500.h"
#include "xbee.h"

/*
 * Read signature bytes - Direct copy of the Arduino behaviour to
 * satisfy Optiboot.
 */
static int xbee_read_sig_bytes(PROGRAMMER *pgm, AVRPART *p, AVRMEM *m)
{
  unsigned char buf[32];

  /* Signature byte reads are always 3 bytes. */

  if (m->size < 3) {
    avrdude_message(MSG_INFO, "%s: memsize too small for sig byte read",
                    progname);
    return -1;
  }

  buf[0] = Cmnd_STK_READ_SIGN;
  buf[1] = Sync_CRC_EOP;

  serial_send(&pgm->fd, buf, 2);

  if (serial_recv(&pgm->fd, buf, 5) < 0)
    return -1;
  if (buf[0] == Resp_STK_NOSYNC) {
    avrdude_message(MSG_INFO, "%s: stk500_cmd(): programmer is out of sync\n",
                    progname);
    return -1;
  } else if (buf[0] != Resp_STK_INSYNC) {
    avrdude_message(MSG_INFO,
                    "\n%s: xbee_read_sig_bytes(): (a) protocol error, "
                    "expect=0x%02x, resp=0x%02x\n",
                    progname, Resp_STK_INSYNC, buf[0]);
    return -2;
  }
  if (buf[4] != Resp_STK_OK) {
    avrdude_message(MSG_INFO,
                    "\n%s: xbee_read_sig_bytes(): (a) protocol error, "
                    "expect=0x%02x, resp=0x%02x\n",
                    progname, Resp_STK_OK, buf[4]);
    return -3;
  }

  m->buf[0] = buf[1];
  m->buf[1] = buf[2];
  m->buf[2] = buf[3];

  return 3;
}

struct XBeeBootSession {
  struct serial_device *serialDevice;
  union filedescriptor serialDescriptor;

  unsigned char xbee_address[10];
  int directMode;
  unsigned char outSequence;
  unsigned char inSequence;

  size_t inInIndex;
  size_t inOutIndex;
  unsigned char inBuffer[256];
};

static void XBeeBootSessionInit(struct XBeeBootSession *xbs) {
  xbs->serialDevice = &serial_serdev;
  xbs->directMode = 1;
  xbs->outSequence = 0;
  xbs->inSequence = 0;
  xbs->inInIndex = 0;
  xbs->inOutIndex = 0;
}

#define xbeebootsession(fdp) (struct XBeeBootSession*)((fdp)->pfd)

static void sendAPIRequest(struct XBeeBootSession *xbs,
                           unsigned char apiType,
                           int apiOption,
                           int prePayload1,
                           int prePayload2,
                           int packetType,
                           int sequence,
                           int appType,
                           unsigned int dataLength,
                           const unsigned char *data)
{
  unsigned char frame[256];

  unsigned char *fp = &frame[5];
  unsigned char *dataStart = fp;
  unsigned char checksum = 0xff;
  unsigned char length = 0;

  if (verbose >= MSG_NOTICE2) {
    struct timeval time;
    gettimeofday(&time, NULL);
    avrdude_message(MSG_NOTICE2,
                    "%s: sendAPIRequest(): %lu.%06lu %d, %d, %d, %d\n",
                    progname, (unsigned long)time.tv_sec,
                    (unsigned long)time.tv_usec,
                    (int)packetType, (int)sequence, appType,
                    data == NULL ? -1 : (int)*data);
  }

#define fpput(x)                                                \
  do {                                                          \
    const unsigned char v = (x);                                \
    if (v == 0x7d || v == 0x7e || v == 0x11 || v == 0x13) {     \
      *fp++ = 0x7d;                                             \
      *fp++ = v ^ 0x20;                                         \
    } else {                                                    \
      *fp++ = v;                                                \
    }                                                           \
    checksum -= v;                                              \
    length++;                                                   \
  } while (0)

  fpput(apiType); /* ZigBee Receive Packet or ZigBee Transmit Request */

  if (apiOption >= 0)
    fpput(apiOption); /* Receive options (RX) or Delivery sequence (TX/AT) */

  if (apiType != 0x08) {
    /* Automatically inhibit addressing for local AT command requests. */
    size_t index;
    for (index = 0; index < 10; index++) {
      const unsigned char val = xbs->xbee_address[index];
      fpput(val);
    }
  }

  if (prePayload1 >= 0)
    fpput(prePayload1); /* Transmit broadcast radius */

  if (prePayload2 >= 0)
    fpput(prePayload2); /* Transmit options */

  if (packetType >= 0)
    fpput(packetType); /* REQUEST */

  if (sequence >= 0)
    fpput(sequence);

  if (appType >= 0)
    fpput(appType); /* FIRMWARE_DELIVER */

  {
    size_t index;
    for (index = 0; index < dataLength; index++)
      fpput(data[index]);
  }

  /* Length BEFORE checksum byte */
  const unsigned char unescapedLength = length;

  fpput(checksum);

  /* Length AFTER checksum byte */
  const unsigned int finalLength = fp - dataStart;

  frame[0] = 0x7e;
  fp = &frame[1];
  fpput(0);
  fpput(unescapedLength);
  const unsigned int prefixLength = fp - frame;
  unsigned char *frameStart = dataStart - prefixLength;
  memmove(frameStart, frame, prefixLength);

  xbs->serialDevice->send(&xbs->serialDescriptor,
                          frameStart, finalLength + prefixLength);
}

static unsigned char txSequence = 0;

static void sendPacket(struct XBeeBootSession *xbs,
                       unsigned char packetType,
                       unsigned char sequence,
                       int appType,
                       unsigned int dataLength,
                       const unsigned char *data)
{
  unsigned char apiType;
  int prePayload1;
  int prePayload2;

  if (xbs->directMode) {
    /*
     * In direct mode we are pretending to be an XBee device
     * forwarding on data received from the transmitting XBee.  We
     * therefore format the data as a remote XBee would, encapsulated
     * in a 0x90 packet.
     */
    apiType = 0x90; /* ZigBee Receive Packet */
    prePayload1 = -1;
    prePayload2 = -1;
  } else {
    /*
     * In normal mode we are requesting a payload delivery,
     * encapsulated in a 0x10 packet.
     */
    apiType = 0x10; /* ZigBee Transmit Request */
    prePayload1 = 0;
    prePayload2 = 0;
  }

  while ((++txSequence & 0xff) == 0);
  sendAPIRequest(xbs, apiType, txSequence,
                 prePayload1, prePayload2, packetType,
                 sequence, appType, dataLength, data);
}

/*
 * Return 0 on success.
 * Return -1 on generic error (normally serial timeout).
 * Return -512 + XBee AT Response code
 */
#define XBEE_AT_RETURN_CODE(x) (((x) >= -512 && (x) <= -256) ? (x) + 512 : -1)
static int xbeedev_poll(struct XBeeBootSession *xbs,
                        unsigned char *buf, size_t buflen,
                        int waitForAck,
                        int waitForSequence)
{
#define XBEE_LENGTH_LEN 2
#define XBEE_CHECKSUM_LEN 1
#define XBEE_APITYPE_LEN 1
#define XBEE_APISEQUENCE_LEN 1
#define XBEE_ADDRESS_64BIT_LEN 8
#define XBEE_ADDRESS_16BIT_LEN 2
#define XBEE_RADIUS_LEN 1
#define XBEE_TXOPTIONS_LEN 1
#define XBEE_RXOPTIONS_LEN 1
  for (;;) {
    unsigned char byte;
    unsigned char frame[256];
    unsigned int frameSize;

  before_frame:
    do {
      const int rc = xbs->serialDevice->recv(&xbs->serialDescriptor, &byte, 1);
      if (rc < 0)
        return rc;
    } while (byte != 0x7e);

  start_of_frame:
    {
      size_t index = 0;
      int escaped = 0;
      frameSize = XBEE_LENGTH_LEN;
      do {
        const int rc = xbs->serialDevice->recv(&xbs->serialDescriptor,
                                               &byte, 1);
        if (rc < 0)
          return rc;

        if (byte == 0x7e)
          /*
           * No matter when we receive a frame start byte, we should
           * abort parsing and start a fresh frame.
           */
          goto start_of_frame;

        if (escaped) {
          byte ^= 0x20;
          escaped = 0;
        } else if (byte == 0x7d) {
          escaped = 1;
          continue;
        }

        if (index >= sizeof(frame))
          goto before_frame;

        frame[index++] = byte;

        if (index == XBEE_LENGTH_LEN) {
          /* Length plus the two length bytes, plus the checksum byte */
          frameSize = (frame[0] << 8 | frame[1]) +
            XBEE_LENGTH_LEN + XBEE_CHECKSUM_LEN;

          if (frameSize >= sizeof(frame))
            /* Too long - immediately give up on this frame */
            goto before_frame;
        }
      } while (index < frameSize);

      /* End of frame */
      unsigned char checksum = 1;
      size_t cIndex;
      for (cIndex = 2; cIndex < index; cIndex++) {
        checksum += frame[cIndex];
      }

      if (checksum) {
        /* Checksum didn't match */
        avrdude_message(MSG_NOTICE2,
                        "%s: xbeedev_poll(): Bad checksum %d\n",
                        progname, (int)checksum);
        continue;
      }
    }

    const unsigned char frameType = frame[2];

    avrdude_message(MSG_NOTICE2,
                    "%s: xbeedev_poll(): Received frame type %x\n",
                    progname, (unsigned int)frameType);

    if (frameType == 0x97 && frameSize > 16) {
      /* Remote command response */
      unsigned char resultCode = frame[16];

      avrdude_message(MSG_NOTICE,
                      "%s: xbeedev_poll(): Remote command %d result code %d\n",
                      progname, (int)frame[3], (int)resultCode);

      if (waitForSequence >= 0 && waitForSequence == frame[3])
        /* Received result for our sequence numbered request */
        return -512 + resultCode;
    } else if (frameType == 0x88 && frameSize > 6) {
      /* Local command response */
      avrdude_message(MSG_NOTICE,
                      "%s: xbeedev_poll(): Local command %c%c result code %d\n",
                      progname, frame[4], frame[5], (int)frame[6]);

      if (waitForSequence >= 0 && waitForSequence == frame[3])
        /* Received result for our sequence numbered request */
        return 0;
    } else if (frameType == 0x8b && frameSize > 7) {
      /* Transmit status */
      avrdude_message(MSG_NOTICE2,
                      "%s: xbeedev_poll(): Transmit status %d result code %d\n",
                      progname, (int)frame[3], (int)frame[7]);
    } else if (frameType == 0x10 || frameType == 0x90) {
      unsigned char *dataStart;
      unsigned int dataLength;

      if (frameType == 0x10) {
        /* Direct mode frame */
        const unsigned int header = XBEE_LENGTH_LEN +
          XBEE_APITYPE_LEN + XBEE_APISEQUENCE_LEN +
          XBEE_ADDRESS_64BIT_LEN + XBEE_ADDRESS_16BIT_LEN +
          XBEE_RADIUS_LEN + XBEE_TXOPTIONS_LEN;

        if (frameSize <= header + XBEE_CHECKSUM_LEN)
          /* Bounds check: Frame is too small */
          continue;

        dataLength = frameSize - header - XBEE_CHECKSUM_LEN;
        dataStart = &frame[header];
      } else {
        /* Remote reply frame */
        const unsigned int header = XBEE_LENGTH_LEN +
          XBEE_APITYPE_LEN + XBEE_ADDRESS_64BIT_LEN + XBEE_ADDRESS_16BIT_LEN +
          XBEE_RXOPTIONS_LEN;

        if (frameSize <= header + XBEE_CHECKSUM_LEN)
          /* Bounds check: Frame is too small */
          continue;

        dataLength = frameSize - header - XBEE_CHECKSUM_LEN;
        dataStart = &frame[header];

        if (memcmp(&frame[XBEE_LENGTH_LEN + XBEE_APITYPE_LEN],
                   xbs->xbee_address, XBEE_ADDRESS_64BIT_LEN) != 0) {
          /*
           * This packet is not from our target device.  Unlikely
           * to ever happen, but if it does we have to ignore
           * it.
           */
          continue;
        }

        /*
         * We don't start out knowing what the 16-bit device address
         * is, but we should receive it on the return packets, and
         * re-use it from that point on.
         */
        {
          const unsigned char * const rx16Bit =
            &frame[XBEE_LENGTH_LEN + XBEE_APITYPE_LEN +
                   XBEE_ADDRESS_64BIT_LEN];
          unsigned char * const tx16Bit =
            &xbs->xbee_address[XBEE_ADDRESS_64BIT_LEN];
          if (memcmp(rx16Bit, tx16Bit, XBEE_ADDRESS_16BIT_LEN) != 0) {
            avrdude_message(MSG_NOTICE2, "%s: xbeedev_poll(): "
                            "New 16-bit address: %02x%02x\n",
                            progname,
                            (unsigned int)rx16Bit[0],
                            (unsigned int)rx16Bit[1]);
            memcpy(tx16Bit, rx16Bit, XBEE_ADDRESS_16BIT_LEN);
          }
        }
      }

      if (dataLength >= 2) {
        const unsigned char protocolType = dataStart[0];
        const unsigned char sequence = dataStart[1];

        if (verbose >= MSG_NOTICE2) {
          struct timeval time;
          gettimeofday(&time, NULL);
          avrdude_message(MSG_NOTICE2, "%s: xbeedev_poll(): "
                          "%lu.%06lu Packet %d #%d\n",
                          progname, (unsigned long)time.tv_sec,
                          (unsigned long)time.tv_usec,
                          (int)protocolType, (int)sequence);
        }

        if (protocolType == 0) {
          /* ACK */
          /*
           * We can't update outSequence here, we already do that
           * somewhere else.
           */
          if (waitForAck >= 0 && waitForAck == sequence)
            return 0;
        } else if (protocolType == 1 && dataLength >= 4 &&
                   dataStart[2] == 24) {
          /* REQUEST FRAME_REPLY */
          unsigned char nextSequence = xbs->inSequence;
          while ((++nextSequence & 0xff) == 0);
          if (sequence == nextSequence) {
            xbs->inSequence = nextSequence;

            const size_t textLength = dataLength - 3;
            size_t index;
            for (index = 0; index < textLength; index++) {
              const unsigned char data = dataStart[3 + index];
              if (buflen > 0) {
                /* If we are receiving right now, and have a buffer... */
                *buf++ = data;
                buflen--;
              } else {
                xbs->inBuffer[xbs->inInIndex++] = data;
                if (xbs->inInIndex == sizeof(xbs->inBuffer))
                  xbs->inInIndex = 0;
                if (xbs->inInIndex == xbs->inOutIndex) {
                  /* Should be impossible */
                  avrdude_message(MSG_INFO, "%s: Buffer overrun", progname);
                  exit(1);
                }
              }
            }

            /*avrdude_message(MSG_INFO, "ACK %x\n", (unsigned int)sequence);*/
            sendPacket(xbs, 0 /* ACK */, sequence, -1, 0, NULL);

            if (buflen == 0 && buf != NULL)
              /* Input buffer has been filled */
              return 0;
          }
        }
      }
    }
  }
}

static int localAT(struct XBeeBootSession *xbs,
                   unsigned char at1, unsigned char at2, int value)
{
  if (xbs->directMode)
    /*
     * Remote XBee AT commands make no sense in direct mode - there is
     * no XBee device to communicate with.
     */
    return 0;

  while ((++txSequence & 0xff) == 0);
  const unsigned char sequence = txSequence;

  unsigned char buf[3];
  size_t length = 0;

  buf[length++] = at1;
  buf[length++] = at2;

  if (value >= 0)
    buf[length++] = (unsigned char)value;

  avrdude_message(MSG_NOTICE, "%s: Local AT command: %c%c\n",
                  progname, at1, at2);

  /* Local AT command 0x08 */
  sendAPIRequest(xbs, 0x08, -1, -1, -1, -1, sequence, -1, length, buf);

  int retries;
  for (retries = 0; retries < 5; retries++) {
    const int rc = xbeedev_poll(xbs, NULL, 0, -1, sequence);
    if (!rc)
      return rc;
  }

  return -1;
}

/*
 * Return 0 on success.
 * Return -1 on generic error (normally serial timeout).
 * Return -512 + XBee AT Response code
 */
static int sendAT(struct XBeeBootSession *xbs,
                  unsigned char at1, unsigned char at2, int value)
{
  if (xbs->directMode)
    /*
     * Remote XBee AT commands make no sense in direct mode - there is
     * no XBee device to communicate with.
     */
    return 0;

  while ((++txSequence & 0xff) == 0);
  const unsigned char sequence = txSequence;

  unsigned char buf[3];
  size_t length = 0;

  buf[length++] = at1;
  buf[length++] = at2;

  if (value >= 0)
    buf[length++] = (unsigned char)value;

  avrdude_message(MSG_NOTICE,
                  "%s: Remote AT command: %c%c\n", progname, at1, at2);

  /* Remote AT command 0x17 with Apply Changes 0x02 */
  sendAPIRequest(xbs, 0x17, sequence,
                 -1, -1, -1,
                 0x02, -1, length, buf);

  int retries;
  for (retries = 0; retries < 30; retries++) {
    const int rc = xbeedev_poll(xbs, NULL, 0, -1, sequence);
    const int xbeeRc = XBEE_AT_RETURN_CODE(rc);
    if (xbeeRc == 0)
      /* Translate to normal success code */
      return 0;
    if (rc != -1)
      return rc;
  }

  return -1;
}

/*
 * Return 0 on no error recognised, 1 if error was detected and
 * reported.
 */
static int xbeeATError(int rc) {
  const int xbeeRc = XBEE_AT_RETURN_CODE(rc);
  if (xbeeRc < 0)
    return 0;

  if (xbeeRc == 1) {
    avrdude_message(MSG_INFO, "%s: Error communicating with Remote XBee\n",
                    progname);
  } else if (xbeeRc == 2) {
    avrdude_message(MSG_INFO, "%s: Remote XBee command error: "
                    "Invalid command\n",
                    progname);
  } else if (xbeeRc == 3) {
    avrdude_message(MSG_INFO, "%s: Remote XBee command error: "
                    "Invalid parameter\n",
                    progname);
  } else if (xbeeRc == 4) {
    avrdude_message(MSG_INFO, "%s: Remote XBee error: "
                    "Transmission failure\n",
                    progname);
  } else {
    avrdude_message(MSG_INFO, "%s: Unrecognised remote XBee error code %d\n",
                    progname, xbeeRc);
  }
  return 1;
}

static void xbeedev_free(struct XBeeBootSession *xbs)
{
  xbs->serialDevice->close(&xbs->serialDescriptor);
  free(xbs);
}

static void xbeedev_close(union filedescriptor *fdp)
{
  struct XBeeBootSession *xbs = xbeebootsession(fdp);
  xbeedev_free(xbs);
}

static int xbeedev_open(char *port, union pinfo pinfo,
                        union filedescriptor *fdp)
{
  /*
   * The syntax for XBee devices is defined as:
   *
   * -P <XBeeAddress>@[serialdevice]
   *
   * ... or ...
   *
   * -P @[serialdevice]
   *
   * ... for a direct connection.
   */
  char *ttySeparator = strchr(port, '@');
  if (ttySeparator == NULL) {
    avrdude_message(MSG_INFO,
                    "%s: XBee: Bad port syntax: "
                    "require \"<xbee-address>@<serial-device>\"\n",
                    progname);
    return -1;
  }

  struct XBeeBootSession *xbs = malloc(sizeof(struct XBeeBootSession));
  if (xbs == NULL) {
    avrdude_message(MSG_INFO, "%s: xbeedev_open(): out of memory\n",
                    progname);
    return -1;
  }

  XBeeBootSessionInit(xbs);

  char *tty = &ttySeparator[1];

  if (ttySeparator == port) {
    /* Direct connection */
    memset(xbs->xbee_address, 0, 8);
    xbs->directMode = 1;
  } else {
    size_t addrIndex = 0;
    int nybble = -1;
    char const *address = port;
    while (address != ttySeparator) {
      char hex = *address++;
      unsigned int val;
      if (hex >= '0' && hex <= '9') {
        val = hex - '0';
      } else if (hex >= 'A' && hex <= 'F') {
        val = hex - 'A' + 10;
      } else if  (hex >= 'a' && hex <= 'f') {
        val = hex - 'a' + 10;
      } else {
        break;
      }
      if (nybble == -1) {
        nybble = val;
      } else {
        xbs->xbee_address[addrIndex++] = (nybble * 16) | val;
        nybble = -1;
        if (addrIndex == 8)
          break;
      }
    }

    if (addrIndex != 8 || address != ttySeparator || nybble != -1) {
      avrdude_message(MSG_INFO,
                      "%s: XBee: Bad XBee address: "
                      "require 16-character hexadecimal address\"\n",
                      progname);
      free(xbs);
      return -1;
    }

    xbs->directMode = 0;
  }

  /* Unknown 16 bit address */
  xbs->xbee_address[8] = 0xff;
  xbs->xbee_address[9] = 0xfe;

  avrdude_message(MSG_TRACE,
                  "%s: XBee address: %02x%02x%02x%02x%02x%02x%02x%02x\n",
                  progname,
                  (unsigned int)xbs->xbee_address[0],
                  (unsigned int)xbs->xbee_address[1],
                  (unsigned int)xbs->xbee_address[2],
                  (unsigned int)xbs->xbee_address[3],
                  (unsigned int)xbs->xbee_address[4],
                  (unsigned int)xbs->xbee_address[5],
                  (unsigned int)xbs->xbee_address[6],
                  (unsigned int)xbs->xbee_address[7]);

  if (pinfo.baud) {
    /*
     * User supplied the correct baud rate.
     */
  } else if (xbs->directMode) {
    /*
     * In direct mode, default to 19200.
     *
     * Why?
     *
     * In this mode, we are NOT talking to an XBee, we are talking
     * directly to an AVR device that thinks it is talking to an XBee
     * itself.
     *
     * Because, an XBee is a 3.3V device defaulting to 9600baud, and
     * the Atmel328P is only rated at a maximum clock rate of 8MHz
     * with a 3.3V supply, so there's a high likelihood a remote
     * Atmel328P will be clocked at 8MHz.
     *
     * With a direct connection, there's a good chance we're talking
     * to an Arduino clocked at 16MHz with an XBee-enabled chip
     * plugged in.  The doubled clock rate means a doubled serial
     * rate.  Double 9600 baud == 19200 baud.
     */
    pinfo.baud = 19200;
  } else {
    /*
     * In normal mode, default to 9600.
     *
     * Why?
     *
     * XBee devices default to 9600 baud.  In this mode we are talking
     * to the XBee device, not the far-end device, so it's the local
     * XBee baud rate we should select.  The baud rate of the AVR
     * device is irrelevant.
     */
    pinfo.baud = 9600;
  }

  avrdude_message(MSG_NOTICE, "%s: Baud %ld\n", progname, (long)pinfo.baud);

  {
    const int rc = xbs->serialDevice->open(tty, pinfo,
                                           &xbs->serialDescriptor);
    if (rc < 0) {
      free(xbs);
      return rc;
    }
  }

  /* Disable RTS */
  if (!xbs->directMode) {
    {
      const int rc = localAT(xbs, 'A', 'P', 2);
      if (rc < 0) {
        avrdude_message(MSG_INFO, "%s: Local XBee is not responding.\n",
                        progname);
        xbeedev_free(xbs);
        return rc;
      }
    }

    const int rc = sendAT(xbs, 'D', '6', 0);
    if (rc < 0) {
      xbeedev_free(xbs);

      if (xbeeATError(rc))
        return -1;

      avrdude_message(MSG_INFO, "%s: Remote XBee is not responding.\n",
                      progname);
      return rc;
    }
  }

  fdp->pfd = xbs;

  return 0;
}

static int xbeedev_send(union filedescriptor *fdp,
                        const unsigned char *buf, size_t buflen)
{
  struct XBeeBootSession *xbs = xbeebootsession(fdp);

  while (buflen > 0) {
    unsigned char sequence = xbs->outSequence;
    while ((++sequence & 0xff) == 0);
    xbs->outSequence = sequence;

    /*
     * Chunk the data into chunks of up to 64 bytes.
     */
    const unsigned char blockLength = (buflen > 64) ? 64 : buflen;

    /* Repeatedly send whilst timing out waiting for ACK responses. */
    for (;;) {
      sendPacket(xbs, 1 /* REQUEST */, sequence,
                 23 /* FIRMWARE_DELIVER */,
                 blockLength, buf);
      if (!xbeedev_poll(xbs, NULL, 0, sequence, -1))
        break;

      /*
       * If we don't receive an ACK it might be because the chip
       * missed an ACK from us.  Resend that too after a timeout,
       * unless it's zero which is an illegal sequence number.
       */
      if (xbs->inSequence != 0)
        sendPacket(xbs, 0 /* ACK */, xbs->inSequence, -1, 0, NULL);
    }

    buflen -= blockLength;
    buf += blockLength;
  }

  return 0;
}

static int xbeedev_recv(union filedescriptor *fdp,
                        unsigned char *buf, size_t buflen)
{
  struct XBeeBootSession *xbs = xbeebootsession(fdp);

  /*
   * First de-buffer anything previously received in a chunk that
   * couldn't be immediately delievered.
   */
  while (xbs->inInIndex != xbs->inOutIndex) {
    *buf++ = xbs->inBuffer[xbs->inOutIndex++];
    if (xbs->inOutIndex == sizeof(xbs->inBuffer))
      xbs->inOutIndex = 0;
    if (--buflen == 0)
      return 0;
  }

  int retries;
  for (retries = 0; retries < 30; retries++) {
    const int rc = xbeedev_poll(xbs, buf, buflen, -1, -1);
    if (!rc)
      return rc;

    /*
     * The chip may have missed an ACK from us.  Resend after a
     * timeout.
     */
    if (xbs->inSequence != 0)
      sendPacket(xbs, 0 /* ACK */, xbs->inSequence, -1, 0, NULL);
  }
  return -1;
}

static int xbeedev_drain(union filedescriptor *fdp, int display)
{
  struct XBeeBootSession *xbs = xbeebootsession(fdp);

  /*
   * Flushing the local serial buffer is unhelpful under this
   * protocol.
   */
  unsigned char flush;
  do {
    xbs->inOutIndex = xbs->inInIndex = 0;
  } while (xbeedev_poll(xbs, &flush, 1, -1, -1) == 0);

  return 0;
}

static int xbeedev_set_dtr_rts(union filedescriptor *fdp, int is_on)
{
  struct XBeeBootSession *xbs = xbeebootsession(fdp);

  if (xbs->directMode)
    /* Correct for direct mode */
    return xbs->serialDevice->set_dtr_rts(&xbs->serialDescriptor, is_on);

  /*
   * For non-direct mode (Over-The-Air) need XBee commands for
   * remote.
   */
  const int rc = sendAT(xbs, 'D', '3', is_on ? 5 : 4);
  if (rc < 0) {
    if (xbeeATError(rc))
      return -1;

    avrdude_message(MSG_INFO,
                    "%s: Remote XBee is not responding.\n", progname);
    return rc;
  }

  return 0;
}

/*
 * Device descriptor for XBee framing.
 */
static struct serial_device xbee_serdev_frame = {
  .open = xbeedev_open,
  .close = xbeedev_close,
  .send = xbeedev_send,
  .recv = xbeedev_recv,
  .drain = xbeedev_drain,
  .set_dtr_rts = xbeedev_set_dtr_rts,
  .flags = SERDEV_FL_NONE,
};

static int xbee_open(PROGRAMMER *pgm, char *port)
{
  union pinfo pinfo;
  strcpy(pgm->port, port);
  pinfo.baud = pgm->baudrate;

  /* Wireless is lossier than normal serial */
  serial_recv_timeout = 1000;

  serdev = &xbee_serdev_frame;

  if (serial_open(port, pinfo, &pgm->fd)==-1) {
    return -1;
  }

  /* Clear DTR and RTS */
  serial_set_dtr_rts(&pgm->fd, 0);
  usleep(250*1000);

  /* Set DTR and RTS back to high */
  serial_set_dtr_rts(&pgm->fd, 1);
  usleep(50*1000);

  /*
   * drain any extraneous input
   */
  stk500_drain(pgm, 0);

  if (stk500_getsync(pgm) < 0)
    return -1;

  return 0;
}

static void xbee_close(PROGRAMMER *pgm)
{
  struct XBeeBootSession *xbs = xbeebootsession(&pgm->fd);

  xbs->serialDevice->set_dtr_rts(&xbs->serialDescriptor, 0);

  /*
   * We have tweaked a few settings on the XBee, including the RTS
   * mode and the reset pin's configuration.  Do a soft full reset,
   * restoring the device to its normal power-on settings.
   */
  if (!xbs->directMode) {
    const int rc = sendAT(xbs, 'F', 'R', -1);
    xbeeATError(rc);
  }

  xbeedev_free(xbs);

  pgm->fd.pfd = NULL;
}

const char xbee_desc[] = "XBee Series 2 Over-The-Air (XBeeBoot)";

void xbee_initpgm(PROGRAMMER *pgm)
{
  /*
   * This behaves like an Arduino, but with packet encapsulation of
   * the serial streams, XBee device management, and XBee GPIO for the
   * Auto-Reset feature.
   */
  stk500_initpgm(pgm);

  strncpy(pgm->type, "XBee", sizeof(pgm->type));
  pgm->read_sig_bytes = xbee_read_sig_bytes;
  pgm->open = xbee_open;
  pgm->close = xbee_close;
}
