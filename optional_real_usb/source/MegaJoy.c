/*
 * MegaJoy - 16U2 firmware for Arduino Mega 2560 (R3)
 *
 * The 16U2 shows up on the PC as TWO things:
 *   1) a game controller (HID joystick: 32 buttons, 4 axes, 1 hat)
 *   2) a serial port (CDC) - used for config tool AND for uploading sketches
 *
 * Mega -> 16U2 over the hardware UART:
 *   - A frame:  0xA5, 13 report bytes, 1 XOR byte  -> becomes the joystick report
 *   - Any other byte (plain text)                  -> goes to the PC serial port
 * PC serial port -> Mega: passed straight through.
 *
 * If the PC opens the serial port at 115200 baud (Arduino IDE upload) we go into
 * pure pass-through mode so the normal bootloader upload works. DTR resets the Mega.
 */
#include "MegaJoy.h"

static RingBuffer_t USBtoUSART_Buffer;
static uint8_t      USBtoUSART_Buffer_Data[64];
static RingBuffer_t USARTtoUSB_Buffer;
static uint8_t      USARTtoUSB_Buffer_Data[128];

static volatile bool Passthrough = false;
static volatile bool RevertPending = false;
static volatile uint16_t DtrLowMs = 0;

static USB_JoystickReport_t JoyReport;           /* newest report from the Mega */
static uint8_t PrevJoystickHIDReportBuffer[sizeof(USB_JoystickReport_t)];

USB_ClassInfo_CDC_Device_t VirtualSerial_CDC_Interface =
{
	.Config =
	{
		.ControlInterfaceNumber = INTERFACE_ID_CDC_CCI,
		.DataINEndpoint =
		{
			.Address = CDC_TX_EPADDR,
			.Size    = CDC_TXRX_EPSIZE,
			.Banks   = 1,
		},
		.DataOUTEndpoint =
		{
			.Address = CDC_RX_EPADDR,
			.Size    = CDC_TXRX_EPSIZE,
			.Banks   = 1,
		},
		.NotificationEndpoint =
		{
			.Address = CDC_NOTIFICATION_EPADDR,
			.Size    = CDC_NOTIFICATION_EPSIZE,
			.Banks   = 1,
		},
	},
};

USB_ClassInfo_HID_Device_t Joystick_HID_Interface =
{
	.Config =
	{
		.InterfaceNumber        = INTERFACE_ID_Joystick,
		.ReportINEndpoint       =
		{
			.Address = JOYSTICK_EPADDR,
			.Size    = JOYSTICK_EPSIZE,
			.Banks   = 1,
		},
		.PrevReportINBuffer     = PrevJoystickHIDReportBuffer,
		.PrevReportINBufferSize = sizeof(PrevJoystickHIDReportBuffer),
	},
};

static void UsartSetup(uint32_t baud)
{
	UCSR1B = 0;
	UCSR1A = 0;
	UCSR1C = 0;
	UBRR1  = USART_2X_UBRR(baud);
	UCSR1C = (1 << UCSZ11) | (1 << UCSZ10);   /* 8N1 */
	UCSR1A = (1 << U2X1);
	UCSR1B = (1 << RXCIE1) | (1 << TXEN1) | (1 << RXEN1);
}

static inline bool HostHasPortOpen(void)
{
	return (VirtualSerial_CDC_Interface.State.ControlLineStates.HostToDevice & CDC_CONTROL_LINE_OUT_DTR);
}

/* Handle one byte that came from the Mega */
static void HandleMegaByte(uint8_t b)
{
	static uint8_t state = 0;       /* 0 = text, 1 = reading frame */
	static uint8_t idx   = 0;
	static uint8_t buf[FRAME_DATA_LEN + 1];

	if (Passthrough)
	{
		if (HostHasPortOpen())
		  CDC_Device_SendByte(&VirtualSerial_CDC_Interface, b);
		return;
	}

	if (state == 0)
	{
		if (b == FRAME_START)
		{
			state = 1;
			idx   = 0;
		}
		else if (HostHasPortOpen())
		{
			CDC_Device_SendByte(&VirtualSerial_CDC_Interface, b);
		}
	}
	else
	{
		buf[idx++] = b;
		if (idx == FRAME_DATA_LEN + 1)
		{
			uint8_t x = 0;
			for (uint8_t i = 0; i < FRAME_DATA_LEN; i++)
			  x ^= buf[i];

			if (x == buf[FRAME_DATA_LEN])
			{
				uint8_t sreg = SREG;
				cli();
				memcpy(&JoyReport, buf, FRAME_DATA_LEN);
				SREG = sreg;
			}
			state = 0;
		}
	}
}

int main(void)
{
	MCUSR &= ~(1 << WDRF);
	wdt_disable();
	clock_prescale_set(clock_div_1);

	/* Mega reset line: idle high */
	MEGA_RESET_DDR  |= MEGA_RESET_MASK;
	MEGA_RESET_PORT |= MEGA_RESET_MASK;

	RingBuffer_InitBuffer(&USBtoUSART_Buffer, USBtoUSART_Buffer_Data, sizeof(USBtoUSART_Buffer_Data));
	RingBuffer_InitBuffer(&USARTtoUSB_Buffer, USARTtoUSB_Buffer_Data, sizeof(USARTtoUSB_Buffer_Data));

	USB_Init();
	UsartSetup(APP_BAUD);
	sei();

	for (;;)
	{
		/* PC serial -> Mega */
		if (!RingBuffer_IsFull(&USBtoUSART_Buffer))
		{
			int16_t rx = CDC_Device_ReceiveByte(&VirtualSerial_CDC_Interface);
			if (rx >= 0)
			  RingBuffer_Insert(&USBtoUSART_Buffer, (uint8_t)rx);
		}

		/* port was closed for a while -> go back to joystick mode */
		if (RevertPending)
		{
			RevertPending = false;
			if (!HostHasPortOpen())
			{
				Passthrough = false;
				UsartSetup(APP_BAUD);
			}
		}

		if ((UCSR1A & (1 << UDRE1)) && !RingBuffer_IsEmpty(&USBtoUSART_Buffer))
		  UDR1 = RingBuffer_Remove(&USBtoUSART_Buffer);

		/* Mega -> PC (serial text or joystick frames) */
		uint8_t n = RingBuffer_GetCount(&USARTtoUSB_Buffer);
		while (n--)
		  HandleMegaByte(RingBuffer_Remove(&USARTtoUSB_Buffer));

		CDC_Device_USBTask(&VirtualSerial_CDC_Interface);
		HID_Device_USBTask(&Joystick_HID_Interface);
		USB_USBTask();
	}
}

ISR(USART1_RX_vect, ISR_BLOCK)
{
	uint8_t b = UDR1;

	if (USB_DeviceState == DEVICE_STATE_Configured)
	{
		if (!RingBuffer_IsFull(&USARTtoUSB_Buffer))
		  RingBuffer_Insert(&USARTtoUSB_Buffer, b);
	}
}

void EVENT_USB_Device_ConfigurationChanged(void)
{
	CDC_Device_ConfigureEndpoints(&VirtualSerial_CDC_Interface);
	HID_Device_ConfigureEndpoints(&Joystick_HID_Interface);
	USB_Device_EnableSOFEvents();
}

void EVENT_USB_Device_ControlRequest(void)
{
	CDC_Device_ProcessControlRequest(&VirtualSerial_CDC_Interface);
	HID_Device_ProcessControlRequest(&Joystick_HID_Interface);
}

void EVENT_USB_Device_StartOfFrame(void)
{
	HID_Device_MillisecondElapsed(&Joystick_HID_Interface);

	/* In upload mode: if the PC port stays closed for 1.5 s, return to joystick mode.
	   (A short DTR drop during the reset trick of an upload must NOT end upload mode.) */
	if (HostHasPortOpen())
	  DtrLowMs = 0;
	else if (Passthrough)
	{
		if (++DtrLowMs >= 1500)
		{
			DtrLowMs = 0;
			RevertPending = true;
		}
	}
}

void EVENT_CDC_Device_LineEncodingChanged(USB_ClassInfo_CDC_Device_t* const CDCInterfaceInfo)
{
	if (CDCInterfaceInfo->State.LineEncoding.BaudRateBPS == UPLOAD_BAUD)
	{
		Passthrough = true;
		UsartSetup(UPLOAD_BAUD);
	}
	else
	{
		Passthrough = false;
		UsartSetup(APP_BAUD);
	}
}

void EVENT_CDC_Device_ControLineStateChanged(USB_ClassInfo_CDC_Device_t* const CDCInterfaceInfo)
{
	bool dtr = (CDCInterfaceInfo->State.ControlLineStates.HostToDevice & CDC_CONTROL_LINE_OUT_DTR);

	/* DTR on -> pull Mega reset low (same trick as the original Arduino firmware) */
	if (dtr)
	  MEGA_RESET_PORT &= ~MEGA_RESET_MASK;
	else
	  MEGA_RESET_PORT |= MEGA_RESET_MASK;

}

bool CALLBACK_HID_Device_CreateHIDReport(USB_ClassInfo_HID_Device_t* const HIDInterfaceInfo,
                                         uint8_t* const ReportID,
                                         const uint8_t ReportType,
                                         void* ReportData,
                                         uint16_t* const ReportSize)
{
	uint8_t sreg = SREG;
	cli();
	memcpy(ReportData, &JoyReport, sizeof(USB_JoystickReport_t));
	SREG = sreg;

	*ReportSize = sizeof(USB_JoystickReport_t);
	return false;
}

void CALLBACK_HID_Device_ProcessHIDReport(USB_ClassInfo_HID_Device_t* const HIDInterfaceInfo,
                                          const uint8_t ReportID,
                                          const uint8_t ReportType,
                                          const void* ReportData,
                                          const uint16_t ReportSize)
{
	/* nothing: we have no output reports */
}
