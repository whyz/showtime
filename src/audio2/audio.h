#pragma once
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
#pragma once

#include <libavutil/samplefmt.h>
#include <libavresample/avresample.h>

#include "arch/threads.h"
#include "media.h"

struct media_pipe;
struct audio_decoder;

typedef struct audio_class {
  size_t ac_alloc_size;

  int (*ac_init)(struct audio_decoder *ad);
  void (*ac_fini)(struct audio_decoder *ad);
  int (*ac_reconfig)(struct audio_decoder *ad);
  int (*ac_deliver_unlocked)(struct audio_decoder *ad, int samples,
			     int64_t pts, int epoch);
  int (*ac_deliver_locked)(struct audio_decoder *ad, int samples,
                           int64_t pts, int epoch);

  void (*ac_pause)(struct audio_decoder *ad);
  void (*ac_play)(struct audio_decoder *ad);
  void (*ac_flush)(struct audio_decoder *ad);
  int (*ac_check_passthru)(struct audio_decoder *ad, int codec);
  void (*ac_set_volume)(struct audio_decoder *ad, float scale);
} audio_class_t;


typedef struct audio_decoder {
  const audio_class_t *ad_ac;
  struct media_pipe *ad_mp;
  hts_thread_t ad_tid;

  struct AVFrame *ad_frame;
  int64_t ad_pts;
  int ad_epoch;

  int ad_discontinuity;

  int ad_tile_size;   // Number of samples to be delivered per round
  int ad_delay;       // Audio output delay in us

  int ad_paused;

  int ad_in_codec_id;
  int ad_in_sample_rate;
  enum AVSampleFormat ad_in_sample_format;
  int64_t ad_in_channel_layout;

  int ad_out_sample_rate;
  enum AVSampleFormat ad_out_sample_format;
  int64_t ad_out_channel_layout;

  int ad_stereo_downmix; /* We can only output stereo so ask for downmix
			    as early as codec initialization */

  AVAudioResampleContext *ad_avr;

  void *ad_mux_buffer;
  
  struct AVFormatContext *ad_spdif_muxer;

  void *ad_spdif_frame;
  int ad_spdif_frame_size;
  int ad_spdif_frame_alloc;

  float ad_vol_scale;
} audio_decoder_t;

audio_class_t *audio_driver_init(void);


