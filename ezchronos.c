/*
    Copyright (C) 2011 Angelo Arrifano <miknix@gmail.com>
      - Improve message display API, with timeout feature.
      - Merged with menu.c;
      - Full modularization of menu code;
      - Integration with build system.

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */
// *************************************************************************************************
//
//	Copyright (C) 2009 Texas Instruments Incorporated - http://www.ti.com/
//
//
//	  Redistribution and use in source and binary forms, with or without
//	  modification, are permitted provided that the following conditions
//	  are met:
//
//	    Redistributions of source code must retain the above copyright
//	    notice, this list of conditions and the following disclaimer.
//
//	    Redistributions in binary form must reproduce the above copyright
//	    notice, this list of conditions and the following disclaimer in the
//	    documentation and/or other materials provided with the
//	    distribution.
//
//	    Neither the name of Texas Instruments Incorporated nor the names of
//	    its contributors may be used to endorse or promote products derived
//	    from this software without specific prior written permission.
//
//	  THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
//	  "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
//	  LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
//	  A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
//	  OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
//	  SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
//	  LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
//	  DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
//	  THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
//	  (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
//	  OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
//
// *************************************************************************************************
// Initialization and control of application.
// *************************************************************************************************

// *************************************************************************************************
// Include section

#include <ezchronos.h>

#include "modinit.h"

/* Driver */
#include "display.h"
#include "vti_as.h"
#include "vti_ps.h"
#include "radio.h"
#include "buzzer.h"
#include "ports.h"
#include "timer.h"
#include "pmm.h"
#include "rf1a.h"
#include "rtca.h"

/* Simpliciti */
#include "mrfi.h"
#include "nwk_types.h"
#include <simpliciti.h>

/* Logic??? TODO: Try to remove remaining */
#include <temperature.h>
#include <rfsimpliciti.h>

/* System */
#include <stdlib.h>
#include <string.h>

// *************************************************************************************************
// Prototypes section
void init_application(void);
void init_global_variables(void);
void wakeup_event(void);
void process_requests(void);
void display_update(void);
void idle_loop(void);
void configure_ports(void);
void read_calibration_values(void);

// *************************************************************************************************
// Defines section

// Number of calibration data bytes in INFOA memory
#define CALIBRATION_DATA_LENGTH		(13u)


// *************************************************************************************************
// Global Variable section

// Variable holding system internal flags
volatile s_system_flags sys;

// Variable holding flags set by logic modules
volatile s_request_flags request;

// Variable holding message flags
volatile s_message_flags message;

// Global radio frequency offset taken from calibration memory
// Compensates crystal deviation from 26MHz nominal value
uint8_t rf_frequoffset;

// Function pointers for LINE1 and LINE2 display function
void (*fptr_lcd_function_line1)(uint8_t line, uint8_t update);
void (*fptr_lcd_function_line2)(uint8_t line, uint8_t update);

/* Menu definitions and declarations */
struct menu {
	/* Pointer to direct function (up/down buttons) */
	menu_item_fn_t sx_function;
	/* Pointer to sub menu function (long star/number buttons) */
	menu_item_fn_t mx_function;
	/* Pointer to display function */
	menu_disp_fn_t display_function;
	/* pointer to next menu item */
	struct menu *next;
};

static struct menu *menu_L1_head;
static struct menu *menu_L1;

static struct menu *menu_L2_head;
static struct menu *menu_L2;

// *************************************************************************************************
// Extern section
#ifdef CONFIG_ALTI_ACCUMULATOR
extern uint8_t alt_accum_enable;	// used by altitude accumulator function
#endif
extern void start_simpliciti_sync(void);

extern uint16_t ps_read_register(uint8_t address, uint8_t mode);
extern uint8_t ps_write_register(uint8_t address, uint8_t data);

// rf hardware address
static const addr_t   sMyROMAddress = {THIS_DEVICE_ADDRESS};

// *************************************************************************************************
// @fn          main
// @brief       Main routine
// @param       none
// @return      none
// *************************************************************************************************
int main(void)
{
	// Init MCU
	init_application();

	// Assign initial value to global variables
	init_global_variables();

#ifdef CONFIG_TEST
	// Branch to welcome screen
	test_mode();
#else
	display_all_off();
#endif

	// Main control loop: wait in low power mode until some event needs to be processed
	while (1) {
		// When idle go to LPM3
		idle_loop();

		// Process wake-up events
		if (button.all_flags || sys.all_flags) wakeup_event();

		// Process actions requested by logic modules
		if (request.all_flags) process_requests();

		// Before going to LPM3, update display
		if (display.all_flags) display_update();
	}
}


// *************************************************************************************************
// @fn          init_application
// @brief       Initialize the microcontroller.
// @param       none
// @return      none
// *************************************************************************************************
void init_application(void)
{
	volatile unsigned char *ptr;

	// ---------------------------------------------------------------------
	// Enable watchdog

	// Watchdog triggers after 16 seconds when not cleared
#ifdef USE_WATCHDOG
	WDTCTL = WDTPW + WDTIS__512K + WDTSSEL__ACLK;
#else
	WDTCTL = WDTPW + WDTHOLD;
#endif

	// ---------------------------------------------------------------------
	// Configure PMM
	SetVCore(3);

	// Set global high power request enable
	PMMCTL0_H  = 0xA5;
	PMMCTL0_L |= PMMHPMRE;
	PMMCTL0_H  = 0x00;

	// ---------------------------------------------------------------------
	// Enable 32kHz ACLK
	P5SEL |= 0x03;                            // Select XIN, XOUT on P5.0 and P5.1
	UCSCTL6 &= ~XT1OFF;        				  // XT1 On, Highest drive strength
	UCSCTL6 |= XCAP_3;                        // Internal load cap

	UCSCTL3 = SELA__XT1CLK;                   // Select XT1 as FLL reference
	UCSCTL4 = SELA__XT1CLK | SELS__DCOCLKDIV | SELM__DCOCLKDIV;

	// ---------------------------------------------------------------------
	// Configure CPU clock for 12MHz
	_BIS_SR(SCG0);                  // Disable the FLL control loop
	UCSCTL0 = 0x0000;          // Set lowest possible DCOx, MODx
	UCSCTL1 = DCORSEL_5;       // Select suitable range
	UCSCTL2 = FLLD_1 + 0x16E;  // Set DCO Multiplier
	_BIC_SR(SCG0);                  // Enable the FLL control loop

	// Worst-case settling time for the DCO when the DCO range bits have been
	// changed is n x 32 x 32 x f_MCLK / f_FLL_reference. See UCS chapter in 5xx
	// UG for optimization.
	// 32 x 32 x 8 MHz / 32,768 Hz = 250000 = MCLK cycles for DCO to settle
#if __GNUC_MINOR__ > 5 || __GNUC_PATCHLEVEL__ > 8
	__delay_cycles(250000);
#else
	__delay_cycles(62500);
        __delay_cycles(62500);
        __delay_cycles(62500);
        __delay_cycles(62500);
#endif
  
	// Loop until XT1 & DCO stabilizes, use do-while to insure that 
	// body is executed at least once
	do {
		UCSCTL7 &= ~(XT2OFFG + XT1LFOFFG + XT1HFOFFG + DCOFFG);
		SFRIFG1 &= ~OFIFG;                      // Clear fault flags
	} while ((SFRIFG1 & OFIFG));


	// ---------------------------------------------------------------------
	// Configure port mapping

	// Disable all interrupts
	__disable_interrupt();
	// Get write-access to port mapping registers:
	PMAPPWD = 0x02D52;
	// Allow reconfiguration during runtime:
	PMAPCTL = PMAPRECFG;

	// P2.7 = TA0CCR1A or TA1CCR0A output (buzzer output)
	ptr  = &P2MAP0;
	*(ptr + 7) = PM_TA1CCR0A;
	P2OUT &= ~BIT7;
	P2DIR |= BIT7;

	// P1.5 = SPI MISO input
	ptr  = &P1MAP0;
	*(ptr + 5) = PM_UCA0SOMI;
	// P1.6 = SPI MOSI output
	*(ptr + 6) = PM_UCA0SIMO;
	// P1.7 = SPI CLK output
	*(ptr + 7) = PM_UCA0CLK;

	// Disable write-access to port mapping registers:
	PMAPPWD = 0;
	// Re-enable all interrupts
	__enable_interrupt();

	// Init the hardwre real time clock (RTC_A)
	rtca_init();
#if (CONFIG_DST > 0)
	/* Initialize the DST. IMPORTANT: DST DEPENDS ON RTCA! */
	dst_init();
#endif
	// ---------------------------------------------------------------------
	// Configure ports

	// ---------------------------------------------------------------------
	// Reset radio core
	radio_reset();
	radio_powerdown();

#ifdef FEATURE_PROVIDE_ACCEL
	// ---------------------------------------------------------------------
	// Init acceleration sensor
	as_init();
#else
	as_disconnect();
#endif

	// ---------------------------------------------------------------------
	// Init LCD
	lcd_init();

	// ---------------------------------------------------------------------
	// Init buttons
	init_buttons();

	// ---------------------------------------------------------------------
	// Configure Timer0 for use by the clock and delay functions
	Timer0_Init();

	// ---------------------------------------------------------------------
	// Init pressure sensor
	ps_init();
}


// *************************************************************************************************
// @fn          init_global_variables
// @brief       Initialize global variables.
// @param       none
// @return      none
// *************************************************************************************************
void init_global_variables(void)
{
	// Init system flags
	button.all_flags 	= 0;
	sys.all_flags 		= 0;
	request.all_flags 	= 0;
	display.all_flags 	= 0;
	message.all_flags	= 0;

	// Force full display update when starting up
	display.flag.full_update = 1;

#ifndef ISM_US
	// Use metric units when displaying values
	sys.flag.use_metric_units = 1;
#else
#ifdef CONFIG_METRIC_ONLY
	sys.flag.use_metric_units = 1;
#endif
#endif

	// Read calibration values from info memory
	read_calibration_values();
	#ifdef CONFIG_INFOMEM

	if (infomem_ready() == -2) {
		infomem_init(INFOMEM_C, INFOMEM_C + 2 * INFOMEM_SEGMENT_SIZE);
	}

#endif

	// Set buzzer to default value
	reset_buzzer();

	// Reset SimpliciTI stack
	reset_rf();

	// Reset temperature measurement
	reset_temp_measurement();

	/* Init modules */
	mod_init();
}


// *************************************************************************************************
// @fn          wakeup_event
// @brief       Process external / internal wakeup events.
// @param       none
// @return      none
// *************************************************************************************************
void wakeup_event(void)
{
	// Enable idle timeout
	sys.flag.idle_timeout_enabled = 1;

	// If buttons are locked, only display "buttons are locked" message
	if (button.all_flags && sys.flag.lock_buttons) {
		// Show "buttons are locked" message synchronously with next second tick
		if (!((BUTTON_NUM_IS_PRESSED && BUTTON_DOWN_IS_PRESSED) || BUTTON_BACKLIGHT_IS_PRESSED)) {
			message.flag.prepare     = 1;
			message.flag.type_locked = 1;
		}

		// Clear buttons
		button.all_flags = 0;
	}
	// Process long button press event (while button is held)
	else if (button.flag.star_long) {
		// Clear button event
		button.flag.star_long = 0;

		// Call sub menu function
		if (menu_L1->mx_function)
			menu_L1->mx_function(LINE1);

		// Set display update flag
		display.flag.full_update = 1;
	} else if (button.flag.num_long) {
		// Clear button event
		button.flag.num_long = 0;

		// Call sub menu function
		if (menu_L2->mx_function)
			menu_L2->mx_function(LINE2);

		// Set display update flag
		display.flag.full_update = 1;
	}
	// Process single button press event (after button was released)
	else if (button.all_flags) {
		// M1 button event ---------------------------------------------------------------------
		// (Short) Advance to next menu item
		if (button.flag.star) {
			//skip to next menu item
			menu_L1_skip_next();

			// Set Line1 display update flag
			display.flag.line1_full_update = 1;

			// Clear button flag
			button.flag.star = 0;
		}
		// NUM button event ---------------------------------------------------------------------
		// (Short) Advance to next menu item
		else if (button.flag.num) {
			//skip to next menu item
			menu_L2_skip_next();

			// Set Line2 display update flag
			display.flag.line2_full_update = 1;

			// Clear button flag
			button.flag.num = 0;
		}
		// UP button event ---------------------------------------------------------------------
		// Activate user function for Line1 menu item
		else if (button.flag.up) {
			// Call direct function
			if (menu_L1->sx_function)
				menu_L1->sx_function(LINE1);

			// Set Line1 display update flag
			display.flag.line1_full_update = 1;

			// Clear button flag
			button.flag.up = 0;
		}
		// DOWN button event ---------------------------------------------------------------------
		// Activate user function for Line2 menu item
		else if (button.flag.down) {
			// Call direct function
			if (menu_L2->sx_function)
				menu_L2->sx_function(LINE2);

			// Set Line1 display update flag
			display.flag.line2_full_update = 1;

			// Clear button flag
			button.flag.down = 0;
		}
	}

	// Process internal events
	if (sys.all_flags) {
		// Idle timeout ---------------------------------------------------------------------
		if (sys.flag.idle_timeout) {
			// Clear timeout flag
			sys.flag.idle_timeout = 0;

			// Clear display
			clear_display();

			// Set display update flags
			display.flag.full_update = 1;
		}
	}

	// Disable idle timeout
	sys.flag.idle_timeout_enabled = 0;
}


// *************************************************************************************************
// @fn          process_requests
// @brief       Process requested actions outside ISR context.
// @param       none
// @return      none
// *************************************************************************************************
void process_requests(void)
{
	// Do temperature measurement
	if (request.flag.temperature_measurement) temperature_measurement(FILTER_ON);

	// Do pressure measurement
#ifdef CONFIG_ALTITUDE

	if (request.flag.altitude_measurement) do_altitude_measurement(FILTER_ON);

#endif
#ifdef CONFIG_ALTI_ACCUMULATOR

	if (request.flag.altitude_accumulator) altitude_accumulator_periodic();

#endif

#ifdef FEATURE_PROVIDE_ACCEL

	// Do acceleration measurement
	if (request.flag.acceleration_measurement) do_acceleration_measurement();

#endif

#ifdef CONFIG_BATTERY

	// Do voltage measurement
	if (request.flag.voltage_measurement) battery_measurement();

#endif

#ifdef CONFIG_ALARM

	// Generate alarm (two signals every second)
	if (request.flag.alarm_buzzer) start_buzzer(2, BUZZER_ON_TICKS, BUZZER_OFF_TICKS);

#endif

#ifdef CONFIG_EGGTIMER

	// Generate alarm (two signals every second)
	if (request.flag.eggtimer_buzzer) start_buzzer(2, BUZZER_ON_TICKS, BUZZER_OFF_TICKS);

#endif


#ifdef CONFIG_STRENGTH

	if (request.flag.strength_buzzer && strength_data.num_beeps != 0) {
		start_buzzer(strength_data.num_beeps,
			     STRENGTH_BUZZER_ON_TICKS,
			     STRENGTH_BUZZER_OFF_TICKS);
		strength_data.num_beeps = 0;
	}

#endif

	// Reset request flag
	request.all_flags = 0;
}


// *************************************************************************************************
// @fn          display_update
// @brief       Process display flags and call LCD update routines.
// @param       none
// @return      none
// *************************************************************************************************
void display_update(void)
{
	uint8_t string[8];

	// ---------------------------------------------------------------------
	// Call Line1 display function
	if (fptr_lcd_function_line1) {
		if (display.flag.full_update || display.flag.line1_full_update) {
			clear_line(LINE1);
			fptr_lcd_function_line1(LINE1, DISPLAY_LINE_UPDATE_FULL);
		} else if (!message.flag.block_line1) {
			fptr_lcd_function_line1(LINE1, DISPLAY_LINE_UPDATE_PARTIAL);
		}
	}

	// ---------------------------------------------------------------------
	// Call Line2 display function
	if (fptr_lcd_function_line2) {
		if (display.flag.full_update || display.flag.line2_full_update) {
			clear_line(LINE2);
			fptr_lcd_function_line2(LINE2, DISPLAY_LINE_UPDATE_FULL);
		} else if (!message.flag.block_line2) {
			fptr_lcd_function_line2(LINE2, DISPLAY_LINE_UPDATE_PARTIAL);
		}
	}

	// ---------------------------------------------------------------------
	// If message text should be displayed
	if (message.flag.show) {
		// Select message to display (system only)
		if (message.flag.type_locked)			memcpy(string, "  LOCT", 6);
		else if (message.flag.type_unlocked)	memcpy(string, "  OPEN", 6);
		else if (message.flag.type_lobatt)		memcpy(string, "LOBATT", 6);
		else if (message.flag.type_no_beep_on)  memcpy(string, " SILNT", 6);
		else if (message.flag.type_no_beep_off) memcpy(string, "  BEEP", 6);

		// Clear previous content
		if (message.flag.line1) {
			clear_line(LINE1);
			fptr_lcd_function_line1(LINE1, DISPLAY_LINE_CLEAR);
		} else {
			clear_line(LINE2);
			fptr_lcd_function_line2(LINE2, DISPLAY_LINE_CLEAR);
		}

		// modular applications should set prepare=1, and user = 1 and chose the line with
		// line1 (1 for LINE1, 0 for LINE2) and then handle their display function
		// with a DISPLAY_LINE_MESSAGE update.
		if (message.flag.user) {
			if (message.flag.line1)
				fptr_lcd_function_line1(LINE1, DISPLAY_LINE_MESSAGE);
			else
				fptr_lcd_function_line2(LINE2, DISPLAY_LINE_MESSAGE);
		} else {
			if (message.flag.line1)
				display_chars(LCD_SEG_L1_3_0, string, SEG_ON);
			else
				display_chars(LCD_SEG_L2_5_0, string, SEG_ON);
		}

		uint8_t timeout = message.flag.timeout;
		message.all_flags = 0;

		if (message.flag.line1)
			message.flag.block_line1 = 1;
		else
			message.flag.block_line2 = 1;

		message.flag.timeout = timeout;
	}

	// Clear display flag
	display.all_flags = 0;
}


// *************************************************************************************************
// @fn          to_lpm
// @brief       Go to LPM0/3.
// @param       none
// @return      none
// *************************************************************************************************
void to_lpm(void)
{
	// Go to LPM3
	_BIS_SR(LPM3_bits + GIE);
	__no_operation();
}


// *************************************************************************************************
// @fn          idle_loop
// @brief       Go to LPM. Service watchdog timer when waking up.
// @param       none
// @return      none
// *************************************************************************************************
void idle_loop(void)
{
#ifdef CONFIG_CW_TIME
	// what I'd like to do here is set a morsepos variable
	// if non-zero it is how many digits we have left to go
	// on sending the time.
	// we also would have a morse var that would only get set
	// the first send and reset when not in view so we'd only
	// send the time once

#define CW_DIT_LEN CONV_MS_TO_TICKS(50)    // basic element size (100mS)

	static int morse = 0;     // should send morse == 1
	static int morsepos = 0; // position in morse time (10 hour =1, hour=2, etc.)
	static uint8_t morsehr; // cached hour for morse code
	static uint8_t morsemin;  // cached minute for morse code
	static uint8_t morsesec;
	static int morsedig = -1; // current digit
	static int morseel; // current element in digit (always 5 elements max)
	static unsigned int morseinitdelay; // start up delay

	// We only send the time in morse code if the seconds display is active, and then only
	// once per activation

	if (sTime.line1ViewStyle == DISPLAY_ALTERNATIVE_VIEW) {
		if (!morse) { // this means its the first time (we reset this to zero in the else)

			morse = 1; // mark that we are sending
			morsepos = 1; // initialize pointer

			rtca_get_time(&morsehr, &morsemin, &morsesec);

			morsedig = -1; // not currently sending digit
			morseinitdelay = 45000; // delay for a bit before starting so the key beep can quiet down

		}

		if (morseinitdelay) { // this handles the initial delay
			morseinitdelay--;
			return;  // do not sleep yet or no event will get scheduled and we'll hang for a very long time
		}

		if (!is_buzzer() && morsedig == -1) { // if not sending anything

			morseel = 0;                   // start a new character

			switch (morsepos++) {          // get the right digit
			case 1:
				morsedig = morsehr / 10;
				break;

			case 2:
				morsedig = morsehr % 10;
				break;

			case 3:
				morsedig = morsemin / 10;
				break;

			case 4:
				morsedig = morsemin % 10;
				break;

			default:
				morsepos = 5; // done for now
			}

			if (morsedig == 0)
				morsedig = 10; // treat zero as 10 for code algorithm
		}

		// now we have a digit and we need to send element
		if (!is_buzzer() && morsedig != -1) {

			int digit = morsedig;
			// assume we are sending dit for 1-5 or dah for 6-10 (zero is 10)
			int ditdah = (morsedig > 5) ? 1 : 0;
			int dit = CW_DIT_LEN;

			if (digit >= 6)
				digit -= 5; // fold digits 6-10 to 1-5

			if (digit >= ++morseel)
				ditdah = ditdah ? 0 : 1; // flip dits and dahs at the right point

			// send the code
			start_buzzer(1, ditdah ? dit : (3 * dit), (morseel >= 5) ? 10 * dit : dit);

			// all digits have 5 elements
			if (morseel == 5)
				morsedig = -1;

		}

	} else {
		morse = 0; // no morse code right now
	}

#endif
	// To low power mode
	to_lpm();

#ifdef USE_WATCHDOG
	// Service watchdog (reset counter)
	WDTCTL = (WDTCTL & 0xff) | WDTPW | WDTCNTCL;
#endif
}


// *************************************************************************************************
// @fn          read_calibration_values
// @brief       Read calibration values for temperature measurement, voltage measurement
//				and radio from INFO memory.
// @param       none
// @return      none
// *************************************************************************************************
void read_calibration_values(void)
{
	uint8_t cal_data[CALIBRATION_DATA_LENGTH];		// Temporary storage for constants
	uint8_t i;
	uint8_t *flash_mem;         					// Memory pointer

	// Read calibration data from Info D memory
	flash_mem = (uint8_t *)0x1800;

	for (i = 0; i < CALIBRATION_DATA_LENGTH; i++) {
		cal_data[i] = *flash_mem++;
	}

	if (cal_data[0] == 0xFF) {
		// If no values are available (i.e. INFO D memory has been erased by user), assign experimentally derived values
		rf_frequoffset	= 4;
		sTemp.offset 	= -250;
#ifdef CONFIG_BATTERY
		sBatt.offset 	= -10;
#endif
		simpliciti_ed_address[0] = sMyROMAddress.addr[0];
		simpliciti_ed_address[1] = sMyROMAddress.addr[1];
		simpliciti_ed_address[2] = sMyROMAddress.addr[2];
		simpliciti_ed_address[3] = sMyROMAddress.addr[3];
#ifdef CONFIG_ALTITUDE
		sAlt.altitude_offset	 = 0;
#endif
	} else {
		// Assign calibration data to global variables
		rf_frequoffset	= cal_data[1];

		// Range check for calibrated FREQEST value (-20 .. + 20 is ok, else use default value)
		if ((rf_frequoffset > 20) && (rf_frequoffset < (256 - 20))) {
			rf_frequoffset = 0;
		}

		sTemp.offset 	= (int16_t)((cal_data[2] << 8) + cal_data[3]);
#ifdef CONFIG_BATTERY
		sBatt.offset 	= (int16_t)((cal_data[4] << 8) + cal_data[5]);
#endif
		simpliciti_ed_address[0] = cal_data[6];
		simpliciti_ed_address[1] = cal_data[7];
		simpliciti_ed_address[2] = cal_data[8];
		simpliciti_ed_address[3] = cal_data[9];
		// S/W version byte set during calibration?
#ifdef CONFIG_ALTITUDE

		if (cal_data[12] != 0xFF) {
			sAlt.altitude_offset = (int16_t)((cal_data[10] << 8) + cal_data[11]);;
		} else {
			sAlt.altitude_offset = 0;
		}

#endif
	}
}

void menu_add_entry(uint8_t const line,
             menu_item_fn_t const sx_fun,
             menu_item_fn_t const mx_fun,
             menu_disp_fn_t const disp_fun)
{
	struct menu **menu_hd = (line == LINE1? &menu_L1_head : &menu_L2_head);
	struct menu *menu_p;

	if (! *menu_hd) {
		/* Head is empty, create new menu item linked to itself */
		menu_p = (struct menu *) malloc(sizeof(struct menu));
		menu_p->next = menu_p;
		*menu_hd = menu_p;
		if (line == LINE1) {
			menu_L1 = menu_p;
			fptr_lcd_function_line1 = disp_fun;
		} else {
			menu_L2 = menu_p;
			fptr_lcd_function_line2 = disp_fun;
		}
	} else {
		/* insert new item after the head */
		menu_p = (struct menu *) malloc(sizeof(struct menu));
		menu_p->next = (*menu_hd)->next;
		(*menu_hd)->next = menu_p;
	}
	menu_p->sx_function = sx_fun;
	menu_p->mx_function = mx_fun;
	menu_p->display_function = disp_fun;
}

void menu_L1_skip_next(void)
{
	if (! menu_L1)
		return;

	/* clean up display before activating next menu item */
	fptr_lcd_function_line1(LINE1, DISPLAY_LINE_CLEAR);

	menu_L1 = menu_L1->next;

	/* Assign new display function */
	fptr_lcd_function_line1 = menu_L1->display_function;
}

void menu_L2_skip_next(void)
{
	if (! menu_L2)
		return;

	/* Clean up display before activating next menu item */
	fptr_lcd_function_line2(LINE2, DISPLAY_LINE_CLEAR);

	menu_L2 = menu_L2->next;

	/* Assign new display function */
	fptr_lcd_function_line2 = menu_L2->display_function;
}

