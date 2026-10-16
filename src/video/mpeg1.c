/**
 * @file mpeg1.c
 * @author Giovanni Bajo <giovannibajo@gmail.com>
 */
#include "mpeg1.h"
#include "n64sys.h"
#include "rspq.h"
#include "rdpq.h"
#include "rdpq_rect.h"
#include "rdpq_mode.h"
#include "yuv.h"
#include "debug.h"
#include "profile.h"
#include "utils.h"
#include "video_internal.h"
#include <assert.h>
#include <errno.h>
#include "mpeg1_internal.h"

typedef struct mpeg1_s {
	video_t video;
	plm_buffer_t *buf;
	plm_video_t *v;
	plm_frame_t *f;
	int buffered_pics;
	bool has_reader;        ///< Opened with video_open_reader
	bool reader_blocking;   ///< The load callback may wait (only while opening)
	bool reader_ended;      ///< The reader returned VIDEO_READ_END
	video_reader_t reader;  ///< Where the bytes come from (reader mode)
} mpeg1_t;

/** @brief Default compressed-data buffer for reader mode (video_parms_t::reader_buffer_size) */
#define MPEG1_READER_BUFFER_DEFAULT   (32 * 1024)

typedef enum {
	PS_MPEG = PROFILE_SLOT_MPEG1,
	PS_MPEG_FINDSTART,
	PS_MPEG_HASSTART,
	PS_MPEG_DECODESLICE,
	PS_MPEG_MB,
	PS_MPEG_MB_MV,
	PS_MPEG_MB_PREDICT,
	PS_MPEG_MB_DECODE,
	PS_MPEG_MB_DECODE_DC,
	PS_MPEG_MB_DECODE_AC,
	PS_MPEG_MB_DECODE_AC_VLC,
	PS_MPEG_MB_DECODE_AC_CODE,
	PS_MPEG_MB_DECODE_AC_DEQUANT,
	PS_MPEG_MB_DECODE_BLOCK,
	PS_MPEG_MB_DECODE_BLOCK_IDCT,
} MPEG1ProfileSlot;

DEFINE_RSP_UCODE(rsp_mpeg1);

static uint32_t ovl_id;
static bool rsp_mpeg1_initialized = false;

void rsp_mpeg1_init(void) {
	if (rsp_mpeg1_initialized) return;
	rspq_init();
	ovl_id = rspq_overlay_register(&rsp_mpeg1);
	rsp_mpeg1_initialized = true;
}

void rsp_mpeg1_close(void) {
	if (!rsp_mpeg1_initialized) return;
	rspq_overlay_unregister(ovl_id); ovl_id = 0;
	rsp_mpeg1_initialized = false;
}

void rsp_mpeg1_load_matrix(int16_t *mtx) {
	assert((PhysicalAddr(mtx) & 7) == 0);
	data_cache_hit_writeback(mtx, 8*8*2);
	rspq_write(ovl_id, 0x0, PhysicalAddr(mtx));
}

void rsp_mpeg1_store_matrix(int16_t *mtx) {
	assert((PhysicalAddr(mtx) & 7) == 0);
	data_cache_hit_writeback_invalidate(mtx, 8*8*2);
	rspq_write(ovl_id, 0x7, PhysicalAddr(mtx));
}

void rsp_mpeg1_store_pixels(void) {
	rspq_write(ovl_id, 0x1);
}

void rsp_mpeg1_load_pixels(void) {
	rspq_write(ovl_id, 0xC);
}

void rsp_mpeg1_zero_pixels(void) {
	rspq_write(ovl_id, 0xD);
}

void rsp_mpeg1_idct(void) {
	rspq_write(ovl_id, 0x2);
}

void rsp_mpeg1_block_begin(int block, uint8_t *pixels, int pitch) {
	assert((PhysicalAddr(pixels) & 7) == 0);
	assert((pitch & 7) == 0);
	assert(block == RSP_MPEG1_BLOCK_Y0 || block == RSP_MPEG1_BLOCK_CR || block == RSP_MPEG1_BLOCK_CB);
	rspq_write(ovl_id, 0x3, block, PhysicalAddr(pixels), pitch);
}

void rsp_mpeg1_block_switch_partition(int partition) {
	rspq_write(ovl_id, 0xB, partition);
}

void rsp_mpeg1_block_coeff(int idx, int16_t coeff) {
	rspq_write(ovl_id, 0x4, ((idx & 0x3F) << 16) | (uint16_t)coeff);
}

void rsp_mpeg1_block_dequant(bool intra, int scale) {
	rspq_write(ovl_id, 0x5, (int)intra | (scale << 8));
}

void rsp_mpeg1_block_decode(int ncoeffs, bool intra) {
	rspq_write(ovl_id, 0x6, ncoeffs, intra);
}

void rsp_mpeg1_block_predict(uint8_t *src, int pitch, bool oddh, bool oddv, bool interpolate) {
	rspq_write(ovl_id, 0xA, PhysicalAddr(src), pitch, oddv | (oddh<<1) | (interpolate<<2));
}

void rsp_mpeg1_set_quant_matrix(bool intra, const uint8_t quant_mtx[64]) {
	uint32_t *qmtx = (uint32_t*)quant_mtx;
	rspq_write(ovl_id, 0x8, intra,
		qmtx[0],  qmtx[1],  qmtx[2],  qmtx[3],
		qmtx[4],  qmtx[5],  qmtx[6],  qmtx[7]);
	rspq_write(ovl_id, 0x9, intra,
		qmtx[8],  qmtx[9],  qmtx[10], qmtx[11],
		qmtx[12], qmtx[13], qmtx[14], qmtx[15]);
}

#define PL_MPEG_IMPLEMENTATION
#include "pl_mpeg/pl_mpeg.h"

static void mpeg1_fill_info(mpeg1_t *mp1) {
	mp1->video.info.width = plm_video_get_width(mp1->v);
	mp1->video.info.height = plm_video_get_height(mp1->v);
	mp1->video.info.framerate = plm_video_get_framerate(mp1->v);
	mp1->video.info.aspect_ratio = (float)plm_video_get_aspect_ratio(mp1->v);

	// MPEG1 does not specify colorspace. Historically, the vast amount of MPEG1
	// streams were encoded using BT.601 standard with TV levels, and that's also
	// the default for ffmpeg when encoding MPEG1, so we assume that here.
	mp1->video.info.colorspace = YUV_BT601_TV;
}

static video_t *mpeg1_open(const char *fn, const video_parms_t *parms) {
	(void)parms;
	mpeg1_t *mp1 = malloc(sizeof(mpeg1_t));
	assertf(mp1, "Out of memory");
	memset(mp1, 0, sizeof(mpeg1_t));

	rsp_mpeg1_init();

	mp1->buf = plm_buffer_create_with_filename(fn);
	assertf(mp1->buf, "error opening file %s: %s\n", fn, strerror(errno));

	// Disable buffering for every filesystem.
	setvbuf(mp1->buf->fh, NULL, _IONBF, 0);

	mp1->v = plm_video_create_with_buffer(mp1->buf, true);
	assert(mp1->v);

	mp1->buffered_pics = parms ? parms->buffered_pics : 1;
	if (mp1->buffered_pics < 1) mp1->buffered_pics = 1;
	plm_video_set_buffered_pics(mp1->v, mp1->buffered_pics);

	// Force decoding of header. This would be done lazily but better do it
	// now to catch any errors early.
	if (!plm_video_has_header(mp1->v)) {
		assertf(0, "invalid header in video stream\n");
	}

	mpeg1_fill_info(mp1);
	return &mp1->video;
}

// pl_mpeg calls this whenever it needs more bits than the buffer holds. It's
// the reader-mode version of plm_buffer_load_file_callback, and follows its
// rules: drop consumed bytes first if allowed (pl_mpeg forbids it while it
// scans ahead and will step back), then append what fits.
static void mpeg1_reader_load(plm_buffer_t *b, void *user) {
	mpeg1_t *mp1 = (mpeg1_t *)user;
	if (mp1->reader_ended)
		return;

	if (b->discard_read_bytes)
		plm_buffer_discard_read_bytes(b);

	int space = b->capacity - b->length;
	if (space == 0)
		return;

	int n = mp1->reader_blocking
		? mp1->reader.read(mp1->reader.ctx, b->bytes + b->length, space)
		: mp1->reader.try_read(mp1->reader.ctx, b->bytes + b->length, space);

	if (n == VIDEO_READ_END) {
		// Like file mode at EOF: "ended" means no more bytes will arrive.
		// What's in the buffer still gets decoded.
		mp1->reader_ended = true;
		b->has_ended = TRUE;
		return;
	}
	assertf(n >= 0 && n <= space, "video reader returned %d (asked for at most %d)", n, space);
	assertf(n > 0 || !mp1->reader_blocking, "video reader: read returned 0");
	b->length += n;
}

static video_t *mpeg1_open_reader(const video_reader_t *reader, const video_parms_t *parms) {
	mpeg1_t *mp1 = malloc(sizeof(mpeg1_t));
	assertf(mp1, "Out of memory");
	memset(mp1, 0, sizeof(mpeg1_t));

	rsp_mpeg1_init();

	// A fixed-size buffer: the load callback only reads what fits, so it never
	// grows. Ring mode lets consumed bytes be discarded.
	int size = parms && parms->reader_buffer_size > 0 ? parms->reader_buffer_size : MPEG1_READER_BUFFER_DEFAULT;
	mp1->buf = plm_buffer_create_with_capacity(size);
	mp1->has_reader = true;
	mp1->reader = *reader;
	plm_buffer_set_load_callback(mp1->buf, mpeg1_reader_load, mp1);

	mp1->v = plm_video_create_with_buffer(mp1->buf, true);
	assert(mp1->v);

	mp1->buffered_pics = parms ? parms->buffered_pics : 1;
	if (mp1->buffered_pics < 1) mp1->buffered_pics = 1;
	plm_video_set_buffered_pics(mp1->v, mp1->buffered_pics);

	// Like video_open, decode the header now, so the info is valid as soon as
	// this returns. This is the only time the reader may block. A blocking
	// read may still return less than the header needs, so keep going until
	// the header is complete or the stream ends.
	mp1->reader_blocking = true;
	while (!plm_video_has_header(mp1->v)) {
		assertf(!mp1->reader_ended, "invalid header in video stream\n");
	}
	mp1->reader_blocking = false;

	mpeg1_fill_info(mp1);
	return &mp1->video;
}

static int mpeg1_poll(video_t *v) {
	mpeg1_t *mp1 = (mpeg1_t *)v;
	if (mp1->buffered_pics <= 0)
		return 0;
	return plm_video_poll(mp1->v) ? 1 : 0;
}

static video_status_t mpeg1_next_frame_status(video_t *v) {
	mpeg1_t *mp1 = (mpeg1_t *)v;

	// Release previous frame (if any). Frames extracted from the output queue stay
	// locked until explicitly released.
	if (mp1->f) {
		plm_video_release_output(mp1->v, (plm_frame_t *)mp1->f);
		mp1->f = NULL;
	}

	// Reader mode: while pl_mpeg scans ahead for the next picture it doesn't
	// let the load callback discard consumed bytes. If that left the buffer
	// full, make room now (everything before the read position is consumed).
	// Retrying after VIDEO_NEED_DATA is cheap: the scan resumes where it
	// stopped, so with no new bytes it only asks the reader once.
	plm_buffer_t *b = mp1->buf;
	if (mp1->has_reader && b->length == b->capacity)
		plm_buffer_discard_read_bytes(b);

	PROFILE_START(PS_MPEG);
	mp1->f = plm_video_decode(mp1->v);
	PROFILE_STOP(PS_MPEG);

	if (mp1->f)
		return VIDEO_FRAME;

	// No frame. In file mode, or once the reader reported the end, that's the
	// end of the stream.
	if (!mp1->has_reader || mp1->reader_ended)
		return VIDEO_END;

	// Reader mode: the decoder only decodes a picture once the next picture's
	// start code is in the buffer. If the unread bytes already fill the whole
	// buffer, nothing more fits and it would wait forever.
	size_t unread = b->length - (b->bit_index >> 3);
	assertf(unread < b->capacity,
		"MPEG1 reader buffer (%d bytes) is too small for a compressed picture; "
		"raise video_parms_t::reader_buffer_size", (int)b->capacity);
	return VIDEO_NEED_DATA;
}

static void mpeg1_rewind(video_t *v) {
	mpeg1_t *mp1 = (mpeg1_t *)v;
	if (mp1->f) {
		plm_video_release_output(mp1->v, (plm_frame_t *)mp1->f);
		mp1->f = NULL;
	}
	// Reader mode: the decoder keeps the header it already decoded, and skips
	// it when it comes around again.
	if (mp1->has_reader) {
		int err = mp1->reader.rewind(mp1->reader.ctx);
		assertf(err == 0, "video reader: rewind failed (%d)", err);
		mp1->reader_ended = false;
	}
	plm_video_rewind(mp1->v);
}

static video_frame_t mpeg1_get_frame(video_t *v) {
	mpeg1_t *mp1 = (mpeg1_t *)v;
	plm_frame_t *frame = (plm_frame_t*)mp1->f;
	surface_t yp  = surface_make_linear(frame->y.data,  FMT_I8, frame->width,   frame->height);
	surface_t cbp = surface_make_linear(frame->cb.data, FMT_I8, frame->width/2, frame->height/2);
	surface_t crp = surface_make_linear(frame->cr.data, FMT_I8, frame->width/2, frame->height/2);
	// pl_mpeg tagged the picture with its index when it entered the output
	// queue, so pictures decoded ahead keep the right index.
	return (video_frame_t){
		.yuv = { .y = yp, .u = cbp, .v = crp },
		.index = frame->index,
	};
}

static void mpeg1_close(video_t *v) {
	mpeg1_t *mp1 = (mpeg1_t *)v;
	if (mp1->f) {
		plm_video_release_output(mp1->v, (plm_frame_t *)mp1->f);
		mp1->f = NULL;
	}
	plm_video_destroy(mp1->v);
	if (mp1->has_reader && mp1->reader.close)
		mp1->reader.close(mp1->reader.ctx);
	free(mp1);
}

void __mpeg1_profile_init(void) {
	profile_register(PS_MPEG, "MPEG", 0);
	profile_register(PS_MPEG_FINDSTART, "FindStart", 1);
	profile_register(PS_MPEG_HASSTART, "HasStart", 1);
	profile_register(PS_MPEG_DECODESLICE, "DecodeSlice", 1);
	profile_register(PS_MPEG_MB, "MacroB", 1);
	profile_register(PS_MPEG_MB_MV, "MV", 2);
	profile_register(PS_MPEG_MB_PREDICT, "Predict", 2);
	profile_register(PS_MPEG_MB_DECODE, "Decode", 2);
	profile_register(PS_MPEG_MB_DECODE_DC, "DecodeDC", 3);
	profile_register(PS_MPEG_MB_DECODE_AC, "DecodeAC", 3);
	profile_register(PS_MPEG_MB_DECODE_AC_VLC, "DecodeACVLC", 4);
	profile_register(PS_MPEG_MB_DECODE_AC_CODE, "DecodeACCode", 4);
	profile_register(PS_MPEG_MB_DECODE_AC_DEQUANT, "DecodeACDeQuant", 4);
	profile_register(PS_MPEG_MB_DECODE_BLOCK, "DecodeBlock", 3);
	profile_register(PS_MPEG_MB_DECODE_BLOCK_IDCT, "DecodeBlockIDCT", 4);
}

video_codec_t mpeg1_codec = {
	.extension = ".m1v",
	.open = mpeg1_open,
	.close = mpeg1_close,
	.poll = mpeg1_poll,
	.next_frame_status = mpeg1_next_frame_status,
	.get_frame = mpeg1_get_frame,
	.rewind = mpeg1_rewind,
	.open_reader = mpeg1_open_reader,
};
