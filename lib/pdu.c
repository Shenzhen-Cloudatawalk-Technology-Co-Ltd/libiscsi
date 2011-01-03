/*
   Copyright (C) 2010 by Ronnie Sahlberg <ronniesahlberg@gmail.com>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU Lesser General Public License as published by
   the Free Software Foundation; either version 2.1 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public License
   along with this program; if not, see <http://www.gnu.org/licenses/>.
*/

#include <stdio.h>
#include <stdlib.h>
#include <strings.h>
#include <string.h>
#include <arpa/inet.h>
#include "iscsi.h"
#include "iscsi-private.h"
#include "scsi-lowlevel.h"
#include "slist.h"

struct iscsi_pdu *
iscsi_allocate_pdu_with_itt_flags(struct iscsi_context *iscsi, enum iscsi_opcode opcode,
				  enum iscsi_opcode response_opcode, uint32_t itt, uint32_t flags)
{
	struct iscsi_pdu *pdu;

	pdu = malloc(sizeof(struct iscsi_pdu));
	if (pdu == NULL) {
		iscsi_set_error(iscsi, "failed to allocate pdu");
		return NULL;
	}
	memset(pdu, 0, sizeof(struct iscsi_pdu));

	pdu->outdata.size = ISCSI_HEADER_SIZE;
	pdu->outdata.data = malloc(pdu->outdata.size);

	if (pdu->outdata.data == NULL) {
		iscsi_set_error(iscsi, "failed to allocate pdu header");
		free(pdu);
		return NULL;
	}
	memset(pdu->outdata.data, 0, pdu->outdata.size);

	/* opcode */
	pdu->outdata.data[0] = opcode;
	pdu->response_opcode = response_opcode;

	/* isid */
	if (opcode == ISCSI_PDU_LOGIN_REQUEST) {
		memcpy(&pdu->outdata.data[8], &iscsi->isid[0], 6);
	}

	/* itt */
	iscsi_pdu_set_itt(pdu, itt);
	pdu->itt = itt;

	/* flags */
	pdu->flags = flags;

	return pdu;
}

struct iscsi_pdu *
iscsi_allocate_pdu(struct iscsi_context *iscsi, enum iscsi_opcode opcode,
		   enum iscsi_opcode response_opcode)
{
	return iscsi_allocate_pdu_with_itt_flags(iscsi, opcode, response_opcode, iscsi->itt++, 0);
}	



void
iscsi_free_pdu(struct iscsi_context *iscsi, struct iscsi_pdu *pdu)
{
	if (pdu == NULL) {
		iscsi_set_error(iscsi, "trying to free NULL pdu");
		return;
	}

	free(pdu->outdata.data);
	pdu->outdata.data = NULL;

	free(pdu->indata.data);
	pdu->indata.data = NULL;

	if (pdu->scsi_cbdata) {
		iscsi_free_scsi_cbdata(pdu->scsi_cbdata);
		pdu->scsi_cbdata = NULL;
	}

	free(pdu);
}


int
iscsi_add_data(struct iscsi_context *iscsi, struct iscsi_data *data,
	       unsigned char *dptr, int dsize, int pdualignment)
{
	int len, aligned;
	unsigned char *buf;

	if (dsize == 0) {
		iscsi_set_error(iscsi, "Trying to append zero size data to "
				"iscsi_data");
		return -1;
	}

	len = data->size + dsize;
	aligned = len;
	if (pdualignment) {
		aligned = (aligned+3)&0xfffffffc;
	}
	buf = malloc(aligned);
	if (buf == NULL) {
		iscsi_set_error(iscsi, "failed to allocate buffer for %d "
				"bytes", len);
		return -1;
	}

	if (data->size > 0) {
		memcpy(buf, data->data, data->size);
	}
	memcpy(buf + data->size, dptr, dsize);
	if (len != aligned) {
		/* zero out any padding at the end */
	  memset(buf+len, 0, aligned-len);
	}

	free(data->data);

	data->data  = buf;
	data->size = len;

	return 0;
}

int
iscsi_pdu_add_data(struct iscsi_context *iscsi, struct iscsi_pdu *pdu,
		   unsigned char *dptr, int dsize)
{
	if (pdu == NULL) {
		iscsi_set_error(iscsi, "trying to add data to NULL pdu");
		return -1;
	}
	if (dsize == 0) {
		iscsi_set_error(iscsi, "Trying to append zero size data to "
				"pdu");
		return -1;
	}

	if (iscsi_add_data(iscsi, &pdu->outdata, dptr, dsize, 1) != 0) {
		iscsi_set_error(iscsi, "failed to add data to pdu buffer");
		return -1;
	}

	/* update data segment length */
	*(uint32_t *)&pdu->outdata.data[4] = htonl(pdu->outdata.size
						   - ISCSI_HEADER_SIZE);

	return 0;
}

int
iscsi_get_pdu_data_size(const unsigned char *hdr)
{
	int size;

	size = (ntohl(*(uint32_t *)&hdr[4])&0x00ffffff);
	size = (size+3)&0xfffffffc;

	return size;
}


int
iscsi_process_pdu(struct iscsi_context *iscsi, struct iscsi_in_pdu *in)
{
	uint32_t itt;
	enum iscsi_opcode opcode;
	struct iscsi_pdu *pdu;
	uint8_t	ahslen;

	opcode = in->hdr[0] & 0x3f;
	ahslen = in->hdr[4];
	itt = ntohl(*(uint32_t *)&in->hdr[16]);

	if (ahslen != 0) {
		iscsi_set_error(iscsi, "cant handle expanded headers yet");
		return -1;
	}

	for (pdu = iscsi->waitpdu; pdu; pdu = pdu->next) {
		enum iscsi_opcode expected_response = pdu->response_opcode;
		int is_finished = 1;

		if (pdu->itt != itt) {
			continue;
		}

		/* we have a special case with scsi-command opcodes,
		 * they are replied to by either a scsi-response
		 * or a data-in, or a combination of both.
		 */
		if (opcode == ISCSI_PDU_DATA_IN
		    && expected_response == ISCSI_PDU_SCSI_RESPONSE) {
			expected_response = ISCSI_PDU_DATA_IN;
		}

		/* Another special case is if we get a R2T.
		 * In this case we should find the original request and just send an additional
		 * DATAOUT segment for this task.
		 */
		if (opcode == ISCSI_PDU_R2T) {
			expected_response = ISCSI_PDU_R2T;
	        }

		if (opcode != expected_response) {
			iscsi_set_error(iscsi, "Got wrong opcode back for "
					"itt:%d  got:%d expected %d",
					itt, opcode, pdu->response_opcode);
			return -1;
		}
		switch (opcode) {
		case ISCSI_PDU_LOGIN_RESPONSE:
			if (iscsi_process_login_reply(iscsi, pdu, in) != 0) {
				SLIST_REMOVE(&iscsi->waitpdu, pdu);
				iscsi_free_pdu(iscsi, pdu);
				iscsi_set_error(iscsi, "iscsi login reply "
						"failed");
				return -1;
			}
			break;
		case ISCSI_PDU_TEXT_RESPONSE:
			if (iscsi_process_text_reply(iscsi, pdu, in) != 0) {
				SLIST_REMOVE(&iscsi->waitpdu, pdu);
				iscsi_free_pdu(iscsi, pdu);
				iscsi_set_error(iscsi, "iscsi text reply "
						"failed");
				return -1;
			}
			break;
		case ISCSI_PDU_LOGOUT_RESPONSE:
			if (iscsi_process_logout_reply(iscsi, pdu, in) != 0) {
				SLIST_REMOVE(&iscsi->waitpdu, pdu);
				iscsi_free_pdu(iscsi, pdu);
				iscsi_set_error(iscsi, "iscsi logout reply "
						"failed");
				return -1;
			}
			break;
		case ISCSI_PDU_SCSI_RESPONSE:
			if (iscsi_process_scsi_reply(iscsi, pdu, in) != 0) {
				SLIST_REMOVE(&iscsi->waitpdu, pdu);
				iscsi_free_pdu(iscsi, pdu);
				iscsi_set_error(iscsi, "iscsi response reply "
						"failed");
				return -1;
			}
			break;
		case ISCSI_PDU_DATA_IN:
			if (iscsi_process_scsi_data_in(iscsi, pdu, in,
						       &is_finished) != 0) {
				SLIST_REMOVE(&iscsi->waitpdu, pdu);
				iscsi_free_pdu(iscsi, pdu);
				iscsi_set_error(iscsi, "iscsi data in "
						"failed");
				return -1;
			}
			break;
		case ISCSI_PDU_NOP_IN:
			if (iscsi_process_nop_out_reply(iscsi, pdu, in) != 0) {
				SLIST_REMOVE(&iscsi->waitpdu, pdu);
				iscsi_free_pdu(iscsi, pdu);
				iscsi_set_error(iscsi, "iscsi nop-in failed");
				return -1;
			}
			break;
		case ISCSI_PDU_R2T:
			if (iscsi_process_r2t(iscsi, pdu, in) != 0) {
				SLIST_REMOVE(&iscsi->waitpdu, pdu);
				iscsi_free_pdu(iscsi, pdu);
				iscsi_set_error(iscsi, "iscsi r2t "
						"failed");
				return -1;
			}
			is_finished = 0;
			break;
		default:
			iscsi_set_error(iscsi, "Dont know how to handle "
					"opcode 0x%02x", opcode);
			return -1;
		}

		if (is_finished) {
			SLIST_REMOVE(&iscsi->waitpdu, pdu);
			iscsi_free_pdu(iscsi, pdu);
		}
		return 0;
	}

	return 0;
}

void
iscsi_pdu_set_itt(struct iscsi_pdu *pdu, uint32_t itt)
{
	*(uint32_t *)&pdu->outdata.data[16] = htonl(itt);
}

void
iscsi_pdu_set_pduflags(struct iscsi_pdu *pdu, unsigned char flags)
{
	pdu->outdata.data[1] = flags;
}

void
iscsi_pdu_set_immediate(struct iscsi_pdu *pdu)
{
	pdu->outdata.data[0] |= ISCSI_PDU_IMMEDIATE;
}

void
iscsi_pdu_set_ttt(struct iscsi_pdu *pdu, uint32_t ttt)
{
	*(uint32_t *)&pdu->outdata.data[20] = htonl(ttt);
}

void
iscsi_pdu_set_cmdsn(struct iscsi_pdu *pdu, uint32_t cmdsn)
{
	*(uint32_t *)&pdu->outdata.data[24] = htonl(cmdsn);
}

void
iscsi_pdu_set_datasn(struct iscsi_pdu *pdu, uint32_t datasn)
{
	*(uint32_t *)&pdu->outdata.data[36] = htonl(datasn);
}

void
iscsi_pdu_set_expstatsn(struct iscsi_pdu *pdu, uint32_t expstatsnsn)
{
	*(uint32_t *)&pdu->outdata.data[28] = htonl(expstatsnsn);
}

void
iscsi_pdu_set_bufferoffset(struct iscsi_pdu *pdu, uint32_t bufferoffset)
{
	*(uint32_t *)&pdu->outdata.data[40] = htonl(bufferoffset);
}

void
iscsi_pdu_set_cdb(struct iscsi_pdu *pdu, struct scsi_task *task)
{
  memset(&pdu->outdata.data[32], 0, 16);
	memcpy(&pdu->outdata.data[32], task->cdb, task->cdb_size);
}

void
iscsi_pdu_set_lun(struct iscsi_pdu *pdu, uint32_t lun)
{
	pdu->outdata.data[9] = lun;
}

void
iscsi_pdu_set_expxferlen(struct iscsi_pdu *pdu, uint32_t expxferlen)
{
	*(uint32_t *)&pdu->outdata.data[20] = htonl(expxferlen);
}
