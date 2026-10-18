/**
 * @file video.c
 * @author Giovanni Bajo <giovannibajo@gmail.com>
 * @brief Video player subsystem
 * 
 * 
 */

#include "video.h"
#include "video_internal.h"
#include "debug.h"
#include "asset.h"
#include <sys/stat.h>

static video_codec_t *registered_codecs = NULL;

typedef struct video_seektable_s {
    char magic[3];              ///< "VSK" magic
    uint8_t version;            ///< Version of the seek table format
    int num_entries;            ///< Number of entries in the seek table
    uint32_t *file_offsets;     ///< Array of file offsets for keyframes
    int *frame_idx;             ///< Array of frame indices for keyframes
    uint32_t mean_offset;       ///< Mean of the offsets deltas
    uint32_t mean_frame;        ///< Mean of the frame deltas
} video_seektable_t;

/** @brief Decode a pointer relative to object start */
#define PTR_DECODE(font, ptr)    ((void*)(((uint8_t*)(font)) + (uint32_t)(ptr)))

static video_seektable_t *video_seektable_load(const char *fn)
{
    video_seektable_t *tbl = asset_load(fn, NULL);
    assertf(memcmp(tbl->magic, "VSK", 3) == 0, "Invalid seek table file: %s", fn);
    assertf(tbl->version == 2, "Unsupported seek table version %d in file: %s", tbl->version, fn);

    tbl->file_offsets = PTR_DECODE(tbl, tbl->file_offsets);
    tbl->frame_idx    = PTR_DECODE(tbl, tbl->frame_idx);

    // Decode residuals to absolute values. The file stores residuals from the mean
    // of the deltas to improve compressibility, but we need absolute values at runtime.
    for (int i=1; i< tbl->num_entries; i++) {
        tbl->file_offsets[i] += tbl->file_offsets[i-1] + tbl->mean_offset;
        tbl->frame_idx[i]    += tbl->frame_idx[i-1]    + tbl->mean_frame;
    }

    return tbl;
}

/** 
 * @brief Lookup the seektable for a specified frame.
 * 
 * This function performs a binary search on the seek table to find the
 * file offset corresponding to the specified frame index. If the exact frame
 * index is not found, it returns the file offset of the closest preceding
 * keyframe.
 * 
 * @param tbl          Pointer to the seek table
 * @param frame_idx    Frame index to seek to. On return, it contains the actual
 *                     frame index found in the seek table (which might be
 *                     different from the requested one).
 * @return             File offset of the keyframe
 */
static uint32_t video_seektable_lookup(video_seektable_t *tbl, int *frame_idx)
{
    int left = 0;
    int right = tbl->num_entries - 1;
    int result_idx = -1;

    while (left <= right) {
        int mid = left + (right - left) / 2;
        if (tbl->frame_idx[mid] == *frame_idx) {
            result_idx = mid;
            break;
        } else if (tbl->frame_idx[mid] < *frame_idx) {
            result_idx = mid; // Keep track of the closest preceding keyframe
            left = mid + 1;
        } else {
            right = mid - 1;
        }
    }

    if (result_idx >= 0) {
        *frame_idx = tbl->frame_idx[result_idx];
        return tbl->file_offsets[result_idx];
    } else {
        // No valid keyframe found; return start of the file
        *frame_idx = 0;
        return 0;
    }
}

static void video_seektable_free(video_seektable_t *tbl)
{
    free(tbl);
}

void video_register_codec(video_codec_t *codec)
{
    if (codec->next_codec != NULL) {
        debugf("video_register_codec: codec for %s already registered\n", codec->extension);
        return;
    }
    codec->next_codec = registered_codecs;
    registered_codecs = codec;
}

video_t* video_open(const char *fn, const video_parms_t *parms)
{
    video_codec_t *codec = registered_codecs;

    const char *ext = strrchr(fn, '.');
    assertf(ext, "File %s has no extension", fn);

    while (codec) {
        if (strcmp(codec->extension, ext) == 0) {
            video_t *v = codec->open(fn, parms);
            if (v) {
                v->codec = codec;

                // If the codec support fast seeking, check if there is a seek
                // table for this video
                if (v->codec->seekfast) {                
                    int fnlen = strlen(fn), extlen = strlen(ext);
                    char *seekfn = alloca(fnlen - extlen + strlen(".seek") + 1);
                    strcpy(seekfn, fn);
                    strcpy(seekfn + (fnlen - extlen), ".seek");

                    struct stat st;
                    if (stat(seekfn, &st) == 0) {
                        v->seektable = video_seektable_load(seekfn);
                    }
                }
            }
            return v;
        }
        codec = codec->next_codec;
    }
    
    assertf(false, "No registered codec found for video file: %s", fn);
    return NULL;
}

video_t* video_open_reader(video_codec_t *codec, const video_reader_t *reader,
    const video_parms_t *parms)
{
    assertf(codec && codec->open_reader, "Codec %s doesn't support readers",
        codec ? codec->extension : "(null)");
    assertf(reader && reader->read && reader->try_read && reader->rewind,
        "video_open_reader: the reader needs read, try_read and rewind");
    video_t *v = codec->open_reader(reader, parms);
    if (v) {
        v->codec = codec;
    }
    return v;
}

void video_close(video_t *v)
{
    if (v->seektable) {
        video_seektable_free(v->seektable);
        v->seektable = NULL;
    }
    v->codec->close(v);
}

video_info_t video_get_info(video_t *v)
{
    return v->info;
}

int video_poll(video_t *v)
{
    if (!v->codec->poll) return 0;
    return v->codec->poll(v);
}

int video_frames_waiting(video_t *v)
{
    assertf(v->codec->frames_waiting, "Codec %s can't count the frames waiting", v->codec->extension);
    return v->codec->frames_waiting(v);
}

video_status_t video_take_frame(video_t *v, video_frame_t *frame)
{
    assertf(frame, "video_take_frame: no frame to fill in");
    video_status_t status;
    if (v->codec->take_frame) {
        // The codec counts references: the frame comes with one, the caller's.
        status = v->codec->take_frame(v, frame);
        if (status == VIDEO_FRAME) {
            frame->video = v;
        }
        return status;
    }
    // A codec that can't count references keeps the last frame itself, until
    // the next take (see video.h).
    if (v->codec->next_frame_status) {
        status = v->codec->next_frame_status(v);
    } else {
        // Codecs without reader mode: no frame always means the end.
        status = v->codec->next_frame(v) ? VIDEO_FRAME : VIDEO_END;
    }
    if (status != VIDEO_FRAME) {
        return status;
    }

    // The codec owns the frame, index included: only it knows which picture
    // this is (it may have decoded ahead, rewound or looped since).
    *frame = v->codec->get_frame(v);
    frame->video = v;
    return VIDEO_FRAME;
}

void video_frame_ref(const video_frame_t *frame)
{
    video_t *v = frame->video;
    assertf(v, "video_frame_ref: not a frame from video_take_frame");
    assertf(v->codec->frame_ref, "Codec %s can't reference frames", v->codec->extension);
    v->codec->frame_ref(v, frame);
}

void video_frame_unref(const video_frame_t *frame)
{
    video_t *v = frame->video;
    assertf(v, "video_frame_unref: not a frame from video_take_frame");
    // A codec that can't count references keeps the last frame taken itself
    // (see video_take_frame): there's nothing to drop.
    if (!v->codec->frame_unref) {
        return;
    }
    v->codec->frame_unref(v, frame);
}

void video_set_loop(video_t *v, bool loop)
{
    assertf(v->codec->set_loop, "Codec %s can't loop", v->codec->extension);
    v->codec->set_loop(v, loop);
}

void video_rewind(video_t *v)
{
    v->codec->rewind(v);
}

int video_seek(video_t *v, int frame_idx)
{
    if (v->seektable) {
        int keyframe_idx = frame_idx;
        uint32_t keyframe_off = video_seektable_lookup(v->seektable, &keyframe_idx);
        v->codec->seekfast(v, keyframe_idx, keyframe_off);
        return keyframe_idx;
    }

    if (v->codec->seek) {
        return v->codec->seek(v, frame_idx);
    }
    
    return -1;
}

bool video_is_seekable(video_t *v, int frame_idx)
{
    if (!v->seektable) return false;

    int found = frame_idx;
    video_seektable_lookup(v->seektable, &found);
    return found == frame_idx;
}

