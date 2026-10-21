/**
 * @file video_internal.h
 * @author Giovanni Bajo <giovannibajo@gmail.com>
 * @brief Video player subsystem
 * 
 * 
 */

#ifndef LIBDRAGON_VIDEO_INTERNAL_H
#define LIBDRAGON_VIDEO_INTERNAL_H

#include "video.h"

///@cond
typedef struct video_seektable_s video_seektable_t;
///@endcond

/** @brief Basic video structure */
typedef struct video_s {
    video_info_t info;              ///< Video information
    video_codec_t *codec;           ///< Codec used to decode this video
    video_seektable_t *seektable;      ///< Optional seek table for fast seeking
} video_t;

/**
 * @brief Pictures that videos decode into (see #video_picture_pool_alloc)
 *
 * Made by a codec (video_codec_t::picture_pool_alloc), which lays the
 * pictures out; the references are counted here, by the video layer
 * (#video_frame_ref), whether or not a video is decoding into them.
 */
typedef struct video_picture_pool_s {
    video_codec_t *codec;       ///< The codec they're laid out for
    int width;                  ///< The widest video they're for
    int height;                 ///< The tallest video they're for
    int buffered_pics;          ///< How far ahead a video decoding into them may go
    bool lent;                  ///< A video is decoding into them
    int count;                  ///< How many pictures
    size_t picture_size;        ///< Bytes from one picture to the next
    uint8_t *data;              ///< The pictures, one after another
    uint8_t refs[];             ///< [count] references to each picture
} video_picture_pool_t;

/** @brief Video codec structure */
typedef struct video_codec_s {
    const char *extension;                  ///< File extension handled by this codec
    video_t* (*open)(const char *fn, const video_parms_t *parms); ///< Open a video file
    void (*close)(video_t *v);              ///< Close a video file
    int (*poll)(video_t *v);                ///< Decode a bit
    bool (*next_frame)(video_t *v);         ///< Advance to the next frame
    video_frame_t (*get_frame)(video_t *v); ///< Get the current frame: picture and index
    void (*rewind)(video_t *v);             ///< Rewind the video to the beginning
    void (*seekfast)(video_t *v, int frame_idx, uint32_t offset); ///< Seek to a specific keyframe
    int (*seek)(video_t *v, int frame_idx);    ///< Seek to a specific frame index

    // Optional: reader mode (see video_open_reader). next_frame_status is
    // also used in file mode when present (next_frame is then not needed).
    video_t* (*open_reader)(const video_reader_t *reader, const video_parms_t *parms); ///< Open from a reader
    video_status_t (*next_frame_status)(video_t *v);    ///< next_frame, with a reason
    video_status_t (*take_frame)(video_t *v, video_frame_t *frame); ///< Optional: take the next frame with a reference (codecs that count references; see video_take_frame)
    void (*set_loop)(video_t *v, bool loop);            ///< Optional: see video_set_loop
    // Optional: codecs that count references decode into a picture pool, and
    // their frames name it (video_frame_t::pool).
    video_picture_pool_t* (*picture_pool_alloc)(int width, int height, int buffered_pics); ///< See video_picture_pool_alloc
    void (*picture_pool_free)(video_picture_pool_t *pool);                                  ///< Frees what picture_pool_alloc made
    int (*frames_waiting)(video_t *v);                              ///< Optional: see video_frames_waiting

    struct video_codec_s* next_codec;       ///< Next registered codec
} video_codec_t;


#endif
