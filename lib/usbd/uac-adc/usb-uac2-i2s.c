/*
* Copyright (c) 2018, 2019 Vladimir Alemasov
* All rights reserved
*
* This program and the accompanying materials are distributed under 
* the terms of GNU General Public License version 2 
* as published by the Free Software Foundation.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*/

// USB UAC2 class
// This module is based on
// Universal Serial Bus Specification Revision 2.0 April 27, 2000
// Universal Serial Bus Device Class Definition for Audio Devices Release 2.0 May 31, 2006
// USB-2 refers to the Universal Serial Bus Specification, Revision 2.0
// ADC-2 refers to the USB Device Class Definition for Audio Devices, Release 2.0

#include "platform.h"
#include "usbd_core.h"
#include "usb_std.h"
#include "hal-usbd-init.h"
#include "usb-uac2.h"
#include "uac2-adc-drv.h"

//--------------------------------------------
extern const audio_in_drv_t audio_in_drv;

#ifdef USBD_OTGFS
extern const struct usbd_driver usbd_otgfs;
#define usbd_hw usbd_otgfs
#elif defined USBD_OTGHS || defined USBD_OTGHS_FS
extern const struct usbd_driver usbd_otghs;
#define usbd_hw usbd_otghs
#endif


//--------------------------------------------
// An AUDIO FRAME means a block of data the host transmits to device (or vice versa)
// inside the usb isochronous packets.
// An AUDIO SAMPLE is the smallest usable quantum of digital audio.
// An USB FRAME is count by the SOF packets, for FS = 1000/s (each 1ms), for HS = 8000/s (each 125 us)
//--------------------------------------------
#define AUDIO_SAMPLE_RATE               I2S_FCLK
#define BITS_PER_AUDIO_SAMPLE           UAC_BITRES
#if BITS_PER_AUDIO_SAMPLE == 16
#define BYTES_PER_AUDIO_SAMPLE          2
#else
#define BYTES_PER_AUDIO_SAMPLE          4
#endif
#define AUDIO_CHANNELS                  I2S_CHANNELS
#ifdef USBD_FULL_SPEED
#define USB_FRAMES_PER_AUDIO_FRAME      1 // bInterval = 1 ms
#define SAMPLES_PER_AUDIO_FRAME         (AUDIO_SAMPLE_RATE / 1000) * USB_FRAMES_PER_AUDIO_FRAME
#else
#define USB_FRAMES_PER_AUDIO_FRAME      2 // 2^bInterval-1 = 2^1 = 2 microframes = 0.25 ms (supported values: 1 or 2)
#define SAMPLES_PER_AUDIO_FRAME         (AUDIO_SAMPLE_RATE / 1000 / 8) * USB_FRAMES_PER_AUDIO_FRAME
#endif
#define BYTES_PER_AUDIO_FRAME           (SAMPLES_PER_AUDIO_FRAME * AUDIO_CHANNELS * BYTES_PER_AUDIO_SAMPLE)
#define MAX_BYTES_PER_AUDIO_FRAME       (BYTES_PER_AUDIO_FRAME + BYTES_PER_AUDIO_SAMPLE * AUDIO_CHANNELS * USB_FRAMES_PER_AUDIO_FRAME)
#define MIN_BYTES_PER_AUDIO_FRAME       (BYTES_PER_AUDIO_FRAME - BYTES_PER_AUDIO_SAMPLE * AUDIO_CHANNELS * USB_FRAMES_PER_AUDIO_FRAME)
#define MAX_SAMPLES_PER_AUDIO_FRAME     (MAX_BYTES_PER_AUDIO_FRAME / BYTES_PER_AUDIO_SAMPLE)
#define AUDIO_FRAMES_IN_BUFFER          16

//--------------------------------------------
#define UAC_EP0_SIZE                64
#define UAC_TXD_EP                  0x82
#define UAC_DATA_SZ                 MAX_BYTES_PER_AUDIO_FRAME

// Interfaces
#define UAC_CONTROL_INTERFACE       0
#define UAC_STREAMING_INTERFACE     1

// Entity IDs
#define UAC_CLOCK_SOURCE_ID         1
#define UAC_INPUT_UNIT_ID           2
#define UAC_FEATURE_UNIT_ID         3
#define UAC_OUTPUT_UNIT_ID          4

//--------------------------------------------
#pragma pack(push, 1)
typedef struct uac_config
{
	struct usb_config_descriptor                        config;
	struct usb_iad_descriptor                           iad;
	struct usb_interface_descriptor                     ac;
	struct usb_uac2_ac_header_desc                      ac_hdr;
	struct usb_uac2_clock_source_desc                   ac_clk;
	struct usb_uac2_input_terminal_desc                 ac_itd;
#if AUDIO_CHANNELS == 1
	struct usb_uac2_feature_unit_sz2x32_desc            ac_fu;
#else
	struct usb_uac2_feature_unit_sz3x32_desc            ac_fu;
#endif
	struct usb_uac2_output_terminal_desc                ac_otd;
	struct usb_interface_descriptor                     as_0;
	struct usb_interface_descriptor                     as_1;
	struct usb_uac2_as_desc                             as_hdr;
	struct usb_uac2_as_format_type1_desc                as_format_type1;
	struct usb_uac2_std_iso_endpoint_descriptor         eptx_data_std;
	struct usb_uac2_cs_iso_endpoint_descriptor          eptx_data_cs;
} uac_config_t;
#pragma pack(pop)

//--------------------------------------------
static const struct usb_device_descriptor device_desc =
{
	.bLength            = sizeof(struct usb_device_descriptor),
	.bDescriptorType    = USB_DTYPE_DEVICE,
	.bcdUSB             = CPU_TO_LE16(VERSION_BCD(2,0,0)),
	.bDeviceClass       = USB_CLASS_MISC,
	.bDeviceSubClass    = USB_SUBCLASS_COMMON,
	.bDeviceProtocol    = USB_PROTO_IAD,
	.bMaxPacketSize0    = UAC_EP0_SIZE,
	.idVendor           = CPU_TO_LE16(0x0483),
	.idProduct          = CPU_TO_LE16(0x5740),
	.bcdDevice          = CPU_TO_LE16(VERSION_BCD(1,0,0)),
	.iManufacturer      = 1,
	.iProduct           = 2,
	.iSerialNumber      = INTSERIALNO_DESCRIPTOR,
	.bNumConfigurations = 1,
};

//--------------------------------------------
static const uac_config_t config_desc =
{
	.config =
	{
		.bLength                   = sizeof(struct usb_config_descriptor),
		.bDescriptorType           = USB_DTYPE_CONFIGURATION,
		.wTotalLength              = CPU_TO_LE16(sizeof(uac_config_t)),
		.bNumInterfaces            = 2,
		.bConfigurationValue       = 1,
		.iConfiguration            = NO_DESCRIPTOR,
		.bmAttributes              = USB_CFG_ATTR_RESERVED | USB_CFG_ATTR_SELFPOWERED,
		.bMaxPower                 = USB_CFG_POWER_MA(100),
	},
	.iad =
	{
		.bLength                   = sizeof(struct usb_iad_descriptor),
		.bDescriptorType           = USB_DTYPE_INTERFACEASSOC,
		.bFirstInterface           = 0,
		.bInterfaceCount           = 2,
		.bFunctionClass            = USB_CLASS_AUDIO,
		.bFunctionSubClass         = USB_SUBCLASS_NONE,
		.bFunctionProtocol         = USB_PROTO_IP_VERSION_02_00,
		.iFunction                 = 2,
	},
	.ac =
	{
		.bLength                   = sizeof(struct usb_interface_descriptor),
		.bDescriptorType           = USB_DTYPE_INTERFACE,
		.bInterfaceNumber          = UAC_CONTROL_INTERFACE,
		.bAlternateSetting         = 0,
		.bNumEndpoints             = 0,
		.bInterfaceClass           = USB_CLASS_AUDIO,
		.bInterfaceSubClass        = USB_SUBCLASS_AUDIOCONTROL,
		.bInterfaceProtocol        = USB_PROTO_IP_VERSION_02_00,
		.iInterface                = NO_DESCRIPTOR,
	},
	.ac_hdr =
	{
		.bLength                   = sizeof(struct usb_uac2_ac_header_desc),
		.bDescriptorType           = USB_DTYPE_CS_INTERFACE,
		.bDescriptorSubType        = USB_DTYPE_UAC2_AC_HEADER,
		.bcdADC                    = CPU_TO_LE16(VERSION_BCD(2,0,0)),
		.bCategory                 = USB_UAC2_AFCC_MICROPHONE,
		.wTotalLength              = CPU_TO_LE16(sizeof(struct usb_uac2_ac_header_desc) +
	                                             sizeof(struct usb_uac2_clock_source_desc) +
	                                             sizeof(struct usb_uac2_input_terminal_desc) +
#if AUDIO_CHANNELS == 1
	                                             sizeof(struct usb_uac2_feature_unit_sz2x32_desc) +
#else
	                                             sizeof(struct usb_uac2_feature_unit_sz3x32_desc) +
#endif
	                                             sizeof(struct usb_uac2_output_terminal_desc)),
		.bmControls                = 0x00,
	},
	.ac_clk =
	{
		.bLength                   = sizeof(struct usb_uac2_clock_source_desc),
		.bDescriptorType           = USB_DTYPE_CS_INTERFACE,
		.bDescriptorSubType        = USB_DTYPE_UAC2_AC_CLOCK_SOURCE,
		.bClockID                  = UAC_CLOCK_SOURCE_ID,
		.bmAttributes              = USB_UAC2_CS_BMA_INTERNAL_FIXED_CLOCK, // internal fixed clock, NOT synchronized to SOF
		.bmControls                = USB_UAC2_CS_BMC_SAM_FREQ_CONTROL_RO,
		.bAssocTerminal            = 0,
		.iClockSource              = 0,
	},
	.ac_itd =
	{
		.bLength                   = sizeof(struct usb_uac2_input_terminal_desc),
		.bDescriptorType           = USB_DTYPE_CS_INTERFACE,
		.bDescriptorSubType        = USB_DTYPE_UAC2_AC_INPUT_TERMINAL,
		.bTerminalID               = UAC_INPUT_UNIT_ID,
		.wTerminalType             = CPU_TO_LE16(USB_UAC2_ITT_MICROPHONE),
		.bAssocTerminal            = 0,
		.bCSourceID                = UAC_CLOCK_SOURCE_ID,
		.bNrChannels               = AUDIO_CHANNELS,
#if AUDIO_CHANNELS == 1
		.bmChannelConfig           = CPU_TO_LE32(0x00000004), // FC
#else
		.bmChannelConfig           = CPU_TO_LE32(0x00000003), // FL, FR
#endif
		.iChannelNames             = 0,
		.bmControls                = CPU_TO_LE16(0x0000),
		.iTerminal                 = 0,
	},
	.ac_fu =
	{
#if AUDIO_CHANNELS == 1
		.bLength                   = sizeof(struct usb_uac2_feature_unit_sz2x32_desc),
#else
		.bLength                   = sizeof(struct usb_uac2_feature_unit_sz3x32_desc),
#endif
		.bDescriptorType           = USB_DTYPE_CS_INTERFACE,
		.bDescriptorSubType        = USB_DTYPE_UAC2_AC_FEATURE_UNIT,
		.bUnitID                   = UAC_FEATURE_UNIT_ID,
		.bSourceID                 = UAC_INPUT_UNIT_ID,
		.bmaControls[0]            = CPU_TO_LE32(0x0000), // Master controls
		.bmaControls[1]            = CPU_TO_LE32(USB_UAC2_FU_BMAC_MUTE_CONTROL_RO), // Channel 0 controls: Mute read-only
#if AUDIO_CHANNELS == 2
		.bmaControls[2]            = CPU_TO_LE32(USB_UAC2_FU_BMAC_MUTE_CONTROL_RO), // Channel 1 controls: Mute read-only
#endif
		.iFeature                  = 0,
	},
	.ac_otd =
	{
		.bLength                   = sizeof(struct usb_uac2_output_terminal_desc),
		.bDescriptorType           = USB_DTYPE_CS_INTERFACE,
		.bDescriptorSubType        = USB_DTYPE_UAC2_AC_OUTPUT_TERMINAL,
		.bTerminalID               = UAC_OUTPUT_UNIT_ID,
		.wTerminalType             = CPU_TO_LE16(USB_UAC2_TT_USB_STREAMING),
		.bAssocTerminal            = 0,
		.bSourceID                 = UAC_FEATURE_UNIT_ID,
		.bCSourceID                = UAC_CLOCK_SOURCE_ID,
		.bmControls                = CPU_TO_LE16(0x0000),
		.iTerminal                 = 0,
	},
	.as_0 =
	{
		.bLength                   = sizeof(struct usb_interface_descriptor),
		.bDescriptorType           = USB_DTYPE_INTERFACE,
		.bInterfaceNumber          = UAC_STREAMING_INTERFACE,
		.bAlternateSetting         = 0,
		.bNumEndpoints             = 0,
		.bInterfaceClass           = USB_CLASS_AUDIO,
		.bInterfaceSubClass        = USB_SUBCLASS_AUDIOSTREAMING,
		.bInterfaceProtocol        = USB_PROTO_IP_VERSION_02_00,
		.iInterface                = NO_DESCRIPTOR,
	},
	.as_1 =
	{
		.bLength                   = sizeof(struct usb_interface_descriptor),
		.bDescriptorType           = USB_DTYPE_INTERFACE,
		.bInterfaceNumber          = UAC_STREAMING_INTERFACE,
		.bAlternateSetting         = 1,
		.bNumEndpoints             = 1,
		.bInterfaceClass           = USB_CLASS_AUDIO,
		.bInterfaceSubClass        = USB_SUBCLASS_AUDIOSTREAMING,
		.bInterfaceProtocol        = USB_PROTO_IP_VERSION_02_00,
		.iInterface                = NO_DESCRIPTOR,
	},
	.as_hdr =
	{
		.bLength                   = sizeof(struct usb_uac2_as_desc),
		.bDescriptorType           = USB_DTYPE_CS_INTERFACE,
		.bDescriptorSubType        = USB_DTYPE_UAC2_AS_GENERAL,
		.bTerminalLink             = UAC_OUTPUT_UNIT_ID,
		.bmControls                = 0x00,
		.bFormatType               = USB_UAC2_DATA_FORMAT_TYPE_I,
		.bmFormats                 = CPU_TO_LE32(USB_UAC2_DATA_FT_I_PCM),
		.bNrChannels               = AUDIO_CHANNELS,
#if AUDIO_CHANNELS == 1
		.bmChannelConfig           = CPU_TO_LE32(0x00000004), // FC
#else
		.bmChannelConfig           = CPU_TO_LE32(0x00000003), // FL, FR
#endif
		.iChannelNames             = 0,
	},
	.as_format_type1 =
	{
		.bLength                   = sizeof(struct usb_uac2_as_format_type1_desc),
		.bDescriptorType           = USB_DTYPE_CS_INTERFACE,
		.bDescriptorSubType        = USB_DTYPE_UAC2_AS_FORMAT_TYPE,
		.bFormatType               = USB_UAC2_DATA_FORMAT_TYPE_I,
		.bSubslotSize              = BYTES_PER_AUDIO_SAMPLE,
		.bBitResolution            = BITS_PER_AUDIO_SAMPLE,
	},
	.eptx_data_std =
	{
		.bLength                   = sizeof(struct usb_uac2_std_iso_endpoint_descriptor),
		.bDescriptorType           = USB_DTYPE_ENDPOINT,
		.bEndpointAddress          = UAC_TXD_EP,
		.bmAttributes              = USB_EPTYPE_ISOCHRONOUS | USB_EPATTR_ASYNC,
		.wMaxPacketSize            = CPU_TO_LE16(UAC_DATA_SZ),
		.bInterval                 = USB_FRAMES_PER_AUDIO_FRAME,
	},
	.eptx_data_cs =
	{
		.bLength                   = sizeof(struct usb_uac2_cs_iso_endpoint_descriptor),
		.bDescriptorType           = USB_DTYPE_CS_ENDPOINT,
		.bDescriptorSubType        = USB_DTYPE_UAC2_EP_GENERAL,
		.bmAttributes              = 0x00,
		.bmControls                = 0x00,
		.bLockDelayUnits           = 0,
		.wLockDelay                = CPU_TO_LE16(0x0000),
	},
};

//--------------------------------------------
static const struct usb_string_descriptor lang_desc = USB_ARRAY_DESC(USB_LANGID_ENG_US);
static const struct usb_string_descriptor manuf_desc_en = USB_STRING_DESC("Homemcu firmware examples");
static const struct usb_string_descriptor prod_desc_en = USB_STRING_DESC("UAC2 I2S-ADC demo");
static const struct usb_string_descriptor *const dtable[] =
{
	&lang_desc,
	&manuf_desc_en,
	&prod_desc_en,
};

//--------------------------------------------
typedef struct audio_state
{
	uint8_t mute[AUDIO_CHANNELS + 1];
	bool record;
	uint8_t cnt_i2s;
	uint8_t cnt_usb;
	uint32_t total_cnt_i2s;
	uint32_t total_cnt_usb;
} audio_state_t;

typedef struct feedback_state
{
	uint32_t flagO;
	uint32_t flagU;
	uint32_t flagU1;
	uint32_t flagU2;
	uint32_t flagU3;
} feedback_state_t;

//--------------------------------------------
static usbd_device udev;
static uint8_t iface_num;
static uint8_t altset_num;
static audio_state_t audio;
static feedback_state_t feedback;
// Due to use with USB FIFO and/or DMA, the data buffers below must be 32-bit aligned:
#define USB_CTRL_BUFF_SZ 128
static uint32_t ubuf[(USB_CTRL_BUFF_SZ + 3) / sizeof(uint32_t)];
// I2S controller captures 32-bit samples only
#define I2S_TO_USB_BUF_LENGTH_RATIO (sizeof(uint32_t) / BYTES_PER_AUDIO_SAMPLE)
#if AUDIO_CHANNELS == 1
// I2S controller can't capture one channel only, so the double buffer is needed
static uint32_t buff_i2s[(MAX_BYTES_PER_AUDIO_FRAME * I2S_TO_USB_BUF_LENGTH_RATIO * 2 + 3) / sizeof(uint32_t)];
#else
static uint32_t buff_i2s[(MAX_BYTES_PER_AUDIO_FRAME * I2S_TO_USB_BUF_LENGTH_RATIO + 3) / sizeof(uint32_t)];
#endif
static uint32_t buff_usb[AUDIO_FRAMES_IN_BUFFER][(UAC_DATA_SZ + 3) / sizeof(uint32_t)];
static uint16_t buff_usb_size[AUDIO_FRAMES_IN_BUFFER];
// UAC_CTRL_BUFF_SZ must not be less than the UAC2 control parameter block of the maximum length
#define UAC_CTRL_BUFF_SZ 16
static uint32_t buff_uac_ctrl[(UAC_CTRL_BUFF_SZ + 3) / sizeof(uint32_t)];


//--------------------------------------------
static usbd_respond uac_getdesc(usbd_ctlreq *req, void **address, uint16_t *length)
{
	uint8_t dtype = req->wValue >> 8;
	uint8_t dnumber = req->wValue;
	const void* desc;
	uint16_t len = 0;

	switch (dtype)
	{
	case USB_DTYPE_DEVICE:
		desc = &device_desc;
		break;
	case USB_DTYPE_CONFIGURATION:
		desc = &config_desc;
		len = sizeof(config_desc);
		break;
	case USB_DTYPE_STRING:
		if (dnumber < sizeof(dtable) / sizeof(dtable[0]))
		{
			desc = dtable[dnumber];
		}
		else
		{
			return usbd_fail;
		}
		break;
	default:
		return usbd_fail;
	}
	if (len == 0)
	{
		len = ((struct usb_header_descriptor*)desc)->bLength;
	}
	*address = (void*)desc;
	*length = len;
	return usbd_ack;
};

//--------------------------------------------
static usbd_respond uac_feature_control(usbd_device *dev, usbd_ctlreq *req)
{
	uint8_t length;
	// Channel Number (ADC-2 5.2.2)
	uint8_t cn = req->wValue & 0xFF;
	// Control Selector (ADC-2 5.2.2)
	uint8_t cs = req->wValue >> 8;
	// Reguest Type (ADC-2 5.2.2)
	bool get_request = req->bmRequestType & 0x80;

	switch (cs)
	{
	case USB_UAC2_FU_MUTE_CONTROL:
		{
			// Mute Control (ADC-2 5.2.5.7.1)
			// The valid range for the Channel Number field is from zero (the �master� channel)
			// up to the number of logical channels in the audio channel cluster.
			switch (req->bRequest)
			{
			case USB_UAC2_CUR:
				if (get_request)
				{
					struct usb_uac2_layout_1_CUR_parameter_block *block = (struct usb_uac2_layout_1_CUR_parameter_block *)&buff_uac_ctrl[0];
					block->bCUR = audio.mute[cn];
					length = sizeof(struct usb_uac2_layout_1_CUR_parameter_block);

					dev->status.data_ptr = &buff_uac_ctrl[0];
					dev->status.data_count = length < req->wLength ? length : req->wLength;
				}
				else
				{
					audio.mute[cn] = req->data[0];
				}
				return usbd_ack;
			}
		}
	}
	return usbd_fail;
}

//--------------------------------------------
static usbd_respond uac_clock_control(usbd_device *dev, usbd_ctlreq *req)
{
	uint8_t length;
#if 0
	// Channel Number (ADC-2 5.2.2)
	uint8_t cn = req->wValue & 0xFF;
#endif
	// Control Selector (ADC-2 5.2.2)
	uint8_t cs = req->wValue >> 8;
	// Reguest Type (ADC-2 5.2.2)
	bool get_request = req->bmRequestType & 0x80;

	switch (cs)
	{
	case USB_UAC2_CS_SAM_FREQ_CONTROL:
		{
			// Sampling Frequency Control (ADC-2 5.2.5.1.1)
			switch (req->bRequest)
			{
			case USB_UAC2_CUR:
				if (get_request)
				{
					struct usb_uac2_layout_3_CUR_parameter_block *block = (struct usb_uac2_layout_3_CUR_parameter_block *)&buff_uac_ctrl[0];
					block->dCUR = CPU_TO_LE32(AUDIO_SAMPLE_RATE);
					length = sizeof(struct usb_uac2_layout_3_CUR_parameter_block);

					dev->status.data_ptr = &buff_uac_ctrl[0];
					dev->status.data_count = length < req->wLength ? length : req->wLength;
				}
				else
				{
					uint32_t ideal_fclk = le32_to_cpup((uint32_t *)&req->data[0]);
				}
				return usbd_ack;
			case USB_UAC2_RANGE:
				if (get_request)
				{
					// Control Attributes (ADC-2 5.2.1)
					// If a subrange consists of only a single value, the corresponding triplet must contain
					// that value for both its MIN and MAX subattribute and the RES subattribute must be set to zero.
					struct usb_uac2_layout_3_RANGE_parameter_sz3x32_block *block = (struct usb_uac2_layout_3_RANGE_parameter_sz3x32_block *)&buff_uac_ctrl[0];
					block->wNumSubRanges = CPU_TO_LE16(1);
					block->dMIN = CPU_TO_LE32(AUDIO_SAMPLE_RATE);
					block->dMAX = CPU_TO_LE32(AUDIO_SAMPLE_RATE);
					block->dRES = CPU_TO_LE32(0);
					length = sizeof(struct usb_uac2_layout_3_RANGE_parameter_sz3x32_block);

					dev->status.data_ptr = &buff_uac_ctrl[0];
					dev->status.data_count = length < req->wLength ? length : req->wLength;
					return usbd_ack;
				}
			}
		}
	case USB_UAC2_CS_CLOCK_VALID_CONTROL:
		{
			// Clock Validity Control (ADC-2 5.2.5.1.2)
			switch (req->bRequest)
			{
			case USB_UAC2_CUR:
				if (get_request)
				{
					struct usb_uac2_layout_1_CUR_parameter_block *block = (struct usb_uac2_layout_1_CUR_parameter_block *)&buff_uac_ctrl[0];
					block->bCUR = 0x01;
					length = sizeof(struct usb_uac2_layout_1_CUR_parameter_block);

					dev->status.data_ptr = &buff_uac_ctrl[0];
					dev->status.data_count = length < req->wLength ? length : req->wLength;
					return usbd_ack;
				}
			}
		}
	}
	return usbd_fail;
}

//--------------------------------------------
static usbd_respond uac_control(usbd_device *dev, usbd_ctlreq *req, usbd_rqc_callback *callback)
{
	if (((USB_REQ_RECIPIENT | USB_REQ_TYPE) & req->bmRequestType) != (USB_REQ_INTERFACE | USB_REQ_CLASS))
	{
		if (req->bRequest == USB_STD_GET_INTERFACE)
		{
			if (req->wIndex == 1)
			{
				dev->status.data_ptr = &altset_num;
				dev->status.data_count = 1;
				return usbd_ack;
			}
		}
		if (req->bRequest == USB_STD_SET_INTERFACE)
		{
			altset_num = req->wValue;
			iface_num = req->wIndex;
			if (iface_num == 1 && altset_num == 1)
			{
				if (!audio.record)
				{
					audio.cnt_i2s = 0;
					audio.cnt_usb = 0;
					audio.total_cnt_i2s = 0;
					audio.total_cnt_usb = 0;
					audio_in_drv.config();
					audio.record = true;
					usbd_ep_write(dev, UAC_TXD_EP, (void *)0, 0);
					buff_usb_size[audio.cnt_i2s] = BYTES_PER_AUDIO_FRAME;
#if AUDIO_CHANNELS == 1
					audio_in_drv.read_dma_rxbuf(&buff_i2s[0], buff_usb_size[audio.cnt_i2s] * I2S_TO_USB_BUF_LENGTH_RATIO * 2);
#else
					audio_in_drv.read_dma_rxbuf(&buff_i2s[0], buff_usb_size[audio.cnt_i2s] * I2S_TO_USB_BUF_LENGTH_RATIO);
#endif
				}
			}
			if (iface_num == 1 && altset_num == 0)
			{
				if (audio.record)
				{
					audio.record = false;
				}
			}
			return usbd_ack;
		}
		return usbd_fail;
	}

	// Interface (ADC-2 5.2.2)
	uint8_t iface = req->wIndex & 0xFF;
	// Entity Id (ADC-2 5.2.2)
	uint8_t entity_id = req->wIndex >> 8;

	switch (iface)
	{
	case UAC_CONTROL_INTERFACE:
		{
			switch (entity_id)
			{
			case UAC_CLOCK_SOURCE_ID:
				return uac_clock_control(dev, req);
			case UAC_FEATURE_UNIT_ID:
				return uac_feature_control(dev, req);
			}
		}
	}
	return usbd_fail;
}

//--------------------------------------------
static void uac_callback(usbd_device *dev, uint8_t event, uint8_t ep)
{
	switch (event)
	{
	case usbd_evt_eptx:
		usbd_ep_write(dev, UAC_TXD_EP, &buff_usb[audio.cnt_usb][0], buff_usb_size[audio.cnt_usb]);
		++audio.total_cnt_usb;
		++audio.cnt_usb;
		if (audio.cnt_usb == AUDIO_FRAMES_IN_BUFFER)
		{
			audio.cnt_usb = 0;
		}
		break;
	default:
		break;
	}
}

//--------------------------------------------
static void i2s_rx_complete_callback(void)
{
	bool convert;

	if (audio.record)
	{
		convert = false;
		uint32_t *buf32_usb;
		uint32_t len;
		buf32_usb = (uint32_t *)&buff_usb[audio.cnt_i2s][0];
		len = buff_usb_size[audio.cnt_i2s];
		if (audio.total_cnt_i2s >= audio.total_cnt_usb)
		{
			++feedback.flagU;
			if (audio.total_cnt_i2s - audio.total_cnt_usb < AUDIO_FRAMES_IN_BUFFER)
			{
				++audio.total_cnt_i2s;
				++audio.cnt_i2s;
				buff_usb_size[audio.cnt_i2s] = BYTES_PER_AUDIO_FRAME;
				if (audio.cnt_i2s == AUDIO_FRAMES_IN_BUFFER)
				{
					audio.cnt_i2s = 0;
				}
				if (audio.total_cnt_i2s - audio.total_cnt_usb >= AUDIO_FRAMES_IN_BUFFER - AUDIO_FRAMES_IN_BUFFER / 8)
				{
					buff_usb_size[audio.cnt_i2s] = MAX_BYTES_PER_AUDIO_FRAME;
					++feedback.flagU1;
				}
				if (audio.total_cnt_i2s - audio.total_cnt_usb <= AUDIO_FRAMES_IN_BUFFER / 8)
				{
					buff_usb_size[audio.cnt_i2s] = MIN_BYTES_PER_AUDIO_FRAME;
					++feedback.flagU2;
				}
				convert = true;
			}
			else
			{
				++feedback.flagU3;
				buff_usb_size[audio.cnt_i2s] = BYTES_PER_AUDIO_FRAME;
			}
		}
		else
		{
			++feedback.flagO;
			audio.total_cnt_i2s = audio.total_cnt_usb + 2;
			audio.cnt_i2s = audio.total_cnt_i2s % AUDIO_FRAMES_IN_BUFFER;
			buff_usb_size[audio.cnt_i2s] = BYTES_PER_AUDIO_FRAME;
		}

#if AUDIO_CHANNELS == 1
		audio_in_drv.read_dma_rxbuf(&buff_i2s[0], buff_usb_size[audio.cnt_i2s] * I2S_TO_USB_BUF_LENGTH_RATIO * 2);
#else
		audio_in_drv.read_dma_rxbuf(&buff_i2s[0], buff_usb_size[audio.cnt_i2s] * I2S_TO_USB_BUF_LENGTH_RATIO);
#endif

		if (convert)
		{
			//-----------
			// This conversion only works with
			// BITS_PER_AUDIO_SAMPLE == 24(32)
			// BYTES_PER_AUDIO_SAMPLE == 4
			// I2S_BITRES == 24(32)
			// I2S_TO_USB_BUF_LENGTH_RATIO == 1
			// It is needed to create another one if macros are different
			//-----------
			uint32_t cnt_usb = 0;
			uint32_t cnt_i2s = 0;
			uint32_t *buf32_i2s;
			buf32_i2s = &buff_i2s[0];
			for (cnt_usb = 0, cnt_i2s = 0; cnt_usb < len / sizeof(uint32_t); cnt_usb += 1)
			{
				audio_in_drv.convert(&buf32_i2s[cnt_i2s], &buf32_usb[cnt_usb]);
#if AUDIO_CHANNELS == 1
				cnt_i2s += 2;
#else
				cnt_i2s += 1;
#endif
			}
		}
	}
}

//--------------------------------------------
static usbd_respond uac_setconf(usbd_device *dev, uint8_t cfg)
{
	switch (cfg)
	{
	case 0:
		// deconfiguring device
		usbd_ep_deconfig(dev, UAC_TXD_EP);
		usbd_reg_endpoint(dev, UAC_TXD_EP, NULL);
		return usbd_ack;
	case 1:
        // configuring device
		usbd_ep_config(dev, UAC_TXD_EP, USB_EPTYPE_ISOCHRONOUS | USB_EPTYPE_DBLBUF, UAC_DATA_SZ);
		usbd_reg_endpoint(dev, UAC_TXD_EP, uac_callback);
		return usbd_ack;
	default:
		return usbd_fail;
	}
}

//--------------------------------------------
void usb_uac_i2s_init(void)
{
	audio_in_drv.init();
	audio_in_drv.init_dma_rxbuf(i2s_rx_complete_callback);

	usbd_hw_init(&udev);
	usbd_init(&udev, &usbd_hw, UAC_EP0_SIZE, ubuf, sizeof(ubuf));
	usbd_reg_control(&udev, uac_control);
	usbd_reg_config(&udev, uac_setconf);
	usbd_reg_descr(&udev, uac_getdesc);
}

//--------------------------------------------
void usb_uac_i2s_loop(void)
{
	usbd_enable(&udev, true);
	usbd_connect(&udev, true);
	while (1);
}
