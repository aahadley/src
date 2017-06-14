/*-
 * Copyright (c) 2009-2012,2016 Microsoft Corp.
 * Copyright (c) 2012 NetApp Inc.
 * Copyright (c) 2012 Citrix Inc.
 * Copyright (c) 2017 Mike Belopuhov <mike@esdenera.com>
 * All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice unmodified, this list of conditions, and the following
 *    disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 *
 * THIS SOFTWARE IS PROVIDED BY THE AUTHOR ``AS IS'' AND ANY EXPRESS OR
 * IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES
 * OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE DISCLAIMED.
 * IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR ANY DIRECT, INDIRECT,
 * INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT
 * NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * The OpenBSD port was done under funding by Esdenera Networks GmbH.
 */

/* #define HVS_DEBUG_IO */

#include <sys/param.h>
#include <sys/systm.h>
#include <sys/atomic.h>
#include <sys/device.h>
#include <sys/kernel.h>
#include <sys/buf.h>
#include <sys/malloc.h>
#include <sys/task.h>

#include <machine/bus.h>

#include <uvm/uvm_extern.h>

#include <dev/pv/hypervreg.h>
#include <dev/pv/hypervvar.h>

#include <scsi/scsi_all.h>
#include <scsi/cd.h>
#include <scsi/scsi_disk.h>
#include <scsi/scsiconf.h>

#define HVS_PROTO_VERSION_WIN6		 0x200
#define HVS_PROTO_VERSION_WIN7		 0x402
#define HVS_PROTO_VERSION_WIN8		 0x501
#define HVS_PROTO_VERSION_WIN8_1	 0x600
#define HVS_PROTO_VERSION_WIN10		 0x602

#define HVS_MSG_IODONE			 0x01
#define HVS_MSG_DEVGONE			 0x02
#define HVS_MSG_ENUMERATE		 0x0b

#define HVS_REQ_SCSIIO			 0x03
#define HVS_REQ_STARTINIT		 0x07
#define HVS_REQ_FINISHINIT		 0x08
#define HVS_REQ_QUERYPROTO		 0x09
#define HVS_REQ_QUERYPROPS		 0x0a

struct hvs_cmd_hdr {
	uint32_t		 hdr_op;
	uint32_t		 hdr_flags;
	uint32_t		 hdr_status;
#define cmd_op			 cmd_hdr.hdr_op
#define cmd_flags		 cmd_hdr.hdr_flags
#define cmd_status		 cmd_hdr.hdr_status
} __packed;

/* Negotiate version */
struct hvs_cmd_ver {
	struct hvs_cmd_hdr	 cmd_hdr;
	uint16_t		 cmd_ver;
	uint16_t		 cmd_rev;
} __packed;

/* Query channel properties */
struct hvs_chp {
	uint16_t		 chp_proto;
	uint8_t			 chp_path;
	uint8_t			 chp_target;
	uint16_t		 chp_maxchan;
	uint16_t		 chp_port;
	uint32_t		 chp_chflags;
	uint32_t		 chp_maxfer;
	uint64_t		 chp_chanid;
} __packed;

struct hvs_cmd_chp {
	struct hvs_cmd_hdr	 cmd_hdr;
	struct hvs_chp		 cmd_chp;
} __packed;

#define SENSE_DATA_LEN_WIN7		 18
#define SENSE_DATA_LEN			 20
#define MAX_SRB_DATA			 20

/* SCSI Request Block */
struct hvs_srb {
	uint16_t		 srb_reqlen;
	uint8_t			 srb_iostatus;
	uint8_t			 srb_scsistatus;

	uint8_t			 srb_initiator;
	uint8_t			 srb_bus;
	uint8_t			 srb_target;
	uint8_t			 srb_lun;

	uint8_t			 srb_cdblen;
	uint8_t			 srb_senselen;
	uint8_t			 srb_direction;
	uint8_t			 _reserved;

	uint32_t		 srb_datalen;
	uint8_t			 srb_data[MAX_SRB_DATA];
} __packed;

#define SRB_DATA_WRITE			 0
#define SRB_DATA_READ			 1
#define SRB_DATA_NONE			 2

#define SRB_STATUS_PENDING		 0x00
#define SRB_STATUS_SUCCESS		 0x01
#define SRB_STATUS_ABORTED		 0x02
#define SRB_STATUS_ERROR		 0x04
#define SRB_STATUS_INVALID_LUN		 0x20
#define SRB_STATUS_QUEUE_FROZEN		 0x40
#define SRB_STATUS_AUTOSENSE_VALID	 0x80

#define SRB_FLAGS_QUEUE_ACTION_ENABLE	 0x00000002
#define SRB_FLAGS_DISABLE_DISCONNECT	 0x00000004
#define SRB_FLAGS_DISABLE_SYNCH_TRANSFER 0x00000008
#define SRB_FLAGS_BYPASS_FROZEN_QUEUE	 0x00000010
#define SRB_FLAGS_DISABLE_AUTOSENSE	 0x00000020
#define SRB_FLAGS_DATA_IN		 0x00000040
#define SRB_FLAGS_DATA_OUT		 0x00000080
#define SRB_FLAGS_NO_DATA_TRANSFER	 0x00000000
#define SRB_FLAGS_NO_QUEUE_FREEZE	 0x00000100
#define SRB_FLAGS_ADAPTER_CACHE_ENABLE	 0x00000200
#define SRB_FLAGS_FREE_SENSE_BUFFER	 0x00000400

/* SRB command for Win7 and older */
struct hvs_cmd_io {
	struct hvs_cmd_hdr	 cmd_hdr;
	struct hvs_srb		 cmd_srb;
} __packed;

/* SRB command with Win8 extensions */
struct hvs_cmd_xio {
	struct hvs_cmd_hdr	 cmd_hdr;
	struct hvs_srb		 cmd_srb;
	uint16_t		 _reserved;
	uint8_t			 cmd_qtag;
	uint8_t			 cmd_qaction;
	uint32_t		 cmd_srbflags;
	uint32_t		 cmd_timeout;
	uint32_t		 cmd_qsortkey;
} __packed;

#define HVS_INIT_TID			 0x1984
#define HVS_CMD_SIZE			 64

union hvs_cmd {
	struct hvs_cmd_hdr	 cmd_hdr;
	struct hvs_cmd_ver	 ver;
	struct hvs_cmd_chp	 chp;
	struct hvs_cmd_io	 io;
	struct hvs_cmd_xio	 xio;
	uint8_t			 pad[HVS_CMD_SIZE];
} __packed;

#define HVS_RING_SIZE			 (20 * PAGE_SIZE)
#define HVS_MAX_CCB			 128
#define HVS_MAX_SGE			 (MAXPHYS / PAGE_SIZE + 1)

struct hvs_ccb {
	struct scsi_xfer	*ccb_xfer;  /* associated transfer */
	bus_dmamap_t		 ccb_dmap;  /* transfer map */
	uint64_t		 ccb_rid;   /* request id */
	struct vmbus_gpa_range	*ccb_sgl;
	int			 ccb_nsge;
	SIMPLEQ_ENTRY(hvs_ccb)	 ccb_link;
};
SIMPLEQ_HEAD(hvs_ccb_queue, hvs_ccb);

struct hvs_softc {
	struct device		 sc_dev;
	struct hv_softc		*sc_hvsc;
	struct hv_channel	*sc_chan;
	bus_dma_tag_t		 sc_dmat;

	int			 sc_proto;
	int			 sc_flags;
#define  HVSF_SCSI		  0x0001
#define  HVSF_XIO		  0x0002
	struct hvs_chp		 sc_props;

	union hvs_cmd		 sc_resp;
	struct mutex		 sc_resplck;
	uint32_t		 sc_rid;

	/* CCBs */
	int			 sc_nccb;
	struct hvs_ccb		*sc_ccbs;
	struct hvs_ccb_queue	 sc_ccb_fq; /* free queue */
	struct mutex		 sc_ccb_fqlck;

	int			 sc_bus;
	int			 sc_initiator;

	struct scsi_iopool	 sc_iopool;
	struct scsi_adapter	 sc_switch;
	struct scsi_link         sc_link;
	struct device		*sc_scsibus;
	struct task		 sc_probetask;
};

int	hvs_match(struct device *, void *, void *);
void	hvs_attach(struct device *, struct device *, void *);

void	hvs_scsi_cmd(struct scsi_xfer *);
void	hvs_scsi_probe(void *arg);
void	hvs_intr(void *);
void	hvs_complete_cmd(struct hvs_softc *, union hvs_cmd *, uint64_t);
void	hvs_scsi_done(struct scsi_xfer *, int);

int	hvs_connect(struct hvs_softc *);
int	hvs_cmd(struct hvs_softc *, void *);

int	hvs_alloc_ccbs(struct hvs_softc *);
void	hvs_free_ccbs(struct hvs_softc *);
void	*hvs_get_ccb(void *);
void	hvs_put_ccb(void *, void *);

struct cfdriver hvs_cd = {
	NULL, "hvs", DV_DULL
};

const struct cfattach hvs_ca = {
	sizeof(struct hvs_softc), hvs_match, hvs_attach
};

int
hvs_match(struct device *parent, void *match, void *aux)
{
	struct hv_attach_args *aa = aux;

	if (/* strcmp("ide", aa->aa_ident) && */
	    strcmp("scsi", aa->aa_ident))
		return (0);

	return (1);
}

void
hvs_attach(struct device *parent, struct device *self, void *aux)
{
	struct hv_attach_args *aa = aux;
	struct hvs_softc *sc = (struct hvs_softc *)self;
	struct scsibus_attach_args saa;

	sc->sc_hvsc = (struct hv_softc *)parent;
	sc->sc_chan = aa->aa_chan;
	sc->sc_dmat = aa->aa_dmat;

	if (strcmp("scsi", aa->aa_ident) == 0)
		sc->sc_flags |= HVSF_SCSI;

	if (hv_channel_open(sc->sc_chan, HVS_RING_SIZE, &sc->sc_props,
	    sizeof(sc->sc_props), hvs_intr, sc)) {
		printf(": failed to open channel\n");
		return;
	}

	hv_evcount_attach(sc->sc_chan, sc->sc_dev.dv_xname);

	printf(" channel %u: %s", sc->sc_chan->ch_id, aa->aa_ident);

	if (hvs_connect(sc))
		return;

	printf(", protocol %u.%u\n", (sc->sc_proto >> 8) & 0xff,
	    sc->sc_proto & 0xff);

	if (sc->sc_proto >= HVS_PROTO_VERSION_WIN8)
		sc->sc_flags |= HVSF_XIO;

	if (hvs_alloc_ccbs(sc))
		return;

	task_set(&sc->sc_probetask, hvs_scsi_probe, sc);

	sc->sc_switch.scsi_cmd = hvs_scsi_cmd;
	sc->sc_switch.scsi_minphys = scsi_minphys;

	sc->sc_link.adapter = &sc->sc_switch;
	sc->sc_link.adapter_softc = self;
	sc->sc_link.adapter_buswidth = sc->sc_flags & HVSF_SCSI ? 64 : 1;
	sc->sc_link.adapter_target = sc->sc_flags & HVSF_SCSI ? 64 : 1;
	sc->sc_link.openings = sc->sc_nccb;
	sc->sc_link.pool = &sc->sc_iopool;

	memset(&saa, 0, sizeof(saa));
	saa.saa_sc_link = &sc->sc_link;
	sc->sc_scsibus = config_found(self, &saa, scsiprint);
}

void
hvs_scsi_cmd(struct scsi_xfer *xs)
{
	struct scsi_link *link = xs->sc_link;
	struct hvs_softc *sc = link->adapter_softc;
	struct hvs_ccb *ccb = xs->io;
	union hvs_cmd cmd;
	struct hvs_cmd_io *io = &cmd.io;
	struct hvs_cmd_xio *xio = &cmd.xio;
	struct hvs_srb *srb = &io->cmd_srb;
	int i, rv, flags = BUS_DMA_NOWAIT;
	uint64_t rid;

	if (xs->cmdlen > HVS_CMD_SIZE) {
		printf("%s: CDB is too big: %d\n", sc->sc_dev.dv_xname,
		    xs->cmdlen);
		memset(&xs->sense, 0, sizeof(xs->sense));
		xs->sense.error_code = SSD_ERRCODE_VALID | 0x70;
		xs->sense.flags = SKEY_ILLEGAL_REQUEST;
		xs->sense.add_sense_code = 0x20;
		hvs_scsi_done(xs, XS_SENSE);
		return;
	}

	KERNEL_UNLOCK();

	memset(&cmd, 0, sizeof(cmd));

	srb->srb_initiator = sc->sc_initiator;
	srb->srb_bus = sc->sc_bus;
	srb->srb_target = link->target;
	srb->srb_lun = link->lun;

	srb->srb_cdblen = xs->cmdlen;
	memcpy(srb->srb_data, xs->cmd, xs->cmdlen);

	switch (xs->flags & (SCSI_DATA_IN | SCSI_DATA_OUT)) {
	case SCSI_DATA_IN:
		srb->srb_direction = SRB_DATA_READ;
		if (sc->sc_flags & HVSF_XIO)
			xio->cmd_srbflags |= SRB_FLAGS_DATA_IN;
		flags |= BUS_DMA_WRITE;
		break;
	case SCSI_DATA_OUT:
		srb->srb_direction = SRB_DATA_WRITE;
		if (sc->sc_flags & HVSF_XIO)
			xio->cmd_srbflags |= SRB_FLAGS_DATA_OUT;
		flags |= BUS_DMA_READ;
		break;
	default:
		srb->srb_direction = SRB_DATA_NONE;
		if (sc->sc_flags & HVSF_XIO)
			xio->cmd_srbflags |= SRB_FLAGS_NO_DATA_TRANSFER;
		break;
	}

	srb->srb_datalen = xs->datalen;

	if (sc->sc_flags & HVSF_XIO) {
		srb->srb_reqlen = sizeof(*xio);
		srb->srb_senselen = SENSE_DATA_LEN;
	} else {
		srb->srb_reqlen = sizeof(*io);
		srb->srb_senselen = SENSE_DATA_LEN_WIN7;
	}

	cmd.cmd_op = HVS_REQ_SCSIIO;
	cmd.cmd_flags = VMBUS_CHANPKT_FLAG_RC;

	rid = (uint64_t)ccb->ccb_rid << 32;

	if (xs->datalen > 0) {
		rv = bus_dmamap_load(sc->sc_dmat, ccb->ccb_dmap, xs->data,
		    xs->datalen, NULL, flags);
		if (rv) {
			printf("%s: failed to load %d bytes (%d)\n",
			    sc->sc_dev.dv_xname, xs->datalen, rv);
			KERNEL_LOCK();
			hvs_scsi_done(xs, XS_DRIVER_STUFFUP);
			return;
		}

		ccb->ccb_sgl->gpa_len = xs->datalen;
		ccb->ccb_sgl->gpa_ofs = (vaddr_t)xs->data & PAGE_MASK;
		for (i = 0; i < ccb->ccb_dmap->dm_nsegs; i++)
			ccb->ccb_sgl->gpa_page[i] =
			    atop(ccb->ccb_dmap->dm_segs[i].ds_addr);
		ccb->ccb_nsge = ccb->ccb_dmap->dm_nsegs;
	}

	ccb->ccb_xfer = xs;

	if (xs->datalen > 0) {
		rv = hv_channel_send_prpl(sc->sc_chan, ccb->ccb_sgl,
		    ccb->ccb_nsge, &cmd, sizeof(cmd), rid);
		if (rv) {
			printf("%s: failed to submit operation %x via prpl\n",
			    sc->sc_dev.dv_xname, xs->cmd->opcode);
			bus_dmamap_unload(sc->sc_dmat, ccb->ccb_dmap);
		}
	} else {
		rv = hv_channel_send(sc->sc_chan, &cmd, sizeof(cmd), rid,
		    VMBUS_CHANPKT_TYPE_INBAND, VMBUS_CHANPKT_FLAG_RC);
		if (rv)
			printf("%s: failed to submit operation %x\n",
			    sc->sc_dev.dv_xname, xs->cmd->opcode);
	}
	if (rv) {
		KERNEL_LOCK();
		hvs_scsi_done(xs, XS_DRIVER_STUFFUP);
		return;
	}

#ifdef HVS_DEBUG_IO
	DPRINTF("%s: opcode %#x flags %#x datalen %d\n", sc->sc_dev.dv_xname,
	    xs->cmd->opcode, xs->flags, xs->datalen);
#endif

	if (xs->flags & SCSI_POLL) {
		int timo = 1000;

		do {
			if (xs->flags & ITSDONE)
				break;
			if (xs->flags & SCSI_NOSLEEP)
				delay(100);
			else
				tsleep(xs, PRIBIO, "hvspoll", 1);
			hvs_intr(sc);
		} while(--timo > 0);

		if (!(xs->flags & ITSDONE)) {
			printf("%s: operation %#x datalen %d timed out\n",
			    sc->sc_dev.dv_xname, xs->cmd->opcode, xs->datalen);
			KERNEL_LOCK();
			hvs_scsi_done(xs, XS_TIMEOUT);
			return;
		}
	}

	KERNEL_LOCK();
}

void
hvs_scsi_probe(void *arg)
{
	struct hvs_softc *sc = arg;

	if (sc->sc_scsibus)
		scsi_probe_bus((void *)sc->sc_scsibus);
}

void
hvs_intr(void *xsc)
{
	struct hvs_softc *sc = xsc;
	union hvs_cmd cmd;
	uint64_t rid;
	uint32_t rlen;
	int rv;

	for (;;) {
		rv = hv_channel_recv(sc->sc_chan, &cmd, sizeof(cmd), &rlen,
		    &rid, 0);
		if (rv != 0 || rlen == 0) {
			if (rv != EAGAIN)
				printf("%s: failed to receive a packet (%d-%u)\n",
				    sc->sc_dev.dv_xname, rv, rlen);
			break;
		}
		if (rlen != sizeof(cmd)) {
			printf("%s: short read: %u\n", sc->sc_dev.dv_xname,
			    rlen);
			return;
		}

#ifdef HVS_DEBUG_IO
		DPRINTF("%s: opertaion %u flags %#x status %#x\n",
		    sc->sc_dev.dv_xname, cmd.cmd_op, cmd.cmd_flags,
		    cmd.cmd_status);
#endif

		/* Initialization */
		if (rid == HVS_INIT_TID) {
			memcpy(&sc->sc_resp, &cmd, sizeof(cmd));
			wakeup_one(&sc->sc_resp);
			continue;
		}

		switch (cmd.cmd_op) {
		case HVS_MSG_IODONE:
			hvs_complete_cmd(sc, &cmd, rid);
			break;
		case HVS_MSG_ENUMERATE:
			task_add(systq, &sc->sc_probetask);
			break;
		default:
			printf("%s: operation %u is not implemented\n",
			    sc->sc_dev.dv_xname, cmd.cmd_op);
		}
	}
}

static inline int
is_inquiry_valid(struct scsi_inquiry_data *inq)
{
	if ((inq->device & SID_TYPE) == T_NODEVICE)
		return (0);
	if ((inq->device & SID_QUAL) == SID_QUAL_BAD_LU)
		return (0);
	return (1);
}

static inline void
fixup_inquiry(struct scsi_xfer *xs, struct hvs_srb *srb)
{
	struct hvs_softc *sc = xs->sc_link->adapter_softc;
	struct scsi_inquiry_data *inq = (struct scsi_inquiry_data *)xs->data;
	int datalen, resplen;
	char vendor[16];

	resplen = srb->srb_datalen >= 5 ? inq->additional_length + 5 : 0;
	datalen = MIN(resplen, srb->srb_datalen);

	/* Fixup wrong response from WS2012 */
	if ((sc->sc_proto == HVS_PROTO_VERSION_WIN8_1 ||
	    sc->sc_proto == HVS_PROTO_VERSION_WIN8 ||
	    sc->sc_proto == HVS_PROTO_VERSION_WIN7) &&
	    !is_inquiry_valid(inq) && datalen >= 4 &&
	    (inq->version == 0 || inq->response_format == 0)) {
		inq->version = 0x05; /* SPC-3 */
		inq->response_format = 2;
	} else if (datalen >= SID_INQUIRY_HDR + SID_SCSI2_ALEN) {
		/*
		 * Upgrade SPC2 to SPC3 if host is Win8 or WS2012 R2
		 * to support UNMAP feature.
		 */
		scsi_strvis(vendor, inq->vendor, sizeof(vendor));
		if ((sc->sc_proto == HVS_PROTO_VERSION_WIN8_1 ||
		    sc->sc_proto == HVS_PROTO_VERSION_WIN8) &&
		    SCSISPC(inq->version) == 2 &&
		    !strncmp(vendor, "Msft", 4))
			inq->version = 0x05; /* SPC-3 */
	}
}

void
hvs_complete_cmd(struct hvs_softc *sc, union hvs_cmd *cmd, uint64_t rid)
{
	struct scsi_xfer *xs;
	struct hvs_ccb *ccb;
	struct hvs_srb *srb;
	bus_dmamap_t map;
	int error;

	if ((rid & 0xffffffff) != 0 || rid >> 32 >= sc->sc_nccb) {
		printf("%s: invalid response %#llx\n", sc->sc_dev.dv_xname,
		    rid);
		return;
	}

	ccb = &sc->sc_ccbs[rid >> 32];

	map = ccb->ccb_dmap;
	bus_dmamap_sync(sc->sc_dmat, map, 0, map->dm_mapsize,
	    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
	bus_dmamap_unload(sc->sc_dmat, map);

	xs = ccb->ccb_xfer;
	srb = &cmd->io.cmd_srb;

	if (srb->srb_datalen > xs->datalen)
		printf("%s: transfer length %u too large: %u\n",
		    sc->sc_dev.dv_xname, srb->srb_datalen, xs->datalen);
	else if (srb->srb_datalen)
		xs->resid = xs->datalen - srb->srb_datalen;

	if ((srb->srb_scsistatus & 0xff) == SCSI_CHECK &&
	    srb->srb_iostatus & SRB_STATUS_AUTOSENSE_VALID)
		memcpy(&xs->sense, srb->srb_data, MIN(sizeof(xs->sense),
		    srb->srb_senselen));

	error = srb->srb_scsistatus & 0xff;

	if (srb->srb_scsistatus != SCSI_OK) {
		KERNEL_LOCK();
		hvs_scsi_done(xs, error);
		KERNEL_UNLOCK();
		return;
	}

	if ((srb->srb_iostatus & ~(SRB_STATUS_AUTOSENSE_VALID |
	    SRB_STATUS_QUEUE_FROZEN)) != SRB_STATUS_SUCCESS)
		error = XS_SELTIMEOUT;
	else if (xs->cmd->opcode == INQUIRY)
		fixup_inquiry(xs, srb);

	KERNEL_LOCK();
	hvs_scsi_done(xs, error);
	KERNEL_UNLOCK();
}

void
hvs_scsi_done(struct scsi_xfer *xs, int error)
{
	int s;

	KERNEL_ASSERT_LOCKED();

	xs->error = error;

	s = splbio();
	scsi_done(xs);
	splx(s);
}

int
hvs_connect(struct hvs_softc *sc)
{
	const uint32_t protos[] = {
		HVS_PROTO_VERSION_WIN10,
		HVS_PROTO_VERSION_WIN8_1,
		HVS_PROTO_VERSION_WIN8,
		HVS_PROTO_VERSION_WIN7,
		HVS_PROTO_VERSION_WIN6
	};
	union hvs_cmd ucmd;
	struct hvs_cmd_ver *cmd;
	struct hvs_chp *chp;
	int i;

	mtx_init(&sc->sc_resplck, IPL_BIO);

	cmd = (struct hvs_cmd_ver *)&ucmd;

	/*
	 * Begin initialization
	 */

	memset(&ucmd, 0, sizeof(ucmd));

	cmd->cmd_op = HVS_REQ_STARTINIT;
	cmd->cmd_flags = VMBUS_CHANPKT_FLAG_RC;

	if (hvs_cmd(sc, cmd)) {
		printf(": failed to send initialization command\n");
		return (-1);
	}
	if (sc->sc_resp.cmd_op != HVS_MSG_IODONE ||
	    sc->sc_resp.cmd_status != 0) {
		printf(": failed to initialize, status %#x\n",
		    sc->sc_resp.cmd_status);
		return (-1);
	}

	/*
	 * Negotiate protocol version
	 */

	memset(&ucmd, 0, sizeof(ucmd));

	cmd->cmd_op = HVS_REQ_QUERYPROTO;
	cmd->cmd_flags = VMBUS_CHANPKT_FLAG_RC;

	for (i = 0; i < nitems(protos); i++) {
		cmd->cmd_ver = protos[i];

		if (hvs_cmd(sc, cmd)) {
			printf(": failed to send protocol query\n");
			return (-1);
		}
		if (sc->sc_resp.cmd_op != HVS_MSG_IODONE) {
			printf(": failed to negotiate protocol, status %#x\n",
			    sc->sc_resp.cmd_status);
			return (-1);
		}
		if (sc->sc_resp.cmd_status == 0) {
			sc->sc_proto = protos[i];
			break;
		}
	}
	if (!sc->sc_proto) {
		printf(": failed to negotiate protocol version\n");
		return (-1);
	}

	/*
	 * Query channel properties
	 */

	memset(&ucmd, 0, sizeof(ucmd));

	cmd->cmd_op = HVS_REQ_QUERYPROPS;
	cmd->cmd_flags = VMBUS_CHANPKT_FLAG_RC;

	if (hvs_cmd(sc, cmd)) {
		printf(": failed to send channel properties query\n");
		return (-1);
	}
	if (sc->sc_resp.cmd_op != HVS_MSG_IODONE ||
	    sc->sc_resp.cmd_status != 0) {
		printf(": failed to obtain channel properties, status %#x\n",
		    sc->sc_resp.cmd_status);
		return (-1);
	}
	chp = &sc->sc_resp.chp.cmd_chp;

	DPRINTF(": proto %#x path %u target %u maxchan %u",
	    chp->chp_proto, chp->chp_path, chp->chp_target,
	    chp->chp_maxchan);
	DPRINTF(" port %u chflags %#x maxfer %u chanid %#llx",
	    chp->chp_port, chp->chp_chflags, chp->chp_maxfer,
	    chp->chp_chanid);

	/* XXX */
	sc->sc_bus = chp->chp_path;
	sc->sc_initiator = chp->chp_target;

	/*
	 * Finish initialization
	 */

	memset(&ucmd, 0, sizeof(ucmd));

	cmd->cmd_op = HVS_REQ_FINISHINIT;
	cmd->cmd_flags = VMBUS_CHANPKT_FLAG_RC;

	if (hvs_cmd(sc, cmd)) {
		printf(": failed to send initialization finish\n");
		return (-1);
	}
	if (sc->sc_resp.cmd_op != HVS_MSG_IODONE ||
	    sc->sc_resp.cmd_status != 0) {
		printf(": failed to finish initialization, status %#x\n",
		    sc->sc_resp.cmd_status);
		return (-1);
	}

	return (0);
}

int
hvs_cmd(struct hvs_softc *sc, void *xcmd)
{
	union hvs_cmd *cmd = xcmd;
	int tries = 10;
	int rv;

	do {
		rv = hv_channel_send(sc->sc_chan, cmd, HVS_CMD_SIZE,
		    HVS_INIT_TID, VMBUS_CHANPKT_TYPE_INBAND,
		    VMBUS_CHANPKT_FLAG_RC);
		if (rv == EAGAIN)
			tsleep(cmd, PRIBIO, "hvsout", 1);
		else if (rv) {
			DPRINTF("%s: operation %u send error %d\n",
			    sc->sc_dev.dv_xname, cmd->cmd_op, rv);
			return (rv);
		}
	} while (rv != 0 && --tries > 0);

	mtx_enter(&sc->sc_resplck);
	rv = msleep(&sc->sc_resp, &sc->sc_resplck, PRIBIO, "hvscmd", 5 * hz);
	mtx_leave(&sc->sc_resplck);
	if (rv == EWOULDBLOCK)
		printf("%s: operation %u timed out\n", sc->sc_dev.dv_xname,
		    cmd->cmd_op);
	return (rv);
}

int
hvs_alloc_ccbs(struct hvs_softc *sc)
{
	int i, error;

	SIMPLEQ_INIT(&sc->sc_ccb_fq);
	mtx_init(&sc->sc_ccb_fqlck, IPL_BIO);

	sc->sc_nccb = HVS_MAX_CCB;

	sc->sc_ccbs = mallocarray(sc->sc_nccb, sizeof(struct hvs_ccb),
	    M_DEVBUF, M_ZERO | M_NOWAIT);
	if (sc->sc_ccbs == NULL) {
		printf("%s: failed to allocate CCBs\n", sc->sc_dev.dv_xname);
		return (-1);
	}

	for (i = 0; i < sc->sc_nccb; i++) {
		error = bus_dmamap_create(sc->sc_dmat, MAXPHYS, HVS_MAX_SGE,
		    PAGE_SIZE, PAGE_SIZE, BUS_DMA_NOWAIT,
		    &sc->sc_ccbs[i].ccb_dmap);
		if (error) {
			printf("%s: failed to create a CCB memory map (%d)\n",
			    sc->sc_dev.dv_xname, error);
			goto errout;
		}

		sc->sc_ccbs[i].ccb_sgl = malloc(sizeof(struct vmbus_gpa_range) *
		    (HVS_MAX_SGE + 1), M_DEVBUF, M_ZERO | M_NOWAIT);
		if (sc->sc_ccbs[i].ccb_sgl == NULL) {
			printf("%s: failed to allocate SGL array\n",
			    sc->sc_dev.dv_xname);
			goto errout;
		}

		sc->sc_ccbs[i].ccb_rid = i;
		hvs_put_ccb(sc, &sc->sc_ccbs[i]);
	}

	scsi_iopool_init(&sc->sc_iopool, sc, hvs_get_ccb, hvs_put_ccb);

	return (0);

 errout:
	hvs_free_ccbs(sc);
	return (-1);
}

void
hvs_free_ccbs(struct hvs_softc *sc)
{
	struct hvs_ccb *ccb;
	int i;

	for (i = 0; i < sc->sc_nccb; i++) {
		ccb = &sc->sc_ccbs[i];
		if (ccb->ccb_dmap == NULL)
			continue;
		bus_dmamap_sync(sc->sc_dmat, ccb->ccb_dmap, 0, 0,
		    BUS_DMASYNC_POSTREAD | BUS_DMASYNC_POSTWRITE);
		bus_dmamap_unload(sc->sc_dmat, ccb->ccb_dmap);
		bus_dmamap_destroy(sc->sc_dmat, ccb->ccb_dmap);

		free(ccb->ccb_sgl, M_DEVBUF, sizeof(struct vmbus_gpa_range) *
		    (HVS_MAX_SGE + 1));
	}

	free(sc->sc_ccbs, M_DEVBUF, sc->sc_nccb * sizeof(struct hvs_ccb));
	sc->sc_ccbs = NULL;
	sc->sc_nccb = 0;
}

void *
hvs_get_ccb(void *xsc)
{
	struct hvs_softc *sc = xsc;
	struct hvs_ccb *ccb;

	mtx_enter(&sc->sc_ccb_fqlck);
	ccb = SIMPLEQ_FIRST(&sc->sc_ccb_fq);
	if (ccb != NULL)
		SIMPLEQ_REMOVE_HEAD(&sc->sc_ccb_fq, ccb_link);
	mtx_leave(&sc->sc_ccb_fqlck);

	return (ccb);
}

void
hvs_put_ccb(void *xsc, void *io)
{
	struct hvs_softc *sc = xsc;
	struct hvs_ccb *ccb = io;

	ccb->ccb_xfer = NULL;

	mtx_enter(&sc->sc_ccb_fqlck);
	SIMPLEQ_INSERT_HEAD(&sc->sc_ccb_fq, ccb, ccb_link);
	mtx_leave(&sc->sc_ccb_fqlck);
}