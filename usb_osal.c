/* 
 * This file contains implementation for usb_osal for bare metal USB examples 
 */
#include <stdio.h>
#include "types.h"
#ifdef TIRTOS
#include <xdc/runtime/System.h>
#endif
#include "usb_osal.h"
#include "timer.h"
#include "hardware.h"
#include <ti/drv/uart/UART.h>
#include <ti/drv/uart/UART_stdio.h>
#include <ti/csl/arch/csl_arch.h>

void usb_osalDelayMs(uint32_t delay_ms)
{
    osalTimerDelay(delay_ms);
}

void usb_osalStartTimerMs(uint32_t ms)
{
    osalTimerStart(ms);
}

void usb_osalStopTimer()
{
    osalTimerStop();
}

uint32_t usb_osalIsTimerExpired()
{
    return osalTimerExpired();
}

void usb_osalDisableInterruptNum(uint32_t intNum)
{
    HwiP_disableInterrupt(intNum);
}

void usb_osalEnableInterruptNum(uint32_t intNum)
{
    HwiP_enableInterrupt(intNum);
}

uint32_t usb_osalHardwareIntDisable(void)
{
    return HwiP_disable();
}

void usb_osalHardwareIntRestore(uint32_t intCx)
{
    HwiP_restore(intCx);
}

void usb_osalClearInterrupt(uint32_t intNum)
{
    HwiP_clearInterrupt(intNum);
}
