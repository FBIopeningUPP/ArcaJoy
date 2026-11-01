#ifndef _DESCRIPTORS_H_
#define _DESCRIPTORS_H_

#include <avr/pgmspace.h>
#include <LUFA/Drivers/USB/USB.h>

/* HID report that goes to the PC (13 bytes) */
typedef struct
{
	uint32_t Buttons;   /* 32 buttons, bit0 = button 1 */
	uint16_t Axis[4];   /* X, Y, Z, Rz  (0..1023) */
	uint8_t  Hat;       /* 0 = centered, 1=N 2=NE 3=E 4=SE 5=S 6=SW 7=W 8=NW */
} ATTR_PACKED USB_JoystickReport_t;

typedef struct
{
	USB_Descriptor_Configuration_Header_t    Config;

	/* Serial (CDC) */
	USB_Descriptor_Interface_Association_t   CDC_IAD;
	USB_Descriptor_Interface_t               CDC_CCI_Interface;
	USB_CDC_Descriptor_FunctionalHeader_t    CDC_Functional_Header;
	USB_CDC_Descriptor_FunctionalACM_t       CDC_Functional_ACM;
	USB_CDC_Descriptor_FunctionalUnion_t     CDC_Functional_Union;
	USB_Descriptor_Endpoint_t                CDC_ManagementEndpoint;
	USB_Descriptor_Interface_t               CDC_DCI_Interface;
	USB_Descriptor_Endpoint_t                CDC_DataOutEndpoint;
	USB_Descriptor_Endpoint_t                CDC_DataInEndpoint;

	/* Joystick (HID) */
	USB_Descriptor_Interface_t               HID_Interface;
	USB_HID_Descriptor_HID_t                 HID_JoystickHID;
	USB_Descriptor_Endpoint_t                HID_ReportINEndpoint;
} USB_Descriptor_Configuration_t;

enum InterfaceDescriptors_t
{
	INTERFACE_ID_CDC_CCI = 0,
	INTERFACE_ID_CDC_DCI = 1,
	INTERFACE_ID_Joystick = 2,
};

enum StringDescriptors_t
{
	STRING_ID_Language     = 0,
	STRING_ID_Manufacturer = 1,
	STRING_ID_Product      = 2,
};

#define CDC_NOTIFICATION_EPADDR   (ENDPOINT_DIR_IN  | 2)
#define CDC_TX_EPADDR             (ENDPOINT_DIR_IN  | 3)
#define CDC_RX_EPADDR             (ENDPOINT_DIR_OUT | 4)
#define JOYSTICK_EPADDR           (ENDPOINT_DIR_IN  | 1)

#define CDC_NOTIFICATION_EPSIZE   8
#define CDC_TXRX_EPSIZE           16
#define JOYSTICK_EPSIZE           16

uint16_t CALLBACK_USB_GetDescriptor(const uint16_t wValue,
                                    const uint16_t wIndex,
                                    const void** const DescriptorAddress)
                                    ATTR_WARN_UNUSED_RESULT ATTR_NON_NULL_PTR_ARG(3);

#endif
