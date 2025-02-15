/****************************************************************************
 *
 * Copyright (c) 2014 - 2021 Samsung Electronics Co., Ltd. All rights reserved
 *
 ****************************************************************************/

/**
 * MIF Byte Stream (Implementation)
 */

/* Implements */
#include "scsc_mx_impl.h"
#include "mifstream.h"

/* Uses */
#include "mifintrbit.h"

/* Public Functions */

void mif_stream_config_serialise(struct mif_stream *stream, struct mxstreamconf *stream_conf)
{
	stream_conf->read_bit_idx = stream->read_bit_idx;
	stream_conf->write_bit_idx = stream->write_bit_idx;
	cpacketbuffer_config_serialise(&stream->buffer, &stream_conf->buf_conf);
}

int mif_stream_init(struct mif_stream *stream, enum scsc_mif_abs_target target, enum MIF_STREAM_DIRECTION direction, uint32_t num_packets, uint32_t packet_size,
		    struct scsc_mx *mx, enum MIF_STREAM_INTRBIT_TYPE intrbit, mifintrbit_handler tohost_irq_handler, void *data)
{
	struct mifintrbit *intr;
	int               r, r1, r2;

#if IS_ENABLED(CONFIG_SCSC_INDEPENDENT_SUBSYSTEM)
	stream->buffer.target = target;
#endif
	stream->mx = mx;
	r = cpacketbuffer_init(&stream->buffer, num_packets, packet_size, mx);
	if (r)
		return r;

#if IS_ENABLED(CONFIG_SCSC_INDEPENDENT_SUBSYSTEM)
	/* Get the proper intr instance */
	if (target == SCSC_MIF_ABS_TARGET_WPAN)
		intr = scsc_mx_get_intrbit_wpan(mx);
	/* WLAN and Fleximac cores will use the WLAN instance */
	else
		intr = scsc_mx_get_intrbit(mx);

	stream->target = target;
#else
	intr = scsc_mx_get_intrbit(mx);
#endif

	r1 = mifintrbit_alloc_tohost(intr, tohost_irq_handler, data);
	if (r1 < 0) {
		cpacketbuffer_release(&stream->buffer);
		return r1;
	}

	/**
	 * MIF interrupt bit 0 in both the to-wpan and to-wlan registers are reserved
	 * for purpose of forcing panics from the MX Manager directly or via the
	 * gdb monitor stacks.
	 *
	 * At stream initialization the gdb transport requests for the reserved bits
	 * rather than dynamic allocation of interrupt bits.
	 *
	 * So if the interrupt bit type requested is Reserved, just Assign the pre-
	 * reserved interrupt bits.
	 */
	if (intrbit == MIF_STREAM_INTRBIT_TYPE_RESERVED) {
		if (target == SCSC_MIF_ABS_TARGET_FXM_1)
			r2 = MIFINTRBIT_RESERVED_PANIC_WLAN;
#ifdef CONFIG_SCSC_MX450_GDB_SUPPORT
		else if (target == SCSC_MIF_ABS_TARGET_FXM_2)
			r2 = MIFINTRBIT_RESERVED_PANIC_WLAN;
#endif
		else
			r2 = MIFINTRBIT_RESERVED_PANIC_WLAN;
	} else
#if IS_ENABLED(CONFIG_SCSC_INDEPENDENT_SUBSYSTEM)
		r2 = mifintrbit_alloc_fromhost(intr);
#else
		r2 = mifintrbit_alloc_fromhost(intr, target);
#endif

	if (r2 < 0) {
		cpacketbuffer_release(&stream->buffer);
		mifintrbit_free_tohost(intr, r1);
		return r2;
	}
	switch (direction) {
	case MIF_STREAM_DIRECTION_OUT:
		stream->read_bit_idx = r1;
		stream->write_bit_idx = r2;
		break;
	case MIF_STREAM_DIRECTION_IN:
		/* Default value for the shared memory region */
		memset(stream->buffer.buffer, 0xff, num_packets * packet_size);
		/* Commit */
		smp_wmb();
		stream->read_bit_idx = r2;
		stream->write_bit_idx = r1;
		break;
	default:
		cpacketbuffer_release(&stream->buffer);
		mifintrbit_free_tohost(intr, r1);
#if IS_ENABLED(CONFIG_SCSC_INDEPENDENT_SUBSYSTEM)
		mifintrbit_free_fromhost(intr, r2);
#else
		mifintrbit_free_fromhost(intr, r2, target);
#endif
		return -EINVAL;
	}
	stream->direction = direction;
	stream->peer = (enum MIF_STREAM_PEER)target;
	return 0;
}

void mif_stream_release(struct mif_stream *stream)
{
	struct mifintrbit *intr;

	if (!stream->mx)
		return;

#if defined(CONFIG_SCSC_INDEPENDENT_SUBSYSTEM)
	if (stream->target == SCSC_MIF_ABS_TARGET_WPAN)
		intr = scsc_mx_get_intrbit_wpan(stream->mx);
	else
		intr = scsc_mx_get_intrbit(stream->mx);
#else
	intr = scsc_mx_get_intrbit(stream->mx);
#endif
	if (stream->direction == MIF_STREAM_DIRECTION_IN) {
		mifintrbit_free_tohost(intr, stream->write_bit_idx);
#if IS_ENABLED(CONFIG_SCSC_INDEPENDENT_SUBSYSTEM)
		mifintrbit_free_fromhost(intr, stream->read_bit_idx);
#else
		mifintrbit_free_fromhost(intr, stream->read_bit_idx, (enum scsc_mif_abs_target)stream->peer);
#endif
	} else {
		mifintrbit_free_tohost(intr, stream->read_bit_idx);
#if IS_ENABLED(CONFIG_SCSC_INDEPENDENT_SUBSYSTEM)
		mifintrbit_free_fromhost(intr, stream->write_bit_idx);
#else
		mifintrbit_free_fromhost(intr, stream->write_bit_idx, (enum scsc_mif_abs_target)stream->peer);
#endif
	}
	cpacketbuffer_release(&stream->buffer);

	stream->mx = NULL;
}

uint32_t mif_stream_read(struct mif_stream *stream, void *buf, uint32_t num_bytes)
{
	struct scsc_mif_abs *mif_abs = scsc_mx_get_mif_abs(stream->mx);

	uint32_t            num_bytes_read = cpacketbuffer_read(&stream->buffer, buf, num_bytes);

#if IS_ENABLED(CONFIG_SCSC_INDEPENDENT_SUBSYSTEM)
	if (num_bytes_read > 0)
		mif_abs->irq_bit_set(mif_abs, stream->read_bit_idx, stream->target);
#else
	if (num_bytes_read > 0)
		/* Signal that the read is finished to anyone interested */
		mif_abs->irq_bit_set(mif_abs, stream->read_bit_idx, (enum scsc_mif_abs_target)stream->peer);
#endif

	return num_bytes_read;
}

const void *mif_stream_peek(struct mif_stream *stream, const void *current_packet)
{
	return cpacketbuffer_peek(&stream->buffer, current_packet);
}

void mif_stream_peek_complete(struct mif_stream *stream, const void *packet)
{
	struct scsc_mif_abs *mif_abs = scsc_mx_get_mif_abs(stream->mx);

	cpacketbuffer_peek_complete(&stream->buffer, packet);

	/* Signal that the read is finished to anyone interested */
#if IS_ENABLED(CONFIG_SCSC_INDEPENDENT_SUBSYSTEM)
	mif_abs->irq_bit_set(mif_abs, stream->read_bit_idx, stream->target);
#else
	mif_abs->irq_bit_set(mif_abs, stream->read_bit_idx, (enum scsc_mif_abs_target)stream->peer);
#endif
}

bool mif_stream_write(struct mif_stream *stream, const void *buf, uint32_t num_bytes)
{
	struct scsc_mif_abs *mif_abs = scsc_mx_get_mif_abs(stream->mx);

	if (!cpacketbuffer_write(&stream->buffer, buf, num_bytes))
		return false;

	/* Kick the assigned interrupt to let others know new data is available */
#if IS_ENABLED(CONFIG_SCSC_INDEPENDENT_SUBSYSTEM)
	mif_abs->irq_bit_set(mif_abs, stream->write_bit_idx, stream->target);
#else
	mif_abs->irq_bit_set(mif_abs, stream->write_bit_idx, (enum scsc_mif_abs_target)stream->peer);
#endif

	return true;
}

bool mif_stream_write_gather(struct mif_stream *stream, const void **bufs, uint32_t *lengths, uint32_t num_bufs)
{
	struct scsc_mif_abs *mif_abs = NULL;

	if (!stream->mx || !stream->buffer.mx)
		return false;

	mif_abs = scsc_mx_get_mif_abs(stream->mx);

	if (!cpacketbuffer_write_gather(&stream->buffer, bufs, lengths, num_bufs))
		return false;

	/* Kick the assigned interrupt to let others know new data is available */
#if IS_ENABLED(CONFIG_SCSC_INDEPENDENT_SUBSYSTEM)
	mif_abs->irq_bit_set(mif_abs, stream->write_bit_idx, stream->target);
#else
	mif_abs->irq_bit_set(mif_abs, stream->write_bit_idx, (enum scsc_mif_abs_target)stream->peer);
#endif
	return true;
}

uint32_t mif_stream_block_size(struct mif_stream *stream)
{
	return cpacketbuffer_packet_size(&stream->buffer);
}

uint8_t mif_stream_read_interrupt(struct mif_stream *stream)
{
	return stream->read_bit_idx;
}

uint8_t mif_stream_write_interrupt(struct mif_stream *stream)
{
	return stream->write_bit_idx;
}

void mif_stream_log(const struct mif_stream *stream, enum scsc_log_level log_level)
{
	cpacketbuffer_log(&stream->buffer, log_level);
}

