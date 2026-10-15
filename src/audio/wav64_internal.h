/**
 * @file wav64_internal.h
 * @author Giovanni Bajo <giovannibajo@gmail.com>
 */
#ifndef __LIBDRAGON_WAV64_INTERNAL_H
#define __LIBDRAGON_WAV64_INTERNAL_H

#include "n64types.h"
#include "mixer_internal.h"
#ifdef N64
#include <stddef.h>
#include "wav64.h"          // wav64_reader_t (runtime only; host tools share this header)
#endif

#define WAV64_ID            "WV64"    ///< WAV64 file identifier
#define WAV64_FORMAT_RAW    0         ///< Raw audio format
#define WAV64_FORMAT_VADPCM 1         ///< VADPCM compressed format
#define WAV64_FORMAT_ULC    2         ///< ULC compressed format
#define WAV64_FORMAT_OPUS   3         ///< Opus compressed format
#define WAV64_NUM_FORMATS   4         ///< Number of supported formats

#define WAV64_FLAG_PRELOAD  (1 << 1)  ///< Waveform will be fully preloaded in RDRAM

/// @cond
typedef struct wav64_s wav64_t;
typedef struct wav64_loadparms_s wav64_loadparms_t;
typedef struct samplebuffer_s samplebuffer_t;
/// @endcond

/** @brief Header of a WAV64 file. */
typedef struct __attribute__((packed)) {
	char id[4];             ///< ID of the file (WAV64_ID)
	int8_t version;         ///< Version of the file
	int8_t format;          ///< Format of the file (WAV64_FORMAT_RAW)
	int8_t channels;        ///< Number of interleaved channels
	int8_t nbits;           ///< Width of sample in bits (8 or 16)
	int32_t freq;           ///< Default playback frequency
	int32_t len;            ///< Length of the file (in samples)
	int32_t loop_len;       ///< Length of the sustain loop (0 if no loop)
	int32_t loop_end;       ///< Exclusive end of the sustain loop (0 ⇒ #len)
	uint32_t start_offset;  ///< Offset of the first sample in the file
	uint32_t state_size;    ///< Size of per-mixer-channel state to allocate at runtime
} wav64_header_t;

_Static_assert(sizeof(wav64_header_t) == 32, "invalid wav64_header size");

#ifdef N64
/**
 * @brief State of the built-in file readers (plain file, and file in ROM).
 *
 * Offsets given to the reader are relative to the WAV64 data; @c start is
 * where that data begins in the file (non-zero for #wav64_loadfd on a file
 * descriptor that was already positioned).
 */
typedef struct {
	int fd;                  ///< File descriptor (-1 once closed)
	int start;               ///< File offset of the WAV64 data
	pi_addr_t rom_base;      ///< PI address of file start (0 if not in ROM)
	bool owned;              ///< The WAV64 opened the fd and must close it
} wav64_fdreader_t;

/** @brief WAV64 state */
typedef struct wav64_state_s {
	int format;			     ///< Internal format of the file
	void *ext;               ///< Pointer to extended header data (format-dependent)
	void *samples;           ///< Pointer to the preloaded samples (if streaming is disabled)
	/** Where bytes come from. All codec I/O goes through #wav64_io_seek,
	 *  #wav64_io_read and #wav64_io_dma_async. */
	wav64_reader_t reader;
	/** Storage for the built-in file/ROM readers (reader.ctx points here). */
	wav64_fdreader_t fdr;
	int base_offset;		 ///< Start of the samples (offset from the start of the WAV64 data)
	uint8_t flags;           ///< Misc flags
	/**
	 * First N compressed VADPCM frames kept in RDRAM so a note-on from the
	 * start of the waveform does not wait on the PI. NULL when unused
	 * (Huffman, full preload, or non-ROM source). #attack_n is the frame count.
	 */
	void *attack;
	uint16_t attack_n;
	/** Codec side-data for in-mixer VADPCM mono (will be stored in waveform_t::codec). */
	waveform_vadpcm_t vadpcm;
} wav64_state_t;

/** @brief WAV64 pluggable compression algorithm */
typedef struct {
	/** @brief Init function: parses extra header information for the specific codec */
	void (*init)(wav64_t *wav, int state_size);
	/** @brief Close function: deallocates memory for codec-specific data */
	void (*close)(wav64_t *wav);
	/** @brief Return the compressed bitrate, mainly used for statistics */
	int (*get_bitrate)(wav64_t *wav);
	/**
	 * @brief Adjust a requested seek position (in samples) to a valid codec-specific seek point.
	 *
	 * If NULL, the codec supports sample-accurate seeking and no adjustment is performed.
	 * If non-NULL, the function must return a valid seek point (in samples) for this codec.
	 */
	int (*adjust_seek)(wav64_t *wav, int wpos);
} wav64_compression_t;

/** @brief Move the read position (offset relative to the WAV64 data). */
static inline void wav64_io_seek(wav64_t *wav, uint32_t offset) {
	wav->st->reader.seek(wav->st->reader.ctx, offset);
}

/** @brief Read len bytes at the read position and advance; returns bytes read. */
static inline int wav64_io_read(wav64_t *wav, void *dst, int len) {
	return wav->st->reader.read(wav->st->reader.ctx, dst, len);
}

/** @brief True if the reader can DMA asynchronously (files in ROM). */
static inline bool wav64_io_can_dma(wav64_t *wav) {
	return wav->st->reader.dma_async != NULL;
}

/**
 * @brief Try to start an async DMA of len bytes at offset into dst, for sbuf.
 *
 * If the reader supports it, waits for sbuf's previous DMA, starts the new one
 * and records its ticket in sbuf, returning the ticket. Returns 0 if the
 * reader can't DMA (or not at this alignment); the caller then uses
 * #wav64_io_seek + #wav64_io_read.
 */
uint64_t wav64_io_dma_async(wav64_t *wav, samplebuffer_t *sbuf, uint32_t offset, void *dst, int len);
#endif /* N64 */

/**
 * Similar to #wav64_load, but uses a file descriptor instead of a filename.
 */
wav64_t *wav64_loadfd(int fd, const char *debug_file_name, wav64_loadparms_t *parms);

#endif
