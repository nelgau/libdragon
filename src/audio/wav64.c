/**
 * @file wav64.c
 * @author Giovanni Bajo <giovannibajo@gmail.com>
 * @brief Support for WAV64 audio files
 * @ingroup mixer
 */

#include "wav64.h"
#include "wav64_internal.h"
#include "wav64_vadpcm_internal.h"
#include "wav64_opus_internal.h"
#include "wav64_ulc_internal.h"
#include "mixer.h"
#include "mixer_internal.h"
#include "dragonfs.h"
#include "n64sys.h"
#include "dma.h"
#include "samplebuffer.h"
#include "debug.h"
#include "utils.h"
#include "rspq.h"
#include "asset_internal.h"
#include <stdbool.h>
#include <string.h>
#include <assert.h>
#include <stdlib.h>
#include <errno.h>
#include <stdalign.h>
#include <stddef.h>
#include <fcntl.h>
#include <unistd.h>
#include <malloc.h>

/** ID of a standard WAV file */
#define WAV_RIFF_ID   "RIFF"
/** ID of a WAVX file (big-endian WAV) */
#define WAV_RIFX_ID   "RIFX"

/** @brief None compression init function */
static void wav64_none_init(wav64_t *wav, int state_size);
/** @brief None compression get_bitrate function */
static int wav64_none_get_bitrate(wav64_t *wav);

static wav64_compression_t algos[WAV64_NUM_FORMATS] = {
	// None compression
    [WAV64_FORMAT_RAW] = {
		.init = wav64_none_init,
		.get_bitrate = wav64_none_get_bitrate,
    },
	// VADPCM compression. This is always linked in as it's the default algorithm
	// for audioconv64, and it's very little code at runtime.
	[WAV64_FORMAT_VADPCM] = {
		.init = wav64_vadpcm_init,
		.close = wav64_vadpcm_close,
		.get_bitrate = wav64_vadpcm_get_bitrate,
		.adjust_seek = wav64_vadpcm_adjust_seek,
	},
};

// ---------------------------------------------------------------------------
// Readers: where a WAV64's bytes come from. All codec I/O goes through
// wav64_io_seek / wav64_io_read / wav64_io_dma_async, never the fd directly.
//
// Built-in strategies (ctx is the wav64_fdreader_t in the state):
//   - file: lseek + read on a file descriptor
//   - ROM:  the same, plus async PI DMA, for files in DragonFS
// Applications can supply their own with wav64_load_reader().
// ---------------------------------------------------------------------------

static int fdreader_seek(void *ctx, uint32_t offset) {
	wav64_fdreader_t *f = ctx;
	return lseek(f->fd, f->start + offset, SEEK_SET) < 0 ? -1 : 0;
}

static int fdreader_read(void *ctx, void *dst, int len) {
	wav64_fdreader_t *f = ctx;
	return read(f->fd, dst, len);
}

static void fdreader_close(void *ctx) {
	wav64_fdreader_t *f = ctx;
	if (f->owned && f->fd >= 0)
		close(f->fd);
	f->fd = -1;
}

static uint64_t romreader_dma_async(void *ctx, uint32_t offset, void *dst, int len) {
	wav64_fdreader_t *f = ctx;
	uint32_t pi_addr = f->rom_base + f->start + offset;
	// PI DMA needs RAM and PI addresses with the same 1-bit misalignment.
	if (((uint32_t)dst ^ pi_addr) & 1)
		return 0;
	return dma_read_async(dst, pi_addr, len);
}

uint64_t wav64_io_dma_async(wav64_t *wav, samplebuffer_t *sbuf, uint32_t offset, void *dst, int len) {
	if (!wav64_io_can_dma(wav))
		return 0;
	// A prior chunk for this sample buffer may still be in flight; wait before
	// replacing its ticket (or before the caller writes the buffer itself).
	samplebuffer_dma_wait(sbuf);
	uint64_t ticket = wav->st->reader.dma_async(wav->st->reader.ctx, offset, dst, len);
	if (ticket)
		sbuf->dma_ticket = ticket;
	return ticket;
}

/** @brief Release the reader (if any) and forget it: no more I/O after this. */
static void wav64_io_close(wav64_t *wav) {
	if (wav->st->reader.close)
		wav->st->reader.close(wav->st->reader.ctx);
	memset(&wav->st->reader, 0, sizeof(wav->st->reader));
}

static void raw_waveform_read(samplebuffer_t *sbuf, wav64_t *wav, int wpos, int wlen, int bps) {
	while (wlen > 0) {
		int n = MIN(wlen, SAMPLEBUFFER_MARGIN_UNITS);
		uint8_t* ram_addr = (uint8_t*)samplebuffer_append(sbuf, n);
		int bytes = n << bps;
		uint32_t offset = wav->st->base_offset + (wpos << bps);

		if (!wav64_io_dma_async(wav, sbuf, offset, ram_addr, bytes)) {
			wav64_io_seek(wav, offset);
			// FIXME: remove CachedAddr() when read() supports uncached addresses
			wav64_io_read(wav, CachedAddr(ram_addr), bytes);
		}
		wlen -= n;
		wpos += n;
	}
}

static void wav64_none_read(void *ctx, samplebuffer_t *sbuf, int wpos, int wlen, bool seeking) {
	wav64_t *wav = (wav64_t*)sbuf->wave;
	int bps = (wav->wave.bits == 8 ? 0 : 1) + (wav->wave.channels == 2 ? 1 : 0);
	(void)ctx; (void)seeking;
	raw_waveform_read(sbuf, wav, wpos, wlen, bps);
}

static void wav64_none_init(wav64_t *wav, int state_size) {
	// Initialize none compression. Setup read callback for streaming;
	// preloaded waveforms set wave->mem and need no read callback.
	if (!wav->st->samples) {
		wav->wave.read = wav64_none_read;
		// Samples come straight from ROM via async PI DMA; a file on any other
		// medium is read synchronously instead.
		wav->wave.async_read = wav64_io_can_dma(wav);
	} else {
		wav->wave.read = NULL;
		wav->wave.async_read = false;
		wav->wave.mem = wav->st->samples;
	}
	// Also clear start callback (needed in the preloading codepath)
	wav->wave.start = NULL;
}

static int wav64_none_get_bitrate(wav64_t *wav) {
	return wav->wave.frequency * wav->wave.channels * wav->wave.bits;
}

/** @brief Build a built-in reader over fd: ROM when the file is in DragonFS
 *  (async DMA available), plain file otherwise. State goes in *fdr. */
static wav64_reader_t fdreader_make(wav64_fdreader_t *fdr, int fd, int start, bool owned)
{
	*fdr = (wav64_fdreader_t){ .fd = fd, .start = start, .owned = owned };
	ioctl(fd, IODFS_GET_ROM_BASE, &fdr->rom_base);
	return (wav64_reader_t){
		.ctx = fdr,
		.seek = fdreader_seek,
		.read = fdreader_read,
		.close = fdreader_close,
		.dma_async = fdr->rom_base ? romreader_dma_async : NULL,
	};
}

/**
 * @brief Load a WAV64 from a reader: everything after "where do the bytes
 *        come from".
 *
 * @param fdr   For the built-in readers: their state, which reader.ctx points
 *              to. It is adopted into the wav64 state (the caller's copy only
 *              needs to live until this returns). NULL for application readers.
 */
static wav64_t* internal_open(wav64_t *wav, wav64_reader_t reader, const wav64_fdreader_t *fdr,
	const char *file_name, wav64_loadparms_t *parms)
{
	wav64_loadparms_t default_parms = {0};
	if (!parms) parms = &default_parms;

	// Read the header, at offset 0 (the beginning of the WAV64 data).
	wav64_header_t head;
	reader.seek(reader.ctx, 0);
	reader.read(reader.ctx, &head, sizeof(head));

	if (memcmp(head.id, WAV64_ID, 4) != 0) {
		assertf(memcmp(head.id, WAV_RIFF_ID, 4) != 0 && memcmp(head.id, WAV_RIFX_ID, 4) != 0,
			"wav64 %s: use audioconv64 to convert to wav64 format", file_name);
		assertf(0, "wav64 %s: invalid ID: %02x%02x%02x%02x\n",
			file_name, head.id[0], head.id[1], head.id[2], head.id[3]);
	}
	assertf(head.version == 10,
		"wav64 %s: unsupported version %02x; reconvert with audioconv64)",
		file_name, head.version);
	assertf(head.format < WAV64_NUM_FORMATS, "Unknown wav64 compression format %d; corrupted file?", head.format);
	assertf(head.format < WAV64_NUM_FORMATS && algos[head.format].init != NULL,
        "wav64: compression level %d not initialized. Call wav64_init_compression(%d) at initialization time", head.format, head.format);

	int ext_size = head.start_offset - sizeof(wav64_header_t);
	bool preload = parms->streaming_mode == WAV64_STREAMING_NONE;
	// VADPCM preloads stay compressed, laid out fully planar in RDRAM for
	// direct MIX_CHANNEL addressing. The frame size depends on the residual
	// width, which lives in the extended header: that is only read into its
	// final home further down, so peek at its prefix to size the buffer.
	bool preload_vadpcm = false;
	int vframe_bytes = 0;
	if (head.format == WAV64_FORMAT_VADPCM) {
		uint8_t prefix[offsetof(wav64_header_vadpcm_t, huff_tbl)];
		reader.read(reader.ctx, prefix, sizeof(prefix));
		reader.seek(reader.ctx, sizeof(wav64_header_t));
		int bits = prefix[offsetof(wav64_header_vadpcm_t, residual_bits)];
		vframe_bytes = VADPCM_FRAME_BYTES(VADPCM_RESIDUAL_BITS(bits));
		uint16_t vflags;
		memcpy(&vflags, prefix + offsetof(wav64_header_vadpcm_t, flags), sizeof(vflags));
		if (vflags & VADPCM_FLAG_RESIDENT)
			preload = true;
		preload_vadpcm = preload;
	}
	int nframes = WAV64_VADPCM_FRAMES(head.len);
	int preload_size = preload_vadpcm
		? ROUND_UP(nframes * vframe_bytes * head.channels, 16)
		: ROUND_UP(head.len * head.channels * (head.nbits >> 3), 16);
	if (preload && !preload_vadpcm) {
		int ub = head.channels * (head.nbits >> 3);
		preload_size += SAMPLEBUFFER_MARGIN_UNITS * ub;
		preload_size = ROUND_UP(preload_size, 16);
	}
	int preload_extra_alloc = ROUND_UP(
		(head.format == WAV64_FORMAT_RAW || preload_vadpcm) ? 0 : 4096, 16);
	int state_size = ROUND_UP(head.state_size, 16);

	// Calculate required allocation
	int heap_size = 0;
	heap_size += ROUND_UP(sizeof(wav64_state_t), 16);				// wav64_state_t

	int heap_off_waveform = heap_size;
	if (!wav) heap_size += ROUND_UP(sizeof(wav64_t), 16);		// wav64_t (wave + st)

	int heap_off_name = heap_size;
	heap_size += ROUND_UP(strlen(file_name) + 1, 16);				// Filename

	int heap_off_samples = heap_size;
	if (preload) heap_size += preload_size;							// Preloaded samples

	int heap_off_preload_end = heap_size;
	if (preload) heap_size += preload_extra_alloc + state_size;		// Extra allocation for preload

	int heap_off_ext = heap_size;
	heap_size += ROUND_UP(ext_size, 16);							// Extended header data
	
	// Allocate heap memory
	assert(heap_size % 16 == 0);
	void *heap = memalign(16, heap_size);
	assertf(heap, "Out of memory");
	if (!wav) wav = heap + heap_off_waveform;
	wav->st = heap;
	wav->st->ext = heap + heap_off_ext;
	wav->st->samples = NULL;

	// Fill waveforms struct
	memset(&wav->wave, 0, sizeof(waveform_t));
	wav->wave.name = heap + heap_off_name;
	strcpy(heap + heap_off_name, file_name);
	wav->wave.channels = head.channels;
	wav->wave.bits = head.nbits;
	wav->wave.frequency = head.freq;
	wav->wave.len = head.len;
	wav->wave.loop_len = head.loop_len;
	wav->wave.loop_end = head.loop_end;
	wav->wave.state_size = head.state_size;
	int wloop_end = head.loop_end ? head.loop_end : head.len;
	assertf(head.loop_len >= 0 && head.loop_len <= wloop_end &&
		head.loop_end >= 0 && head.loop_end <= head.len,
		"wav64 %s: invalid loop %d..%d (len %d)", file_name,
		wloop_end - (int)head.loop_len, wloop_end, (int)head.len);

	// Read ext data
	reader.read(reader.ctx, wav->st->ext, ext_size);
	data_cache_hit_writeback(wav->st->ext, ext_size);

	// Finish initialization of wav64 state. The built-in readers keep their
	// state inside wav64_state_t from now on.
	wav->st->format = head.format;
	wav->st->reader = reader;
	wav->st->fdr = fdr ? *fdr : (wav64_fdreader_t){ .fd = -1 };
	if (fdr)
		wav->st->reader.ctx = &wav->st->fdr;
	wav->st->base_offset = head.start_offset;
	wav->st->flags = (preload ? WAV64_FLAG_PRELOAD : 0);

	// Initialize the compression algorithm
	algos[wav->st->format].init(wav, head.state_size);

	// Preload the samples if requested
	if (preload) {
		data_cache_hit_invalidate(heap + heap_off_samples, preload_size + preload_extra_alloc + state_size);
		wav->st->samples = UncachedAddr(heap + heap_off_samples);

		if (wav->wave.format == WAVEFORM_FORMAT_VADPCM) {
			wav64_vadpcm_preload(wav, wav->st->samples);
			wav->wave.mem = wav->st->samples;
			if (algos[wav->st->format].close)
				algos[wav->st->format].close(wav);
		} else {
			samplebuffer_t sbuf;
			samplebuffer_init(&sbuf, wav->st->samples, preload_size + preload_extra_alloc, state_size);
			int wlen = wav->wave.len;
			samplebuffer_set_bps(&sbuf, wav->wave.bits * wav->wave.channels);
			samplebuffer_set_waveform(&sbuf, &wav->wave, wav->wave.read);
			if (wav->wave.start) wav->wave.start(wav->wave.ctx, &sbuf);
			samplebuffer_get(&sbuf, 0, &wlen);
			samplebuffer_dma_wait(&sbuf);
			rspq_highpri_sync();
			assertf(wlen == wav->wave.len, "wav64: preload failed for %s: wlen=%x/%x", wav->wave.name, wlen, wav->wave.len);

			// Terminal loops: the RSP overreads past len, so pad the margin
			// with the loop start. Non-terminal loops keep the release tail
			// intact; streamed playback fills overread via #waveform_read.
			if (wav->wave.loop_len && (!wav->wave.loop_end || wav->wave.loop_end == wav->wave.len)) {
				int ub = wav->wave.channels * (wav->wave.bits >> 3);
				int loop_start = wav->wave.len - wav->wave.loop_len;
				uint8_t *base = (uint8_t *)wav->st->samples;
				for (int i = 0; i < SAMPLEBUFFER_MARGIN_UNITS; i++)
					memcpy(base + (wav->wave.len + i) * ub,
						base + (loop_start + i % wav->wave.loop_len) * ub, ub);
			}

			if (algos[wav->st->format].close)
				algos[wav->st->format].close(wav);

			wav->st = realloc(wav->st, heap_off_preload_end);
			wav->st->ext = NULL;
			// The built-in readers point into the state, which may have moved.
			if (fdr)
				wav->st->reader.ctx = &wav->st->fdr;

			// Reinitialize as RAW format after preloading
			wav->st->format = WAV64_FORMAT_RAW;
			algos[wav->st->format].init(wav, head.state_size);
		}

		// Fully preloaded: the bytes are all in RDRAM, the reader isn't needed.
		wav64_io_close(wav);
	}

	return wav;
}


/** @brief Front door for filenames: open the file, then load through a built-in reader. */
static wav64_t* open_by_name(wav64_t *wav, const char *file_name, wav64_loadparms_t *parms)
{
	// For backwards compatibility with old versions of this file, we support
	// an unprefixed file name as a dfs file. This is deprecated and not documented
	// but we just want to avoid breaking existing code
	if (file_name && strchr(file_name, ':') == NULL) {
		char* dfs_name = alloca(5 + strlen(file_name) + 1);
		strcpy(dfs_name, "rom:/");
		strcat(dfs_name, file_name);
		file_name = dfs_name;
	}

	wav64_fdreader_t fdr;
	wav64_reader_t reader = fdreader_make(&fdr, must_open(file_name), 0, true);
	return internal_open(wav, reader, &fdr, file_name, parms);
}

void wav64_open(wav64_t *wav, const char *file_name)
{
	open_by_name(wav, file_name, NULL);
}

wav64_t* wav64_load(const char *file_name, wav64_loadparms_t *parms)
{
	return open_by_name(NULL, file_name, parms);
}

wav64_t* wav64_loadfd(int fd, const char *debug_file_name, wav64_loadparms_t *parms)
{
	// The WAV64 data starts wherever fd is positioned now.
	wav64_fdreader_t fdr;
	wav64_reader_t reader = fdreader_make(&fdr, fd, lseek(fd, 0, SEEK_CUR), false);
	return internal_open(NULL, reader, &fdr, debug_file_name, parms);
}

wav64_t* wav64_load_reader(const wav64_reader_t *reader, const char *debug_name, wav64_loadparms_t *parms)
{
	if (!debug_name) debug_name = "(reader)";
	assertf(reader && reader->seek && reader->read, "wav64 %s: reader needs seek and read", debug_name);
	return internal_open(NULL, *reader, NULL, debug_name, parms);
}

void wav64_play(wav64_t *wav, int ch)
{
	mixer_ch_play(ch, &wav->wave);
}

double wav64_seek(wav64_t *wav, int ch, double time_sec)
{
	// Compute a target sample position (rounded to nearest integer sample)
	int spos = (int)(time_sec * wav->wave.frequency + 0.5);
	spos = CLAMP(spos, 0, wav->wave.len);

	// Apply codec-specific seek constraints (if any).
	if (algos[wav->st->format].adjust_seek)
		spos = algos[wav->st->format].adjust_seek(wav, spos);

	mixer_ch_set_pos(ch, spos);

	// Return the adjusted time in seconds
	return (double)spos / wav->wave.frequency;
}

void wav64_set_loop(wav64_t *wav, bool loop) {
	// Full-file terminal loop (or disarm). Sustain loops with a release tail
	// come from the file header; use #mixer_ch_set_loop to toggle those.
	wav->wave.loop_len = loop ? wav->wave.len : 0;
	wav->wave.loop_end = 0;

	// Odd loop lengths are not supported for 8-bit waveforms because they would
	// change the 2-byte phase between ROM and RDRAM addresses during loop unrolling.
	// We shorten the loop by 1 sample which shouldn't matter.
	// Notice that audioconv64 does the same during conversion.
	if (wav->wave.bits == 8 && wav->wave.loop_len & 1)
		wav->wave.loop_len -= 1;
}

int wav64_get_bitrate(wav64_t *wav) {
	if (algos[wav->st->format].get_bitrate)
		return algos[wav->st->format].get_bitrate(wav);
	return algos[WAV64_FORMAT_RAW].get_bitrate(wav);
}

void wav64_close(wav64_t *wav)
{
	// Heap allocation always begins at wav->st.
	void *heap = wav->st;

	// For user-allocated wav64_t instances (opened via wav64_open), we allowed
	// in the past to call this function multiple times without crashing. Let's
	// keep this working, as it's easy to do.
	if (!heap)
		return;

	// Stop playing the waveform on all channels
	for (int i=0; i<MIXER_MAX_CHANNELS; i++) {
		if (mixer_ch_playing_waveform(i) == &wav->wave)
			mixer_ch_stop(i);
	}

	// Wait for in-flight mix/decode rounds that may still reference
	// codebook/state/ext before freeing the heap.
	rspq_highpri_sync();

	if (algos[wav->st->format].close)
		algos[wav->st->format].close(wav);

	wav64_io_close(wav);

	memset(wav, 0, sizeof(wav64_t));

	// Free the heap allocation (that might or might not include the wav64_t instance)
	free(heap);
}

/** @brief Initialize wav64 compression level 2 */
void __wav64_init_compression_lvl2(void)
{
	algos[WAV64_FORMAT_ULC] = (wav64_compression_t){
		.init = wav64_ulc_init,
		.get_bitrate = wav64_ulc_get_bitrate,
		.adjust_seek = wav64_ulc_adjust_seek,
	};
}

/** @brief Initialize wav64 compression level 3 */
void __wav64_init_compression_lvl3(void)
{
	algos[WAV64_FORMAT_OPUS] = (wav64_compression_t){
		.init = wav64_opus_init,
		.close = wav64_opus_close,
		.get_bitrate = wav64_opus_get_bitrate,
		.adjust_seek = wav64_opus_adjust_seek,
	};
}
