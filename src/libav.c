/*
 *  Showtime Mediacenter
 *  Copyright (C) 2007-2013 Lonelycoder AB
 *
 *  This program is free software: you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation, either version 3 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 *  This program is also available under a commercial proprietary license.
 *  For more information, contact andreas@lonelycoder.com
 */

#include <ctype.h>
#include <libavformat/avformat.h>
#include <libavutil/audioconvert.h>
#include <libswscale/swscale.h>
#include <libavutil/pixdesc.h>

#include "showtime.h"
#include "media.h"
#include "libav.h"
#include "fileaccess/fa_libav.h"
#include "video/video_decoder.h"

#if ENABLE_VDPAU
#include "video/vdpau.h"
#endif




static const int libav_colorspace_tbl[] = {
  [AVCOL_SPC_BT709]     = COLOR_SPACE_BT_709,
  [AVCOL_SPC_BT470BG]   = COLOR_SPACE_BT_601,
  [AVCOL_SPC_SMPTE170M] = COLOR_SPACE_BT_601,
  [AVCOL_SPC_SMPTE240M] = COLOR_SPACE_SMPTE_240M,
};


#define vd_valid_duration(t) ((t) > 10000ULL && (t) < 1000000ULL)


/**
 *
 */
static void
libav_deliver_frame(video_decoder_t *vd,
                    media_pipe_t *mp, media_queue_t *mq,
                    AVCodecContext *ctx, AVFrame *frame,
                    const media_buf_meta_t *mbm, int decode_time,
                    const media_codec_t *mc)
{
  frame_info_t fi;

  /* Compute aspect ratio */
  switch(mbm->mbm_aspect_override) {
  case 0:

    fi.fi_dar_num = frame->width;
    fi.fi_dar_den = frame->height;

    if(frame->sample_aspect_ratio.num) {
      fi.fi_dar_num *= frame->sample_aspect_ratio.num;
      fi.fi_dar_den *= frame->sample_aspect_ratio.den;
    } else if(mc->sar_num) {
      fi.fi_dar_num *= mc->sar_num;
      fi.fi_dar_den *= mc->sar_den;
    }

    break;
  case 1:
    fi.fi_dar_num = 4;
    fi.fi_dar_den = 3;
    break;
  case 2:
    fi.fi_dar_num = 16;
    fi.fi_dar_den = 9;
    break;
  }

  int64_t pts = video_decoder_infer_pts(mbm, vd,
					frame->pict_type == AV_PICTURE_TYPE_B);

  int duration = mbm->mbm_duration;

  if(!vd_valid_duration(duration)) {
    /* duration is zero or very invalid, use duration from last output */
    duration = vd->vd_estimated_duration;
  }

  if(pts == AV_NOPTS_VALUE && vd->vd_nextpts != AV_NOPTS_VALUE)
    pts = vd->vd_nextpts; /* no pts set, use estimated pts */

  if(pts != AV_NOPTS_VALUE && vd->vd_prevpts != AV_NOPTS_VALUE) {
    /* we know PTS of a prior frame */
    int64_t t = (pts - vd->vd_prevpts) / vd->vd_prevpts_cnt;

    if(vd_valid_duration(t)) {
      /* inter frame duration seems valid, store it */
      vd->vd_estimated_duration = t;
      if(duration == 0)
	duration = t;

    }
  }
  
  duration += frame->repeat_pict * duration / 2;
 
  if(pts != AV_NOPTS_VALUE) {
    vd->vd_prevpts = pts;
    vd->vd_prevpts_cnt = 0;
  }
  vd->vd_prevpts_cnt++;

  if(duration == 0) {
    TRACE(TRACE_DEBUG, "Video", "Dropping frame with duration = 0");
    return;
  }

  prop_set_int(mq->mq_prop_too_slow, decode_time > duration);

  if(pts != AV_NOPTS_VALUE) {
    vd->vd_nextpts = pts + duration;
  } else {
    vd->vd_nextpts = AV_NOPTS_VALUE;
  }
#if 0
  static int64_t lastpts = AV_NOPTS_VALUE;
  if(lastpts != AV_NOPTS_VALUE) {
    printf("DEC: %20"PRId64" : %-20"PRId64" %d %"PRId64" %d\n", pts, pts - lastpts, mbm->mbm_drive_clock,
           mbm->mbm_delta, duration);
    if(pts - lastpts > 1000000) {
      abort();
    }
  }
  lastpts = pts;
#endif

  vd->vd_interlaced |=
    frame->interlaced_frame && !mbm->mbm_disable_deinterlacer;

  fi.fi_width = frame->width;
  fi.fi_height = frame->height;
  fi.fi_pts = pts;
  fi.fi_epoch = mbm->mbm_epoch;
  fi.fi_delta = mbm->mbm_delta;
  fi.fi_duration = duration;
  fi.fi_drive_clock = mbm->mbm_drive_clock;

  fi.fi_interlaced = !!vd->vd_interlaced;
  fi.fi_tff = !!frame->top_field_first;
  fi.fi_prescaled = 0;

  fi.fi_color_space = 
    ctx->colorspace < ARRAYSIZE(libav_colorspace_tbl) ? 
    libav_colorspace_tbl[ctx->colorspace] : 0;

  fi.fi_type = 'LAVC';

  // Check if we should skip directly to convert code
  if(vd->vd_convert_width  != frame->width ||
     vd->vd_convert_height != frame->height ||
     vd->vd_convert_pixfmt != frame->format) {

    // Nope, go ahead and deliver frame as-is

    fi.fi_data[0] = frame->data[0];
    fi.fi_data[1] = frame->data[1];
    fi.fi_data[2] = frame->data[2];

    fi.fi_pitch[0] = frame->linesize[0];
    fi.fi_pitch[1] = frame->linesize[1];
    fi.fi_pitch[2] = frame->linesize[2];

    fi.fi_pix_fmt = frame->format;
    fi.fi_avframe = frame;

    int r = video_deliver_frame(vd, &fi);

    /* return value
     * 0  = OK
     * 1  = Need convert to YUV420P
     * -1 = Fail
     */

    if(r != 1)
      return;
  }

  // Need to convert frame

  vd->vd_sws =
    sws_getCachedContext(vd->vd_sws,
                         frame->width, frame->height, frame->format,
                         frame->width, frame->height, PIX_FMT_YUV420P,
                         0, NULL, NULL, NULL);

  if(vd->vd_convert_width  != frame->width  ||
     vd->vd_convert_height != frame->height ||
     vd->vd_convert_pixfmt != frame->format) {
    avpicture_free(&vd->vd_convert);

    vd->vd_convert_width  = frame->width;
    vd->vd_convert_height = frame->height;
    vd->vd_convert_pixfmt = frame->format;

    avpicture_alloc(&vd->vd_convert, PIX_FMT_YUV420P, frame->width,
                    frame->height);

    TRACE(TRACE_DEBUG, "Video", "Converting from %s to %s",
	  av_get_pix_fmt_name(frame->format),
	  av_get_pix_fmt_name(PIX_FMT_YUV420P));
  }

  sws_scale(vd->vd_sws, (void *)frame->data, frame->linesize, 0,
            frame->height, vd->vd_convert.data, vd->vd_convert.linesize);

  fi.fi_data[0] = vd->vd_convert.data[0];
  fi.fi_data[1] = vd->vd_convert.data[1];
  fi.fi_data[2] = vd->vd_convert.data[2];

  fi.fi_pitch[0] = vd->vd_convert.linesize[0];
  fi.fi_pitch[1] = vd->vd_convert.linesize[1];
  fi.fi_pitch[2] = vd->vd_convert.linesize[2];

  fi.fi_type = 'LAVC';
  fi.fi_pix_fmt = PIX_FMT_YUV420P;
  fi.fi_avframe = NULL;
  video_deliver_frame(vd, &fi);
}


/**
 *
 */
static void
libav_decode_video(struct media_codec *mc, struct video_decoder *vd,
                   struct media_queue *mq, struct media_buf *mb, int reqsize)
{
  int got_pic = 0;
  media_pipe_t *mp = vd->vd_mp;
  AVCodecContext *ctx = mc->ctx;
  AVFrame *frame = vd->vd_frame;
  int t;

  copy_mbm_from_mb(&vd->vd_reorder[vd->vd_reorder_ptr], mb);
  ctx->reordered_opaque = vd->vd_reorder_ptr;
  vd->vd_reorder_ptr = (vd->vd_reorder_ptr + 1) & VIDEO_DECODER_REORDER_MASK;

  /*
   * If we are seeking, drop any non-reference frames
   */
  ctx->skip_frame = mb->mb_skip == 1 ? AVDISCARD_NONREF : AVDISCARD_DEFAULT;
  avgtime_start(&vd->vd_decode_time);

  avcodec_decode_video2(ctx, frame, &got_pic, &mb->mb_pkt);

  t = avgtime_stop(&vd->vd_decode_time, mq->mq_prop_decode_avg,
		   mq->mq_prop_decode_peak);

  if(mp->mp_stats)
    mp_set_mq_meta(mq, ctx->codec, ctx);

  const media_buf_meta_t *mbm = &vd->vd_reorder[frame->reordered_opaque];

  if(got_pic == 0 || mbm->mbm_skip == 1)
    return;

  libav_deliver_frame(vd, mp, mq, ctx, frame, mbm, t, mc);
  av_frame_unref(frame);
}


/**
 *
 */
static void
libav_video_flush(media_codec_t *mc, video_decoder_t *vd)
{
  AVCodecContext *ctx = mc->ctx;
  int got_pic = 0;
  AVPacket avpkt;
  av_init_packet(&avpkt);
  avpkt.data = NULL;
  avpkt.size = 0;
  do {
    avcodec_decode_video2(ctx, vd->vd_frame, &got_pic, &avpkt);
  } while(got_pic);
  avcodec_flush_buffers(ctx);
}


/**
 *
 */
static enum PixelFormat
libav_get_format(struct AVCodecContext *ctx, const enum PixelFormat *fmt)
{
  media_codec_t *mc = ctx->opaque;
  if(mc->close != NULL) {
    mc->close(mc);
    mc->close = NULL;
  }

#if ENABLE_VDPAU
  if(!vdpau_init_libav_decode(mc, ctx)) {
    return AV_PIX_FMT_VDPAU;
  }
#endif
  mc->get_buffer2 = &avcodec_default_get_buffer2;
  return avcodec_default_get_format(ctx, fmt);
}


/**
 *
 */
static int
get_buffer2_wrapper(struct AVCodecContext *s, AVFrame *frame, int flags)
{
  media_codec_t *mc = s->opaque;
  return mc->get_buffer2(s, frame, flags);
}

/**
 *
 */
static int
media_codec_create_lavc(media_codec_t *cw, const media_codec_params_t *mcp,
                        media_pipe_t *mp)
{
  const AVCodec *codec = avcodec_find_decoder(cw->codec_id);

  if(codec == NULL)
    return -1;

  if(cw->codec_id == AV_CODEC_ID_AC3 ||
     cw->codec_id == AV_CODEC_ID_EAC3 ||
     cw->codec_id == AV_CODEC_ID_DTS) {

    // We create codec instances later in audio thread.
    return 0;
  }

  cw->ctx = cw->fmt_ctx ?: avcodec_alloc_context3(codec);
  //  cw->codec_ctx->debug = FF_DEBUG_PICT_INFO | FF_DEBUG_BUGS;

  if(mcp != NULL && mcp->extradata != NULL && !cw->ctx->extradata) {
    cw->ctx->extradata = calloc(1, mcp->extradata_size +
				FF_INPUT_BUFFER_PADDING_SIZE);
    memcpy(cw->ctx->extradata, mcp->extradata, mcp->extradata_size);
    cw->ctx->extradata_size = mcp->extradata_size;
  }

  if(cw->codec_id == AV_CODEC_ID_H264 && gconf.concurrency > 1) {
    cw->ctx->thread_count = gconf.concurrency;
    if(mcp && mcp->cheat_for_speed)
      cw->ctx->flags2 |= CODEC_FLAG2_FAST;
  }

  if(codec->type == AVMEDIA_TYPE_VIDEO) {

    cw->get_buffer2 = &avcodec_default_get_buffer2;

    cw->ctx->opaque = cw;
    cw->ctx->refcounted_frames = 1;
    cw->ctx->get_format = &libav_get_format;
    cw->ctx->get_buffer2 = &get_buffer2_wrapper;

    cw->decode = &libav_decode_video;
    cw->flush  = &libav_video_flush;
  }

  if(avcodec_open2(cw->ctx, codec, NULL) < 0) {
    TRACE(TRACE_INFO, "libav", "Unable to open codec %s",
	  codec ? codec->name : "<noname>");

    if(cw->fmt_ctx != cw->ctx)
      av_freep(&cw->ctx);

    return -1;
  }

  return 0;
}


REGISTER_CODEC(NULL, media_codec_create_lavc, 1000);

/**
 *
 */
media_format_t *
media_format_create(AVFormatContext *fctx)
{
  media_format_t *fw = malloc(sizeof(media_format_t));
  atomic_set(&fw->refcount, 1);
  fw->fctx = fctx;
  return fw;
}


/**
 *
 */
void
media_format_deref(media_format_t *fw)
{
  if(atomic_dec(&fw->refcount))
    return;
  fa_libav_close_format(fw->fctx);
  free(fw);
}


/**
 *
 */
void
metadata_from_libav(char *dst, size_t dstlen,
		    const AVCodec *codec, const AVCodecContext *avctx)
{
  char *n;
  int off = snprintf(dst, dstlen, "%s", codec->name);

  n = dst;
  while(*n) {
    *n = toupper((int)*n);
    n++;
  }

  if(codec->id  == AV_CODEC_ID_H264) {
    const char *p;
    switch(avctx->profile) {
    case FF_PROFILE_H264_BASELINE:  p = "Baseline";  break;
    case FF_PROFILE_H264_MAIN:      p = "Main";      break;
    case FF_PROFILE_H264_EXTENDED:  p = "Extended";  break;
    case FF_PROFILE_H264_HIGH:      p = "High";      break;
    case FF_PROFILE_H264_HIGH_10:   p = "High 10";   break;
    case FF_PROFILE_H264_HIGH_422:  p = "High 422";  break;
    case FF_PROFILE_H264_HIGH_444:  p = "High 444";  break;
    case FF_PROFILE_H264_CAVLC_444: p = "CAVLC 444"; break;
    default:                        p = NULL;        break;
    }

    if(p != NULL && avctx->level != FF_LEVEL_UNKNOWN)
      off += snprintf(dst + off, dstlen - off,
		      ", %s (Level %d.%d)",
		      p, avctx->level / 10, avctx->level % 10);
  }

  if(avctx->codec_type == AVMEDIA_TYPE_AUDIO) {
    char buf[64];

    av_get_channel_layout_string(buf, sizeof(buf), avctx->channels,
                                 avctx->channel_layout);

    off += snprintf(dst + off, dstlen - off, ", %d Hz, %s",
		    avctx->sample_rate, buf);
  }

  if(avctx->width)
    off += snprintf(dst + off, dstlen - off,
		    ", %dx%d", avctx->width, avctx->height);

  if(avctx->codec_type == AVMEDIA_TYPE_AUDIO && avctx->bit_rate)
    off += snprintf(dst + off, dstlen - off,
		    ", %d kb/s", avctx->bit_rate / 1000);

  if(avctx->hwaccel != NULL)
    off += snprintf(dst + off, dstlen - off, " (%s)",
                    avctx->hwaccel->name);
}

/**
 *
 */
void
mp_set_mq_meta(media_queue_t *mq, const AVCodec *codec,
	       const AVCodecContext *avctx)
{
  char buf[128];
  metadata_from_libav(buf, sizeof(buf), codec, avctx);
  prop_set_string(mq->mq_prop_codec, buf);
}


