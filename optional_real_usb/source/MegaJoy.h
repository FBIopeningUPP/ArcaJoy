#ifndef _MEGAJOY_H_
#define _MEGAJOY_H_

#include <avr/io.h>
#include <avr/wdt.h>
#include <avr/power.h>
#include <avr/interrupt.h>
#include <string.h>

#include "Descriptors.h"

#include <LUFA/Drivers/Misc/RingBuffer.h>
#include <LUFA/Drivers/USB/USB.h>

#define FRAME_START        0xA5   /* Mega sends: 0xA5 + 13 report bytes + 1 xor byte */
#define FRAME_DATA_LEN     13
#define APP_BAUD           500000UL
#define UPLOAD_BAUD        115200UL

/* ATmega16U2 pin PD7 drives the Mega's RESET line */
#define MEGA_RESET_PORT    PORTD
#define MEGA_RESET_DDR     DDRD
#define MEGA_RESET_MASK    (1 << 7)

#define USART_2X_UBRR(baud)  ((((F_CPU / 8) + ((baud) / 2)) / (baud)) - 1)

void EVENT_USB_Device_ConfigurationChanged(void);
void EVENT_USB_Device_ControlRequest(void);
void EVENT_USB_Device_StartOfFrame(void);
void EVENT_CDC_Device_LineEncodingChanged(USB_ClassInfo_CDC_Device_t* const CDCInterfaceInfo);
void EVENT_CDC_Device_ControLineStateChanged(USB_ClassInfo_CDC_Device_t* const CDCInterfaceInfo);

bool CALLBACK_HID_Device_CreateHIDReport(USB_ClassInfo_HID_Device_t* const HIDInterfaceInfo,
                                         uint8_t* const ReportID,
                                         const uint8_t ReportType,
                                         void* ReportData,
                                         uint16_t* const ReportSize);
void CALLBACK_HID_Device_ProcessHIDReport(USB_ClassInfo_HID_Device_t* const HIDInterfaceInfo,
                                          const uint8_t ReportID,
                                          const uint8_t ReportType,
                                          const void* ReportData,
                                          const uint16_t ReportSize);
#endif
