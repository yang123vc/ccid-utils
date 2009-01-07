/*
 * This file is part of cci-utils
 * Copyright (c) 2008 Gianni Tedesco <gianni@scaramanga.co.uk>
 * Released under the terms of the GNU GPL version 3
*/

#include <ccid.h>
#include <ccid-spec.h>

#include <stdio.h>

#include "ccid-internal.h"

unsigned int _RDR_to_PC_DataBlock(const struct ccid_msg *msg)
{
	const uint8_t *ptr;

	ptr = (void *)msg;

	printf("     : RDR_to_PC_DataBlock: %u bytes\n",
		sys_le32(msg->dwLength));
	hex_dump(msg + 10, sys_le32(msg->dwLength), 16);

	return msg->in.bApp;
}

unsigned int _RDR_to_PC_SlotStatus(const struct ccid_msg *msg)
{
	assert(msg->bMessageType == RDR_to_PC_SlotStatus);

	printf("     : RDR_to_PC_SlotStatus: ");
	switch(msg->in.bApp) {
	case 0x00:
		printf("Clock running\n");
		return CHIPCARD_CLOCK_START;
	case 0x01:
		printf("Clock stopped in L state\n");
		return CHIPCARD_CLOCK_STOP_L;
	case 0x02:
		printf("Clock stopped in H state\n");
		return CHIPCARD_CLOCK_STOP_H;
	case 0x03:
		printf("Clock stopped in unknown state\n");
		return CHIPCARD_CLOCK_STOP;
	default:
		return CHIPCARD_CLOCK_ERR;
	}
}

int _cci_get_cmd_result(const struct ccid_msg *msg, int *code)
{
	switch( msg->in.bStatus & CCID_STATUS_RESULT_MASK ) {
	case CCID_RESULT_SUCCESS:
		printf("     : Command: SUCCESS\n");
		return 1;
	case CCID_RESULT_ERROR:
		printf("     : Command: FAILED (%d)\n", msg->in.bError);
		if ( code )
			*code = msg->in.bError;
		return 0;
	case CCID_RESULT_TIMEOUT:
		printf("     : Command: Time Extension Request\n");
		if ( code )
			*code = -1;
		return 0;
	default:
		fprintf(stderr, "*** error: unknown command result\n");
		if ( code )
			*code = -1;
		return 0;
	}
}

static int do_recv(struct _cci *cci)
{
	const struct ccid_msg *msg;
	int ret;

	ret = usb_bulk_read(cci->cci_dev, cci->cci_inp,
				(void *)cci->cci_rcvbuf, cci->cci_max_in, 0);
	if ( ret < 0 ) {
		fprintf(stderr, "*** error: usb_bulk_read()\n");
		return 0;
	}

	cci->cci_rcvlen = (size_t)ret;

	if ( cci->cci_rcvlen < sizeof(*msg) ) {
		fprintf(stderr, "*** error: truncated CCI msg\n");
		return 0;
	}

	msg = (struct ccid_msg *)cci->cci_rcvbuf;

	if ( sizeof(*msg) + sys_le32(msg->dwLength) > cci->cci_rcvlen) {
		fprintf(stderr, "*** error: bad dwLength in CCI msg\n");
		return 0;
	}

	return (size_t)ret;
}

const struct ccid_msg *_RDR_to_PC(struct _cci *cci)
{
	const struct ccid_msg *msg;

	if ( !do_recv(cci) )
		return NULL;

	msg = (struct ccid_msg *)cci->cci_rcvbuf;

	if ( msg->bSlot >= cci->cci_max_slots ) {
		fprintf(stderr, "*** error: unknown slot %u\n", msg->bSlot);
		return NULL;
	}

	if ( msg->bSeq + 1 != cci->cci_seq ) {
		fprintf(stderr, "*** error: expected seq 0x%.2x got 0x%.2x\n",
			cci->cci_seq, msg->bSeq);
		return NULL;
	}

	printf(" Recv: %d bytes for slot %u (seq = 0x%.2x)\n",
		cci->cci_rcvlen, msg->bSlot, msg->bSeq);

	_chipcard_set_status(&cci->cci_slot[msg->bSlot], msg->in.bStatus);
	
	return msg;
}

static int _PC_to_RDR(struct _cci *cci, unsigned int slot,
			struct ccid_msg *msg, size_t len)
{
	int ret;

	assert(slot < cci->cci_num_slots);

	/* TODO: fill in any extra len in msg->dwLength */
	msg->bSlot = slot;
	msg->bSeq = cci->cci_seq++;

	ret = usb_bulk_write(cci->cci_dev, cci->cci_outp,
				(void *)msg, len, -1);
	if ( ret < 0 )
		return 0;
	if ( (size_t)ret < sizeof(msg) )
		return 0;

	return 1;
}

int _PC_to_RDR_GetSlotStatus(struct _cci *cci, unsigned int slot)
{
	struct ccid_msg msg;
	int ret;

	memset(&msg, 0, sizeof(msg));
	msg.bMessageType = PC_to_RDR_GetSlotStatus;
	ret = _PC_to_RDR(cci, slot, &msg, sizeof(msg));
	if ( ret )
		printf(" Xmit: PC_to_RDR_GetSlotStatus(%u)\n", slot);
	return ret;
}

int _PC_to_RDR_IccPowerOn(struct _cci *cci, unsigned int slot,
				unsigned int voltage)
{
	struct ccid_msg msg;
	int ret;

	assert(voltage <= CHIPCARD_1_8V);

	memset(&msg, 0, sizeof(msg));
	msg.bMessageType = PC_to_RDR_IccPowerOn;
	msg.out.bApp[0] = voltage & 0xff;
	ret = _PC_to_RDR(cci, slot, &msg, sizeof(msg));
	if ( ret ) {
		printf(" Xmit: PC_to_RDR_IccPowerOn(%u)\n", slot);
		switch(voltage) {
		case CHIPCARD_AUTO_VOLTAGE:
			printf("     : Automatic Voltage Selection\n");
			break;
		case CHIPCARD_5V:
			printf("     : 5 Volts\n");
			break;
		case CHIPCARD_3V:
			printf("     : 3 Volts\n");
			break;
		case CHIPCARD_1_8V:
			printf("     : 1.8 Volts\n");
			break;
		}
	}
	return ret;
}

int _PC_to_RDR_IccPowerOff(struct _cci *cci, unsigned int slot)
{
	struct ccid_msg msg;
	int ret;

	memset(&msg, 0, sizeof(msg));
	msg.bMessageType = PC_to_RDR_IccPowerOff;
	ret = _PC_to_RDR(cci, slot, &msg, sizeof(msg));
	if ( ret ) {
		printf(" Xmit: PC_to_RDR_IccPowerOff(%u)\n", slot);
	}
	return ret;
}
