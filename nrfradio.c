/*
 * nrfradio.c
 *
 *  Created on: May 18, 2015
 *      Author: Broderick Gardner
 *      based heavily on code by Spirilis
 *
 */

#include <msp430.h>
#include "nrfradio.h"
#include "io_buffer.h"
#include "msp430_spi.h"

/* Configuration parameters used to set-up the RF configuration */
uint8_t rf_crc;
uint8_t rf_addr_width;
uint8_t rf_speed_power;
uint8_t rf_channel;
/* Status variable updated every time SPI I/O is performed */
uint8_t rf_status;
uint8_t rf_feature;  // Used to track which features have been enabled
/* IRQ state is stored in here after msprf24_get_irq_reason(), RF24_IRQ_FLAGGED raised during
 * the IRQ port ISR--user application issuing LPMx sleep or polling should watch for this to
 * determine if the wakeup reason was due to nRF24 IRQ.
 */
volatile uint8_t rf_irq;

// private variables
static IO_buffer* rx_pipe_buffers[6] = { 0 };
static void (*callbacks[6])(void) = {0 };
static IO_buffer* tx_pipe_buffers[6] = { 0 };

static const uint8_t pipe_addr[6][5] = { { 0xDE, 0xAD, 0xBE, 0xEF, 0x00 }, {
		0xFA, 0xDE, 0xDA, 0xCE, 0x51 }, { 0xFA, 0xDE, 0xDA, 0xCE, 0x52 }, {
		0xFA, 0xDE, 0xDA, 0xCE, 0x53 }, { 0xFA, 0xDE, 0xDA, 0xCE, 0x54 }, {
		0xFA, 0xDE, 0xDA, 0xCE, 0x55 } };

// Initialize radio, pass NULL for default values:
// frequency=		speed=
int radio_init(int freq, int speed) {
	const uint8_t speeds[3] = { RF24_SPEED_250KBPS, RF24_SPEED_1MBPS,
	RF24_SPEED_2MBPS };

	if (freq)
		rf_channel = freq;
	else
		rf_channel = 120;

	rf_speed_power = speeds[speed] | RF24_POWER_0DBM;

	/* Initial values for nRF24L01+ library config variables */
	rf_crc = RF24_EN_CRC | RF24_CRCO; // CRC enabled, 16-bit
	rf_addr_width = 5;

	port_init();
	// Wait 100ms for RF transceiver to initialize.
	uint8_t c = 20;
	for (; c; c--) {
		__delay_cycles(DELAY_CYCLES_5MS);
	}

	// Configure RF transceiver with current value of rf_* configuration variables
	msprf24_close_pipe_all(); /* Start off with no pipes enabled, let the user open as needed.  This also
	 * clears the DYNPD register.
	 */
	msprf24_irq_clear(RF24_IRQ_MASK);  // Forget any outstanding IRQs
	msprf24_set_retransmit_delay(300);  // A default I chose
	msprf24_set_retransmit_count(8);    // A default I chose
	msprf24_set_speed_power();
	msprf24_set_channel();
	msprf24_set_address_width();
	rf_feature = 0x00;  // Initialize this so we're starting from a clean slate
	msprf24_enable_feature(RF24_EN_DPL); // Dynamic payload size capability (set with msprf24_set_pipe_packetsize(x, 0))
	msprf24_enable_feature(RF24_EN_DYN_ACK); // Ability to use w_tx_payload_noack()

	// the following line enables ack payloads
	//msprf24_enable_feature(RF24_EN_ACK_PAY);

	msprf24_powerdown();
	flush_tx();
	flush_rx();

	while (!msprf24_is_alive())
		;

	int i;
	for (i = 0; i < 6; i++) {
		write_rx_address(i, pipe_addr[i]);
		rx_pipe_buffers[i] = 0;
		tx_pipe_buffers[i] = 0;
	}

	return 1;
}

// ... open given pipe
int radio_open_pipe(int pipe, pipe_mode mode) {

	msprf24_standby();
	msprf24_set_pipe_packetsize(pipe, 0);

	uint8_t rxen, enaa;

	if (pipe > 5)
		return 0;

	rxen = r_reg(RF24_EN_RXADDR);
	enaa = r_reg(RF24_EN_AA);

	enaa |= (1 << pipe);
	rxen |= (1 << pipe);
	w_reg(RF24_EN_RXADDR, rxen);
	w_reg(RF24_EN_AA, enaa);

	if (mode == RCV) {
		rx_pipe_buffers[pipe] = create_buffer(6);
	} else {
		tx_pipe_buffers[pipe] = create_buffer(6);
	}
	recieve_mode();
	return 1;

}

// close given pipe
void radio_close_pipe(int pipe) {
	uint8_t rxen, enaa;

	if (pipe > 5)
		return;

	rxen = r_reg(RF24_EN_RXADDR);
	enaa = r_reg(RF24_EN_AA);

	rxen &= ~(1 << pipe);
	enaa &= ~(1 << pipe);

	w_reg(RF24_EN_RXADDR, rxen);
	w_reg(RF24_EN_AA, enaa);

	if (rx_pipe_buffers[pipe])
		destroy_buffer(rx_pipe_buffers[pipe]);
	rx_pipe_buffers[pipe] = 0;
	if (tx_pipe_buffers[pipe])
		destroy_buffer(tx_pipe_buffers[pipe]);
	tx_pipe_buffers[pipe] = 0;
}

// Non blocking listen, callback function called upon recieve on given pipe
int radio_listen(int pipe, void (*func)(void)) {
	callbacks[pipe] = func;
	return 1;
}

// Blocking get_char, returns char sent by given pipe
int radio_get_char(int pipe, char* data) {

	if (pipe > 5)
		return 0;

	while (!(rx_pipe_buffers[pipe]->count)) {

		while (!(rf_irq && RF24_IRQ_FLAGGED)) {
		}
		msprf24_get_irq_reason();
		while (rf_irq & RF24_IRQ_RX) {
			read_payload();
			msprf24_irq_clear(RF24_IRQ_RX);
		}

	}
	io_get_char(rx_pipe_buffers[pipe], (uint8_t*) data);
	return 1;
}

// Conditional get_char returns 0 if no data available from given pipe
int radio_cget_char(int pipe, char* data) {

	if (pipe > 5)
		return 0;

	if (rf_irq & RF24_IRQ_FLAGGED) {
		msprf24_get_irq_reason();
		while (rf_irq & RF24_IRQ_RX) {
			read_payload();
			msprf24_irq_clear(RF24_IRQ_RX);
		}

	}
	if (data && rx_pipe_buffers[pipe]->count) {
		io_get_char(rx_pipe_buffers[pipe], (uint8_t*) data);
		return 1;
	} else {
		return 0;
	}
}

// Read from RX FIFO, based on Spirilis' API function r_rx_payload()
uint8_t read_payload() {
	uint16_t i = 0;
	uint8_t pipe;
	uint8_t len = r_rx_peek_payload_size();

	CSN_EN;
	rf_status = spi_transfer(RF24_R_RX_PAYLOAD);
	pipe = (rf_status & 0x0E) >> 1;

	for (i = len - 1; i; i--)
		io_put_char(rx_pipe_buffers[pipe], spi_transfer(0xFF));

	CSN_DIS;
	// The RX pipe this data belongs to is stored in STATUS
	return pipe;
}

// Put char on buffer to transmit to given pipe
int radio_place_char(int pipe, char data) {
	io_put_char(tx_pipe_buffers[pipe], data);
	if (tx_pipe_buffers[pipe]->count >= 32) {
		return send_payload(pipe);
	}
	return 1;
}

// put char on buffer and flush
int radio_put_chars(int pipe, char* data, uint16_t size) {

	uint16_t i;
	for (i = 0; i < size; i++) {
		io_put_char(tx_pipe_buffers[pipe], data[i]);
	}
	return radio_flush_pipe(pipe);
}

// returns 0 on failed send
uint8_t send_payload(int pipe) {
	uint16_t i;
	uint8_t size;
	uint8_t data;

	if (!tx_pipe_buffers[pipe]->count)
		return 0;

	msprf24_standby();
	// Read any standing recieve requests
	if (rf_irq & RF24_IRQ_FLAGGED) {
		msprf24_get_irq_reason();
		while (rf_irq & RF24_IRQ_RX) {
			read_payload();
			msprf24_irq_clear(RF24_IRQ_RX);
		}
	}

	size = tx_pipe_buffers[pipe]->count;
	size = (size >= 32) ? 32 : size;

	write_tx_address(pipe_addr[pipe]);
	write_rx_address(0, pipe_addr[pipe]);

	CSN_EN;
	rf_status = spi_transfer(RF24_W_TX_PAYLOAD);
	for (i = 0; i < size; i++) {
		io_get_char(tx_pipe_buffers[pipe], &data);
		spi_transfer(data);
	}

	CSN_DIS;

	// Cancel any outstanding TX interrupt
	w_reg(RF24_STATUS, RF24_TX_DS | RF24_MAX_RT);

	// Pulse CE for 10us to activate PTX
	pulse_ce();

	while (1) {
		while (!(rf_irq & RF24_IRQ_FLAGGED)) {
		}
		msprf24_get_irq_reason();
		if (rf_irq & (RF24_IRQ_TX | RF24_IRQ_TXFAILED)) {
			break;
		}
	}

	write_rx_address(0, pipe_addr[0]);
	recieve_mode();

	if (rf_irq & RF24_IRQ_TX) {
		msprf24_irq_clear(RF24_IRQ_TX);
		return 1;
	} else if (rf_irq & RF24_IRQ_TXFAILED) {
		flush_tx();
		msprf24_irq_clear(RF24_IRQ_TXFAILED);
		return 0;
	}
	return 0;
}

// Flush the given pipe (force send any data in the buffer)
int radio_flush_pipe(int pipe) {
	int success;
	while (tx_pipe_buffers[pipe]->count) {
		success = send_payload(pipe);
	}
	return success;
}

void recieve_mode() {
	msprf24_standby();
	w_reg(RF24_STATUS, RF24_RX_DR);

	// Enable PRIM_RX
	msprf24_set_config(RF24_PWR_UP | RF24_PRIM_RX);
	CE_EN;
}

void msprf24_close_pipe_all() {
	w_reg(RF24_EN_RXADDR, 0x00);
	w_reg(RF24_EN_AA, 0x00);
	w_reg(RF24_DYNPD, 0x00);
}

// Set the rx pipe address, sending to the radio
void write_rx_address(uint8_t pipe, const uint8_t *addr) {
	int i;

	if (pipe > 5)
		return;  // Only 6 pipes available
	CSN_EN;
	rf_status = spi_transfer((RF24_RX_ADDR_P0 + pipe) | RF24_W_REGISTER);
	if (pipe > 1) {  // Pipes 2-5 differ from pipe1's addr only in the LSB.
		spi_transfer(addr[rf_addr_width - 1]);
	} else {
		for (i = rf_addr_width - 1; i >= 0; i--) {
			spi_transfer(addr[i]);
		}
	}
	CSN_DIS;
}

// Set the tx pipe address, used to burst send
void write_tx_address(const uint8_t *addr) {
	int i;

	CSN_EN;
	rf_status = spi_transfer(RF24_TX_ADDR | RF24_W_REGISTER);
	for (i = rf_addr_width - 1; i >= 0; i--) {
		spi_transfer(addr[i]);
	}
	CSN_DIS;
}

/* Clear IRQ flags */
void msprf24_irq_clear(uint8_t irqflag) {
	uint8_t fifostat;

	rf_irq = 0x00; // Clear IRQs; afterward analyze RX FIFO to see if we should re-set RX IRQ flag.
	CSN_EN;
	rf_status = spi_transfer(RF24_STATUS | RF24_W_REGISTER);
	spi_transfer(irqflag);
	CSN_DIS;

// Per datasheet procedure, check FIFO_STATUS to see if there's more RX FIFO data to process.
	if (irqflag & RF24_IRQ_RX) {
		CSN_EN;
		rf_status = spi_transfer(RF24_FIFO_STATUS | RF24_R_REGISTER);
		fifostat = spi_transfer(RF24_NOP);
		CSN_DIS;
		if (!(fifostat & RF24_RX_EMPTY))
			rf_irq |= RF24_IRQ_RX | RF24_IRQ_FLAGGED; // Signal to user that there is remaining data, even if it's not "new"
	}
}

// Get IRQ flag status
uint8_t msprf24_get_irq_reason() {
	uint8_t rf_irq_old = rf_irq;

	//rf_irq &= ~RF24_IRQ_FLAGGED;  -- Removing in lieu of having this check determined at irq_clear() time
	CSN_EN;
	rf_status = spi_transfer(RF24_NOP);
	CSN_DIS;
	rf_irq = (rf_status & RF24_IRQ_MASK) | rf_irq_old;
	return rf_irq;
}

/* Evaluate state of TX, RX FIFOs
 * Compare this with RF24_QUEUE_* #define's from msprf24.h
 */
uint8_t msprf24_queue_state() {
	return r_reg(RF24_FIFO_STATUS);
}

void msprf24_set_pipe_packetsize(uint8_t pipe, uint8_t size) {
	uint8_t dynpdcfg;

	if (pipe > 5)
		return;

	dynpdcfg = r_reg(RF24_DYNPD);
	if (size < 1) {
		if (!(rf_feature & RF24_EN_DPL)) // Cannot set dynamic payload if EN_DPL is disabled.
			return;
		if (!((1 << pipe) & dynpdcfg)) {
			// DYNPD not enabled for this pipe, enable it
			dynpdcfg |= 1 << pipe;
		}
	} else {
		dynpdcfg &= ~(1 << pipe);  // Ensure DynPD is disabled for this pipe
		if (size > 32)
			size = 32;
		w_reg(RF24_RX_PW_P0 + pipe, size);
	}
	w_reg(RF24_DYNPD, dynpdcfg);
}

void msprf24_set_retransmit_delay(uint16_t us) {
	uint8_t c;

// using 'c' to evaluate current RF speed
	c = rf_speed_power & RF24_SPEED_MASK;
	if (us > 4000)
		us = 4000;
	if (us < 1500 && c == RF24_SPEED_250KBPS)
		us = 1500;
	if (us < 500)
		us = 500;

// using 'c' to save current value of ARC (auto-retrans-count) since we're not changing that here
	c = r_reg(RF24_SETUP_RETR) & 0x0F;
	us = (us - 250) / 250;
	us <<= 4;
	w_reg(RF24_SETUP_RETR, c | (us & 0xF0));
}

// Power down device, 0.9uA power draw
void msprf24_powerdown() {
	CE_DIS;
	msprf24_set_config(0);  // PWR_UP=0
}

// Enable Standby-I, 26uA power draw
void msprf24_standby() {
	uint8_t state = msprf24_current_state();
	if (state == RF24_STATE_NOTPRESENT || state == RF24_STATE_STANDBY_I)
		return;
	CE_DIS;
	msprf24_set_config(RF24_PWR_UP);  // PWR_UP=1, PRIM_RX=0
	if (state == RF24_STATE_POWERDOWN) { // If we're powering up from deep powerdown...
		//CE_EN;  // This is a workaround for SI24R1 chips, though it seems to screw things up so disabled for now til I can obtain an SI24R1 for testing.
		__delay_cycles(DELAY_CYCLES_5MS); // Then wait 5ms for the crystal oscillator to spin up.
		//CE_DIS;
	}
}

uint8_t r_reg(uint8_t addr) {
	uint16_t i;

	CSN_EN;
	i = spi_transfer16(RF24_NOP | ((addr & RF24_REGISTER_MASK) << 8));
	rf_status = (uint8_t) ((i & 0xFF00) >> 8);
	CSN_DIS;
	return (uint8_t) (i & 0x00FF);
}

void w_reg(uint8_t addr, uint8_t data) {
	uint16_t i;
	CSN_EN;
	i = spi_transfer16(
			(data & 0x00FF)
					| (((addr & RF24_REGISTER_MASK) | RF24_W_REGISTER) << 8));
	rf_status = (uint8_t) ((i & 0xFF00) >> 8);
	CSN_DIS;
}

inline uint8_t _msprf24_crc_mask() {
	return (rf_crc & 0x0C);
}

inline uint8_t _msprf24_irq_mask() {
	return ~(RF24_MASK_RX_DR | RF24_MASK_TX_DS | RF24_MASK_MAX_RT);
}

void msprf24_set_retransmit_count(uint8_t count) {
	uint8_t c;

	c = r_reg(RF24_SETUP_RETR) & 0xF0;
	w_reg(RF24_SETUP_RETR, c | (count & 0x0F));
}

uint8_t msprf24_get_last_retransmits() {
	return r_reg(RF24_OBSERVE_TX) & 0x0F;
}

uint8_t msprf24_get_lostpackets() {
	return (r_reg(RF24_OBSERVE_TX) >> 4) & 0x0F;
}

void msprf24_set_address_width() {
	if (rf_addr_width < 3 || rf_addr_width > 5)
		return;
	w_reg(RF24_SETUP_AW, ((rf_addr_width - 2) & 0x03));
}

void msprf24_set_channel() {
	if (rf_channel > 125)
		rf_channel = 0;
	w_reg(RF24_RF_CH, (rf_channel & 0x7F));
}

void msprf24_set_speed_power() {
	if ((rf_speed_power & RF24_SPEED_MASK) == RF24_SPEED_MASK) // Speed setting RF_DR_LOW=1, RF_DR_HIGH=1 is reserved, clamp it to minimum
		rf_speed_power = (rf_speed_power & ~RF24_SPEED_MASK) | RF24_SPEED_MIN;
	w_reg(RF24_RF_SETUP, (rf_speed_power & 0x2F));
}

void msprf24_enable_feature(uint8_t feature) {
	if ((rf_feature & feature) != feature) {
		rf_feature |= feature;
		rf_feature &= 0x07;  // Only bits 0, 1, 2 allowed to be set
		w_reg(RF24_FEATURE, rf_feature);
	}
}

void msprf24_disable_feature(uint8_t feature) {
	if ((rf_feature & feature) == feature) {
		rf_feature &= ~feature;
		w_reg(RF24_FEATURE, rf_feature);
	}
}

uint8_t msprf24_set_config(uint8_t cfgval) {
	uint8_t previous_config;

	previous_config = r_reg(RF24_CONFIG);
	w_reg(RF24_CONFIG, (_msprf24_crc_mask() | cfgval) & _msprf24_irq_mask());
	return previous_config;
}

uint8_t msprf24_is_alive() {
	uint8_t aw;

	aw = r_reg(RF24_SETUP_AW);
	return ((aw & 0xFC) == 0x00 && (aw & 0x03) != 0x00);
}

uint8_t msprf24_current_state() {
	uint8_t config;

	if (!msprf24_is_alive()) // Can't read/detect a valid value from SETUP_AW? (typically SPI or device fault)
		return RF24_STATE_NOTPRESENT;
	config = r_reg(RF24_CONFIG);
	if ((config & RF24_PWR_UP) == 0x00)  // PWR_UP=0?
		return RF24_STATE_POWERDOWN;
	if (!(nrfCEportout & nrfCEpin))      // PWR_UP=1 && CE=0?
		return RF24_STATE_STANDBY_I;
	if (!(config & RF24_PRIM_RX)) {      // PWR_UP=1 && CE=1 && PRIM_RX=0?
		if ((r_reg(RF24_FIFO_STATUS) & RF24_TX_EMPTY))  // TX FIFO empty?
			return RF24_STATE_STANDBY_II;
		return RF24_STATE_PTX; // If TX FIFO is not empty, we are in PTX (active transmit) mode.
	}
	if (r_reg(RF24_RF_SETUP) & 0x90)     // Testing CONT_WAVE or PLL_LOCK?
		return RF24_STATE_TEST;
	return RF24_STATE_PRX;           // PWR_UP=1, PRIM_RX=1, CE=1 -- Must be PRX
}

inline void pulse_ce() {
	CE_EN;
	__delay_cycles(DELAY_CYCLES_15US);
	CE_DIS;
}

void flush_tx() {
	CSN_EN;
	rf_status = spi_transfer(RF24_FLUSH_TX);
	CSN_DIS;
}

void flush_rx() {
	CSN_EN;
	rf_status = spi_transfer(RF24_FLUSH_RX);
	CSN_DIS;
}

void tx_reuse_lastpayload() {
	CSN_EN;
	rf_status = spi_transfer(RF24_REUSE_TX_PL);
	CSN_DIS;
}

uint8_t r_rx_peek_payload_size() {
	uint16_t i;

	CSN_EN;
	i = spi_transfer16(RF24_NOP | (RF24_R_RX_PL_WID << 8));
	rf_status = (uint8_t) ((i & 0xFF00) >> 8);
	CSN_DIS;
	return (uint8_t) (i & 0x00FF);
}
inline void port_init() {
#if nrfIRQport == 1
	P1DIR &= ~nrfIRQpin;  // IRQ line is input
	P1OUT |= nrfIRQpin;// Pull-up resistor enabled
	P1REN |= nrfIRQpin;
	P1IES |= nrfIRQpin;// Trigger on falling-edge
	P1IFG &= ~nrfIRQpin;// Clear any outstanding IRQ
	P1IE |= nrfIRQpin;// Enable IRQ interrupt
#elif nrfIRQport == 2
	P2DIR &= ~nrfIRQpin;  // IRQ line is input
	P2OUT |= nrfIRQpin;  // Pull-up resistor enabled
	P2REN |= nrfIRQpin;
	P2IES |= nrfIRQpin;  // Trigger on falling-edge
	P2IFG &= ~nrfIRQpin;  // Clear any outstanding IRQ
	P2IE |= nrfIRQpin;  // Enable IRQ interrupt
#elif nrfIRQport == 3
			P3DIR &= ~nrfIRQpin;  // IRQ line is input
			P3OUT |= nrfIRQpin;// Pull-up resistor enabled
			P3REN |= nrfIRQpin;
			P3IES |= nrfIRQpin;// Trigger on falling-edge
			P3IFG &= ~nrfIRQpin;// Clear any outstanding IRQ
			P3IE |= nrfIRQpin;// Enable IRQ interrupt
#elif nrfIRQport == 4
			P4DIR &= ~nrfIRQpin;  // IRQ line is input
			P4OUT |= nrfIRQpin;// Pull-up resistor enabled
			P4REN |= nrfIRQpin;
			P4IES |= nrfIRQpin;// Trigger on falling-edge
			P4IFG &= ~nrfIRQpin;// Clear any outstanding IRQ
			P4IE |= nrfIRQpin;// Enable IRQ interrupt
#elif nrfIRQport == 5
			P5DIR &= ~nrfIRQpin;  // IRQ line is input
			P5OUT |= nrfIRQpin;// Pull-up resistor enabled
			P5REN |= nrfIRQpin;
			P5IES |= nrfIRQpin;// Trigger on falling-edge
			P5IFG &= ~nrfIRQpin;// Clear any outstanding IRQ
			P5IE |= nrfIRQpin;// Enable IRQ interrupt
#elif nrfIRQport == 6
			P6DIR &= ~nrfIRQpin;  // IRQ line is input
			P6OUT |= nrfIRQpin;// Pull-up resistor enabled
			P6REN |= nrfIRQpin;
			P6IES |= nrfIRQpin;// Trigger on falling-edge
			P6IFG &= ~nrfIRQpin;// Clear any outstanding IRQ
			P6IE |= nrfIRQpin;// Enable IRQ interrupt
#elif nrfIRQport == 7
			P7DIR &= ~nrfIRQpin;  // IRQ line is input
			P7OUT |= nrfIRQpin;// Pull-up resistor enabled
			P7REN |= nrfIRQpin;
			P7IES |= nrfIRQpin;// Trigger on falling-edge
			P7IFG &= ~nrfIRQpin;// Clear any outstanding IRQ
			P7IE |= nrfIRQpin;// Enable IRQ interrupt
#elif nrfIRQport == 8
			P8DIR &= ~nrfIRQpin;  // IRQ line is input
			P8OUT |= nrfIRQpin;// Pull-up resistor enabled
			P8REN |= nrfIRQpin;
			P8IES |= nrfIRQpin;// Trigger on falling-edge
			P8IFG &= ~nrfIRQpin;// Clear any outstanding IRQ
			P8IE |= nrfIRQpin;// Enable IRQ interrupt
#elif nrfIRQport == 9
			P9DIR &= ~nrfIRQpin;  // IRQ line is input
			P9OUT |= nrfIRQpin;// Pull-up resistor enabled
			P9REN |= nrfIRQpin;
			P9IES |= nrfIRQpin;// Trigger on falling-edge
			P9IFG &= ~nrfIRQpin;// Clear any outstanding IRQ
			P9IE |= nrfIRQpin;// Enable IRQ interrupt
#elif nrfIRQport == 10
			P10DIR &= ~nrfIRQpin;  // IRQ line is input
			P10OUT |= nrfIRQpin;// Pull-up resistor enabled
			P10REN |= nrfIRQpin;
			P10IES |= nrfIRQpin;// Trigger on falling-edge
			P10IFG &= ~nrfIRQpin;// Clear any outstanding IRQ
			P10IE |= nrfIRQpin;// Enable IRQ interrupt
#endif

// Setup CSN/CE ports
#if nrfCSNport == 1
	P1DIR |= nrfCSNpin;
#elif nrfCSNport == 2
	P2DIR |= nrfCSNpin;
#elif nrfCSNport == 3
	P3DIR |= nrfCSNpin;
#elif nrfCSNport == 4
	P4DIR |= nrfCSNpin;
#elif nrfCSNport == 5
	P5DIR |= nrfCSNpin;
#elif nrfCSNport == 6
	P6DIR |= nrfCSNpin;
#elif nrfCSNport == 7
	P7DIR |= nrfCSNpin;
#elif nrfCSNport == 8
	P8DIR |= nrfCSNpin;
#elif nrfCSNport == 9
	P9DIR |= nrfCSNpin;
#elif nrfCSNport == 10
	P10DIR |= nrfCSNpin;
#elif nrfCSNport == J
	PJDIR |= nrfCSNpin;
#endif
	CSN_DIS;

#if nrfCEport == 1
	P1DIR |= nrfCEpin;
#elif nrfCEport == 2
	P2DIR |= nrfCEpin;
#elif nrfCEport == 3
	P3DIR |= nrfCEpin;
#elif nrfCEport == 4
	P4DIR |= nrfCEpin;
#elif nrfCEport == 5
	P5DIR |= nrfCEpin;
#elif nrfCEport == 6
	P6DIR |= nrfCEpin;
#elif nrfCEport == 7
	P7DIR |= nrfCEpin;
#elif nrfCEport == 8
	P8DIR |= nrfCEpin;
#elif nrfCEport == 9
	P9DIR |= nrfCEpin;
#elif nrfCEport == 10
	P10DIR |= nrfCEpin;
#elif nrfCEport == J
	PJDIR |= nrfCEpin;
#endif
	CE_DIS;

	/* Straw-man spi_transfer with no Chip Select lines enabled; this is to workaround errata bug USI5
	 * on the MSP430G2452 and related (see http://www.ti.com/lit/er/slaz072/slaz072.pdf)
	 * Shouldn't hurt anything since we expect no CS lines enabled by the user during this function's execution.
	 */
	spi_transfer(RF24_NOP);
}
