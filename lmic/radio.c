// Copyright (C) 2016-2019 Semtech (International) AG. All rights reserved.
// Copyright (C) 2014-2016 IBM Corporation. All rights reserved.
//
// This file is subject to the terms and conditions defined in file 'LICENSE',
// which is part of this source code package.

//#define DEBUG_TX
//#define DEBUG_RX

#include "board.h"
#include "lmic.h"

// ----------------------------------------
// RADIO STATE
static struct {
    ostime_t irqtime;
    osjob_t irqjob;
    u1_t diomask;
    u1_t txmode;
} state;

// stop radio, disarm interrupts, cancel jobs
static void radio_stop (void) {
    hal_disableIRQs();
    // put radio to sleep
    radio_sleep();
    // disable antenna switch
    hal_ant_switch(HAL_ANTSW_OFF);
    // power-down TCXO
    hal_pin_tcxo(0);
    // disable IRQs in HAL
    hal_irqmask_set(0);
    // cancel radio job
    os_clearCallback(&state.irqjob);
    // clear state
    state.diomask = 0;
    hal_enableIRQs();
}

// guard timeout in case no completion interrupt is generated by radio
// protected job - runs with irqs disabled!
static void radio_irq_timeout (osjob_t* j) {
    BACKTRACE();
    (void)j; // unused

    // stop everything (antenna switch, hal irqs, sleep, irq job)
    radio_stop();

    // re-initialize radio if tx operation timed out
    if (state.txmode) {
        radio_init(true);
    }

    // enable IRQs!
    hal_enableIRQs();

    debug_printf("WARNING: radio irq timeout!\r\n");

    // indicate timeout
    LMIC.dataLen = 0;

    // run os job (use preset func ptr)
    os_setCallback(&LMIC.osjob, LMIC.osjob.func);
}

void radio_set_irq_timeout (ostime_t timeout) {
    // schedule irq-protected timeout function
    os_setProtectedTimedCallback(&state.irqjob, timeout, radio_irq_timeout);
}

// (run by irqjob)
static void radio_irq_func (osjob_t* j) {
    (void)j; // unused
    // call radio-specific processing function
    if( radio_irq_process(state.irqtime, state.diomask) ) {
        // current radio operation has completed
        radio_stop(); // (disable antenna switch and HAL irqs, make radio sleep)

        // run LMIC job (use preset func ptr)
        os_setCallback(&LMIC.osjob, LMIC.osjob.func);
    }

    // clear irq state (job has been run)
    state.diomask = 0;
}

// called by hal exti IRQ handler
// (all radio operations are performed on radio job!)
void radio_irq_handler (u1_t diomask, ostime_t ticks) {
    BACKTRACE();

    // make sure previous job has been run
    ASSERT( state.diomask == 0 );

    // save interrupt source and time
    state.irqtime = ticks;
    state.diomask = diomask;

    // schedule irq job
    // (timeout job will be replaced, intermediate interrupts must rewind timeout!)
    os_setCallback(&state.irqjob, radio_irq_func);
}

void os_radio (u1_t mode) {
    switch (mode) {
        case RADIO_STOP:
            radio_stop();
            break;

        case RADIO_TX:
            radio_stop();
#ifdef DEBUG_TX
            debug_printf("TX[fcnt=%ld,freq=%.1F,sf=%d,bw=%d,pow=%d,len=%d%s]: %.80h\r\n",
                         LMIC.seqnoUp - 1, LMIC.freq, 6, getSf(LMIC.rps) - SF7 + 7, 125 << getBw(LMIC.rps),
                         LMIC.txpow, LMIC.dataLen,
                         (LMIC.pendTxPort != 0 && (LMIC.frame[OFF_DAT_FCT] & FCT_ADRARQ)) ? ",ADRARQ" : "",
                         LMIC.frame, LMIC.dataLen);
#endif
            // transmit frame now (wait for completion interrupt)
            radio_starttx(false);
            // set timeout for tx operation (should not happen)
            state.txmode = 1;
            radio_set_irq_timeout(os_getTime() + ms2osticks(20) + LMIC_calcAirTime(LMIC.rps, LMIC.dataLen) * 110 / 100);
            break;

        case RADIO_RX:
            radio_stop();
#ifdef DEBUG_RX
            debug_printf("RX_MODE[freq=%.1F,sf=%d,bw=%s,rxtime=%.0F]\r\n",
                         LMIC.freq, 6,
                         getSf(LMIC.rps) - SF7 + 7, ("125\0" "250\0" "500\0" "rfu") + (4 * getBw(LMIC.rps)),
                         LMIC.rxtime, 0);
#endif
            // receive frame at rxtime/now (wait for completion interrupt)
            radio_startrx(false);
            // set timeout for rx operation (should not happen, might be updated by radio driver)
            state.txmode = 0;
            radio_set_irq_timeout(LMIC.rxtime + ms2osticks(5) + LMIC_calcAirTime(LMIC.rps, 255) * 110 / 100);
            break;

        case RADIO_RXON:
            radio_stop();
#ifdef DEBUG_RX
            debug_printf("RXON_MODE[freq=%.1F,sf=%d,bw=%s]\r\n",
                         LMIC.freq, 6,
                         getSf(LMIC.rps) - SF7 + 7, ("125\0" "250\0" "500\0" "rfu") + (4 * getBw(LMIC.rps)));
#endif
            // start scanning for frame now (wait for completion interrupt)
            state.txmode = 0;
            radio_startrx(true);
            break;

        case RADIO_TXCW:
            radio_stop();
            // transmit continuous wave (until abort)
            radio_cw();
            break;

        case RADIO_CCA:
            radio_stop();
            // clear channel assessment
            radio_cca();
            break;

        case RADIO_INIT:
            // reset and calibrate radio (uses LMIC.freq)
            radio_init(true);
            break;

        case RADIO_TXCONT:
            radio_stop();
            radio_starttx(true);
            break;

        case RADIO_CAD:
            radio_stop();
            // set timeout for cad/rx operation (should not happen, might be updated by radio driver)
            state.txmode = 0;
            radio_set_irq_timeout(os_getTime() + ms2osticks(10) + LMIC_calcAirTime(LMIC.rps, 255) * 110 / 100);
            // channel activity detection and rx if preamble symbol found
            radio_cad();
            break;
    }
}
