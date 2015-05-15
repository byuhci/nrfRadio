/*
 * interrupts.c
 *
 *  Created on: Apr 30, 2015
 *      Author: bsnga
 */

#include <msp430.h>
#include "interrupts.h"
#include "events.h"
#include "nrf24api.h"
#include "stdint.h"

static volatile uint32_t WDT_Sec_Cnt = WDT_CPS;
static volatile uint32_t toggle_led_cnt = 0;
static uint32_t WDT_PWM_cnt = WDT_CPS / 16; // default 1/16 second interval. DO NOT DEFAULT TO ZERO.
static const uint32_t WDT_PWM_max = WDT_CPS; // max 1 second interval

volatile uint16_t data_sender = DATA_DELAY;
volatile uint16_t counter = TIMEOUT;
volatile uint16_t tics = 0;
volatile uint16_t delay_cnt = 0;

uint32_t interrupts_set_WDT_interval(uint32_t interval) {
	if (interval >= WDT_PWM_max)
		WDT_PWM_cnt = 1; // reset to very low value (will toggle quickly)
	else
		WDT_PWM_cnt = interval;
	return WDT_PWM_cnt;
}

void interrupts_start_WDT_PWM() {
	toggle_led_cnt = WDT_PWM_cnt;
}

void interrupts_stop_WDT_PWM() {
	toggle_led_cnt = 0;
}

void interrupts_WDT_init() {
	// configure Watchdog

	tics = 0;
	WDTCTL = WDT_CTL;					// Set Watchdog interval
	WDT_Sec_Cnt = WDT_CPS;			// set WD 1 second counter
	IE1 |= WDTIE;						// enable WDT interrupt
	_enable_interrupts(); // enable global interrupts
	return;
}

void interrupts_timerA_init() {
	//crystal init
	BCSCTL1 |= DIVA0 | DIVA1;	// Set ACLK divider to 8
	BCSCTL3 |= XCAP0 | XCAP1;	// Set crystal capacitors to 12.5 pF
	P2DIR &= ~BIT6;		// Set P2.6/7 to XIN/XOUT
	P2DIR |= BIT7;
	P2SEL |= BIT6 | BIT7;
	P2SEL2 &= ~(BIT6 | BIT7);
	while (BCSCTL3 & LFXT1OF)
		;		// wait for crystal to stabilize

	//initialize timerA
	P1OUT &= ~(GLED | RLED);
	TACCR0 = 0;
	TACTL = TASSEL_1 | ID_1 | MC_1;
	TACCTL0 |= CCIE;
	TACCR0 = 4;
}

//----------------------------------------------------------------------
void delay(uint16_t time) {
	delay_cnt = time;
	LPM3;
}

//---------------------------------------------------------------------
void set_timeout() {
	counter = DELAY;
}

void reset_timeout() {
	counter = 0;
}

//-- Watchdog Timer ISR ---------------------------------------------
//
#pragma vector = WDT_VECTOR
__interrupt void WDT_ISR(void) {
	// one second event --------------------------------
	if (--WDT_Sec_Cnt == 0) {
		WDT_Sec_Cnt = WDT_CPS;
		tics++;
	}

#if NOTEST
	if (--counter == DELAY / 2)
	reset_connected();
	else if (counter == 0) {
		counter = DELAY;
		sys_event |= PING_EVENT;
	}
#endif

#if PTX_DEV
	if (--data_sender == 0) {
		data_sender = DATA_DELAY;
		sys_event |= SPI_TX_EVENT;
	}
#endif

	if (delay_cnt && !(--delay_cnt)) {
		__bic_SR_register_on_exit(LPM3_bits);
	}

	if (sys_event)
		__bic_SR_register_on_exit(LPM4_bits);
}

// Timer A1 interrupt service routine
//
#pragma vector = TIMER0_A1_VECTOR
__interrupt void TIMER0A1_ISR(void) {
	TACCTL1 &= ~CCIFG;
	if (timestep_count && --timestep_count == 0) {
		sys_event |= NRF_TIMESTEP;
	}
	return;
} // end TIMERA0_VECTOR
// Timer A1 interrupt service routine
//
#pragma vector = TIMER0_A0_VECTOR
__interrupt void TIMER0A0_ISR(void) {
	if (timestep_count && --timestep_count == 0) {
		sys_event |= NRF_TIMESTEP;
	}
	return;
} // end TIMERA0_VECTOR

