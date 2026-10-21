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
#include <malloc.h>
#include "mpeg1_internal.h"

typedef struct mpeg1_decoder_s mpeg1_decoder_t;

typedef struct mpeg1_s {
	video_t video;
	mpeg1_decoder_t *decoder;  ///< Where buf and v live
	plm_buffer_t *buf;         ///< &decoder->buffer
	plm_video_t *v;            ///< &decoder->video
	int buffered_pics;
	video_picture_pool_t *pool;  ///< The pictures it decodes into
	bool own_pool;          ///< Its own, made at open and freed at close (no video_parms_t::picture_pool)
	bool has_reader;        ///< Opened with video_open_reader
	bool reader_ended;      ///< The reader returned VIDEO_READ_END
	bool loop;              ///< At the end of the data, rewind the reader and keep going
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

/**
 * @brief The decoder's two structs, in one block aligned to the data cache's size
 *
 * The CPU's data cache is 8 KB, direct-mapped: where a line falls in it is
 * its address modulo 8 KB. The decoder reads these two structs every few
 * bits. In a block aligned to 8 KB, they always take the cache's first slots
 * (904 bytes: slots 0 to 56), wherever the rest of the program lands: away
 * from the small data (n64.ld starts it 2048 bytes into the 8 KB period,
 * slots 128 and up) and from the stack's hot frames (under the top of RAM,
 * the last slots). Left to the heap, they could share slots with either and
 * evict each other on every call (hi-player NOTES 2026-10-20).
 */
typedef struct mpeg1_decoder_s {
	plm_video_t video;
	plm_buffer_t buffer;
} mpeg1_decoder_t;

#define MPEG1_DECODER_ALIGN   8192

_Static_assert(sizeof(mpeg1_decoder_t) <= 2048,
	"the decoder block reaches the small data's slots (n64.ld)");

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

// Allocates the decoder block; buf and v point into it, and are built there
// by the open functions.
static void mpeg1_alloc_decoder(mpeg1_t *mp1) {
	mp1->decoder = memalign(MPEG1_DECODER_ALIGN, sizeof(mpeg1_decoder_t));
	assertf(mp1->decoder, "Out of memory");
	mp1->buf = &mp1->decoder->buffer;
	mp1->v = &mp1->decoder->video;
}

// How far ahead it decodes: the pool's, if one is lent (so one place says
// it), or the parameters'.
static int mpeg1_buffered_pics(const video_parms_t *parms) {
	video_picture_pool_t *pool = parms ? parms->picture_pool : NULL;
	if (pool) {
		assertf(parms->buffered_pics == 0 || parms->buffered_pics == pool->buffered_pics,
			"MPEG1: buffered_pics is %d, and the picture pool's is %d", parms->buffered_pics, pool->buffered_pics);
		return pool->buffered_pics;
	}
	int buffered_pics = parms ? parms->buffered_pics : 1;
	return buffered_pics < 1 ? 1 : buffered_pics;
}

// Once the header says the video's size: the pictures it decodes into, lent
// (video_parms_t::picture_pool) or its own, made now.
static void mpeg1_take_pictures(mpeg1_t *mp1, const video_parms_t *parms) {
	int width = plm_video_get_width(mp1->v), height = plm_video_get_height(mp1->v);
	video_picture_pool_t *pool = parms ? parms->picture_pool : NULL;
	mp1->own_pool = !pool;
	if (mp1->own_pool) {
		pool = video_picture_pool_alloc(&mpeg1_codec, width, height, mp1->buffered_pics);
	}
	assertf(pool->codec == &mpeg1_codec, "MPEG1: a picture pool made for codec %s", pool->codec->extension);
	assertf(width <= pool->width && height <= pool->height,
		"MPEG1: a %dx%d video doesn't fit a picture pool made for %dx%d", width, height, pool->width, pool->height);
	assertf(!pool->lent, "MPEG1: another video is decoding into the picture pool");
	pool->lent = true;
	plm_video_set_pictures(mp1->v, pool->data, pool->picture_size, pool->count, pool->refs);
	mp1->pool = pool;
}

static video_t *mpeg1_open(const char *fn, const video_parms_t *parms) {
	mpeg1_t *mp1 = malloc(sizeof(mpeg1_t));
	assertf(mp1, "Out of memory");
	memset(mp1, 0, sizeof(mpeg1_t));

	rsp_mpeg1_init();

	FILE *fh = fopen(fn, "rb");
	assertf(fh, "error opening file %s: %s\n", fn, strerror(errno));
	mpeg1_alloc_decoder(mp1);
	plm_buffer_init_with_file(mp1->buf, fh, true);

	// Disable buffering for every filesystem.
	setvbuf(mp1->buf->fh, NULL, _IONBF, 0);

	plm_video_init_with_buffer(mp1->v, mp1->buf, false);

	mp1->buffered_pics = mpeg1_buffered_pics(parms);
	plm_video_set_buffered_pics(mp1->v, mp1->buffered_pics);

	// Force decoding of header. This would be done lazily but better do it
	// now to catch any errors early.
	if (!plm_video_has_header(mp1->v)) {
		assertf(0, "invalid header in video stream\n");
	}
	mpeg1_take_pictures(mp1, parms);

	mpeg1_fill_info(mp1);
	return &mp1->video;
}

// pl_mpeg calls this whenever it needs more bits than the buffer holds. It's
// the reader-mode version of plm_buffer_load_file_callback, and follows its
// rules: drop consumed bytes first if allowed (pl_mpeg forbids it while it
// scans ahead and will step back), then append what fits. It only ever uses
// try_read: it's installed once the header is decoded (see
// mpeg1_open_reader), and never waits.
static void mpeg1_reader_load(plm_buffer_t *b, void *user) {
	mpeg1_t *mp1 = (mpeg1_t *)user;
	if (mp1->reader_ended)
		return;

	if (b->discard_read_bytes)
		plm_buffer_discard_read_bytes(b);

	int space = b->capacity - b->length;
	if (space == 0)
		return;

	int n = mp1->reader.try_read(mp1->reader.ctx, b->bytes + b->length, space);

	if (n == VIDEO_READ_END && mp1->loop) {
		// Looping: the stream goes on from its start. The decoder keeps its
		// state and skips the repeated header; each picture's index comes
		// from the stream (GOP time code), so the new pass starts at 0 on
		// its own.
		int err = mp1->reader.rewind(mp1->reader.ctx);
		assertf(err == 0, "video reader: rewind failed (%d)", err);
		n = mp1->reader.try_read(mp1->reader.ctx, b->bytes + b->length, space);
		assertf(n != VIDEO_READ_END, "video reader: empty stream can't loop");
	}

	if (n == VIDEO_READ_END) {
		// Like file mode at EOF: "ended" means no more bytes will arrive.
		// What's in the buffer still gets decoded.
		mp1->reader_ended = true;
		b->has_ended = TRUE;
		return;
	}
	assertf(n >= 0 && n <= space, "video reader returned %d (asked for at most %d)", n, space);
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
	mpeg1_alloc_decoder(mp1);
	plm_buffer_init_with_capacity(mp1->buf, size);
	mp1->has_reader = true;
	mp1->reader = *reader;

	plm_video_init_with_buffer(mp1->v, mp1->buf, false);

	mp1->buffered_pics = mpeg1_buffered_pics(parms);
	plm_video_set_buffered_pics(mp1->v, mp1->buffered_pics);

	// Like video_open, decode the header now, so the info is valid as soon as
	// this returns. This is the only place the reader may block: read with it
	// until the whole sequence header is in the buffer, then decode the
	// header with no load callback installed, so pl_mpeg can't ask for more.
	// (The optional LD_SAR user data comes before the header, so it's in the
	// buffer too: pl_mpeg looks for it only once.) From then on the decoder
	// pulls through the callback, which never waits.
	plm_buffer_t *b = mp1->buf;
	while (!plm_video_header_in_buffer(mp1->v)) {
		int space = b->capacity - b->length;
		assertf(space > 0, "no MPEG1 sequence header in the first %d bytes", (int)b->capacity);
		int n = mp1->reader.read(mp1->reader.ctx, b->bytes + b->length, space);
		assertf(n != VIDEO_READ_END, "invalid header in video stream\n");
		assertf(n > 0 && n <= space, "video reader: read returned %d (asked for at most %d)", n, space);
		b->length += n;
	}
	bool has_header = plm_video_has_header(mp1->v);
	assertf(has_header, "invalid header in video stream\n");
	plm_buffer_set_load_callback(b, mpeg1_reader_load, mp1);
	mpeg1_take_pictures(mp1, parms);

	mpeg1_fill_info(mp1);
	return &mp1->video;
}

// Reader mode: while pl_mpeg scans ahead for the next picture it doesn't
// let the load callback discard consumed bytes, so the buffer can fill up
// with bytes already decoded. If it's full, discard those (everything before
// the read position). Returns true if that made room: false if the buffer
// wasn't full, or if every byte in it is still unread.
static bool mpeg1_make_room(mpeg1_t *mp1) {
	plm_buffer_t *b = mp1->buf;
	if (!mp1->has_reader || b->length < b->capacity)
		return false;
	size_t consumed = b->bit_index >> 3;
	plm_buffer_discard_read_bytes(b);
	return consumed > 0;
}

// Reader mode: the decoder only decodes a picture once the next picture's
// start code is in the buffer. If the unread bytes already fill the whole
// buffer, nothing more fits and it would wait forever: say so instead.
static void mpeg1_assert_picture_fits(mpeg1_t *mp1) {
	plm_buffer_t *b = mp1->buf;
	size_t unread = b->length - (b->bit_index >> 3);
	assertf(unread < b->capacity,
		"MPEG1 reader buffer (%d bytes) is too small for a compressed picture; "
		"raise video_parms_t::reader_buffer_size", (int)b->capacity);
}

static int mpeg1_poll(video_t *v) {
	mpeg1_t *mp1 = (mpeg1_t *)v;
	if (mp1->buffered_pics <= 0)
		return 0;
	if (plm_video_poll(mp1->v))
		return 1;
	// Stuck. A full picture queue is a reason: wait for a frame to be taken.
	// So is every picture in use: wait for one to be dropped (meanwhile the
	// bitstream buffer may well fill up with bytes not yet decoded).
	if (plm_video_out_full(mp1->v) || plm_video_needs_picture(mp1->v))
		return 0;
	// Otherwise the bitstream buffer may be full of bytes already decoded:
	// make room, and try once more. Taking a frame makes room too
	// (mpeg1_take_frame), but decoding ahead with no frame taken, as
	// a player does while getting ready to start, mustn't stall on it. Only
	// when stuck: making room copies every byte not yet read, too costly for
	// every step.
	if (!mpeg1_make_room(mp1)) {
		mpeg1_assert_picture_fits(mp1);
		return 0;
	}
	return plm_video_poll(mp1->v) ? 1 : 0;
}

// The frame a pl_mpeg picture becomes: planes, index, and which picture of
// which pool it is.
static video_frame_t mpeg1_frame_of(mpeg1_t *mp1, const plm_frame_t *picture) {
	surface_t yp  = surface_make_linear(picture->y.data,  FMT_I8, picture->width,   picture->height);
	surface_t cbp = surface_make_linear(picture->cb.data, FMT_I8, picture->width/2, picture->height/2);
	surface_t crp = surface_make_linear(picture->cr.data, FMT_I8, picture->width/2, picture->height/2);
	// pl_mpeg tagged the picture with its index when it entered the output
	// queue, so pictures decoded ahead keep the right index.
	return (video_frame_t){
		.yuv = { .y = yp, .u = cbp, .v = crp },
		.index = picture->index,
		.pool = mp1->pool,
		.picture_id = plm_video_picture_id(mp1->v, picture),
	};
}

static video_status_t mpeg1_take_frame(video_t *v, video_frame_t *frame) {
	mpeg1_t *mp1 = (mpeg1_t *)v;

	// Make room if the buffer is full (mpeg1_make_room). Retrying after
	// VIDEO_NEED_DATA is cheap: the scan resumes where it stopped, so with no
	// new bytes it only asks the reader once.
	mpeg1_make_room(mp1);

	// The picture comes with the queue's reference, which becomes the
	// caller's (video_frame_unref drops it).
	plm_frame_t picture;
	PROFILE_START(PS_MPEG);
	int taken = plm_video_take_output(mp1->v, &picture);
	PROFILE_STOP(PS_MPEG);
	if (taken) {
		*frame = mpeg1_frame_of(mp1, &picture);
		return VIDEO_FRAME;
	}

	// No frame. Every picture in use: one must be dropped first. Otherwise,
	// in file mode, or once the reader reported the end, that's the end of
	// the stream.
	if (plm_video_needs_picture(mp1->v))
		return VIDEO_NEED_PICTURE;
	if (!mp1->has_reader || mp1->reader_ended)
		return VIDEO_END;

	mpeg1_assert_picture_fits(mp1);
	return VIDEO_NEED_DATA;
}

static void mpeg1_set_loop(video_t *v, bool loop) {
	mpeg1_t *mp1 = (mpeg1_t *)v;
	assertf(mp1->has_reader, "MPEG1: looping needs video_open_reader");
	mp1->loop = loop;
}

static void mpeg1_rewind(video_t *v) {
	mpeg1_t *mp1 = (mpeg1_t *)v;
	// Frames already taken are their takers': a rewind leaves them intact.
	// Reader mode: the decoder keeps the header it already decoded, and skips
	// it when it comes around again.
	if (mp1->has_reader) {
		int err = mp1->reader.rewind(mp1->reader.ctx);
		assertf(err == 0, "video reader: rewind failed (%d)", err);
		mp1->reader_ended = false;
	}
	plm_video_rewind(mp1->v);
}

// The pictures MPEG-1 decodes into: each one's planes (Y, then Cr, then Cb,
// rounded up to whole macroblocks), one after another, as pl_mpeg lays them
// out (plm_video_set_pictures); in uncached memory when the RSP writes them.
// The three it decodes with come on top of the ones decoded ahead.
static video_picture_pool_t *mpeg1_picture_pool_alloc(int width, int height, int buffered_pics) {
	if (buffered_pics < 1) buffered_pics = 1;
	int count = 3 + buffered_pics;
	size_t picture_size = plm_video_picture_size(width, height);
	uint8_t *data;
	if (RSP_MODE >= 2)
		data = malloc_uncached(picture_size * (size_t)count);
	else
		data = memalign(16, picture_size * (size_t)count);
	assertf(data, "Out of memory");
	video_picture_pool_t *pool = calloc(1, sizeof(video_picture_pool_t) + (size_t)count);
	assertf(pool, "Out of memory");
	pool->width = width;
	pool->height = height;
	pool->buffered_pics = buffered_pics;
	pool->count = count;
	pool->picture_size = picture_size;
	pool->data = data;
	return pool;
}

static void mpeg1_picture_pool_free(video_picture_pool_t *pool) {
	if (RSP_MODE >= 2)
		free_uncached(pool->data);
	else
		free(pool->data);
	free(pool);
}

static int mpeg1_frames_waiting(video_t *v) {
	mpeg1_t *mp1 = (mpeg1_t *)v;
	return plm_video_output_waiting(mp1->v);
}

static void mpeg1_close(video_t *v) {
	mpeg1_t *mp1 = (mpeg1_t *)v;
	// The decoder lets go of the pictures it holds (its queue's). Its own
	// pool goes with it, so every frame taken must have been dropped by now
	// (video_picture_pool_free asserts it); a lent pool keeps the frames
	// still referenced, and can take the next video at once.
	plm_video_deinit(mp1->v);
	mp1->pool->lent = false;
	if (mp1->own_pool)
		video_picture_pool_free(mp1->pool);
	plm_buffer_deinit(mp1->buf);
	free(mp1->decoder);
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
	.take_frame = mpeg1_take_frame,
	.rewind = mpeg1_rewind,
	.open_reader = mpeg1_open_reader,
	.set_loop = mpeg1_set_loop,
	.picture_pool_alloc = mpeg1_picture_pool_alloc,
	.picture_pool_free = mpeg1_picture_pool_free,
	.frames_waiting = mpeg1_frames_waiting,
};
