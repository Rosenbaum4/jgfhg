/*
 * Copyright (C) 2009
 * Guennadi Liakhovetski, DENX Software Engineering, <lg@denx.de>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 3 as
 * published by the Free Software Foundation.
 */
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdio.h>
#include <memory.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <time.h>
#include <semaphore.h>
#include <iconv.h>
#include <dirent.h>
#include <stdint.h>

#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/poll.h>
#include <sys/mman.h>
#include <sys/vfs.h>
#include <sys/wait.h>
#include <sys/utsname.h>

#include <asm/byteorder.h>

#include <linux/types.h>
#include <linux/usb/gadgetfs.h>
#include <linux/usb/ch9.h>

#include "usbstring.h"

#define min(a,b) ({ typeof(a) __a = (a); typeof(b) __b = (b); __a < __b ? __a : __b; })
#define ARRAY_SIZE(arr) (sizeof(arr) / sizeof((arr)[0]))

static int verbose;

/* Still Image class-specific requests: */
#define USB_REQ_PTP_CANCEL_REQUEST		0x64
#define USB_REQ_PTP_GET_EXTENDED_EVENT_DATA	0x65
#define USB_REQ_PTP_DEVICE_RESET_REQUEST	0x66
#define USB_REQ_PTP_GET_DEVICE_STATUS_REQUEST	0x67

#define DRIVER_VENDOR_NUM	0x1d6b
#define DRIVER_PRODUCT_NUM	0x0100
#define DRIVER_MFGR		"Linux"
#define DRIVER_PRODUCT		"PTP Gadget"
#define DRIVER_CONFIG		"Configuration 0"
#define DRIVER_INTERFACE	"Source/Sink"

/* Will be used for bcdDevice: remember to update on major changes */
#define MAJOR			1
#define MINOR			0
#define DRIVER_VERSION		((MAJOR << 8) | MINOR)
#define VERSION_STRING		__stringify(MAJOR) "." __stringify(MINOR)

#define PTP_MANUFACTURER	"Linux Foundation"
#define PTP_MODEL		"PTP Gadget"
#define PTP_STORAGE_DESC	"SD/MMC"
#define PTP_MODEL_DIR		"100LINUX"

/*-------------------------------------------------------------------------*/

/* these descriptors are modified based on what controller we find */

#define	STRINGID_MFGR		1
#define	STRINGID_PRODUCT	2
#define	STRINGID_CONFIG		3
#define	STRINGID_INTERFACE	4

static struct usb_device_descriptor device_desc = {
	.bLength =		sizeof device_desc,
	.bDescriptorType =	USB_DT_DEVICE,

	.bcdUSB =		__constant_cpu_to_le16(0x0200),
	.bcdDevice =		__constant_cpu_to_le16(DRIVER_VERSION),
	.bDeviceClass =		USB_CLASS_PER_INTERFACE,
	.bDeviceSubClass =	0,
	.bDeviceProtocol =	0,
	/* .bMaxPacketSize0 ... set by gadgetfs */
	.idVendor =		__constant_cpu_to_le16(DRIVER_VENDOR_NUM),
	.idProduct =		__constant_cpu_to_le16(DRIVER_PRODUCT_NUM),
	.iManufacturer =	STRINGID_MFGR,
	.iProduct =		STRINGID_PRODUCT,
	.bNumConfigurations =	1,
};

#define	MAX_USB_POWER		1

#define	CONFIG_VALUE		1

static const struct usb_config_descriptor config = {
	.bLength =		sizeof config,
	.bDescriptorType =	USB_DT_CONFIG,

	/* must compute wTotalLength ... */
	.bNumInterfaces =	1,
	.bConfigurationValue =	CONFIG_VALUE,
	.iConfiguration =	STRINGID_CONFIG,
	.bmAttributes =		USB_CONFIG_ATT_ONE | USB_CONFIG_ATT_SELFPOWER,
	.bMaxPower =		(MAX_USB_POWER + 1) / 2,
};

/* USB subclass value = the protocol encapsulation */
#define USB_SC_IMAGE_CAPTURE	0x01		/* Still Image Capture Subclass */
#define USB_PR_CB		0x01		/* Control/Bulk w/o interrupt */

static struct usb_interface_descriptor source_sink_intf = {
	.bLength =		sizeof source_sink_intf,
	.bDescriptorType =	USB_DT_INTERFACE,

	.bInterfaceClass =	USB_CLASS_STILL_IMAGE,
	.bInterfaceSubClass =	USB_SC_IMAGE_CAPTURE,
	.bInterfaceProtocol =	USB_PR_CB,
	.iInterface =		STRINGID_INTERFACE,
};

#define MAX_PACKET_SIZE_FS 64
#define MAX_PACKET_SIZE_HS 512

/* Full speed configurations are used for full-speed only devices as
 * well as dual-speed ones (the only kind with high speed support).
 */
static struct usb_endpoint_descriptor fs_source_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	/* NOTE some controllers may need FS bulk max packet size
	 * to be smaller.  it would be a chip-specific option.
	 */
	.wMaxPacketSize =	__constant_cpu_to_le16(MAX_PACKET_SIZE_FS),
};

static struct usb_endpoint_descriptor fs_sink_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	__constant_cpu_to_le16(MAX_PACKET_SIZE_FS),
};

/* some devices can handle other status packet sizes */
#define STATUS_MAXPACKET	8
//#define	LOG2_STATUS_POLL_MSEC	3

static struct usb_endpoint_descriptor fs_status_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	__constant_cpu_to_le16(STATUS_MAXPACKET),
//	.bInterval =		(1 << LOG2_STATUS_POLL_MSEC),
	.bInterval =		10,
};

static const struct usb_endpoint_descriptor *fs_eps[] = {
	&fs_source_desc,
	&fs_sink_desc,
	&fs_status_desc,
};


/* High speed configurations are used only in addition to a full-speed
 * ones ... since all high speed devices support full speed configs.
 * Of course, not all hardware supports high speed configurations.
 */

static struct usb_endpoint_descriptor hs_source_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	__constant_cpu_to_le16(MAX_PACKET_SIZE_HS),
};

static struct usb_endpoint_descriptor hs_sink_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bmAttributes =		USB_ENDPOINT_XFER_BULK,
	.wMaxPacketSize =	__constant_cpu_to_le16(MAX_PACKET_SIZE_HS),
};

static struct usb_endpoint_descriptor hs_status_desc = {
	.bLength =		USB_DT_ENDPOINT_SIZE,
	.bDescriptorType =	USB_DT_ENDPOINT,

	.bmAttributes =		USB_ENDPOINT_XFER_INT,
	.wMaxPacketSize =	__constant_cpu_to_le16(STATUS_MAXPACKET),
//	.bInterval =		LOG2_STATUS_POLL_MSEC + 3,
	.bInterval =		10,
};

static const struct usb_endpoint_descriptor *hs_eps[] = {
	&hs_source_desc,
	&hs_sink_desc,
	&hs_status_desc,
};

/*-------------------------------------------------------------------------*/


static char driver_mfgr[64];

static struct usb_string stringtab[] = {
	{ STRINGID_MFGR,	driver_mfgr, },
	{ STRINGID_PRODUCT,	DRIVER_PRODUCT, },
	{ STRINGID_CONFIG,	DRIVER_CONFIG, },
	{ STRINGID_INTERFACE,	DRIVER_INTERFACE, },
};

static struct usb_gadget_strings strings = {
	.language =	0x0409,		/* "en-us" */
	.strings =	stringtab,
};

/*-------------------------------------------------------------------------*/

/* kernel drivers could autoconfigure like this too ... if
 * they were willing to waste the relevant code/data space.
 */

static int	HIGHSPEED;
static char	*DEVNAME;
static char	*EP_IN_NAME, *EP_OUT_NAME, *EP_STATUS_NAME;

/* gadgetfs currently has no chunking (or O_DIRECT/zerocopy) support
 * to turn big requests into lots of smaller ones; so this is "small".
 */
#define	USB_BUFSIZE	(7 * 1024)

static enum usb_device_speed current_speed;

#define CHECK_COUNT(cnt, min, max, op) do {			\
	if (cnt & 3 || cnt < min || cnt > max) {		\
		fprintf(stderr, "Wrong " op " size: %u\n",	\
			cnt);					\
		errno = EPIPE;					\
		return -1;					\
	}							\
} while (0)

#define CHECK_SESSION(s_container, r_container, cnt, ret) do {	\
	if (session <= 0) {					\
		make_response(s_container, r_container,		\
			PIMA15740_RESP_SESSION_NOT_OPEN,	\
			sizeof(*s_container));			\
		*cnt = 0;					\
		*ret = 0;					\
		break;						\
	}							\
} while (0)

#define THUMB_LOCATION		"/var/cache/ptp/thumb/"
#define STORE_ID		0x00010001

#define PTP_PARAM_UNUSED	0
#define PTP_PARAM_ANY		0xffffffff

/* All little endian */
struct ptp_container {
	uint32_t	length;
	uint16_t	type;
	uint16_t	code;
	uint32_t	id;
	uint8_t		payload[];
} __attribute__ ((packed));

enum ptp_container_type {
	PTP_CONTAINER_TYPE_UNDEFINED		= 0,
	PTP_CONTAINER_TYPE_COMMAND_BLOCK	= 1,
	PTP_CONTAINER_TYPE_DATA_BLOCK		= 2,
	PTP_CONTAINER_TYPE_RESPONSE_BLOCK	= 3,
	PTP_CONTAINER_TYPE_EVENT_BLOCK		= 4,
};

enum pima15740_operation_code {
	PIMA15740_OP_UNDEFINED			= 0x1000,
	PIMA15740_OP_GET_DEVICE_INFO		= 0x1001,
	PIMA15740_OP_OPEN_SESSION		= 0x1002,
	PIMA15740_OP_CLOSE_SESSION		= 0x1003,
	PIMA15740_OP_GET_STORAGE_IDS		= 0x1004,
	PIMA15740_OP_GET_STORAGE_INFO		= 0x1005,
	PIMA15740_OP_GET_NUM_OBJECTS		= 0x1006,
	PIMA15740_OP_GET_OBJECT_HANDLES		= 0x1007,
	PIMA15740_OP_GET_OBJECT_INFO		= 0x1008,
	PIMA15740_OP_GET_OBJECT			= 0x1009,
	PIMA15740_OP_GET_THUMB			= 0x100a,
	PIMA15740_OP_DELETE_OBJECT		= 0x100b,
	PIMA15740_OP_SEND_OBJECT_INFO		= 0x100c,
	PIMA15740_OP_SEND_OBJECT		= 0x100d,
	PIMA15740_OP_INITIATE_CAPTURE		= 0x100e,
	PIMA15740_OP_FORMAT_STORE		= 0x100f,
	PIMA15740_OP_RESET_DEVICE		= 0x1010,
	PIMA15740_OP_SELF_TEST			= 0x1011,
	PIMA15740_OP_SET_OBJECT_PROTECTION	= 0x1012,
	PIMA15740_OP_POWER_DOWN			= 0x1013,
	PIMA15740_OP_GET_DEVICE_PROP_DESC	= 0x1014,
	PIMA15740_OP_GET_DEVICE_PROP_VALUE	= 0x1015,
	PIMA15740_OP_SET_DEVICE_PROP_VALUE	= 0x1016,
	PIMA15740_OP_RESET_DEVICE_PROP_VALUE	= 0x1017,
	PIMA15740_OP_TERMINATE_OPEN_CAPTURE	= 0x1018,
	PIMA15740_OP_MOVE_OBJECT		= 0x1009,
	PIMA15740_OP_COPY_OBJECT		= 0x100a,
	PIMA15740_OP_GET_PARTIAL_OBJECT		= 0x100b,
	PIMA15740_OP_INITIATE_OPEN_CAPTURE	= 0x100c,
};

enum pima15740_response_code {
	PIMA15740_RESP_UNDEFINED				= 0x2000,
	PIMA15740_RESP_OK					= 0x2001,
	PIMA15740_RESP_GENERAL_ERROR				= 0x2002,
	PIMA15740_RESP_SESSION_NOT_OPEN				= 0x2003,
	PIMA15740_RESP_INVALID_TRANSACTION_ID			= 0x2004,
	PIMA15740_RESP_OPERATION_NOT_SUPPORTED			= 0x2005,
	PIMA15740_RESP_PARAMETER_NOT_SUPPORTED			= 0x2006,
	PIMA15740_RESP_INCOMPLETE_TRANSFER			= 0x2007,
	PIMA15740_RESP_INVALID_STORAGE_ID			= 0x2008,
	PIMA15740_RESP_INVALID_OBJECT_HANDLE			= 0x2009,
	PIMA15740_RESP_DEVICE_PROP_NOT_SUPPORTED		= 0x200a,
	PIMA15740_RESP_INVALID_OBJECT_FORMAT_CODE		= 0x200b,
	PIMA15740_RESP_STORE_FULL				= 0x200c,
	PIMA15740_RESP_OBJECT_WRITE_PROTECTED			= 0x200d,
	PIMA15740_RESP_STORE_READ_ONLY				= 0x200e,
	PIMA15740_RESP_ACCESS_DENIED				= 0x200f,
	PIMA15740_RESP_NO_THUMBNAIL_PRESENT			= 0x2010,
	PIMA15740_RESP_SELFTEST_FAILED				= 0x2011,
	PIMA15740_RESP_PARTIAL_DELETION				= 0x2012,
	PIMA15740_RESP_STORE_NOT_AVAILABLE			= 0x2013,
	PIMA15740_RESP_SPECIFICATION_BY_FORMAT_NOT_SUPPORTED	= 0x2014,
	PIMA15740_RESP_NO_VALID_OBJECT_INFO			= 0x2015,
	PIMA15740_RESP_INVALID_CODE_FORMAT			= 0x2016,
	PIMA15740_RESP_UNKNOWN_VENDOR_CODE			= 0x2017,
	PIMA15740_RESP_CAPTURE_ALREADY_TERMINATED		= 0x2018,
	PIMA15740_RESP_DEVICE_BUSY				= 0x2019,
	PIMA15740_RESP_INVALID_PARENT_OBJECT			= 0x201a,
	PIMA15740_RESP_INVALID_DEVICE_PROP_FORMAT		= 0x201b,
	PIMA15740_RESP_INVALID_DEVICE_PROP_VALUE		= 0x201c,
	PIMA15740_RESP_INVALID_PARAMETER			= 0x201d,
	PIMA15740_RESP_SESSION_ALREADY_OPEN			= 0x201e,
	PIMA15740_RESP_TRANSACTION_CANCELLED			= 0x201f,
	PIMA15740_RESP_SPECIFICATION_OF_DESTINATION_UNSUPPORTED	= 0x2020,
};

enum pima15740_data_format {
	PIMA15740_FMT_A_UNDEFINED		= 0x3000,
	PIMA15740_FMT_A_ASSOCIATION		= 0x3001,
	PIMA15740_FMT_I_UNDEFINED		= 0x3800,
	PIMA15740_FMT_I_EXIF_JPEG		= 0x3801,
	PIMA15740_FMT_I_TIFF_EP			= 0x3802,
	PIMA15740_FMT_I_JFIF			= 0x3808,
	PIMA15740_FMT_I_PNG			= 0x380b,
	PIMA15740_FMT_I_TIFF			= 0x380d,
	PIMA15740_FMT_I_TIFF_IT			= 0x380e,
};

enum pima15740_storage_type {
	PIMA15740_STORAGE_UNDEFINED		= 0,
	PIMA15740_STORAGE_FIXED_ROM		= 0x0001,
	PIMA15740_STORAGE_REMOVABLE_ROM		= 0x0002,
	PIMA15740_STORAGE_FIXED_RAM		= 0x0003,
	PIMA15740_STORAGE_REMOVABLE_RAM		= 0x0004,
};

enum pima15740_filesystem_type {
	PIMA15740_FILESYSTEM_UNDEFINED		= 0,
	PIMA15740_FILESYSTEM_GENERIC_FLAT	= 0x0001,
	PIMA15740_FILESYSTEM_GENERIC_HIERARCH	= 0x0002,
	PIMA15740_FILESYSTEM_DCF		= 0x0003,
};

enum pima15740_access_capability {
	PIMA15740_ACCESS_CAP_RW			= 0,
	PIMA15740_ACCESS_CAP_RO_WITHOUT_DEL	= 0x0001,
	PIMA15740_ACCESS_CAP_RO_WITH_DEL	= 0x0002,
};

static const char manuf[] = PTP_MANUFACTURER;
static const char model[] = PTP_MODEL;
static const char storage_desc[] = PTP_STORAGE_DESC;

#define SUPPORTED_OPERATIONS					\
	__constant_cpu_to_le16(PIMA15740_OP_GET_DEVICE_INFO),	\
	__constant_cpu_to_le16(PIMA15740_OP_OPEN_SESSION),	\
	__constant_cpu_to_le16(PIMA15740_OP_CLOSE_SESSION),	\
	__constant_cpu_to_le16(PIMA15740_OP_GET_STORAGE_IDS),	\
	__constant_cpu_to_le16(PIMA15740_OP_GET_STORAGE_INFO),	\
	__constant_cpu_to_le16(PIMA15740_OP_GET_NUM_OBJECTS),	\
	__constant_cpu_to_le16(PIMA15740_OP_GET_OBJECT_HANDLES),\
	__constant_cpu_to_le16(PIMA15740_OP_GET_OBJECT_INFO),	\
	__constant_cpu_to_le16(PIMA15740_OP_GET_OBJECT),	\
	__constant_cpu_to_le16(PIMA15740_OP_GET_THUMB),		\
	__constant_cpu_to_le16(PIMA15740_OP_DELETE_OBJECT),

static uint16_t dummy_supported_operations[] = {
	SUPPORTED_OPERATIONS
};

#define SUPPORTED_FORMATS					\
	__constant_cpu_to_le16(PIMA15740_FMT_I_EXIF_JPEG),	\
	__constant_cpu_to_le16(PIMA15740_FMT_I_TIFF_EP),	\
	__constant_cpu_to_le16(PIMA15740_FMT_I_PNG),		\
	__constant_cpu_to_le16(PIMA15740_FMT_I_TIFF),		\
	__constant_cpu_to_le16(PIMA15740_FMT_I_TIFF_IT),	\
	__constant_cpu_to_le16(PIMA15740_FMT_I_JFIF),

static uint16_t dummy_supported_formats[] = {
	SUPPORTED_FORMATS
};

struct my_device_info {
	uint16_t	std_ver;
	uint32_t	vendor_ext_id;
	uint16_t	vendor_ext_ver;
	uint8_t		vendor_ext_desc_len;
	uint16_t	func_mode;
	uint32_t	operations_n;
	uint16_t	operations[ARRAY_SIZE(dummy_supported_operations)];
	uint32_t	events_n;
	uint32_t	device_properties_n;
	uint32_t	capture_formats_n;
	uint32_t	image_formats_n;
	uint16_t	image_formats[ARRAY_SIZE(dummy_supported_formats)];
	uint8_t		manuf_len;
	uint8_t		manuf[sizeof(manuf) * 2];
	uint8_t		model_len;
	uint8_t		model[sizeof(model) * 2];
	uint8_t		dev_version_len;
	uint8_t		serial_num_len;
} __attribute__ ((packed));

struct my_device_info dev_info = {
	.std_ver		= __constant_cpu_to_le16(100),	/* Standard version 1.00 */
	.vendor_ext_id		= __constant_cpu_to_le32(0),
	.vendor_ext_ver		= __constant_cpu_to_le16(0),
	.vendor_ext_desc_len	= __constant_cpu_to_le16(0),
	.func_mode		= __constant_cpu_to_le16(0),
	.operations_n		= __constant_cpu_to_le32(ARRAY_SIZE(dummy_supported_operations)),
	.operations = {
		SUPPORTED_OPERATIONS
	},
	.events_n		= __constant_cpu_to_le32(0),
	.device_properties_n	= __constant_cpu_to_le32(0),
	.capture_formats_n	= __constant_cpu_to_le32(0),
	.image_formats_n	= __constant_cpu_to_le32(ARRAY_SIZE(dummy_supported_formats)),
	.image_formats = {
		SUPPORTED_FORMATS
	},
	.manuf_len = sizeof(manuf),
	.model_len = sizeof(model),
	.dev_version_len = 0,
	.serial_num_len	= 0,
};

struct my_storage_info {
	uint16_t	storage_type;
	uint16_t	filesystem_type;
	uint16_t	access_capability;
	uint64_t	max_capacity;
	uint64_t	free_space_in_bytes;
	uint32_t	free_space_in_images;
	uint8_t		desc_len;
	uint8_t		desc[sizeof(storage_desc) * 2];
	uint8_t		volume_label_len;
} __attribute__ ((packed));

static struct my_storage_info storage_info = {
	.storage_type		= __constant_cpu_to_le16(PIMA15740_STORAGE_REMOVABLE_RAM),
	.filesystem_type	= __constant_cpu_to_le16(PIMA15740_FILESYSTEM_DCF),
	.access_capability	= __constant_cpu_to_le16(PIMA15740_ACCESS_CAP_RW),
	.desc_len		= sizeof(storage_desc),
	.volume_label_len	= 0,
};

/* full duplex data, with at least three threads: ep0, sink, and source */

static int bulk_in = -ENXIO;
static int bulk_out = -ENXIO;
static int control = -ENXIO;
static int interrupt = -ENXIO;
static int session = -EINVAL;
static sem_t reset;

static iconv_t ic;
static char *root;

#define	NEVENT		5

enum ptp_status {
	PTP_WAITCONFIG,	/* Waiting to be configured */
	PTP_IDLE,	/* Waiting for control / bulk-out */
	PTP_DATA_OUT,	/* Data arrival on bulk-out expected */
	PTP_DATA_READY,	/* Finished receive, have to process and send (Data and) Response */
	PTP_DATA_IN,	/* Waiting for bulk-in to become free for more data */
};

static enum ptp_status status = PTP_WAITCONFIG;

static pthread_t bulk_pthread;

#define __stringify_1(x)	#x
#define __stringify(x)		__stringify_1(x)

#define BUF_SIZE	4096
#define THUMB_WIDTH	160
#define THUMB_HEIGHT	120
#define THUMB_SIZE	__stringify(THUMB_WIDTH) "x" __stringify(THUMB_HEIGHT)

struct ptp_object_info {
	uint32_t	storage_id;
	uint16_t	object_format;
	uint16_t	protection_status;
	uint32_t	object_compressed_size;
	uint16_t	thumb_format;
	uint32_t	thumb_compressed_size;
	uint32_t	thumb_pix_width;
	uint32_t	thumb_pix_height;
	uint32_t	image_pix_width;
	uint32_t	image_pix_height;
	uint32_t	image_bit_depth;
	uint32_t	parent_object;
	uint16_t	association_type;
	uint32_t	association_desc;
	uint32_t	sequence_number;
	uint8_t		strings[];
} __attribute__ ((packed));

static struct ptp_object_info association = {
	.storage_id		= __constant_cpu_to_le32(STORE_ID),
	.object_format		= __constant_cpu_to_le16(PIMA15740_FMT_A_ASSOCIATION),
	.protection_status	= __constant_cpu_to_le16(0),	/* Read-only */
	.object_compressed_size	= __constant_cpu_to_le32(4096),
	.thumb_format		= __constant_cpu_to_le16(0),
	.thumb_compressed_size	= __constant_cpu_to_le32(0),
	.thumb_pix_width	= __constant_cpu_to_le32(0),
	.thumb_pix_height	= __constant_cpu_to_le32(0),
	.image_pix_width	= __constant_cpu_to_le32(0),
	.image_pix_height	= __constant_cpu_to_le32(0),
	.image_bit_depth	= __constant_cpu_to_le32(0),
	.parent_object		= __constant_cpu_to_le32(0),	/* Will be overwritten */
	.association_type	= __constant_cpu_to_le16(1),	/* Generic Folder */
	.association_desc	= __constant_cpu_to_le32(0),
	.sequence_number	= __constant_cpu_to_le32(0),
};

struct obj_list {
	struct obj_list		*next;
	uint32_t		handle;
	size_t			info_size;
	char			name[256];
	struct ptp_object_info	info;
};

static struct obj_list *images;
/* number of objects, including associations - decrement when deleting */
static int object_number;

static size_t put_string(iconv_t ic, char *buf, const char *s, size_t len);

static int object_handle_valid(unsigned int h)
{
	struct obj_list *obj;

	/* First two handles: dcim and PTP_MODEL_DIR */
	if (h == 1 || h == 2)
		return 1;

	for (obj = images; obj; obj = obj->next)
		if (obj->handle == h)
			return 1;

	return 0;
}

static int autoconfig(void)
{
	struct stat	statb;
	struct utsname	uts;
	int ret;

	/* NetChip 2280 PCI device or dummy_hcd, high/full speed */
	if (stat(DEVNAME = "net2280", &statb) == 0 ||
			stat(DEVNAME = "dummy_udc", &statb) == 0) {
		HIGHSPEED = 1;

		fs_source_desc.bEndpointAddress
			= hs_source_desc.bEndpointAddress
			= USB_DIR_IN | 7;
		EP_IN_NAME = "ep-a";
		fs_sink_desc.bEndpointAddress = hs_sink_desc.bEndpointAddress
			= USB_DIR_OUT | 3;
		EP_OUT_NAME = "ep-b";

		source_sink_intf.bNumEndpoints = 3;
		fs_status_desc.bEndpointAddress
			= hs_status_desc.bEndpointAddress
			= USB_DIR_IN | 11;
		EP_STATUS_NAME = "ep-f";

	/* Intel PXA 2xx processor, full speed only */
	} else if (stat(DEVNAME = "pxa2xx_udc", &statb) == 0) {
		HIGHSPEED = 0;

		fs_source_desc.bEndpointAddress = USB_DIR_IN | 6;
		EP_IN_NAME = "ep6in-bulk";
		fs_sink_desc.bEndpointAddress = USB_DIR_OUT | 7;
		EP_OUT_NAME = "ep7out-bulk";

		/* using bulk for this since the pxa interrupt endpoints
		 * always use the no-toggle scheme (discouraged).
		 */
		source_sink_intf.bNumEndpoints = 3;
		fs_status_desc.bEndpointAddress = USB_DIR_IN | 11;
		EP_STATUS_NAME = "ep11in-bulk";
#if 0
	/* AMD au1x00 processor, full speed only */
	} else if (stat(DEVNAME = "au1x00_udc", &statb) == 0) {
		HIGHSPEED = 0;

		fs_source_desc.bEndpointAddress = USB_DIR_IN | 2;
		EP_IN_NAME = "ep2in";
		fs_sink_desc.bEndpointAddress = USB_DIR_OUT | 4;
		EP_OUT_NAME = "ep4out";

		source_sink_intf.bNumEndpoints = 3;
		fs_status_desc.bEndpointAddress = USB_DIR_IN | 3;
		EP_STATUS_NAME = "ep3in";

	/* Intel SA-1100 processor, full speed only */
	} else if (stat(DEVNAME = "sa1100", &statb) == 0) {
		HIGHSPEED = 0;

		fs_source_desc.bEndpointAddress = USB_DIR_IN | 2;
		EP_IN_NAME = "ep2in-bulk";
		fs_sink_desc.bEndpointAddress = USB_DIR_OUT | 1;
		EP_OUT_NAME = "ep1out-bulk";

		source_sink_intf.bNumEndpoints = 2;
		EP_STATUS_NAME = 0;
#endif

	/* Toshiba TC86c001 PCI device, full speed only */
	} else if (stat(DEVNAME = "goku_udc", &statb) == 0) {
		HIGHSPEED = 0;

		fs_source_desc.bEndpointAddress = USB_DIR_IN | 2;
		EP_IN_NAME = "ep2-bulk";
		fs_sink_desc.bEndpointAddress = USB_DIR_OUT | 1;
		EP_OUT_NAME = "ep1-bulk";

		source_sink_intf.bNumEndpoints = 3;
		fs_status_desc.bEndpointAddress = USB_DIR_IN | 3;
		EP_STATUS_NAME = "ep3-bulk";

	/* Renesas SH77xx processors, full speed only */
	} else if (stat(DEVNAME = "sh_udc", &statb) == 0) {
		HIGHSPEED = 0;

		fs_source_desc.bEndpointAddress = USB_DIR_IN | 2;
		EP_IN_NAME = "ep2in-bulk";
		fs_sink_desc.bEndpointAddress = USB_DIR_OUT | 1;
		EP_OUT_NAME = "ep1out-bulk";

		source_sink_intf.bNumEndpoints = 3;
		fs_status_desc.bEndpointAddress = USB_DIR_IN | 3;
		EP_STATUS_NAME = "ep3in-bulk";

	/* OMAP 1610 and newer devices, full speed only, fifo mode 0 or 3 */
	} else if (stat(DEVNAME = "omap_udc", &statb) == 0) {
		HIGHSPEED = 0;

		fs_source_desc.bEndpointAddress = USB_DIR_IN | 1;
		EP_IN_NAME = "ep1in-bulk";
		fs_sink_desc.bEndpointAddress = USB_DIR_OUT | 2;
		EP_OUT_NAME = "ep2out-bulk";

		source_sink_intf.bNumEndpoints = 3;
		fs_status_desc.bEndpointAddress = USB_DIR_IN | 3;
		EP_STATUS_NAME = "ep3in-int";

	/* Something based on Mentor USB Highspeed Dual-Role Controller */
	} else if (stat(DEVNAME = "musb_hdrc", &statb) == 0) {
		HIGHSPEED = 1;

		fs_source_desc.bEndpointAddress
			= hs_source_desc.bEndpointAddress
			= USB_DIR_IN | 1;
		EP_IN_NAME = "ep1in";
		fs_sink_desc.bEndpointAddress = hs_sink_desc.bEndpointAddress
			= USB_DIR_OUT | 1;
		EP_OUT_NAME = "ep1out";

		source_sink_intf.bNumEndpoints = 3;
		fs_status_desc.bEndpointAddress
			= hs_status_desc.bEndpointAddress
			= USB_DIR_IN | 3;
		EP_STATUS_NAME = "ep3in";

	/* Atmel AT91 processors, full speed only */
	} else if (stat(DEVNAME = "at91_udc", &statb) == 0) {
		HIGHSPEED = 0;

		fs_source_desc.bEndpointAddress = USB_DIR_IN | 1;
		EP_IN_NAME = "ep1";
		fs_sink_desc.bEndpointAddress = USB_DIR_OUT | 2;
		EP_OUT_NAME = "ep2";

		source_sink_intf.bNumEndpoints = 3;
		fs_status_desc.bEndpointAddress = USB_DIR_IN | 3;
		EP_STATUS_NAME = "ep3-int";

	/* Sharp LH740x processors, full speed only */
	} else if (stat(DEVNAME = "lh740x_udc", &statb) == 0) {
		HIGHSPEED = 0;

		fs_source_desc.bEndpointAddress = USB_DIR_IN | 1;
		EP_IN_NAME = "ep1in-bulk";
		fs_sink_desc.bEndpointAddress = USB_DIR_OUT | 2;
		EP_OUT_NAME = "ep2out-bulk";

		source_sink_intf.bNumEndpoints = 3;
		fs_status_desc.bEndpointAddress = USB_DIR_IN | 3;
		EP_STATUS_NAME = "ep3in-int";

	/* Atmel AT32AP700x processors, high/full speed */
	} else if (stat(DEVNAME = "atmel_usba_udc", &statb) == 0) {
		HIGHSPEED = 1;

		fs_source_desc.bEndpointAddress
			= hs_source_desc.bEndpointAddress
			= USB_DIR_IN | 1;
		EP_IN_NAME = "ep1in-bulk";
		fs_sink_desc.bEndpointAddress
			= hs_sink_desc.bEndpointAddress
			= USB_DIR_OUT | 2;
		EP_OUT_NAME = "ep2out-bulk";

		source_sink_intf.bNumEndpoints = 3;
		fs_status_desc.bEndpointAddress
			= hs_status_desc.bEndpointAddress
			= USB_DIR_IN | 3;
		EP_STATUS_NAME = "ep3in-int";

	/* Freescale i.MX31 SoC, high/full speed */
	} else if (stat(DEVNAME = "fsl-usb2-udc", &statb) == 0) {
		HIGHSPEED = 1;

		fs_source_desc.bEndpointAddress
			= hs_source_desc.bEndpointAddress
			= USB_DIR_IN | 1;
		EP_IN_NAME = "ep1in";
		fs_sink_desc.bEndpointAddress
			= hs_sink_desc.bEndpointAddress
			= USB_DIR_OUT | 1;
		EP_OUT_NAME = "ep1out";

		source_sink_intf.bNumEndpoints = 3;
		fs_status_desc.bEndpointAddress
			= hs_status_desc.bEndpointAddress
			= USB_DIR_IN | 2;
		EP_STATUS_NAME = "ep2in";
	} else if (stat(DEVNAME = "arc_udc", &statb) == 0) {
		HIGHSPEED = 1;

		fs_source_desc.bEndpointAddress
			= hs_source_desc.bEndpointAddress
			= USB_DIR_IN | 1;
		EP_IN_NAME = "ep1in";
		fs_sink_desc.bEndpointAddress
			= hs_sink_desc.bEndpointAddress
			= USB_DIR_OUT | 1;
		EP_OUT_NAME = "ep1out";

		source_sink_intf.bNumEndpoints = 3;
		fs_status_desc.bEndpointAddress
			= hs_status_desc.bEndpointAddress
			= USB_DIR_IN | 2;
		EP_STATUS_NAME = "ep2in";
	} else {
		DEVNAME = 0;
		return -ENODEV;
	}

	ret = uname(&uts);
	snprintf(driver_mfgr, sizeof(driver_mfgr), DRIVER_MFGR " %s with %s",
		 ret ? "unknown" : uts.release, DEVNAME);

	return 0;
}

/*-------------------------------------------------------------------------*/

/* you should be able to open and configure endpoints
 * whether or not the host is connected
 */
static int ep_config(char *name,
		     struct usb_endpoint_descriptor *fs,
		     struct usb_endpoint_descriptor *hs)
{
	int	fd, err;
	char	buf[USB_BUFSIZE];

	/* open and initialize with endpoint descriptor(s) */
	fd = open(name, O_RDWR);
	if (fd < 0) {
		err = -errno;
		fprintf(stderr, "open %s error %d (%s)\n",
			name, errno, strerror(errno));
		return err;
	}

	/* one (fs or ls) or two (fs + hs) sets of config descriptors */
	*(uint32_t *)buf = 1;	/* tag for this format */
	memcpy(buf + 4, fs, USB_DT_ENDPOINT_SIZE);
	if (HIGHSPEED)
		memcpy(buf + 4 + USB_DT_ENDPOINT_SIZE,
			hs, USB_DT_ENDPOINT_SIZE);
	err = write(fd, buf, 4 + USB_DT_ENDPOINT_SIZE
			+ (HIGHSPEED ? USB_DT_ENDPOINT_SIZE : 0));
	if (err < 0) {
		err = -errno;
		fprintf(stderr, "config %s error %d (%s)\n",
			name, errno, strerror(errno));
		close(fd);
		return err;
	} else if (verbose)
		fprintf(stderr, "%s start fd %d\n", name, fd);

	return fd;
}

#define source_open(name) \
	ep_config(name, &fs_source_desc, &hs_source_desc)
#define sink_open(name) \
	ep_config(name, &fs_sink_desc, &hs_sink_desc)
#define int_open(name) \
	ep_config(name, &fs_status_desc, &hs_status_desc)

/*-------------------------------------------------------------------------*/

static char *build_config(char *cp, const struct usb_endpoint_descriptor **ep)
{
	struct usb_config_descriptor *c;
	int i;

	c = (struct usb_config_descriptor *)cp;

	memcpy(cp, &config, config.bLength);
	cp += config.bLength;
	memcpy(cp, &source_sink_intf, source_sink_intf.bLength);
	cp += source_sink_intf.bLength;

	for (i = 0; i < source_sink_intf.bNumEndpoints; i++) {
		memcpy(cp, ep[i], USB_DT_ENDPOINT_SIZE);
		cp += USB_DT_ENDPOINT_SIZE;
	}
	c->wTotalLength = __cpu_to_le16(cp - (char *)c);
	return cp;
}

static void init_device(void)
{
	char		buf[4096], *cp = buf;
	int		err;

	err = autoconfig();
	if (err < 0) {
		fprintf(stderr, "?? don't recognize /dev/gadget bulk device\n");
		control = err;
		return;
	}

	control = open(DEVNAME, O_RDWR);
	if (control < 0) {
		perror(DEVNAME);
		control = -errno;
		return;
	}

	*(uint32_t *)cp = 0;	/* tag for this format */
	cp += 4;

	/* write full then high speed configs */
	cp = build_config(cp, fs_eps);
	if (HIGHSPEED)
		cp = build_config(cp, hs_eps);

	/* and device descriptor at the end */
	memcpy(cp, &device_desc, sizeof device_desc);
	cp += sizeof device_desc;

	err = write(control, buf, cp - buf);
	if (err < 0) {
		perror("write dev descriptors");
		close(control);
		control = -errno;
		return;
	} else if (err != cp - buf) {
		fprintf(stderr, "dev init, wrote %d expected %d\n",
				err, cp - buf);
		close(control);
		control = -errno;
		return;
	}
	return;
}

static const char *speed(enum usb_device_speed s)
{
	switch (s) {
	case USB_SPEED_LOW:	return "low speed";
	case USB_SPEED_FULL:	return "full speed";
	case USB_SPEED_HIGH:	return "high speed";
	default:		return "UNKNOWN speed";
	}
}

/*-------------------------------------------------------------------------*/

static void make_response(struct ptp_container *s_cntn, struct ptp_container *r_cntn,
			  enum pima15740_response_code code, size_t len)
{
	s_cntn->id = r_cntn->id;
	s_cntn->type = __cpu_to_le16(PTP_CONTAINER_TYPE_RESPONSE_BLOCK);
	s_cntn->code = __cpu_to_le16(code);
	s_cntn->length = __cpu_to_le32(len);
}

static int bulk_write(void *buf, size_t length)
{
	size_t count = 0;
	int ret;

	do {
		ret = write(bulk_in, buf + count, length - count);
		if (ret < 0) {
			if (errno != EINTR)
				return ret;

			/* Need to wait for control thread to finish reset */
			sem_wait(&reset);
		} else
			count += ret;
	} while (count < length);

	if (verbose)
		fprintf(stderr, "BULK-IN Sent %u bytes\n", count);

	return count;
}

static int send_association_handle(int n, struct ptp_container *s)
{
	uint32_t *handle;

	s->type = __cpu_to_le16(PTP_CONTAINER_TYPE_DATA_BLOCK);
	/* One array element */
	*(uint32_t *)s->payload = __cpu_to_le32(1);
	s->length = __cpu_to_le32(2 * sizeof(uint32_t) + sizeof(*s));

	handle = (uint32_t *)s->payload + 1;
	/* The next directory */
	*handle = __cpu_to_le32(n);
	return bulk_write(s, (void *)(handle + 1) - (void *)s);
}

static int send_object_handles(void *recv_buf, void *send_buf, size_t send_len)
{
	struct ptp_container *r_container = recv_buf;
	struct ptp_container *s_container = send_buf;
	unsigned long length;
	uint32_t *param;
	uint32_t store_id;
	struct obj_list *obj;
	int ret;
	uint32_t *handle;
	uint32_t format, association;
	int obj_to_send = object_number;

	length	= __le32_to_cpu(r_container->length);

	param = (uint32_t *)r_container->payload;
	store_id = __le32_to_cpu(*param);

	/* supported storage IDs: 0x00010001 - our single storage, 0xffffffff - all stores */
	if (store_id != STORE_ID && store_id != PTP_PARAM_ANY) {
		make_response(s_container, r_container,
			      PIMA15740_RESP_INVALID_STORAGE_ID, sizeof(*s_container));
		return 0;
	}

	format = __le32_to_cpu(*(param + 1));
	if (length > 16 && format != PTP_PARAM_UNUSED && format != PTP_PARAM_ANY) {
		make_response(s_container, r_container,
			      PIMA15740_RESP_SPECIFICATION_BY_FORMAT_NOT_SUPPORTED,
			      sizeof(*s_container));
		return 0;
	}

	association = __le32_to_cpu(*(param + 2));
	if (length > 20 && association != PTP_PARAM_UNUSED && association != 2) {
		enum pima15740_response_code code;
		if (!object_handle_valid(association) && association != PTP_PARAM_ANY)
			code = PIMA15740_RESP_INVALID_OBJECT_HANDLE;
		else if (association == PTP_PARAM_ANY) {
			/* "/" is requested */
			ret = send_association_handle(1, s_container);
			if (ret < 0) {
				errno = EPIPE;
				return ret;
			} else
				code = PIMA15740_RESP_OK;
		} else if (association == 1) {
			/* The subdirectory of "/DCIM" is requested */
			ret = send_association_handle(2, s_container);
			if (ret < 0) {
				errno = EPIPE;
				return ret;
			} else
				code = PIMA15740_RESP_OK;
		} else
			code = PIMA15740_RESP_INVALID_PARENT_OBJECT;

		make_response(s_container, r_container, code, sizeof(*s_container));
		return 0;
	}

	if (association == 2)
		/* Only send contents of /DCIM/100LINUX */
		obj_to_send -= 2;

	s_container->type = __cpu_to_le16(PTP_CONTAINER_TYPE_DATA_BLOCK);
	*(uint32_t *)s_container->payload = __cpu_to_le32(obj_to_send);
	s_container->length = __cpu_to_le32((obj_to_send + 1) * sizeof(uint32_t) +
					    sizeof(*s_container));

	handle = (uint32_t *)s_container->payload + 1;

	if (association != 2) {
		/* The two directories */
		*handle++ = __cpu_to_le32(1);
		*handle++ = __cpu_to_le32(2);
	}

	for (obj = images; obj; obj = obj->next) {
		if ((void *)handle == send_buf + send_len) {
			ret = bulk_write(send_buf, send_len);
			if (ret < 0) {
				errno = EPIPE;
				return ret;
			}
			handle = send_buf;
		}

		*handle++ = __cpu_to_le32(obj->handle);
	}
	if ((void *)handle > send_buf) {
		ret = bulk_write(send_buf, (void *)handle - send_buf);
		if (ret < 0) {
			errno = EPIPE;
			return ret;
		}
	}

	/* Prepare response */
	make_response(s_container, r_container, PIMA15740_RESP_OK, sizeof(*s_container));

	return 0;
}

static int send_association(int n, struct ptp_container *s, size_t size)
{
	struct ptp_object_info *objinfo = (struct ptp_object_info *)s->payload;
	size_t len, total;
	int ret;

	s->type = __cpu_to_le16(PTP_CONTAINER_TYPE_DATA_BLOCK);
	memcpy(objinfo, &association, sizeof(association));
	objinfo->object_compressed_size = __cpu_to_le32(size);
	switch (n) {
	case 1:
		len = strlen("DCIM") + 1;
		ret = put_string(ic, (char *)objinfo->strings + 1, "DCIM", len);
		objinfo->parent_object = __cpu_to_le32(0);
		break;
	case 2:
		len = strlen(PTP_MODEL_DIR) + 1;
		ret = put_string(ic, (char *)objinfo->strings + 1, PTP_MODEL_DIR, len);
		objinfo->parent_object = __cpu_to_le32(1);
		break;
	}
	if (ret < 0)
		return ret;
	objinfo->strings[0] = len;
	objinfo->strings[2 * len + 1] = 0;	/* Empty Capture Date */
	objinfo->strings[2 * len + 2] = 0;	/* Empty Modification Date */
	objinfo->strings[2 * len + 3] = 0;	/* Empty Keywords */
	total = 2 * len + 4 + sizeof(*s) + sizeof(*objinfo);
	s->length = __cpu_to_le32(total);

	return bulk_write(s, total);
}

static int send_object_info(void *recv_buf, void *send_buf, size_t send_len)
{
	struct ptp_container *r_container = recv_buf;
	struct ptp_container *s_container = send_buf;
	uint32_t *param;
	struct obj_list *obj;
	int ret;
	uint32_t handle;
	size_t count, total, offset;
	void *info;
	enum pima15740_response_code code = PIMA15740_RESP_OK;

	param = (uint32_t *)r_container->payload;
	handle = __le32_to_cpu(*param);

	if (handle == 1 || handle == 2) {
		struct stat dstat;
		size_t size;

		/* Directory information requested */
		if (handle == 2) {
			ret = stat(root, &dstat);
			if (ret < 0) {
				errno = EPIPE;
				return ret;
			}
			size = dstat.st_size;
			if (verbose > 1)
				fprintf(stderr, "%s size %u\n", root, size);
		} else
			size = 4096;
		ret = send_association(handle, s_container, size);
		if (ret < 0) {
			errno = EPIPE;
			return ret;
		}

		goto send_resp;
	}

	for (obj = images; obj; obj = obj->next)
		if (obj->handle == handle)
			break;

	if (!obj) {
		code = PIMA15740_RESP_INVALID_OBJECT_HANDLE;
		goto send_resp;
	}

	s_container->type = __cpu_to_le16(PTP_CONTAINER_TYPE_DATA_BLOCK);
	total = obj->info_size + sizeof(*s_container);
	s_container->length = __cpu_to_le32(total);
	offset = sizeof(*s_container);
	info = &obj->info;

	/* Object Info cannot get > 4096 bytes - four strings make a maximum of 2048
	 * bytes plus a fixed-size block, but we play safe for the case someone
	 * makes the buffers smaller */

	while (total) {
		count = min(total, send_len);
		memcpy(send_buf + offset, info, count - offset);
		info += count - offset;
		ret = bulk_write(send_buf, count);
		if (ret < 0) {
			errno = EPIPE;
			return ret;
		}
		offset = 0;
		total -= count;
	}

send_resp:
	/* Prepare response */
	make_response(s_container, r_container, code, sizeof(*s_container));

	return 0;
}

static int send_object_or_thumb(void *recv_buf, void *send_buf, size_t send_len, int thumb)
{
	struct ptp_container *r_container = recv_buf;
	struct ptp_container *s_container = send_buf;
	uint32_t *param;
	struct obj_list *obj;
	int ret;
	uint32_t handle;
	size_t count, total, offset, file_size;
	void *data, *map;
	int fd;
	char name[256];

	param = (uint32_t *)r_container->payload;
	handle = __le32_to_cpu(*param);

	for (obj = images; obj; obj = obj->next)
		if (obj->handle == handle)
			break;

	if (!obj) {
		make_response(s_container, r_container, PIMA15740_RESP_INVALID_OBJECT_HANDLE,
			      sizeof(*s_container));
		return 0;
	}

	s_container->type = __cpu_to_le16(PTP_CONTAINER_TYPE_DATA_BLOCK);
	offset = sizeof(*s_container);

	if (!thumb) {
		strncpy(name, obj->name, sizeof(name) - 1);
		name[sizeof(name) - 1] = '\0';
		ret = chdir(root);
		file_size = __le32_to_cpu(obj->info.object_compressed_size);
	} else {
		char *dot = strrchr(obj->name, '.');
		*dot = '\0';			/* We know there is a dot in the name */
		snprintf(name, sizeof(name) - 1, "%s.thumb.jpeg", obj->name);
		*dot = '.';
		name[sizeof(name) - 1] = '\0';
		ret = chdir(THUMB_LOCATION);
		file_size = __le32_to_cpu(obj->info.thumb_compressed_size);
	}

	total = file_size + sizeof(*s_container);
	if (verbose)
		fprintf(stderr, "%s(): total %d\n", __func__, total);
	s_container->length = __cpu_to_le32(total);

	if (!ret)
		fd = open(name, O_RDONLY);
	if (ret < 0 || fd < 0) {
		make_response(s_container, r_container, PIMA15740_RESP_INCOMPLETE_TRANSFER,
			      sizeof(*s_container));
		return 0;
	}

	map = mmap(NULL, file_size, PROT_READ, MAP_SHARED, fd, 0);
	if (map == MAP_FAILED) {
		close(fd);
		make_response(s_container, r_container, PIMA15740_RESP_INCOMPLETE_TRANSFER,
			      sizeof(*s_container));
		return 0;
	}

	count = min(total, send_len);
	memcpy(send_buf + offset, map, count - offset);
	ret = bulk_write(send_buf, count);
	if (ret < 0) {
		errno = EPIPE;
		goto out;
	}
	total -= count;
	data = map + count - offset;
	send_len = 8 * 1024;

	while (total) {
		count = min(total, send_len);
		ret = bulk_write(data, count);
		if (ret < 0) {
			errno = EPIPE;
			goto out;
		}
		total -= count;
		data += count;
	}
	ret = 0;

out:
	munmap(map, file_size);
	close(fd);

	if (!ret)
		/* Prepare response */
		make_response(s_container, r_container, PIMA15740_RESP_OK, sizeof(*s_container));

	return ret;
}

static int send_storage_ids(void *recv_buf, void *send_buf, size_t send_len)
{
	struct ptp_container *s_container = send_buf;
	uint32_t *param;
	int ret;

	s_container->type = __cpu_to_le16(PTP_CONTAINER_TYPE_DATA_BLOCK);
	s_container->length = __cpu_to_le32(20);
	param = (uint32_t *)s_container->payload;

	*param = __cpu_to_le32(1);
	*(param + 1) = __cpu_to_le32(STORE_ID);
	ret = bulk_write(send_buf, 20);
	if (ret < 0) {
		errno = EPIPE;
		return ret;
	}

	/* Prepare response */
	memcpy(send_buf, recv_buf, sizeof(*s_container));
	s_container->type = __cpu_to_le16(PTP_CONTAINER_TYPE_RESPONSE_BLOCK);
	s_container->code = __cpu_to_le16(PIMA15740_RESP_OK);
	s_container->length = __cpu_to_le32(sizeof(*s_container));

	return 0;
}

static int send_storage_info(void *recv_buf, void *send_buf, size_t send_len)
{
	struct ptp_container *r_container = recv_buf;
	struct ptp_container *s_container = send_buf;
	uint32_t *param;
	uint32_t store_id;
	unsigned long long bytes;
	int ret;
	size_t count;
	struct statfs fs;

	param = (uint32_t *)r_container->payload;
	store_id = __le32_to_cpu(*param);

	if (verbose)
		fprintf(stderr, "%u bytes storage info\n", sizeof(storage_info));

	if (store_id != STORE_ID) {
		make_response(s_container, r_container,
			      PIMA15740_RESP_INVALID_STORAGE_ID, sizeof(*s_container));
		return 0;
	}

	ret = statfs(root, &fs);
	if (ret < 0) {
		make_response(s_container, r_container,
			      PIMA15740_RESP_ACCESS_DENIED, sizeof(*s_container));
		return 0;
	}

	if (verbose > 1)
		fprintf(stderr, "Block-size %d, total 0x%lx, free 0x%lx\n",
			fs.f_bsize, fs.f_blocks, fs.f_bfree);

	count = sizeof(storage_info) + sizeof(*s_container);

	s_container->type	= __cpu_to_le16(PTP_CONTAINER_TYPE_DATA_BLOCK);
	s_container->length	= __cpu_to_le32(count);

	bytes					= (unsigned long long)fs.f_bsize * fs.f_blocks;
	storage_info.max_capacity		= __cpu_to_le64(bytes);
	bytes					= (unsigned long long)fs.f_bsize * fs.f_bfree;
	storage_info.free_space_in_bytes	= __cpu_to_le64(bytes);
	storage_info.free_space_in_images	= __cpu_to_le32(PTP_PARAM_ANY);

	memcpy(send_buf + sizeof(*s_container), &storage_info, sizeof(storage_info));
	ret = bulk_write(s_container, count);
	if (ret < 0) {
		errno = EPIPE;
		return ret;
	}

	/* Prepare response */
	make_response(s_container, r_container, PIMA15740_RESP_OK, sizeof(*s_container));

	return 0;
}

static void delete_thumb(struct obj_list *obj)
{
	char thumb[256];
	char *dot;

	if (__le16_to_cpu(obj->info.thumb_format) != PIMA15740_FMT_I_JFIF)
		return;

	dot = strrchr(obj->name, '.');
	if (!dot || dot == obj->name)
		return;

	*dot = 0;
	snprintf(thumb, sizeof(thumb), THUMB_LOCATION "%s.thumb.jpeg",
		 obj->name);
	*dot = '.';

	if (unlink(thumb))
		fprintf(stderr, "Cannot delete %s: %s\n",
			thumb, strerror(errno));
}

static enum pima15740_response_code delete_file(const char *name)
{
	struct stat st;
	int ret;
	uid_t euid;
	gid_t egid;

	/* access() is unreliable on NFS, we use stat() instead */
	ret = stat(name, &st);
	if (ret < 0) {
		fprintf(stderr, "Cannot stat %s: %s\n", name, strerror(errno));
		return PIMA15740_RESP_GENERAL_ERROR;
	}

	euid = geteuid();
	if (euid == st.st_uid) {
		if (!(st.st_mode & S_IWUSR))
			return PIMA15740_RESP_OBJECT_WRITE_PROTECTED;
		goto del;
	}

	egid = getegid();
	if (egid == st.st_gid) {
		if (!(st.st_mode & S_IWGRP))
			return PIMA15740_RESP_OBJECT_WRITE_PROTECTED;
		goto del;
	}

	if (!(st.st_mode & S_IWOTH))
		return PIMA15740_RESP_OBJECT_WRITE_PROTECTED;

del:
	ret = unlink(name);
	if (ret) {
		fprintf(stderr, "Cannot delete %s: %s\n",
			name, strerror(errno));
		return PIMA15740_RESP_GENERAL_ERROR;
	}

	return PIMA15740_RESP_OK;
}

static int update_free_space(void)
{
	unsigned long long bytes;
	struct statfs fs;
	int ret;

	ret = statfs(root, &fs);
	if (ret < 0) {
		fprintf(stderr, "statfs %s: %s\n", root, strerror(errno));
		return ret;
	}

	if (verbose > 1)
		fprintf(stdout, "Block-size %d, total %d, free %d\n",
			fs.f_bsize, (int)fs.f_blocks, (int)fs.f_bfree);

	bytes = (unsigned long long)fs.f_bsize * fs.f_bfree;
	storage_info.free_space_in_bytes = __cpu_to_le64(bytes);
	return 0;
}

static void delete_object(void *recv_buf, void *send_buf)
{
	struct ptp_container *r_container = recv_buf;
	struct ptp_container *s_container = send_buf;
	enum pima15740_response_code code = PIMA15740_RESP_OK;
	uint32_t format, handle;
	uint32_t *param;
	unsigned long length;
	int ret = 0;

	length = __le32_to_cpu(r_container->length);

	param = (uint32_t *)r_container->payload;
	handle = __le32_to_cpu(*param);
	format = __le32_to_cpu(*(param + 1));

	if (length > 16 && format != PTP_PARAM_UNUSED) {
		/* ObjectFormatCode not supported */
		code = PIMA15740_RESP_SPECIFICATION_BY_FORMAT_NOT_SUPPORTED;
		goto resp;
	} else if (handle == 1 || handle == 2) {
		/* read-only /DCIM and /DCIM/100LINUX */
		code = PIMA15740_RESP_OBJECT_WRITE_PROTECTED;
		goto resp;
	}

	ret = chdir(root);
	if (ret) {
		fprintf(stderr, "chdir %s: %s\n", root, strerror(errno));
		code = PIMA15740_RESP_GENERAL_ERROR;
		goto resp;
	}

	if (handle == PTP_PARAM_ANY) {
		struct obj_list *obj, **anchor;
		int partial = 0;

		anchor = &images;
		obj = images;

		if (!obj) {
			code = PIMA15740_RESP_OK;
			goto resp;
		}

		while (obj) {
			code = delete_file(obj->name);
			if (code == PIMA15740_RESP_OK) {
				delete_thumb(obj);
				*anchor = obj->next;
				free(obj);
				obj = *anchor;
				object_number--;
			} else {
				anchor = &obj->next;
				obj = obj->next;
				partial++;
			}
		}

		if (partial)
			code = PIMA15740_RESP_PARTIAL_DELETION;
	} else {
		struct obj_list *obj, **anchor;

		anchor = &images;
		obj = images;

		while (obj) {
			if (obj->handle == handle)
				break;
			anchor = &obj->next;
			obj = obj->next;
		}

		if (obj) {
			code = delete_file(obj->name);
			if (code == PIMA15740_RESP_OK) {
				delete_thumb(obj);
				*anchor = obj->next;
				free(obj);
				object_number--;
			}
		} else {
			code = PIMA15740_RESP_INVALID_OBJECT_HANDLE;
		}
	}

	ret = update_free_space();
	if (ret < 0)
		code = PIMA15740_RESP_STORE_NOT_AVAILABLE;

resp:
	make_response(s_container, r_container, code, sizeof(*s_container));
}

static int process_one_request(void *recv_buf, size_t *recv_size, void *send_buf, size_t *send_size)
{
	struct ptp_container *r_container = recv_buf;
	struct ptp_container *s_container = send_buf;
	uint32_t *param, p1, p2, p3;
	unsigned long length = *recv_size, type, code, id;
	size_t count = 0;
	int ret;

	do {
		ret = read(bulk_out, recv_buf + count, *recv_size - count);
		if (ret < 0) {
			if (errno != EINTR)
				return ret;

			/* Need to wait for control thread to finish reset */
			sem_wait(&reset);
		} else {
			count += ret;
			if (count >= sizeof(*s_container)) {
				length	= __le32_to_cpu(r_container->length);
				type	= __le16_to_cpu(r_container->type);
				code	= __le16_to_cpu(r_container->code);
				id	= __le32_to_cpu(r_container->id);
			}
		}
	} while (count < length);

	if (count > length) {
		/* TODO: have to stall according to Figure 7.2-1? */
		fprintf(stderr, "BULK-OUT ERROR: received %u byte, expected %lu\n",
			count, length);
		errno = EPIPE;
		return -1;
	}

	memcpy(send_buf, recv_buf, sizeof(*s_container));

	if (verbose)
		fprintf(stderr, "BULK-OUT Received %lu byte, type %lu, code 0x%lx, id %lu\n",
			length, type, code, id);

	ret = -1;

	switch (type) {
	case PTP_CONTAINER_TYPE_COMMAND_BLOCK:
		switch (code) {
		case PIMA15740_OP_GET_DEVICE_INFO:
			CHECK_COUNT(count, 12, 12, "GET_DEVICE_INFO");

			if (verbose)
				fprintf(stderr, "%u bytes device info\n", sizeof(dev_info));
			count = sizeof(dev_info) + sizeof(*s_container);

			/* First part: data block */
			s_container->type = __cpu_to_le16(PTP_CONTAINER_TYPE_DATA_BLOCK);
			s_container->length = __cpu_to_le32(count);
			memcpy(send_buf + sizeof(*s_container), &dev_info, sizeof(dev_info));
			ret = bulk_write(s_container, count);
			if (ret < 0)
				return ret;

			/* Second part: response block */
			s_container = send_buf + count;
			make_response(s_container, r_container, PIMA15740_RESP_OK, sizeof(*s_container));

			break;
		case PIMA15740_OP_OPEN_SESSION:
			CHECK_COUNT(count, 16, 16, "OPEN_SESSION");

			ret = sizeof(*s_container);
			param = (uint32_t *)r_container->payload;
			p1 = __le32_to_cpu(*param);
			if (verbose)
				fprintf(stderr, "OpenSession %d\n", p1);
			/* No multiple sessions. */
			if (session > 0) {
				/* already open */
				code = PIMA15740_RESP_SESSION_ALREADY_OPEN;
				*param = __cpu_to_le32(session);
				ret += sizeof(*param);
			} else if (!p1) {
				code = PIMA15740_RESP_INVALID_PARAMETER;
			} else {
				code = PIMA15740_RESP_OK;
				session = p1;
			}
			make_response(s_container, r_container, code, ret);
			count = 0;
			break;
		case PIMA15740_OP_CLOSE_SESSION:
			CHECK_COUNT(count, 12, 12, "CLOSE_SESSION");

			if (session > 0) {
				code = PIMA15740_RESP_OK;
				session = -EINVAL;
			} else {
				code = PIMA15740_RESP_SESSION_NOT_OPEN;
			}
			make_response(s_container, r_container, code, sizeof(*s_container));
			ret = 0;
			count = 0;
			break;
		case PIMA15740_OP_GET_OBJECT_HANDLES:
			CHECK_COUNT(count, 16, 24, "GET_OBJECT_HANDLES");
			CHECK_SESSION(s_container, r_container, &count, &ret);

			ret = send_object_handles(recv_buf, send_buf, *send_size);
			count = ret; /* even if ret is negative, handled below */
			break;
		case PIMA15740_OP_GET_OBJECT_INFO:
			CHECK_COUNT(count, 16, 16, "GET_OBJECT_INFO");
			CHECK_SESSION(s_container, r_container, &count, &ret);

			ret = send_object_info(recv_buf, send_buf, *send_size);
			count = ret; /* even if ret is negative, handled below */
			break;
		case PIMA15740_OP_GET_STORAGE_IDS:
			CHECK_COUNT(count, 12, 12, "GET_STORAGE_IDS");
			CHECK_SESSION(s_container, r_container, &count, &ret);

			ret = send_storage_ids(recv_buf, send_buf, *send_size);
			count = ret; /* even if ret is negative, handled below */
			break;
		case PIMA15740_OP_GET_STORAGE_INFO:
			CHECK_COUNT(count, 16, 16, "GET_STORAGE_INFO");
			CHECK_SESSION(s_container, r_container, &count, &ret);

			ret = send_storage_info(recv_buf, send_buf, *send_size);
			count = ret; /* even if ret is negative, handled below */
			break;
		case PIMA15740_OP_GET_OBJECT:
			CHECK_COUNT(count, 16, 16, "GET_OBJECT");
			CHECK_SESSION(s_container, r_container, &count, &ret);

			ret = send_object_or_thumb(recv_buf, send_buf, *send_size, 0);
			count = ret; /* even if ret is negative, handled below */
			break;
		case PIMA15740_OP_GET_NUM_OBJECTS:
			CHECK_COUNT(count, 16, 24, "GET_NUM_OBJECTS");
			CHECK_SESSION(s_container, r_container, &count, &ret);

			ret = 12;
			param = (uint32_t *)r_container->payload;
			p1 = __le32_to_cpu(*param);
			p2 = __le32_to_cpu(*(param + 1));
			p3 = __le32_to_cpu(*(param + 2));
			if (p1 != PTP_PARAM_ANY && p1 != STORE_ID)
				code = PIMA15740_RESP_INVALID_STORAGE_ID;
			else if (count > 16 && p2 != PTP_PARAM_UNUSED && p2 != PTP_PARAM_ANY)
				code = PIMA15740_RESP_SPECIFICATION_BY_FORMAT_NOT_SUPPORTED;
			else if (count > 20 && p3 != PTP_PARAM_UNUSED) {
				if (!object_handle_valid(p3))
					code = PIMA15740_RESP_INVALID_OBJECT_HANDLE;
				else if (p3 == PTP_PARAM_ANY || p3 == 1) {
					/* root or DCIM - report one handle */
					code = PIMA15740_RESP_OK;
					ret += sizeof(*param);
					*param = __cpu_to_le32(1);
				} else if (p3 == 2) {
					/* Contents of 100LINUX */
					code = PIMA15740_RESP_OK;
					ret += sizeof(*param);
					*param = __cpu_to_le32(object_number - 2);
				} else
					code = PIMA15740_RESP_INVALID_PARENT_OBJECT;
			} else {
				/* No parent Association specified or 0 */
				code = PIMA15740_RESP_OK;
				ret += sizeof(*param);
				*param = __cpu_to_le32(object_number);
			}
			make_response(s_container, r_container, code, ret);
			count = 0;
			break;
		case PIMA15740_OP_GET_THUMB:
			CHECK_COUNT(count, 16, 16, "GET_THUMB");
			CHECK_SESSION(s_container, r_container, &count, &ret);

			ret = send_object_or_thumb(recv_buf, send_buf, *send_size, 1);
			count = ret; /* even if ret is negative, handled below */
			break;
		case PIMA15740_OP_DELETE_OBJECT:
			CHECK_COUNT(count, 16, 20, "DELETE_OBJECT");
			CHECK_SESSION(s_container, r_container, &count, &ret);

			delete_object(recv_buf, send_buf);
			count = 0;
			ret = 0;
			break;
		}
		break;
	}

	if (ret < 0) {
		if (errno == EPIPE)
			return -1;

		if (verbose)
			fprintf(stderr, "Unsupported type %lu code %lu\n", type, code);
		errno = EOPNOTSUPP;
		make_response(s_container, r_container,
			      PIMA15740_RESP_OPERATION_NOT_SUPPORTED, sizeof(*s_container));
		count = 0;
	}

	/* send out response at send_buf + count */
	s_container = send_buf + count;
	length = __le32_to_cpu(s_container->length);
	return bulk_write(s_container, length);
}

static void *bulk_thread(void *param)
{
	void *recv_buf, *send_buf;
	int ret;
	size_t s_size = BUF_SIZE, r_size = BUF_SIZE;

	recv_buf = malloc(BUF_SIZE);
	send_buf = malloc(BUF_SIZE);
	if (!recv_buf || !send_buf) {
		if (verbose)
			fprintf(stderr, "No memory!\n");
		goto done;
	}

	do {
		ret = process_one_request(recv_buf, &r_size, send_buf, &s_size);
		if (ret < 0 && errno == EPIPE) {
			/* TODO: Have to stall and wait to be unstalled / exit
			 * thread to be restarted */
			fprintf(stderr, "Protocol error!\n");
			break;
		}

		pthread_testcancel();
	} while (ret >= 0);

done:
	free(recv_buf);
	free(send_buf);
	pthread_exit(NULL);
}

static int start_io(void)
{
	int ret;
	char buf[256];

	if (verbose)
		fprintf(stderr, "Start bulk EPs\n");

	if (bulk_in >= 0 && bulk_out >= 0)
		return 0;

	snprintf(buf, sizeof(buf), "/dev/gadget/%s", EP_IN_NAME);
	bulk_in = source_open(buf);
	if (bulk_in < 0)
		return bulk_in;

	snprintf(buf, sizeof(buf), "/dev/gadget/%s", EP_OUT_NAME);
	bulk_out = sink_open(buf);
	if (bulk_out < 0)
		return bulk_out;

	snprintf(buf, sizeof(buf), "/dev/gadget/%s", EP_STATUS_NAME);
	interrupt = int_open(buf);
	if (interrupt < 0)
		return interrupt;

	status = PTP_IDLE;

	ret = pthread_create(&bulk_pthread, NULL, bulk_thread, NULL);
	if (ret < 0) {
		perror ("can't create bulk thread");
		return ret;
	}

	return 0;
}

static void stop_io(void)
{
	fprintf(stderr, "Stop bulk EPs\n");

	if (bulk_in < 0 || bulk_out < 0)
		return;

	pthread_cancel(bulk_pthread);
	pthread_join(bulk_pthread, NULL);

	status = PTP_WAITCONFIG;

	close(bulk_out);
	bulk_out = -EINVAL;
	close(bulk_in);
	bulk_in = -EINVAL;
	close(interrupt);
	interrupt = -EINVAL;
}

static int reset_interface(void)
{
	/* just reset toggle/halt for the interface's endpoints */
	int err;

	if (status == PTP_WAITCONFIG)
		return 0;

	sem_init(&reset, 0, 0);

	pthread_kill(bulk_pthread, SIGINT);

	err = ioctl(bulk_in, GADGETFS_CLEAR_HALT);
	if (err < 0)
		perror("reset source fd");

	err = ioctl(bulk_out, GADGETFS_CLEAR_HALT);
	if (err < 0)
		perror("reset sink fd");

	sem_post(&reset);

	/* FIXME eventually reset the status endpoint too */

	/* Always return "success"... */
	return 0;
}

static void handle_control(struct usb_ctrlrequest *setup)
{
	int		err, tmp;
	uint8_t		buf[256];
	uint16_t	value, index, length;

	value = __le16_to_cpu(setup->wValue);
	index = __le16_to_cpu(setup->wIndex);
	length = __le16_to_cpu(setup->wLength);

	if (verbose)
		fprintf(stderr, "SETUP %02x.%02x "
				"v%04x i%04x %d\n",
			setup->bRequestType, setup->bRequest,
			value, index, length);

	/*
	if ((setup->bRequestType & USB_TYPE_MASK) != USB_TYPE_STANDARD)
		goto special;
	*/

	switch (setup->bRequest) {	/* usb 2.0 spec ch9 requests */
	case USB_REQ_GET_DESCRIPTOR:
		if (setup->bRequestType != USB_DIR_IN)
			goto stall;
		switch (value >> 8) {
		case USB_DT_STRING:
			tmp = value & 0xff;
			if (verbose > 1)
				fprintf(stderr,
					"... get string %d lang %04x\n",
					tmp, index);
			if (tmp != 0 && index != strings.language)
				goto stall;
			err = usb_gadget_get_string(&strings, tmp, buf);
			if (err < 0)
				goto stall;
			tmp = err;
			if (length < tmp)
				tmp = length;
			err = write(control, buf, tmp);
			if (err < 0) {
				if (errno == EIDRM)
					fprintf(stderr, "string timeout\n");
				else
					perror("write string data");
			} else if (err != tmp) {
				fprintf(stderr, "short string write, %d\n",
					err);
			}
			break;
		default:
			goto stall;
		}
		return;
	case USB_REQ_SET_CONFIGURATION:
		if (setup->bRequestType != USB_DIR_OUT)
			goto stall;
		if (verbose)
			fprintf(stderr, "CONFIG #%d\n", value);

		/* Kernel is normally waiting for us to finish reconfiguring
		 * the device.
		 *
		 * Some hardware can't, notably older PXA2xx hardware.  (With
		 * racey and restrictive config change automagic.  PXA 255 is
		 * OK, most PXA 250s aren't.  If it has a UDC CFR register,
		 * it can handle deferred response for SET_CONFIG.)  To handle
		 * such hardware, don't write code this way ... instead, keep
		 * the endpoints always active and don't rely on seeing any
		 * config change events, either this or SET_INTERFACE.
		 */
		switch (value) {
		case CONFIG_VALUE:
			start_io();
			break;
		case 0:
			stop_io();
			break;
		default:
			/* kernel bug -- "can't happen" */
			fprintf(stderr, "? illegal config\n");
			goto stall;
		}

		/* ... ack (a write would stall) */
		err = read(control, &err, 0);
		if (err)
			perror("ack SET_CONFIGURATION");
		return;
	case USB_REQ_GET_INTERFACE:
		if (setup->bRequestType != (USB_DIR_IN|USB_RECIP_INTERFACE)
				|| index != 0
				|| length > 1)
			goto stall;

		/* only one altsetting in this driver */
		buf[0] = 0;
		err = write(control, buf, length);
		if (err < 0) {
			if (errno == EIDRM)
				fprintf(stderr, "GET_INTERFACE timeout\n");
			else
				perror("write GET_INTERFACE data");
		} else if (err != length) {
			fprintf(stderr, "short GET_INTERFACE write, %d\n",
				err);
		}
		return;
	case USB_REQ_SET_INTERFACE:
		if (setup->bRequestType != USB_RECIP_INTERFACE
				|| index != 0
				|| value != 0)
			goto stall;

		err = reset_interface();
		if (err)
			goto stall;

		/* ... and ack (a write would stall) */
		err = read(control, &err, 0);
		if (err)
			perror("ack SET_INTERFACE");
		return;
	/* Still Image class-specific requests */
	case USB_REQ_PTP_CANCEL_REQUEST:
		return;
	case USB_REQ_PTP_GET_EXTENDED_EVENT_DATA:
		/* Optional, may stall */
		goto stall;
	case USB_REQ_PTP_DEVICE_RESET_REQUEST:
		if (setup->bRequestType != 0x21
				|| index != 0
				|| value != 0)
			goto stall;

		err = reset_interface();
		if (err)
			goto stall;

		/* ... and ack (a write would stall) */
		err = read(control, &err, 0);
		if (err)
			perror("ack DEVICE_RESET_REQUEST");
		return;
	case USB_REQ_PTP_GET_DEVICE_STATUS_REQUEST:
		if (setup->bRequestType != 0xa1
				|| index != 0
				|| value != 0)
			goto stall;
		else {
			uint16_t resp_ok[] = {
				__constant_cpu_to_le16(4),
				__constant_cpu_to_le16(PIMA15740_RESP_OK),
			};
			memcpy(buf, resp_ok, 4);
			err = write(control, buf, 4);
			if (err != 4)
				fprintf(stderr, "DEVICE_STATUS_REQUEST %d\n", err);
		}

		return;
	default:
		goto stall;
	}

stall:
	if (verbose)
		fprintf(stderr, "... protocol stall %02x.%02x\n",
			setup->bRequestType, setup->bRequest);

	/* non-iso endpoints are stalled by issuing an i/o request
	 * in the "wrong" direction.  ep0 is special only because
	 * the direction isn't fixed.
	 */
	if (setup->bRequestType & USB_DIR_IN)
		err = read(control, &err, 0);
	else
		err = write(control, &err, 0);
	if (err != -1)
		fprintf(stderr, "can't stall ep0 for %02x.%02x\n",
			setup->bRequestType, setup->bRequest);
	else if (errno != EL2HLT)
		perror("ep0 stall");
}

static int read_control(void)
{
	struct usb_gadgetfs_event event[NEVENT];
	int i, nevent, ret;

	ret = read(control, &event, sizeof(event));
	if (ret < 0) {
		if (errno == EAGAIN) {
			sleep(1);
			return ret;
		}
		perror("ep0 read after poll");
		return ret;
	}
	nevent = ret / sizeof event[0];

	for (i = 0; i < nevent; i++) {
		switch (event[i].type) {
		case GADGETFS_NOP:
			if (verbose)
				fprintf(stderr, "NOP\n");
			break;
		case GADGETFS_CONNECT:
			if (status != PTP_WAITCONFIG)
				status = PTP_IDLE;
			current_speed = event[i].u.speed;
			if (verbose)
				fprintf(stderr,
					"CONNECT %s\n",
				    speed(event[i].u.speed));
			break;
		case GADGETFS_SETUP:
			if (status != PTP_WAITCONFIG)
				status = PTP_IDLE;
			handle_control(&event[i].u.setup);
			break;
		case GADGETFS_DISCONNECT:
			status = PTP_WAITCONFIG;
			current_speed = USB_SPEED_UNKNOWN;
			if (verbose)
				fprintf(stderr, "DISCONNECT\n");
			break;
		case GADGETFS_SUSPEND:
			if (verbose)
				fprintf(stderr, "SUSPEND\n");
			stop_io();
			break;
		default:
			fprintf(stderr,
				"* unhandled event %d\n",
				event[i].type);
		}
	}

	return ret;
}

static int main_loop(void)
{
	struct pollfd ep_poll[1];
	int ret;

	do {
		/* Always listen on control */
		ep_poll[0].fd = control;
		ep_poll[0].events = POLLIN | POLLHUP;

		ret = poll(ep_poll, 1, -1);
		if (ret < 0) {
			perror("poll");
			break;
		}

		/* TODO: What to do with HUP? */
		if (ep_poll[0].revents & POLLIN) {
			ret = read_control();
			if (ret < 0) {
				if (errno == EAGAIN)
					continue;
				goto done;
			}
		}
	} while (1);

	return 0;

done:
	switch (status) {
	case PTP_IDLE:
	case PTP_DATA_OUT:
	case PTP_DATA_IN:
	case PTP_DATA_READY:
		stop_io();
	case PTP_WAITCONFIG:
		break;
	}

	return ret;
}

/*-------------------------------------------------------------------------*/

static size_t put_string(iconv_t ic, char *buf, const char *s, size_t len)
{
	char *in = (char *)s;
	size_t ret, inl = len, outl = len * 2;

	ret = iconv(ic, &in, &inl, &buf, &outl);
	if (inl || outl)
		fprintf(stdout, "iconv() error %d: %u input / %u output bytes left!\n",
			errno, inl, outl);

	return ret;
}

static int enum_objects(const char *path)
{
	struct dirent *dentry;
	char /*creat[32], creat_ucs2[64], */mod[32], mod_ucs2[64], fname_ucs2[512],
		thumb[256];
	DIR *d;
	int ret;
	struct obj_list **obj = &images;
	/* First two handles used for /DCIM/PTP_MODEL_DIR */
	uint32_t handle = 2;

	ret = chdir(path);
	if (ret < 0)
		return ret;

	d = opendir(".");

	while ((dentry = readdir(d))) {
		struct stat fstat, tstat;
		char *dot;
		size_t namelen, datelen, osize;
		enum pima15740_data_format format;
		struct tm mod_tm;

		dot = strrchr(dentry->d_name, '.');

		if (!dot || dot == dentry->d_name)
			continue;

		if (strcasecmp(dot, ".tif") &&
		    strcasecmp(dot, ".tiff") &&
		    strcasecmp(dot, ".jpg") &&
		    strcasecmp(dot, ".jpeg"))
			continue;

		/* TODO: use identify from ImageMagick and parse its output */
		switch (dot[1]) {
		case 't':
		case 'T':
			format = PIMA15740_FMT_I_TIFF;
			break;
		case 'j':
		case 'J':
			format = PIMA15740_FMT_I_EXIF_JPEG;
			break;
		default:
			format = PIMA15740_FMT_I_UNDEFINED;
		}

		ret = stat(dentry->d_name, &fstat);
		if (ret < 0)
			break;

		namelen = strlen(dentry->d_name) + 1;

		ret = put_string(ic, fname_ucs2, dentry->d_name, namelen);
		if (ret)
			break;

		gmtime_r(&fstat.st_mtime, &mod_tm);
		snprintf(mod, sizeof(mod),"%04u%02u%02uT%02u%02u%02u.0Z",
			 mod_tm.tm_year + 1900, mod_tm.tm_mon + 1,
			 mod_tm.tm_mday, mod_tm.tm_hour,
			 mod_tm.tm_min, mod_tm.tm_sec);

		/* String length including the trailing '\0' */
		datelen = strlen(mod) + 1;
		ret = put_string(ic, mod_ucs2, mod, datelen);
		if (ret) {
			mod[0] = '\0';
			datelen = 0;
		}

		/* Put thumbnails under /var/cache/ptp/thumb/
		 * and call them <filename>.thumb.<extension> */
		*dot = '\0';
		snprintf(thumb, sizeof(thumb), THUMB_LOCATION "%s.thumb.jpeg",
			 dentry->d_name);
		*dot = '.';
		if (stat(thumb, &tstat) < 0 || tstat.st_mtime < fstat.st_mtime) {
			pid_t converter;
			if (verbose)
				fprintf(stderr, "No or old thumbnail for %s\n", dentry->d_name);
			converter = fork();
			if (converter < 0) {
				if (verbose)
					fprintf(stderr, "Cannot generate thumbnail for %s\n",
						dentry->d_name);
				continue;
			} else if (converter) {
				int status;
				waitpid(converter, &status, 0);
				if (!WIFEXITED(status) || WEXITSTATUS(status) ||
				    stat(thumb, &tstat) < 0) {
					if (verbose)
						fprintf(stderr,
							"Generate thumbnail for %s failed\n",
							dentry->d_name);
					continue;
				}
			} else
				execlp("convert", "convert", "-thumbnail", THUMB_SIZE,
				       dentry->d_name, thumb, NULL);
		}

		/* namelen and datelen include terminating '\0', plus 4 string-size bytes */
		osize = sizeof(**obj) + 2 * (datelen + namelen) + 4;

		if (verbose)
			fprintf(stderr, "Listing image %s, modified %s, info-size %u\n",
				dentry->d_name, mod, osize);

		*obj = malloc(osize);
		if (!*obj) {
			ret = -1;
			break;
		}

		(*obj)->handle = ++handle;

		/* Fixed size object info, filename, capture date, and two empty strings */
		(*obj)->info_size = sizeof((*obj)->info) + 2 * (datelen + namelen) + 4;

		(*obj)->info.storage_id			= __cpu_to_le32(STORE_ID);
		(*obj)->info.object_format		= __cpu_to_le16(format);
		(*obj)->info.protection_status		= __cpu_to_le16(fstat.st_mode & S_IWUSR ? 0 : 1);
		(*obj)->info.object_compressed_size	= __cpu_to_le32(fstat.st_size);
		(*obj)->info.thumb_format		= __cpu_to_le16(PIMA15740_FMT_I_JFIF);
		(*obj)->info.thumb_compressed_size	= __cpu_to_le32(tstat.st_size);
		(*obj)->info.thumb_pix_width		= __cpu_to_le32(THUMB_WIDTH);
		(*obj)->info.thumb_pix_height		= __cpu_to_le32(THUMB_HEIGHT);
		(*obj)->info.image_pix_width		= __cpu_to_le32(0);	/* 0 == */
		(*obj)->info.image_pix_height		= __cpu_to_le32(0);	/* not */
		(*obj)->info.image_bit_depth		= __cpu_to_le32(0);	/* supported */
		(*obj)->info.parent_object		= __cpu_to_le32(2);	/* Fixed /dcim/xxx/ */
		(*obj)->info.association_type		= __cpu_to_le16(0);
		(*obj)->info.association_desc		= __cpu_to_le32(0);
		(*obj)->info.sequence_number		= __cpu_to_le32(0);
		strncpy((*obj)->name, dentry->d_name, sizeof((*obj)->name));

		(*obj)->info.strings[0]					= namelen;
		memcpy((*obj)->info.strings + 1, fname_ucs2, namelen * 2);
		/* We use file modification date as Capture Date */
		(*obj)->info.strings[1 + namelen * 2]			= datelen;
		memcpy((*obj)->info.strings + 2 + namelen * 2, mod_ucs2, datelen * 2);
		/* Empty Modification Date */
		(*obj)->info.strings[2 + (namelen + datelen) * 2]	= 0;
		/* Empty Keywords */
		(*obj)->info.strings[3 + (namelen + datelen) * 2]	= 0;

		obj = &(*obj)->next;
		*obj = NULL;
	}

	object_number = handle;

	closedir(d);
	return ret;
}

static void init_strings(iconv_t ic)
{
	put_string(ic, (char *)dev_info.manuf, manuf, sizeof(manuf));
	put_string(ic, (char *)dev_info.model, model, sizeof(model));
	put_string(ic, (char *)storage_info.desc,
		   storage_desc, sizeof(storage_desc));
}

static void signothing(int sig, siginfo_t *info, void *ptr)
{
	/* NOP */
	if (verbose > 2)
		fprintf(stderr, "%s %d\n", __func__, sig);
}

static int init_signal(void)
{
	struct sigaction sa = {
		.sa_sigaction = signothing,
		.sa_flags = SA_SIGINFO,
	};

	sigfillset(&sa.sa_mask);
	/* We will use SIGINT to wake up the bulk thread from read() */
	if (sigaction(SIGINT, &sa, NULL) < 0) {
		perror("SIGINT");
		return -1;
	}
	return 0;
}

int main(int argc, char *argv[])
{
	int c, ret;
	struct stat root_stat;

	puts("Linux PTP Gadget v" VERSION_STRING);

	ic = iconv_open("UCS-2LE", "ISO8859-1");
	if (ic == (iconv_t)-1) {
		perror("iconv_open");
		return -1;
	}

	init_strings(ic);

	if (init_signal() < 0)
		exit(EXIT_FAILURE);

	if (sem_init(&reset, 0, 0) < 0)
		exit(EXIT_FAILURE);

	while ((c = getopt(argc, argv, "v")) != EOF) {
		switch (c) {
		case 'v':
			verbose++;
			break;
		default:
			fprintf(stderr, "Unsupported option %c\n", c);
			exit(EXIT_FAILURE);
		}
	}

	root = argv[argc - 1];

	enum_objects(root);

	if (chdir("/dev/gadget") < 0) {
		perror("can't chdir /dev/gadget");
		exit(EXIT_FAILURE);
	}

	ret = stat(root, &root_stat);
	if (ret < 0 || !S_ISDIR(root_stat.st_mode) || access(root, R_OK | W_OK) < 0) {
		fprintf(stderr, "Invalid base directory %s\n", root);
		exit(EXIT_FAILURE);
	}

	init_device();
	if (control < 0)
		exit(EXIT_FAILURE);

	fflush(stderr);

	ret = main_loop();

	iconv_close(ic);

	exit(ret ? EXIT_FAILURE : EXIT_SUCCESS);
}
