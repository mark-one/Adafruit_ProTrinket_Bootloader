/* VUSBtinyBoot by me@frank-zhao.com
 *  
 * VUSBtinyBoot is a bootloader that emulates a USBtinyISP (from Adafruit Industries)
 *  
 * Trinket Pro (from Adafruit Industries) will use VUSBtinyBoot
 *
 * This code is heavily derived from USBaspLoader, but also from USBtiny, with USBtinyISP's settings
 
   Copyright (c) 2013 Adafruit Industries
   All rights reserved.

   Redistribution and use in source and binary forms, with or without
   modification, are permitted provided that the following conditions
   are met:

   * Redistributions of source code must retain the above copyright
     notice, this list of conditions and the following disclaimer.
   * Redistributions in binary form must reproduce the above copyright
     notice, this list of conditions and the following disclaimer in the
     documentation and/or other materials provided with the distribution.
   * Neither the name of the authors nor the names of its contributors
     may be used to endorse or promote products derived from this software
     without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
    AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
    IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
    ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
    LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
    CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
    SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
    INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
    CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
    ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
    POSSIBILITY OF SUCH DAMAGE.
 */

#include <avr/io.h>
#ifndef MCUSR
#define MCUSR MCUCSR // hack for enabling ATmega8 support
#endif
#define	SIGRD	5	// this is missing from some of the io.h files, this is a hack so avr/boot.h can be used
#include "avr_boot.h"
#include <avr/pgmspace.h>
//#include <avr/fuse.h>
#include <avr/interrupt.h>
#include <avr/wdt.h>
//#include <util/delay.h>
#include "pin_defs.h"
#include "optiboot.h"
#include <usbconfig.h>
#include <bootloaderconfig.h>
#include <usbdrv/usbdrv.c>	// must be included, because of static function declarations are being used, which saves flash space

// enable features here
#define ENABLE_BOOTLOAD_BYPASS
#define ENABLE_FLASH_WRITING
#define ENABLE_FLASH_READING
#define ENABLE_EEPROM_WRITING
#define ENABLE_EEPROM_READING
//#define ENABLE_CHIP_ERASE
//#define ENABLE_CHIP_ERASE_EEPROM_FUSE_CHECK
#define ENABLE_SIG_READING
#define ENABLE_FUSE_READING
#define ENABLE_REQUEST_EXIT // note: enabling this actually decreases code size
#define ENABLE_CLEAN_EXIT // must be used with ENABLE_REQUEST_EXIT
#define ENABLE_OPTIBOOT
#define ENABLE_BLANK_CHECK

// timeout for the bootloader
#define USBBOOTLOADER_TIMEOUT 10
#define UARTBOOTLOADER_TIMEOUT 3

enum
{
	// Generic requests
	USBTINY_ECHO,		// echo test
	USBTINY_READ,		// read byte
	USBTINY_WRITE,		// write byte
	USBTINY_CLR,		// clear bit 
	USBTINY_SET,		// set bit
	// Programming requests
	USBTINY_POWERUP,	// apply power (wValue:SCK-period, wIndex:RESET)
	USBTINY_POWERDOWN,	// remove power from chip
	USBTINY_SPI,		// issue SPI command (wValue:c1c0, wIndex:c3c2)
	USBTINY_POLL_BYTES,	// set poll bytes for write (wValue:p1p2)
	USBTINY_FLASH_READ,	// read flash (wIndex:address)
	USBTINY_FLASH_WRITE,	// write flash (wIndex:address, wValue:timeout)
	USBTINY_EEPROM_READ,	// read eeprom (wIndex:address)
	USBTINY_EEPROM_WRITE,	// write eeprom (wIndex:address, wValue:timeout)
	USBTINY_DDRWRITE,		// set port direction
	USBTINY_SPI1			// a single SPI command
};

#if (FLASHEND) > 0xFFFF		// need long addressing for large flash
#	define CUR_ADDR			cur_addr.addr
#	define addr_t			uint32_t
#else
#	define CUR_ADDR			cur_addr.u16[0]
#	define addr_t			uint16_t
#endif

typedef union longConverter { // utility for manipulating address pointer with proper endianness
	addr_t		addr;
	uint16_t	u16[sizeof(addr_t)/2];
	uint8_t		u8[sizeof(addr_t)];
} longConverter_t;

#ifdef ENABLE_REQUEST_EXIT
static	uint8_t				req_boot_exit;
#endif
static	longConverter_t		cur_addr;
static	uchar				dirty = 0;			// if flash needs to be written
static	uchar				cmd0;				// current read/write command byte
static	uint8_t				remaining;			// bytes remaining in current transaction
static	uchar				buffer[8];			// talk via setup
static	uint8_t				timeout = 0;		// timeout counter for USB comm
volatile	char			usbHasRxed = 0;		// whether or not USB comm is active
#ifdef ENABLE_BLANK_CHECK
static uchar				isBlank;			// only allow exit if chip isn't blank
#endif

void (*app_start)(void) = 0x0000; // function at start of flash memory, call to exit bootloader

// ----------------------------------------------------------------------
// finishes a write operation if already started
// ----------------------------------------------------------------------
static void finalize_flash_if_dirty()
{
	if (dirty != 0)
	{
		#ifdef ENABLE_FLASH_WRITING
		cli();
		boot_page_write(CUR_ADDR - 2);
		sei();
		boot_spm_busy_wait();
		cli();
		boot_rww_enable();
		sei();
		#endif
		dirty = 0;
	}
}

#ifdef ENABLE_CHIP_ERASE
// ----------------------------------------------------------------------
// chip erase
// ----------------------------------------------------------------------
static void perform_chip_erase()
{
	addr_t i;

	for (i = 0; i
	#ifdef BOOTLOADER_ADDRESS
	< (addr_t)BOOTLOADER_ADDRESS;
	#else
	<= (addr_t)FLASHEND;
	#endif
	i += SPM_PAGESIZE)
	{
		boot_spm_busy_wait();
		cli();
		boot_page_erase(i);
		sei();
	}

	#ifdef ENABLE_CHIP_ERASE_EEPROM_FUSE_CHECK
	uint8_t hfuse = boot_lock_fuse_bits_get(GET_HIGH_FUSE_BITS);
	if ((hfuse & (1 << 3)) != 0) // the bit location of EESAVE
	{
	#else
	{
	#endif
		for (i = 0; i <= E2END; i++)
		{
			eeprom_update_byte((uint8_t *)i, 0xFF);
		}
	}
}
#endif

// ----------------------------------------------------------------------
// Handle a non-standard SETUP packet.
// ----------------------------------------------------------------------
uchar	usbFunctionSetup ( uchar data[8] )
{
	uchar	req;
	usbRequest_t *rq = (void *)data;

	// reset the bootloader timeout timer
	timeout = 0;
	// indicate activity
	LED_PORT |= _BV(LED);

	// Generic requests
	req = data[1];
	if ( req == USBTINY_ECHO )
	{
		//usbMsgPtr = data;
		return 8;
	}
	/*
	else if ( req == USBTINY_SET || req == USBTINY_CLR || req == USBTINY_WRITE || req == USBTINY_DDRWRITE || req == USBTINY_POWERUP) {
		// do nothing
		return 0;
	}
	//*/
	else if ( req == USBTINY_READ) {
		// do nothing
		return 1;
	}
	else if ( req == USBTINY_POWERDOWN )
	{
		finalize_flash_if_dirty();
		#ifdef ENABLE_REQUEST_EXIT
		req_boot_exit = 1;
		#endif
		return 0;
	}
	else if ( req == USBTINY_SPI )
	{
		finalize_flash_if_dirty(); // partial page writes are not fully written unless this is called here, it must be HERE

		usbMsgPtr = (usbMsgPtr_t)buffer;

		// this tricks "usbtiny_cmd" into succeeding
		buffer[2] = data[3];

		// for the commands, refer to ATmega datasheet under "Serial Programming Instruction Set"
		// usage of avr/boot.h here is experimental

		#ifdef ENABLE_SIG_READING
		if (data[2] == 0x30 && data[3] == 0x00) {
			// read signature byte
			buffer[3] = boot_signature_byte_get(data[4] * 2);
		}
		#ifndef ENABLE_FUSE_READING
		else
		{
			buffer[3] = 0;
		}
		#endif
		#endif
		#ifdef ENABLE_FUSE_READING
		if (data[2] == 0x50 && data[3] == 0x00 && data[4] == 0x00) {
			// read LFUSE
			buffer[3] = boot_lock_fuse_bits_get(GET_LOW_FUSE_BITS);
		}
		else if (data[2] == 0x58 && data[3] == 0x08 && data[4] == 0x00) {
			// read HFUSE
			buffer[3] = boot_lock_fuse_bits_get(GET_HIGH_FUSE_BITS);
		}
		else if (data[2] == 0x50 && data[3] == 0x08 && data[4] == 0x00) {
			// read EFUSE
			buffer[3] = boot_lock_fuse_bits_get(GET_EXTENDED_FUSE_BITS);
		}
		else if (data[2] == 0x58 && data[3] == 0x00 && data[4] == 0x00) {
			// read lock bits
			buffer[3] = boot_lock_fuse_bits_get(GET_LOCK_BITS);
		}
		else if (data[2] == 0x38 && data[3] == 0x00 && data[4] == 0x00) {
			// read calibration
			buffer[3] = boot_signature_byte_get(0);
		}
		#endif
		#if !defined(ENABLE_SIG_READING) && !defined(ENABLE_FUSE_READING)
		buffer[3] = 0;
		#endif

		#ifdef ENABLE_CHIP_ERASE
		if (data[2] == 0xAC && data[3] == 0x80 && data[4] == 0x00 && data[5] == 0x00) {
			perform_chip_erase();
		}
		#endif

		// all other commands are unhandled

		return 4;
	}
	else if ( req == USBTINY_SPI1 )
	{
		// I don't know what this is used for, there are no single SPI transactions in the ISP protocol
		finalize_flash_if_dirty();
		return 1;
	}
	else if ( req == USBTINY_POLL_BYTES )
	{
		finalize_flash_if_dirty();
		return 0;
	}
	CUR_ADDR = *((uint16_t*)(&data[4]));
	remaining = rq->wLength.bytes[0];
	if ( req >= USBTINY_FLASH_READ && req <= USBTINY_EEPROM_WRITE )
	{
		cmd0 = req;
		if ( cmd0 != USBTINY_FLASH_WRITE ) {
			finalize_flash_if_dirty();
		}
		return USB_NO_MSG;	// usbFunctionRead() or usbFunctionWrite() will be called to handle the data
	}

	// do nothing if nothing done
	return 0;
}

// ----------------------------------------------------------------------
// Handle an IN packet.
// ----------------------------------------------------------------------
uchar	usbFunctionRead ( uchar* data, uchar len )
{
	uchar	i;

	if(len > remaining) {
		len = remaining;
	}

	remaining -= len;

	for	( i = 0; i < len; i++ )
	{
		if (cmd0 == USBTINY_EEPROM_READ) {
			#ifdef ENABLE_EEPROM_READING
			*data = eeprom_read_byte((void *)cur_addr.u16[0]);
			#endif
		}
		else if (cmd0 == USBTINY_FLASH_READ) {
			#ifdef ENABLE_FLASH_READING
			*data = pgm_read_byte((void *)CUR_ADDR);
			#endif
		}
		data++;
		CUR_ADDR++;
	}
	return len;
}

// ----------------------------------------------------------------------
// Handle an OUT packet.
// ----------------------------------------------------------------------
uchar	usbFunctionWrite ( uchar* data, uchar len )
{
	uchar	i, isLast;

	if(len > remaining) {
		len = remaining;
	}
	remaining -= len;
	isLast = remaining == 0;

	if (cmd0 == USBTINY_EEPROM_WRITE)
	{
		#ifdef ENABLE_EEPROM_WRITING
		for	( i = 0; i < len; i++ ) {
			eeprom_write_byte((void *)(cur_addr.u16[0]++), *data++);
		}
		#endif
	}
	else if (cmd0 == USBTINY_FLASH_WRITE)
	{
		#ifdef ENABLE_FLASH_WRITING
		for ( i = 0; i < len; )
		{
			if ((cur_addr.u16[0] & (SPM_PAGESIZE - 1)) == 0) {
				// page start, erase
				cli();
				boot_page_erase(CUR_ADDR);
				sei();
				boot_spm_busy_wait();
			}

			dirty = 1;
			cli();
			boot_page_fill(CUR_ADDR, *(short *)data);
			sei();

			CUR_ADDR += 2;
			data += 2;
			i += 2;

			if ((cur_addr.u16[0] & (SPM_PAGESIZE - 1)) == 0) {
				// end of page
				finalize_flash_if_dirty();
			}
		}
		#endif
		#ifdef ENABLE_BLANK_CHECK
		isBlank = 0;
		#endif
	}

	return isLast;
}

// ----------------------------------------------------------------------
// Bootloader main entry point
// ----------------------------------------------------------------------
int	main ( void )
{
	// disable watchdog if previously enabled
	MCUSR &= ~(1 << WDRF);
	wdt_disable();

	#ifdef ENABLE_BLANK_CHECK
	if (pgm_read_word(0) == 0xFFFF) {
		isBlank = 1;
	}
	else {
		isBlank = 0;
	}
	#endif

	MCUCR = (1 << IVCE);	// enable change of interrupt vectors
	MCUCR = (1 << IVSEL);	// move interrupts to boot flash section

	// start 16-bit timer1 for slow counting time
	TCCR1B = 0x05;

	LED_DDR |= _BV(LED); // LED pin on Trinket Pro

	// start USB and force a re-enumeration by faking a disconnect
	usbInit();
	usbDeviceDisconnect();
	LED_PORT |= _BV(LED);
	while (TCNT1 < 4000);
	LED_PORT &= ~_BV(LED);
	usbDeviceConnect();
	sei();

	TCCR1B = 0x01; // speed up timer for PWM LED pulsing
	uint8_t t1ovf = 0;
	uint16_t duty = 0;
	char dutyDir = 0;

	#ifdef ENABLE_OPTIBOOT
	optiboot_init();
	#endif

	// main program loop
	while (1)
	{

		#ifdef ENABLE_BOOTLOAD_BYPASS
		//Set as an input
		BOOTLOAD_BYPASS_DDR &= ~_BV(BOOTLOAD_BYPASS);
		
		//Enable soft pullup
		BOOTLOAD_BYPASS_PORT |= _BV(BOOTLOAD_BYPASS);
		
		//If the pin is high then lets bypass all this funny business
		if( BOOTLOAD_BYPASS_PIN &= _BV(BOOTLOAD_BYPASS)) {
			
			//Disable pullup
			BOOTLOAD_BYPASS_PORT &= ~_BV(BOOTLOAD_BYPASS);
			
			//Break to execute main code
			break;
		}
		#endif

		usbPoll();

		#ifdef ENABLE_OPTIBOOT
		char ob = optibootPoll();
		if (ob == 1) {
			timeout = 0;
		}
		else if (ob == 2
		#ifdef ENABLE_BLANK_CHECK
		&& isBlank == 0
		#endif
		) {
			break;
		}
		#endif

		if ( ((usbHasRxed != 0) && (timeout > USBBOOTLOADER_TIMEOUT)
		#ifdef ENABLE_BLANK_CHECK
		&& isBlank == 0
		#endif
		    ) || ((usbHasRxed == 0) && (timeout > UARTBOOTLOADER_TIMEOUT)
		#ifdef ENABLE_BLANK_CHECK
		&& isBlank == 0
		#endif
			  )
		#ifdef ENABLE_REQUEST_EXIT
		|| req_boot_exit != 0
		#endif
		
		     )    {
			// requested exit
			// or timed out waiting for activity (timeout means not connected to computer)
			break;
		}

		uint16_t t = TCNT1;
		if ((TIFR1 & _BV(TOV1)) != 0) // if timer has overflowed
		{
			TIFR1 |= _BV(TOV1); // clear the flag
			if (usbHasRxed != 0)
			{
				LED_PORT |= _BV(LED);
				if (duty == 0) {
					dutyDir = dutyDir ? 0 : 1;
				}

				#define WAITING_LED_FADE_RATE 512 // must be power of 2
				if (dutyDir == 0) {
					duty -= WAITING_LED_FADE_RATE;
				}
				else {
					duty += WAITING_LED_FADE_RATE;
				}
			}
			t1ovf++;

			// roughly 1 second
			#if (F_CPU == 12000000)
			if (t1ovf > 183) {
			#elif (F_CPU == 16000000)
			if (t1ovf > 244) {
			#endif
				t1ovf = 0;
				timeout++;
			}
		}

		if (usbHasRxed != 0)
		{
			// fade the LED
			if (t > duty) {
				LED_PORT &= ~_BV(LED);
			}
		}
		else
		{
			#define WAITING_LED_BLINK_RATE 16
			if  ((t1ovf > (WAITING_LED_BLINK_RATE * 0)
			  &&  t1ovf < (WAITING_LED_BLINK_RATE * 1))
			  || (t1ovf > (WAITING_LED_BLINK_RATE * 2)
			  &&  t1ovf < (WAITING_LED_BLINK_RATE * 3))
			  #if (F_CPU == 16000000)
			  || (t1ovf > (WAITING_LED_BLINK_RATE * 4)
			  &&  t1ovf < (WAITING_LED_BLINK_RATE * 5))
			  #endif
			  ) {
			  LED_PORT |= _BV(LED);
			}
			else {
			  LED_PORT &= ~_BV(LED);
			}
		}
	}

	// turn off and return port to normal
	LED_PORT &= ~_BV(LED);
	LED_DDR  &= ~_BV(LED);

	#if defined(ENABLE_REQUEST_EXIT) && defined(ENABLE_CLEAN_EXIT)
	// wait to finish all USB comms, avoids "avrdude: error: usbtiny_transmit: usb_control_msg: sending control message failed"
	TCCR1B = 0x05; // slow down timer
	TCNT1 = 0;
	while (req_boot_exit != 0 && TCNT1 < 4000) usbPoll();
	#endif

	// cleanup!

	// reset timer
	TCCR1B = 0;
	TCNT1 = 0;

	// deinitialize USB
	USB_INTR_ENABLE = 0;
	USB_INTR_CFG = 0;

	// move interrupt back
	MCUCR = (1 << IVCE);	// enable change of interrupt vectors
	MCUCR = (0 << IVSEL);	// move interrupts to app flash section

	cli();// disable interrupts

	app_start(); // jump to user app

	return 0;
}
