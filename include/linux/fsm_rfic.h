/* Copyright (c) 2013, The Linux Foundation. All rights reserved.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#ifndef _FSM_RFIC_H_
#define _FSM_RFIC_H_

#include <linux/ioctl.h>

void fsm9900_gluon_init(void);
void fsm9900_rfic_init(void);


/*
 * Device interface
 */

#define RFIC_FTR_DEVICE_NAME		"rfic_ftr"
#define RFIC_WTR_DEVICE_NAME		"rfic_wtr"
#define RFIC_WGR_DEVICE_NAME		"rfic_wgr"
#define GRFC_DEVICE_NAME		"grfc"
#define PDM_DEVICE_NAME			"pdm"
#define BBIF_DEVICE_NAME		"bbif"

/*
 * IOCTL interface
 */

/*
 * Macro to associate the "bus" and "address" pair when accessing the RFIC.
 * Using a 32 bit address, reserve the upper 8 bits for the bus value, and
 * the lower 24 bits for the address.
 */

#define RFIC_FTR_ADDR(bus, addr)	(((bus&0x03)<<24)|(addr&0xFFFFFF))
#define RFIC_FTR_GET_ADDR(busaddr)	(busaddr&0xFFFFFF)
#define RFIC_FTR_GET_BUS(busaddr)	((busaddr>>24)&0x03)

struct rfic_write_register_param {
	unsigned int rficaddr;
	unsigned int value;
};

struct rfic_write_register_mask_param {
	unsigned int rficaddr;
	unsigned int value;
	unsigned int mask;
};

struct rfic_grfc_param {
	unsigned int grfcid;
	unsigned int maskvalue;
	unsigned int ctrlvalue;
};

struct pdm_write_param {
	unsigned int offset;
	unsigned int value;
};

struct bbif_param {
	unsigned int offset;
	unsigned int value;
};

struct bbif_bw_param {
	unsigned int bw_id;
	unsigned int number;
};

#define RFIC_IOCTL_MAGIC				'f'
#define RFIC_IOCTL_READ_REGISTER \
	_IOC(_IOC_READ, RFIC_IOCTL_MAGIC, 0x01, \
		sizeof(unsigned int *))
#define RFIC_IOCTL_WRITE_REGISTER \
	_IOC(_IOC_WRITE, RFIC_IOCTL_MAGIC, 0x02, \
		sizeof(struct rfic_write_register_param *))
#define RFIC_IOCTL_WRITE_REGISTER_WITH_MASK \
	_IOC(_IOC_WRITE, RFIC_IOCTL_MAGIC, 0x03, \
		sizeof(struct rfic_write_register_mask_param *))
#define RFIC_IOCTL_GET_GRFC \
	_IOC(_IOC_WRITE, RFIC_IOCTL_MAGIC, 0x10, \
		sizeof(struct rfic_grfc_param *))
#define RFIC_IOCTL_SET_GRFC \
	_IOC(_IOC_WRITE, RFIC_IOCTL_MAGIC, 0x11, \
		sizeof(struct rfic_grfc_param *))
#define RFIC_IOCTL_GET_BOARDID \
	_IOC(_IOC_READ, RFIC_IOCTL_MAGIC, 0x20, \
		sizeof(unsigned int *))
#define RFIC_IOCTL_PDM_READ \
	_IOC(_IOC_READ, RFIC_IOCTL_MAGIC, 0x31, \
		sizeof(unsigned int *))
#define RFIC_IOCTL_PDM_WRITE \
	_IOC(_IOC_WRITE, RFIC_IOCTL_MAGIC, 0x32, \
		sizeof(struct pdm_write_param *))
#define BBIF_IOCTL_GET \
	_IOC(_IOC_READ, RFIC_IOCTL_MAGIC, 0x41, \
		sizeof(struct bbif_param *))
#define BBIF_IOCTL_SET \
	_IOC(_IOC_WRITE, RFIC_IOCTL_MAGIC, 0x42, \
		sizeof(struct bbif_param *))
#define BBIF_IOCTL_SET_ADC_BW \
	_IOC(_IOC_WRITE, RFIC_IOCTL_MAGIC, 0x43, \
		sizeof(struct bbif_bw_param *))
#endif /* _FSM_RFIC_H_ */
