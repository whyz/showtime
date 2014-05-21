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
#include <assert.h>
#include <unistd.h>


#include "showtime.h"
#include "media.h"
#include "audio_ext.h"
#include "audio.h"
#include "libav.h"
#include "htsmsg/htsmsg_store.h"
#include "settings.h"

#include <libavutil/avutil.h>
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>

#include <libavutil/opt.h>
#include <libavutil/mem.h>

static audio_class_t *audio_class;

static void *audio_decode_thread(void *aux);


/**
 *
 */
static void
save_matervol(void *opaque, float value)
{
  htsmsg_t *m = htsmsg_create_map();
  TRACE(TRACE_DEBUG, "audio", "Master volume set to %f dB", value);

  htsmsg_add_s32(m, "master-volume", value * 1000);
  htsmsg_store_save(m, "audiomixer");
  htsmsg_release(m);
}


/**
 *
 */
static void
audio_mastervol_init(void)
{
  htsmsg_t *m = htsmsg_store_load("audiomixer");
  int32_t i32;
  prop_t *pa, *mv, *mm;

  pa = prop_create(prop_get_global(), "audio");
  mv = prop_create(pa, "mastervolume");
  mm = prop_create(pa, "mastermute");

  prop_set_float_clipping_range(mv, -75, 12);

  if(m != NULL && !htsmsg_get_s32(m, "master-volume", &i32))
    prop_set_float(mv, (float)i32 / 1000);

  prop_set_int(mm, 0);
  
  htsmsg_release(m);

  prop_subscribe(PROP_SUB_NO_INITIAL_UPDATE,
		 PROP_TAG_CALLBACK_FLOAT, save_matervol, NULL,
		 PROP_TAG_ROOT, mv,
		 NULL);
}


/**
 *
 */
void
audio_init(void)
{
  htsmsg_t *store = htsmsg_store_load("audio2");
  if(store == NULL)
    store = htsmsg_create_map();

  prop_t *asettings =
    settings_add_dir(NULL, _p("Audio settings"), NULL, NULL,
                     _p("Setup audio output"),
                     "settings:audio");

  audio_mastervol_init();
  audio_class = audio_driver_init(asettings, store);

  settings_create_separator(asettings,
			    _p("Audio settings during video playback"));

  gconf.setting_av_volume =
    setting_create(SETTING_INT, asettings,
                   SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(_p("Audio gain adjustment during video playback")),
                   SETTING_RANGE(-12, 12),
                   SETTING_UNIT_CSTR("dB"),
                   SETTING_HTSMSG("videovolume", store, "audio2"),
                   SETTING_VALUE_ORIGIN("global"),
                   NULL);

  gconf.setting_av_sync =
    setting_create(SETTING_INT, asettings,
                   SETTINGS_INITIAL_UPDATE,
                   SETTING_TITLE(_p("Audio delay")),
                   SETTING_RANGE(-5000, 5000),
                   SETTING_STEP(50),
                   SETTING_UNIT_CSTR("ms"),
                   SETTING_HTSMSG("avdelta", store, "audio2"),
                   SETTING_VALUE_ORIGIN("global"),
                   NULL);


#if CONFIG_AUDIOTEST
  audio_test_init(asettings);
#endif
}


/**
 *
 */
void 
audio_fini(void)
{

}


/**
 *
 */
struct audio_decoder *
audio_decoder_create(struct media_pipe *mp)
{
  const audio_class_t *ac = audio_class;

  audio_decoder_t *ad = calloc(1, ac->ac_alloc_size);
  ad->ad_mp = mp;
  ad->ad_ac = ac;

  ad->ad_tile_size = 1024;
  ad->ad_frame = av_frame_alloc();
  ad->ad_pts = AV_NOPTS_VALUE;
  ad->ad_epoch = 0;
  ad->ad_vol_scale = 1.0f;

  hts_thread_create_joinable("audio decoder", &ad->ad_tid,
                             audio_decode_thread, ad, THREAD_PRIO_AUDIO);
  return ad;
}


static void
audio_cleanup_spdif_muxer(audio_decoder_t *ad)
{
  if(ad->ad_spdif_muxer == NULL)
    return;
  free(ad->ad_spdif_frame);
  ad->ad_spdif_frame = NULL;
  ad->ad_spdif_frame_alloc = 0;

  av_free(ad->ad_spdif_muxer->pb);

  free(ad->ad_mux_buffer);
  ad->ad_mux_buffer = NULL;

  avformat_free_context(ad->ad_spdif_muxer);
  ad->ad_spdif_muxer = NULL;
}


/**
 *
 */
void
audio_decoder_destroy(struct audio_decoder *ad)
{
  mp_send_cmd(ad->ad_mp, &ad->ad_mp->mp_audio, MB_CTRL_EXIT);
  hts_thread_join(&ad->ad_tid);
  mq_flush(ad->ad_mp, &ad->ad_mp->mp_audio, 1);
  av_frame_free(&ad->ad_frame);

  if(ad->ad_avr != NULL) {
    avresample_close(ad->ad_avr);
    avresample_free(&ad->ad_avr);
  }

  audio_cleanup_spdif_muxer(ad);
  free(ad);
}


/**
 *
 */
static int
spdif_mux_write(void *opaque, uint8_t *buf, int buf_size)
{
  audio_decoder_t *ad = opaque;
  int nl = ad->ad_spdif_frame_size + buf_size;
  if(nl > ad->ad_spdif_frame_alloc) {
    ad->ad_spdif_frame_alloc = nl;
    ad->ad_spdif_frame = realloc(ad->ad_spdif_frame, ad->ad_spdif_frame_alloc);
  }

  memcpy(ad->ad_spdif_frame + ad->ad_spdif_frame_size, buf, buf_size);
  ad->ad_spdif_frame_size = nl;
  return 0;
}


/**
 *
 */
static void
audio_setup_spdif_muxer(audio_decoder_t *ad, AVCodec *codec,
			media_queue_t *mq)
{
  AVOutputFormat *ofmt = av_guess_format("spdif", NULL, NULL);
  if(ofmt == NULL)
    return;

  const int mux_buffer_size = 16384;
  assert(ad->ad_mux_buffer == NULL);
  ad->ad_mux_buffer = malloc(mux_buffer_size);

  AVFormatContext *fctx = avformat_alloc_context();
  fctx->oformat = ofmt;
  fctx->pb = avio_alloc_context(ad->ad_mux_buffer, mux_buffer_size,
				1, ad, NULL, spdif_mux_write, NULL);
  AVStream *s = avformat_new_stream(fctx, codec);
  s->codec->sample_rate = 48000; // ???
  if(avcodec_open2(s->codec, codec, NULL)) {

    TRACE(TRACE_ERROR, "audio", "Unable to open %s codec for SPDIF",
	  codec->name);
  bad:
    av_free(fctx->pb);
    free(ad->ad_mux_buffer);
    ad->ad_mux_buffer = NULL;
    avformat_free_context(fctx);
    return;
  }
  
  if(avformat_write_header(fctx, NULL)) {
    TRACE(TRACE_ERROR, "audio", "Unable to open SPDIF muxer",
	  codec->name);
    goto bad;
  }
  ad->ad_spdif_muxer = fctx;
  TRACE(TRACE_DEBUG, "audio", "SPDIF muxer opened");

  const char *name;
  switch(codec->id) {
  case AV_CODEC_ID_DTS:  name = "DTS"; break;
  case AV_CODEC_ID_AC3:  name = "AC3"; break;
  case AV_CODEC_ID_EAC3: name = "EAC3"; break;
  default:
    name = "";
    break;
  }

  char str[64];
  snprintf(str, sizeof(str), "%s%sPass-Through", name, *name ? " " : "");
  prop_set_string(mq->mq_prop_codec, str);

  ad->ad_in_sample_rate = 0;
  ad->ad_in_sample_format = 0;
  ad->ad_in_channel_layout = 0;
  prop_set(ad->ad_mp->mp_prop_ctrl, "canAdjustVolume", PROP_SET_INT, 0);
}


/**
 *
 */
static void
audio_process_audio(audio_decoder_t *ad, media_buf_t *mb)
{
  const audio_class_t *ac = ad->ad_ac;
  AVFrame *frame = ad->ad_frame;
  media_pipe_t *mp = ad->ad_mp;
  media_queue_t *mq = &mp->mp_audio;
  int r;
  int got_frame;

  if(mb->mb_skip || mb->mb_stream != mq->mq_stream) 
    return;

  while(mb->mb_size) {

    if(mb->mb_cw == NULL) {
      frame->sample_rate = mb->mb_rate;
      frame->format = AV_SAMPLE_FMT_S16;
      switch(mb->mb_channels) {
      case 1:
	frame->channel_layout = AV_CH_LAYOUT_MONO;
	frame->nb_samples = mb->mb_size / 2;
	break;
      case 2:
	frame->channel_layout = AV_CH_LAYOUT_STEREO;
	frame->nb_samples = mb->mb_size / 4;
	break;
      default:
	abort();
      }
      frame->data[0] = mb->mb_data;
      frame->linesize[0] = 0;
      r = mb->mb_size;
      got_frame = 1;

    } else {

      media_codec_t *mc = mb->mb_cw;

      AVCodecContext *ctx = mc->ctx;

      if(mc->codec_id != ad->ad_in_codec_id) {
	AVCodec *codec = avcodec_find_decoder(mc->codec_id);
	TRACE(TRACE_DEBUG, "audio", "Codec changed to %s (0x%x)",
	      codec ? codec->name : "???", mc->codec_id);
	ad->ad_in_codec_id = mc->codec_id;
	ad->ad_in_sample_rate = 0;

	audio_cleanup_spdif_muxer(ad);

	ad->ad_mode = ac->ac_get_mode != NULL ?
	  ac->ac_get_mode(ad, mc->codec_id,
			  ctx ? ctx->extradata : NULL,
			  ctx ? ctx->extradata_size : 0) : AUDIO_MODE_PCM;

	if(ad->ad_mode == AUDIO_MODE_SPDIF) {
	  audio_setup_spdif_muxer(ad, codec, mq);
	} else if(ad->ad_mode == AUDIO_MODE_CODED) {
	  
	  hts_mutex_lock(&mp->mp_mutex);
	  
	  ac->ac_deliver_coded_locked(ad, mb->mb_data, mb->mb_size,
				      mb->mb_pts, mb->mb_epoch);
	  hts_mutex_unlock(&mp->mp_mutex);
	  return;
	}
      }

      if(ad->ad_spdif_muxer != NULL) {
	mb->mb_pkt.stream_index = 0;
	ad->ad_pts = mb->mb_pts;
	ad->ad_epoch = mb->mb_epoch;

	mb->mb_pts = AV_NOPTS_VALUE;
	mb->mb_dts = AV_NOPTS_VALUE;
	av_write_frame(ad->ad_spdif_muxer, &mb->mb_pkt);
	avio_flush(ad->ad_spdif_muxer->pb);
	return;
      }


      if(ad->ad_mode == AUDIO_MODE_CODED) {
	ad->ad_pts = mb->mb_pts;
	ad->ad_epoch = mb->mb_epoch;
	

      }


      if(ctx == NULL) {

	AVCodec *codec = avcodec_find_decoder(mc->codec_id);
	assert(codec != NULL); // Checked in libav.c

	ctx = mc->ctx = avcodec_alloc_context3(codec);

	if(ad->ad_stereo_downmix)
          ctx->request_channel_layout = AV_CH_LAYOUT_STEREO;

	if(avcodec_open2(mc->ctx, codec, NULL) < 0) {
	  av_freep(&mc->ctx);
	  return;
	}
      }

      r = avcodec_decode_audio4(ctx, frame, &got_frame, &mb->mb_pkt);
      if(r < 0)
	return;

      if(frame->sample_rate == 0) {
	frame->sample_rate = ctx->sample_rate;

	if(frame->sample_rate == 0 && mb->mb_cw->fmt_ctx)
	  frame->sample_rate = mb->mb_cw->fmt_ctx->sample_rate;

	if(frame->sample_rate == 0) {

          if(!ad->ad_sample_rate_fail) {
            ad->ad_sample_rate_fail = 1;
            TRACE(TRACE_ERROR, "Audio",
                  "Unable to determine sample rate");
          }
	  return;
        }
      }

      if(frame->channel_layout == 0) {
        frame->channel_layout = av_get_default_channel_layout(ctx->channels);
        if(frame->channel_layout == 0) {

          if(!ad->ad_channel_layout_fail) {
            ad->ad_channel_layout_fail = 1;
              TRACE(TRACE_ERROR, "Audio",
                    "Unable to map %d channels to channel layout");
          }
	  return;
	}
      }

      if(mp->mp_stats)
	mp_set_mq_meta(mq, ctx->codec, ctx);

    }

    if(mb->mb_pts != PTS_UNSET) {

      int od = 0, id = 0;

      if(ad->ad_avr != NULL) {
	od = avresample_available(ad->ad_avr) *
	  1000000LL / ad->ad_out_sample_rate;
	id = avresample_get_delay(ad->ad_avr) *
	  1000000LL / frame->sample_rate;
      }
      ad->ad_pts = mb->mb_pts - od - id;
      ad->ad_epoch = mb->mb_epoch;

      if(mb->mb_drive_clock)
	mp_set_current_time(mp, mb->mb_pts - ad->ad_delay,
			    mb->mb_epoch, mb->mb_delta);
      mb->mb_pts = PTS_UNSET; // No longer valid
    }


    mb->mb_data += r;
    mb->mb_size -= r;

    if(got_frame) {

      if(frame->sample_rate    != ad->ad_in_sample_rate ||
	 frame->format         != ad->ad_in_sample_format ||
	 frame->channel_layout != ad->ad_in_channel_layout ||
	 ad->ad_want_reconfig) {

	ad->ad_want_reconfig = 0;
	ad->ad_in_sample_rate    = frame->sample_rate;
	ad->ad_in_sample_format  = frame->format;
	ad->ad_in_channel_layout = frame->channel_layout;

	ac->ac_reconfig(ad);

	if(ad->ad_avr == NULL)
	  ad->ad_avr = avresample_alloc_context();
	else
	  avresample_close(ad->ad_avr);

	av_opt_set_int(ad->ad_avr, "in_sample_fmt",
		       ad->ad_in_sample_format, 0);
	av_opt_set_int(ad->ad_avr, "in_sample_rate", 
		       ad->ad_in_sample_rate, 0);
	av_opt_set_int(ad->ad_avr, "in_channel_layout",
		       ad->ad_in_channel_layout, 0);

	av_opt_set_int(ad->ad_avr, "out_sample_fmt",
		       ad->ad_out_sample_format, 0);
	av_opt_set_int(ad->ad_avr, "out_sample_rate",
		       ad->ad_out_sample_rate, 0);
	av_opt_set_int(ad->ad_avr, "out_channel_layout",
		       ad->ad_out_channel_layout, 0);

	char buf1[128];
	char buf2[128];

	av_get_channel_layout_string(buf1, sizeof(buf1), 
				     -1, ad->ad_in_channel_layout);
	av_get_channel_layout_string(buf2, sizeof(buf2), 
				     -1, ad->ad_out_channel_layout);

	TRACE(TRACE_DEBUG, "Audio",
	      "Converting from [%s %dHz %s] to [%s %dHz %s]",
	      buf1, ad->ad_in_sample_rate,
	      av_get_sample_fmt_name(ad->ad_in_sample_format),
	      buf2, ad->ad_out_sample_rate,
	      av_get_sample_fmt_name(ad->ad_out_sample_format));

	if(avresample_open(ad->ad_avr)) {
	  TRACE(TRACE_ERROR, "Audio", "Unable to open resampler");
	  avresample_free(&ad->ad_avr);
	}

	if(ac->ac_set_volume != NULL) {
	  prop_set(mp->mp_prop_ctrl, "canAdjustVolume", PROP_SET_INT, 1);
	  ac->ac_set_volume(ad, ad->ad_vol_scale);
	}
      }
      if(ad->ad_avr != NULL) {
	avresample_convert(ad->ad_avr, NULL, 0, 0,
			   frame->data, frame->linesize[0],
			   frame->nb_samples);
      } else {
	int delay = 1000000LL * frame->nb_samples / frame->sample_rate;
	usleep(delay);
      }
    }
  }
}


/**
 *
 */
void *
audio_decode_thread(void *aux)
{
  audio_decoder_t *ad = aux;
  const audio_class_t *ac = ad->ad_ac;
  int run = 1;
  media_pipe_t *mp = ad->ad_mp;
  media_queue_t *mq = &mp->mp_audio;
  media_buf_t *mb;
  int blocked = 0;

  if(ac->ac_init != NULL)
    ac->ac_init(ad);

  ad->ad_discontinuity = 1;

  hts_mutex_lock(&mp->mp_mutex);

  while(run) {

    int avail;
    
    if(ad->ad_spdif_muxer != NULL) {
      avail = ad->ad_spdif_frame_size;
    } else {
      avail = ad->ad_avr != NULL ? avresample_available(ad->ad_avr) : 0;
    }
    media_buf_t *data = TAILQ_FIRST(&mq->mq_q_data);
    media_buf_t *ctrl = TAILQ_FIRST(&mq->mq_q_ctrl);

    if(avail >= ad->ad_tile_size && blocked == 0 && !ad->ad_paused && !ctrl) {
      assert(avail != 0);

      int samples = MIN(ad->ad_tile_size, avail);
      int r;

      if(ac->ac_deliver_locked != NULL) {
        r = ac->ac_deliver_locked(ad, samples, ad->ad_pts, ad->ad_epoch);
        if(r) {
          hts_cond_wait(&mq->mq_avail, &mp->mp_mutex);
          continue;
        }
      } else {
        hts_mutex_unlock(&mp->mp_mutex);
        r = ac->ac_deliver_unlocked(ad, samples, ad->ad_pts, ad->ad_epoch);
        hts_mutex_lock(&mp->mp_mutex);
      }

      if(r) {
	blocked = 1;
      } else {
	ad->ad_pts = AV_NOPTS_VALUE;
      }
      continue;
    }

    if(ctrl != NULL) {
      TAILQ_REMOVE(&mq->mq_q_ctrl, ctrl, mb_link);
      mb = ctrl;
    } else if(data != NULL && avail < ad->ad_tile_size) {
      TAILQ_REMOVE(&mq->mq_q_data, data, mb_link);
      mb = data;
    } else {
      hts_cond_wait(&mq->mq_avail, &mp->mp_mutex);
      continue;
    }

    mq->mq_packets_current--;
    mp->mp_buffer_current -= mb->mb_size;
    mq_update_stats(mp, mq);
    hts_cond_signal(&mp->mp_backpressure);

    if(mb->mb_data_type == MB_CTRL_UNBLOCK) {
      assert(blocked);
      blocked = 0;
    } else if(ad->ad_mode == AUDIO_MODE_CODED && 
	      ac->ac_deliver_coded_locked != NULL &&
	      mb->mb_data_type == MB_AUDIO) {

      ac->ac_deliver_coded_locked(ad, mb->mb_data, mb->mb_size,
				  mb->mb_pts, mb->mb_epoch);

    } else {

      hts_mutex_unlock(&mp->mp_mutex);

      switch(mb->mb_data_type) {
      case MB_AUDIO:
	audio_process_audio(ad, mb);
	break;

      case MB_SET_PROP_STRING:
        prop_set_string(mb->mb_prop, (void *)mb->mb_data);
	break;

      case MB_CTRL_SET_VOLUME_MULTIPLIER:
        ad->ad_vol_scale = mb->mb_float;
	if(ac->ac_set_volume != NULL)
	  ac->ac_set_volume(ad, ad->ad_vol_scale);
        break;

      case MB_CTRL_PAUSE:
	ad->ad_paused = 1;
	if(ac->ac_pause)
	  ac->ac_pause(ad);
	break;

      case MB_CTRL_PLAY:
	ad->ad_paused = 0;
	if(ac->ac_play)
	  ac->ac_play(ad);
	break;

      case MB_CTRL_FLUSH:
        // Reset some error reporting filters
        ad->ad_channel_layout_fail = 0;
        ad->ad_sample_rate_fail = 0;

	if(ac->ac_flush)
	  ac->ac_flush(ad);
	ad->ad_pts = AV_NOPTS_VALUE;

	if(mp->mp_seek_audio_done != NULL)
	  mp->mp_seek_audio_done(mp);
	ad->ad_discontinuity = 1;

	if(ad->ad_avr != NULL) {
	  avresample_read(ad->ad_avr, NULL, avresample_available(ad->ad_avr));
	  assert(avresample_available(ad->ad_avr) == 0);
	}
	break;

      case MB_CTRL_EXIT:
	run = 0;
	break;

      default:
	abort();
      }

      hts_mutex_lock(&mp->mp_mutex);
    }
    media_buf_free_locked(mp, mb);
  }

  hts_mutex_unlock(&mp->mp_mutex);

  if(ac->ac_fini != NULL)
    ac->ac_fini(ad);
  return NULL;
}



#if 0
/**
 *
 */
static void *
dummy_audio_thread(void *aux)
{
  audio_decoder_t *ad = aux;
  media_pipe_t *mp = ad->ad_mp;
  media_queue_t *mq = &mp->mp_audio;
  media_buf_t *mb;
  int hold = 0;
  int run = 1;
  int64_t rt = 0;
  int64_t base = AV_NOPTS_VALUE;


  hts_mutex_lock(&mp->mp_mutex);

  while(run) {

    if((mb = TAILQ_FIRST(&mq->mq_q)) == NULL) {
      hts_cond_wait(&mq->mq_avail, &mp->mp_mutex);
      continue;
    }

    if(mb->mb_data_type == MB_AUDIO && hold && mb->mb_skip == 0) {
      hts_cond_wait(&mq->mq_avail, &mp->mp_mutex);
      continue;
    }

    TAILQ_REMOVE(&mq->mq_q, mb, mb_link);
    mq->mq_packets_current--;
    mp->mp_buffer_current -= mb->mb_size;
    mq_update_stats(mp, mq);
    hts_cond_signal(&mp->mp_backpressure);
    hts_mutex_unlock(&mp->mp_mutex);

    switch(mb->mb_data_type) {
    case MB_CTRL_EXIT:
      run = 0;
      break;

    case MB_CTRL_PAUSE:
      hold = 1;
      break;

    case MB_CTRL_PLAY:
      hold = 0;
      base = AV_NOPTS_VALUE;
      break;

    case MB_FLUSH:
      base = AV_NOPTS_VALUE;
      break;

    case MB_AUDIO:
      if(mb->mb_skip || mb->mb_stream != mq->mq_stream) 
	break;
      if(mb->mb_pts != AV_NOPTS_VALUE) {
        audio_set_clock(mp, mb->mb_pts, 0, mb->mb_epoch);

        if(base == AV_NOPTS_VALUE) {
          base = mb->mb_pts;
          rt = showtime_get_ts();
        } else {
          int64_t d = mb->mb_pts - base;
          if(d > 0) {
            int sleeptime = rt + d - showtime_get_ts();
	    if(sleeptime > 0)
	      usleep(sleeptime);
          }
        }
      }
      break;

    default:
      abort();
    }
    hts_mutex_lock(&mp->mp_mutex);
    media_buf_free_locked(mp, mb);
  }
  hts_mutex_unlock(&mp->mp_mutex);
  return NULL;
}

/**
 *
 */
static audio_class_t dummy_audio_class = {
  .ac_alloc_size = sizeof(audio_decoder_t),
  .ac_thread = dummy_audio_thread,
};
#endif
