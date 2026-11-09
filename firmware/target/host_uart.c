/* UART0 to the host, 460800 8N1. Pins from the Makefile (UART_RX_DIO/UART_TX_DIO):
 * DIO2/DIO3 reach the board's USB-serial chip; bridge mode moves them to free
 * DIOs so the ESP32 does not share a line with that chip (docs/WIRING.md). */
#include <ti/devices/DeviceFamily.h>
#include DeviceFamily_constructPath(inc/hw_ints.h)
#include DeviceFamily_constructPath(inc/hw_memmap.h)
#include DeviceFamily_constructPath(driverlib/ioc.h)
#include DeviceFamily_constructPath(driverlib/sys_ctrl.h)
#include DeviceFamily_constructPath(driverlib/uart.h)
#include <ti/drivers/Power.h>
#include <ti/drivers/dpl/HwiP.h>
#include <ti/drivers/power/PowerCC26X2.h>

#include "hostlink.h"
#include "platform.h"

#define UART_BAUD 460800
#define PIN_RX    UART_RX_DIO /* IOID_n == n */
#define PIN_TX    UART_TX_DIO

/* Power-of-two rings; RX is filled by the ISR, TX drained by the main loop
 * so a long host write never stalls the radio work. */
#define RX_RING 1024u
#define TX_RING 4096u

static volatile uint8_t rx_ring[RX_RING];
static volatile uint32_t rx_head, rx_tail;
static uint8_t tx_ring[TX_RING];
static uint32_t tx_head, tx_tail;

static void uart_isr(uintptr_t arg)
{
    (void)arg;
    UARTIntClear(UART0_BASE, UARTIntStatus(UART0_BASE, true));
    while (UARTCharsAvail(UART0_BASE)) {
        uint8_t b = (uint8_t)UARTCharGetNonBlocking(UART0_BASE);
        if (rx_head - rx_tail < RX_RING)
            rx_ring[rx_head++ % RX_RING] = b;
    }
}

void uart_init(void)
{
    Power_setDependency(PowerCC26XX_PERIPH_UART0);
    IOCPinTypeUart(UART0_BASE, PIN_RX, PIN_TX, IOID_UNUSED, IOID_UNUSED);
    UARTConfigSetExpClk(UART0_BASE, SysCtrlClockGet(), UART_BAUD,
                        UART_CONFIG_WLEN_8 | UART_CONFIG_STOP_ONE | UART_CONFIG_PAR_NONE);
    UARTFIFOLevelSet(UART0_BASE, UART_FIFO_TX4_8, UART_FIFO_RX4_8);
    UARTFIFOEnable(UART0_BASE);

    HwiP_Params params;
    HwiP_Params_init(&params);
    HwiP_create(INT_UART0_COMB, uart_isr, &params);
    UARTIntEnable(UART0_BASE, UART_INT_RX | UART_INT_RT);
    UARTEnable(UART0_BASE);
}

size_t uart_read(uint8_t *buf, size_t max)
{
    size_t n = 0;
    while (n < max && rx_tail != rx_head)
        buf[n++] = rx_ring[rx_tail++ % RX_RING];
    return n;
}

void uart_flush(void)
{
    while (tx_tail != tx_head && UARTSpaceAvail(UART0_BASE))
        UARTCharPutNonBlocking(UART0_BASE, tx_ring[tx_tail++ % TX_RING]);
}

void hostlink_write(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        while (tx_head - tx_tail == TX_RING)
            uart_flush(); /* ring full: wait for the FIFO */
        tx_ring[tx_head++ % TX_RING] = data[i];
    }
    uart_flush();
}
