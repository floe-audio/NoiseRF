// Copyright 2025 Sam Windell
// SPDX-License-Identifier: LGPL-3.0
//
// This file is a CLAP adaptation of the noise-repellent plugin
// Originally from: https://github.com/lucianodato/noise-repellent
//
// clap-c99-distortion was used as a template for the CLAP interface:
// https://github.com/baconpaul/clap-c99-distortion
// Copyright 2022, Paul Walker and others as listed in the git history
// SPDX-License-Identifier: MIT

#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <assert.h>
#include <clap/clap.h>
#include <math.h>

#include "config.h"
#include "debug.h"
#include "signal_crossfade.h"
#include "specbleach_denoiser.h"

static const clap_plugin_descriptor_t s_desc_stereo = {
    .clap_version = CLAP_VERSION_INIT,
    .id = PROJECT_ID,
    .name = PROJECT_NAME,
    .vendor = PROJECT_VENDOR,
    .url = PROJECT_URL,
    .manual_url = "",
    .support_url = "",
    .version = PROJECT_VERSION,
    .description = PROJECT_DESCRIPTION,
    .features = (const char *[]){CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                                 CLAP_PLUGIN_FEATURE_STEREO,
                                 CLAP_PLUGIN_FEATURE_RESTORATION, NULL},
};

static const clap_plugin_descriptor_t s_desc_mono = {
    .clap_version = CLAP_VERSION_INIT,
    .id = PROJECT_ID ".mono",
    .name = PROJECT_NAME " Mono",
    .vendor = PROJECT_VENDOR,
    .url = PROJECT_URL,
    .manual_url = "",
    .support_url = "",
    .version = PROJECT_VERSION,
    .description = PROJECT_DESCRIPTION,
    .features = (const char *[]){CLAP_PLUGIN_FEATURE_AUDIO_EFFECT,
                                 CLAP_PLUGIN_FEATURE_MONO,
                                 CLAP_PLUGIN_FEATURE_RESTORATION, NULL},
};

enum param_ids {
  pid_AMOUNT = 238,
  pid_OFFSET = 1923048,
  pid_SMOOTHING = 349857,
  pid_WHITENING = 12357,
  pid_TRANSIENT_PROTECTION = 329847,
  pid_LEARN_NOISE = 57433,
  pid_RESIDUAL_LISTEN = 56201,
  pid_RESET_PROFILE = 453689734,
  pid_ENABLE = 239487,
  pid_NOISE_SCALING_TYPE = 6710386,
  pid_POST_FILTER_THRESHOLD = 18613465,
};

#define PARAMS_COUNT 11
#define NOISE_PROFILE_MAX_SIZE 9000

typedef struct {
  float channels[2][NOISE_PROFILE_MAX_SIZE];
  uint32_t blocks_averaged;
  uint32_t size;
} noise_profile_state;

#define TRIPLE_BUFFER_DIRTY_BIT (1u << 31)
#define TRIPLE_BUFFER_MASK (~TRIPLE_BUFFER_DIRTY_BIT)

typedef struct {
  noise_profile_state buffers[3];
  _Atomic uint32_t middle_buffer_state; // producer and consumer
  uint32_t back_buffer_index;           // producer
  uint32_t front_buffer_index;          // consumer
} noise_profile_state_swap_buffers;

static void
init_noise_profile_buffers(noise_profile_state_swap_buffers *buffers) {
  assert(buffers->buffers[0].channels[0][0] == 0); // Must already be zeroed
  buffers->middle_buffer_state = 1;
  buffers->back_buffer_index = 0;
  buffers->front_buffer_index = 2;
}

// producer
static noise_profile_state *
writable_noise_profile_state(noise_profile_state_swap_buffers *buffers) {
  return &buffers->buffers[buffers->back_buffer_index];
}

// producer
static void
publish_noise_profile_state(noise_profile_state_swap_buffers *buffers) {
  const uint32_t old_middle_state =
      atomic_exchange(&buffers->middle_buffer_state,
                      buffers->back_buffer_index | TRIPLE_BUFFER_DIRTY_BIT);
  buffers->back_buffer_index = old_middle_state & TRIPLE_BUFFER_MASK;
}

// consumer
bool noise_profile_state_consume(noise_profile_state_swap_buffers *buffers,
                                 noise_profile_state **out_state) {
  if (!(buffers->middle_buffer_state & TRIPLE_BUFFER_DIRTY_BIT)) {
    *out_state = &buffers->buffers[buffers->front_buffer_index];
    return false;
  }

  const uint32_t prev = atomic_exchange(&buffers->middle_buffer_state,
                                        buffers->front_buffer_index);
  buffers->front_buffer_index = prev & TRIPLE_BUFFER_MASK;
  *out_state = &buffers->buffers[buffers->front_buffer_index];
  return true;
}

typedef struct {
  clap_plugin_t plugin;
  const clap_host_t *host;
  const clap_host_latency_t *hostLatency;
  const clap_host_log_t *hostLog;
  const clap_host_thread_check_t *hostThreadCheck;
  const clap_host_params_t *hostParams;

  uint32_t channel_count;

  _Atomic float amount;
  _Atomic float offset;
  _Atomic float smoothing;
  _Atomic float whitening;
  _Atomic bool transient_protection;
  _Atomic uint32_t learn_noise;
  _Atomic bool residual_listen;
  _Atomic bool reset_profile;
  _Atomic bool enable;
  _Atomic uint32_t noise_scaling_type;
  _Atomic float post_filter_threshold;

  // We use atomic triple buffers for the noise profile state to allow for
  // thread-safe communication between the main thread (which does loading and
  // saving), and the audio thread (which does processing).
  noise_profile_state_swap_buffers pending_noise_profile_change;
  noise_profile_state_swap_buffers current_noise_profile;

  SignalCrossfade *soft_bypass;
  SpectralBleachHandle lib_instance[2];
  float noise_profile[2][NOISE_PROFILE_MAX_SIZE]; // audio thread
} clap_noiserf;

static void process_event(clap_noiserf *plug, const clap_event_header_t *hdr);

static void set_all_params_to_default(clap_noiserf *plug);

/////////////////////////////
// clap_plugin_audio_ports //
/////////////////////////////

static uint32_t audio_ports_count(const clap_plugin_t *plugin, bool is_input) {
  return 1;
}

static bool audio_ports_get(const clap_plugin_t *plugin, uint32_t index,
                            bool is_input, clap_audio_port_info_t *info) {
  clap_noiserf *plug = plugin->plugin_data;
  if (index > 0)
    return false;
  info->id = 0;
  if (is_input)
    strncpy(info->name, "In", sizeof(info->name));
  else
    strncpy(info->name, "Out", sizeof(info->name));
  info->channel_count = plug->channel_count;
  info->flags = CLAP_AUDIO_PORT_IS_MAIN;
  info->port_type =
      plug->channel_count == 1 ? CLAP_PORT_MONO : CLAP_PORT_STEREO;
  info->in_place_pair = CLAP_INVALID_ID;
  return true;
}

static const clap_plugin_audio_ports_t s_audio_ports = {
    .count = audio_ports_count,
    .get = audio_ports_get,
};

//////////////////
// clap_latency //
//////////////////

uint32_t latency_get(const clap_plugin_t *plugin) {
  clap_noiserf *plug = plugin->plugin_data;
  if (!plug->lib_instance[0]) {
    return 0;
  }
  return specbleach_get_latency(plug->lib_instance[0]);
}

static const clap_plugin_latency_t s_latency = {
    .get = latency_get,
};

//////////////////
// clap_params //
//////////////////

static void set_value(clap_noiserf *plug, uint32_t param_id, double value) {
  switch (param_id) {
  case pid_AMOUNT:
    plug->amount = value;
    break;
  case pid_OFFSET:
    plug->offset = value;
    break;
  case pid_SMOOTHING:
    plug->smoothing = value;
    break;
  case pid_WHITENING:
    plug->whitening = value;
    break;
  case pid_TRANSIENT_PROTECTION:
    plug->transient_protection = value >= 0.5;
    break;
  case pid_LEARN_NOISE:
    plug->learn_noise = value;
    break;
  case pid_RESIDUAL_LISTEN:
    plug->residual_listen = value >= 0.5;
    break;
  case pid_RESET_PROFILE:
    plug->reset_profile = value >= 0.5;
    break;
  case pid_ENABLE:
    plug->enable = value >= 0.5;
    break;
  case pid_NOISE_SCALING_TYPE:
    plug->noise_scaling_type = value;
    break;
  case pid_POST_FILTER_THRESHOLD:
    plug->post_filter_threshold = value;
    break;
  }
}

uint32_t param_count(const clap_plugin_t *plugin) { return PARAMS_COUNT; }

bool param_get_info(const clap_plugin_t *plugin, uint32_t param_index,
                    clap_param_info_t *param_info) {
  *param_info = (clap_param_info_t){};
  switch (param_index) {
  case 0:
    param_info->id = pid_AMOUNT;
    strncpy(param_info->name, "Reduction Amount", CLAP_NAME_SIZE);
    param_info->module[0] = 0;
    param_info->default_value = 10.0;
    param_info->min_value = 0.0;
    param_info->max_value = 40.0;
    param_info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    param_info->cookie = NULL;
    break;
  case 1:
    param_info->id = pid_OFFSET;
    strncpy(param_info->name, "Reduction Strength", CLAP_NAME_SIZE);
    param_info->module[0] = 0;
    param_info->default_value = 2.0;
    param_info->min_value = 0.0;
    param_info->max_value = 12.0;
    param_info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    param_info->cookie = NULL;
    break;
  case 2:
    param_info->id = pid_SMOOTHING;
    strncpy(param_info->name, "Smoothing", CLAP_NAME_SIZE);
    param_info->module[0] = 0;
    param_info->default_value = 0.0;
    param_info->min_value = 0.0;
    param_info->max_value = 100.0;
    param_info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    param_info->cookie = NULL;
    break;
  case 3:
    param_info->id = pid_WHITENING;
    strncpy(param_info->name, "Residual Whitening", CLAP_NAME_SIZE);
    param_info->module[0] = 0;
    param_info->default_value = 0.0;
    param_info->min_value = 0.0;
    param_info->max_value = 100.0;
    param_info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    param_info->cookie = NULL;
    break;
  case 4:
    param_info->id = pid_TRANSIENT_PROTECTION;
    strncpy(param_info->name, "Protect Transients", CLAP_NAME_SIZE);
    param_info->module[0] = 0;
    param_info->default_value = 0.0;
    param_info->min_value = 0.0;
    param_info->max_value = 1.0;
    param_info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
    param_info->cookie = NULL;
    break;
  case 5:
    param_info->id = pid_LEARN_NOISE;
    strncpy(param_info->name, "Learn Noise Profile", CLAP_NAME_SIZE);
    param_info->module[0] = 0;
    param_info->default_value = 0.0;
    param_info->min_value = 0.0;
    param_info->max_value = 3.0;
    param_info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
    param_info->cookie = NULL;
    break;
  case 6:
    param_info->id = pid_RESIDUAL_LISTEN;
    strncpy(param_info->name, "Residual Listen", CLAP_NAME_SIZE);
    param_info->module[0] = 0;
    param_info->default_value = 0.0;
    param_info->min_value = 0.0;
    param_info->max_value = 1.0;
    param_info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
    param_info->cookie = NULL;
    break;
  case 7:
    param_info->id = pid_RESET_PROFILE;
    strncpy(param_info->name, "Reset Noise Profile", CLAP_NAME_SIZE);
    param_info->module[0] = 0;
    param_info->default_value = 0.0;
    param_info->min_value = 0.0;
    param_info->max_value = 1.0;
    param_info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
    param_info->cookie = NULL;
    break;
  case 8:
    param_info->id = pid_ENABLE;
    strncpy(param_info->name, "Enable", CLAP_NAME_SIZE);
    param_info->module[0] = 0;
    param_info->default_value = 1.0;
    param_info->min_value = 0.0;
    param_info->max_value = 1.0;
    param_info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
    param_info->cookie = NULL;
    break;
  case 9:
    param_info->id = pid_NOISE_SCALING_TYPE;
    strncpy(param_info->name, "Noise Scaling Type", CLAP_NAME_SIZE);
    param_info->module[0] = 0;
    param_info->default_value = 0.0;
    param_info->min_value = 0.0;
    param_info->max_value = 2.0;
    param_info->flags = CLAP_PARAM_IS_AUTOMATABLE | CLAP_PARAM_IS_STEPPED;
    param_info->cookie = NULL;
    break;
  case 10:
    param_info->id = pid_POST_FILTER_THRESHOLD;
    strncpy(param_info->name, "Post Filter Threshold", CLAP_NAME_SIZE);
    param_info->module[0] = 0;
    param_info->default_value = 0.0;
    param_info->min_value = -10.0;
    param_info->max_value = 10.0;
    param_info->flags = CLAP_PARAM_IS_AUTOMATABLE;
    param_info->cookie = NULL;
    break;
  default:
    return false;
  }
  return true;
}

bool param_get_value(const clap_plugin_t *plugin, clap_id param_id,
                     double *value) {
  clap_noiserf *plug = plugin->plugin_data;

  switch (param_id) {
  case pid_AMOUNT:
    *value = plug->amount;
    return true;
  case pid_OFFSET:
    *value = plug->offset;
    return true;
  case pid_SMOOTHING:
    *value = plug->smoothing;
    return true;
  case pid_WHITENING:
    *value = plug->whitening;
    return true;
  case pid_TRANSIENT_PROTECTION:
    *value = plug->transient_protection ? 1.0 : 0.0;
    return true;
  case pid_LEARN_NOISE:
    *value = round(plug->learn_noise);
    return true;
  case pid_RESIDUAL_LISTEN:
    *value = plug->residual_listen ? 1.0 : 0.0;
    return true;
  case pid_RESET_PROFILE:
    *value = plug->reset_profile ? 1.0 : 0.0;
    return true;
  case pid_ENABLE:
    *value = plug->enable ? 1.0 : 0.0;
    return true;
  case pid_NOISE_SCALING_TYPE:
    *value = round(plug->noise_scaling_type);
    return true;
  case pid_POST_FILTER_THRESHOLD:
    *value = plug->post_filter_threshold;
    return true;
  }

  return false;
}

bool param_value_to_text(const clap_plugin_t *plugin, clap_id param_id,
                         double value, char *display, uint32_t size) {
  switch (param_id) {
  case pid_AMOUNT:
  case pid_OFFSET:
    snprintf(display, size, "%.1f dB", value);
    return true;
  case pid_SMOOTHING:
  case pid_WHITENING:
    snprintf(display, size, "%.1f %%", value);
    return true;
  case pid_TRANSIENT_PROTECTION:
    strncpy(display, value >= 0.5 ? "On" : "Off", size);
    return true;
  case pid_LEARN_NOISE: {
    const char *text = "";
    switch ((int)round(value)) {
    case 0:
      text = "Not Learning";
      break;
    case 1:
      text = "Learning: Average";
      break;
    case 2:
      text = "Learning: Median";
      break;
    case 3:
      text = "Learning: Maximum";
    }
    strncpy(display, text, size);
    return true;
  }
  case pid_RESIDUAL_LISTEN:
    strncpy(display, value >= 0.5 ? "Residual" : "Output", size);
    return true;
  case pid_RESET_PROFILE:
    strncpy(display, value >= 0.5 ? "Reset" : "Normal", size);
    return true;
  case pid_ENABLE:
    strncpy(display, value >= 0.5 ? "Enabled" : "Bypassed", size);
    return true;
  case pid_NOISE_SCALING_TYPE: {
    const char *text = "";
    switch ((int)round(value)) {
    case 0:
      text = "A-posteriori SNR";
      break;
    case 1:
      text = "Critical Bands";
      break;
    case 2:
      text = "Masking Thresholds";
      break;
    }
    strncpy(display, text, size);
    return true;
  }
  case pid_POST_FILTER_THRESHOLD:
    snprintf(display, size, "%.1f dB", value);
    return true;
  }
  return false;
}

bool text_to_value(const clap_plugin_t *plugin, clap_id param_id,
                   const char *display, double *value) {
  // Not implemented
  return false;
}

void flush(const clap_plugin_t *plugin, const clap_input_events_t *in,
           const clap_output_events_t *out) {
  clap_noiserf *plug = plugin->plugin_data;
  int s = in->size(in);
  int q;
  for (q = 0; q < s; ++q) {
    const clap_event_header_t *hdr = in->get(in, q);
    process_event(plug, hdr);
  }
}

static const clap_plugin_params_t s_params = {.count = param_count,
                                              .get_info = param_get_info,
                                              .get_value = param_get_value,
                                              .value_to_text =
                                                  param_value_to_text,
                                              .text_to_value = text_to_value,
                                              .flush = flush};

/////////////////
// clap_state //
/////////////////

static bool read_from_stream(const clap_istream_t *stream, void *buffer,
                             size_t size) {
  size_t bytes_read = 0;
  while (bytes_read != size) {
    const int64_t n =
        stream->read(stream, (uint8_t *)buffer + bytes_read, size - bytes_read);
    if (n == 0)
      return false; // unexpected end of stream
    if (n < 0)
      return false; // error
    bytes_read += (size_t)n;
  }
  return true;
}

static bool write_to_stream(const clap_ostream_t *stream, const void *buffer,
                            size_t size) {
  size_t bytes_written = 0;
  while (bytes_written != size) {
    const int64_t n = stream->write(
        stream, (const uint8_t *)buffer + bytes_written, size - bytes_written);
    if (n < 0)
      return false; // error
    bytes_written += (size_t)n;
  }
  return true;
}

enum CodingMode {
  coding_ENCODE,
  coding_DECODE,
};

// Coder as in encoder/decoder
struct state_coder {
  enum CodingMode mode;
  const clap_istream_t *istream; // when mode == coding_DECODE
  const clap_ostream_t *ostream; // when mode == coding_ENCODE
};

static bool code(const struct state_coder *coder, const void *buffer,
                 size_t size) {
  if (size == 0)
    return true;
  if (coder->mode == coding_ENCODE) {
    return write_to_stream(coder->ostream, buffer, size);
  } else {
    return read_from_stream(coder->istream, (void *)buffer, size);
  }
}

// Main-thread. Only set consume_pending to true if the audio thread isn't
// running.
static noise_profile_state *current_noise_profile(const clap_noiserf *plug,
                                                  bool consume_pending) {
  noise_profile_state *state;

  if (consume_pending && noise_profile_state_consume(
                             &plug->pending_noise_profile_change, &state)) {
    // Put it as the current
    noise_profile_state *current =
        writable_noise_profile_state(&plug->current_noise_profile);
    current->blocks_averaged = state->blocks_averaged;
    current->size = state->size;
    for (uint32_t channel = 0; channel < plug->channel_count; ++channel) {
      memcpy(current->channels[channel], state->channels[channel],
             sizeof(float) * state->size);
    }
    publish_noise_profile_state(&plug->current_noise_profile);
    return state;
  }

  const uint32_t pending_middle_state =
      plug->pending_noise_profile_change.middle_buffer_state;
  if (pending_middle_state & TRIPLE_BUFFER_DIRTY_BIT) {
    state = &plug->pending_noise_profile_change
                 .buffers[pending_middle_state & TRIPLE_BUFFER_MASK];
  } else {
    noise_profile_state_consume(&plug->current_noise_profile, &state);
  }

  return state;
}

static bool code_state(const clap_plugin_t *plugin,
                       const struct state_coder *coder) {
  clap_noiserf *plug = plugin->plugin_data;

  // We might need to use this in the future.
  uint32_t version = 1;
  if (!code(coder, &version, sizeof(version))) {
    return false;
  }

  uint32_t params_count = PARAMS_COUNT;
  if (!code(coder, &params_count, sizeof(params_count))) {
    return false;
  }

  if (coder->mode == coding_DECODE && params_count > PARAMS_COUNT) {
    set_all_params_to_default(plug);
  }

  for (uint32_t i = 0; i < params_count; ++i) {
    uint32_t param_id;
    if (coder->mode == coding_ENCODE) {
      clap_param_info_t param_info;
      const bool got_info = param_get_info(plugin, i, &param_info);
      assert(got_info);
      param_id = param_info.id;
    }
    if (!code(coder, &param_id, sizeof(param_id))) {
      return false;
    }

    double value = 0.0;
    if (coder->mode == coding_ENCODE) {
      const bool got_value = param_get_value(plugin, param_id, &value);
      assert(got_value);
    }
    if (!code(coder, &value, sizeof(value))) {
      return false;
    }

    if (coder->mode == coding_DECODE) {
      set_value(plug, param_id, value);
    }
  }

  noise_profile_state *state;
  if (coder->mode == coding_DECODE) {
    state = writable_noise_profile_state(&plug->pending_noise_profile_change);
  } else {
    // The audio thread might consume this buffer while we use it - making it
    // the front buffer - but it's safe due to the fact that neither us nor
    // the audio thread modify it.
    state = current_noise_profile(plug, false);
  }

  assert(state->size <= NOISE_PROFILE_MAX_SIZE);
  if (!code(coder, &state->blocks_averaged, sizeof(state->blocks_averaged))) {
    return false;
  }
  if (!code(coder, &state->size, sizeof(state->size))) {
    return false;
  }
  for (uint32_t channel = 0; channel < plug->channel_count; ++channel) {
    if (!code(coder, state->channels[channel], sizeof(float) * state->size)) {
      return false;
    }
  }

  if (coder->mode == coding_DECODE) {
    publish_noise_profile_state(&plug->pending_noise_profile_change);
  }

  return true;
}

bool state_save(const clap_plugin_t *plugin, const clap_ostream_t *stream) {
  clap_noiserf *plug = plugin->plugin_data;

  const struct state_coder coder = {
      .mode = coding_ENCODE,
      .ostream = stream,
  };

  return code_state(plugin, &coder);
}

bool state_load(const clap_plugin_t *plugin, const clap_istream_t *stream) {
  clap_noiserf *plug = plugin->plugin_data;

  const struct state_coder coder = {
      .mode = coding_DECODE,
      .istream = stream,
  };

  const bool result = code_state(plugin, &coder);

  // Notify host that parameter values might have changed
  const clap_host_t *clapHost = plug->host;
  const clap_host_params_t *p =
      (const clap_host_params_t *)(clapHost->get_extension(clapHost,
                                                           CLAP_EXT_PARAMS));
  if (p) {
    p->rescan(clapHost, CLAP_PARAM_RESCAN_VALUES);
    p->request_flush(clapHost);
  }

  return result;
}

static const clap_plugin_state_t s_state = {
    .save = state_save,
    .load = state_load,
};

/////////////////
// clap_plugin //
/////////////////

static void set_all_params_to_default(clap_noiserf *plug) {
  for (uint32_t i = 0; i < PARAMS_COUNT; ++i) {
    clap_param_info_t param_info;
    param_get_info(&plug->plugin, i, &param_info);
    set_value(plug, param_info.id, param_info.default_value);
  }
}

static bool init(const struct clap_plugin *plugin) {
  clap_noiserf *plug = plugin->plugin_data;

  // Fetch host's extensions here
  plug->hostLog = plug->host->get_extension(plug->host, CLAP_EXT_LOG);
  plug->hostThreadCheck =
      plug->host->get_extension(plug->host, CLAP_EXT_THREAD_CHECK);
  plug->hostLatency = plug->host->get_extension(plug->host, CLAP_EXT_LATENCY);
  plug->hostParams = plug->host->get_extension(plug->host, CLAP_EXT_PARAMS);

  init_noise_profile_buffers(&plug->pending_noise_profile_change);
  init_noise_profile_buffers(&plug->current_noise_profile);
  set_all_params_to_default(plug);

  return true;
}

static void destroy(const struct clap_plugin *plugin) {
  clap_noiserf *plug = plugin->plugin_data;

  free(plug);
}

static bool activate(const struct clap_plugin *plugin, double sample_rate,
                     uint32_t min_frames_count, uint32_t max_frames_count) {
  clap_noiserf *plug = plugin->plugin_data;

  if (sample_rate > 192000.0)
    return false; // IMPROVE: support higher sample rates

  plug->soft_bypass = signal_crossfade_initialize((uint32_t)sample_rate);
  if (!plug->soft_bypass) {
    return false;
  }

  const float frame_size_ms = 46;

  for (uint32_t channel = 0; channel < plug->channel_count; ++channel) {
    plug->lib_instance[channel] =
        specbleach_initialize((uint32_t)sample_rate, frame_size_ms);
    if (!plug->lib_instance[channel]) {
      return false;
    }
  }

  const uint32_t noise_profile_size =
      specbleach_get_noise_profile_size(plug->lib_instance[0]);

  assert(noise_profile_size <= NOISE_PROFILE_MAX_SIZE);

  // If we have a current noise profile, load it into the instance. This might
  // not be the first time we have been activated.
  noise_profile_state *state = current_noise_profile(plug, true);
  if (state->size != 0) {
    for (uint32_t channel = 0; channel < plug->channel_count; ++channel) {
      const bool loaded = specbleach_load_noise_profile(
          plug->lib_instance[channel], state->channels[channel], state->size,
          state->blocks_averaged);
      assert(loaded);
    }
  }

  return true;
}

static void deactivate(const struct clap_plugin *plugin) {
  clap_noiserf *plug = plugin->plugin_data;

  for (uint32_t channel = 0; channel < plug->channel_count; ++channel) {
    if (plug->lib_instance[channel]) {
      specbleach_free(plug->lib_instance[channel]);
    }
  }

  if (plug->soft_bypass) {
    signal_crossfade_free(plug->soft_bypass);
  }
}

static bool start_processing(const struct clap_plugin *plugin) { return true; }

static void stop_processing(const struct clap_plugin *plugin) {}

static void reset(const struct clap_plugin *plugin) {}

static void process_event(clap_noiserf *plug, const clap_event_header_t *hdr) {
  if (hdr->space_id == CLAP_CORE_EVENT_SPACE_ID) {
    switch (hdr->type) {
    case CLAP_EVENT_PARAM_VALUE: {
      const clap_event_param_value_t *ev =
          (const clap_event_param_value_t *)hdr;

      set_value(plug, ev->param_id, ev->value);
      break;
    }
    }
  }
}

static clap_process_status process(const struct clap_plugin *plugin,
                                   const clap_process_t *process) {
  clap_noiserf *plug = plugin->plugin_data;
  const uint32_t frame_count = process->frames_count;
  const uint32_t ev_count = process->in_events->size(process->in_events);
  uint32_t ev_index = 0;
  uint32_t next_ev_frame = ev_count > 0 ? 0 : frame_count;

  bool noise_profile_changed = false;

  // Consume pending noise profile changes
  {
    noise_profile_state *state;
    if (noise_profile_state_consume(&plug->pending_noise_profile_change,
                                    &state)) {
      noise_profile_changed = true;
      for (uint32_t channel = 0; channel < plug->channel_count; ++channel) {
        if (state->size == 0) {
          const bool reset =
              specbleach_reset_noise_profile(plug->lib_instance[channel]);
          assert(reset);
        } else {
          const bool loaded = specbleach_load_noise_profile(
              plug->lib_instance[channel], state->channels[channel],
              state->size, state->blocks_averaged);
          assert(loaded);
        }
      }
    }
  }

  for (uint32_t i = 0; i < frame_count;) {
    // handle every events that happens at the frame "i"
    while (ev_index < ev_count && next_ev_frame == i) {
      const clap_event_header_t *hdr =
          process->in_events->get(process->in_events, ev_index);
      if (hdr->time != i) {
        next_ev_frame = hdr->time;
        break;
      }

      process_event(plug, hdr);
      ++ev_index;

      if (ev_index == ev_count) {
        // we reached the end of the event list
        next_ev_frame = frame_count;
        break;
      }
    }

    // process audio until the next event
    const uint32_t block_size = next_ev_frame - i;

    // Update the parameters struct for the spectral bleach instances
    SpectralBleachParameters parameters = {
        .learn_noise = (int)plug->learn_noise,
        .residual_listen = plug->residual_listen,
        .reduction_amount = plug->amount,
        .smoothing_factor = plug->smoothing,
        .transient_protection = plug->transient_protection,
        .whitening_factor = plug->whitening,
        .noise_scaling_type = (int)plug->noise_scaling_type,
        .noise_rescale = plug->offset,
        .post_filter_threshold = plug->post_filter_threshold,
    };

    if (plug->learn_noise != 0)
      noise_profile_changed = true;

    for (uint32_t channel = 0; channel < plug->channel_count; ++channel) {
      specbleach_load_parameters(plug->lib_instance[channel], parameters);
    }

    // Handle reset noise profile if needed
    if (plug->reset_profile) {
      for (uint32_t channel = 0; channel < plug->channel_count; ++channel) {
        specbleach_reset_noise_profile(plug->lib_instance[channel]);
      }
      plug->reset_profile = false; // Reset the trigger
    }

    for (uint32_t channel = 0; channel < plug->channel_count; ++channel) {
      specbleach_process(plug->lib_instance[channel], block_size,
                         &process->audio_inputs[0].data32[channel][i],
                         &process->audio_outputs[0].data32[channel][i]);
      signal_crossfade_run(plug->soft_bypass, block_size,
                           &process->audio_inputs[0].data32[channel][i],
                           &process->audio_outputs[0].data32[channel][i],
                           plug->enable);
    }

    i = next_ev_frame;
  }

  // Store the noise profile
  if (specbleach_noise_profile_available(plug->lib_instance[0]) &&
      noise_profile_changed) {
    noise_profile_state *state =
        writable_noise_profile_state(&plug->current_noise_profile);
    state->size = specbleach_get_noise_profile_size(plug->lib_instance[0]);
    state->blocks_averaged =
        specbleach_get_noise_profile_blocks_averaged(plug->lib_instance[0]);
    assert(state->size <= NOISE_PROFILE_MAX_SIZE);
    for (uint32_t channel = 0; channel < plug->channel_count; ++channel) {
      memcpy(state->channels[channel],
             specbleach_get_noise_profile(plug->lib_instance[channel]),
             sizeof(float) * state->size);
    }
    publish_noise_profile_state(&plug->current_noise_profile);
  }

  return CLAP_PROCESS_CONTINUE;
}

static const void *get_extension(const struct clap_plugin *plugin,
                                 const char *id) {
  if (!strcmp(id, CLAP_EXT_LATENCY))
    return &s_latency;
  if (!strcmp(id, CLAP_EXT_AUDIO_PORTS))
    return &s_audio_ports;
  if (!strcmp(id, CLAP_EXT_PARAMS))
    return &s_params;
  if (!strcmp(id, CLAP_EXT_STATE))
    return &s_state;
  return NULL;
}

static void on_main_thread(const struct clap_plugin *plugin) {}

clap_plugin_t *create(const clap_host_t *host,
                      const clap_plugin_descriptor_t *desc,
                      uint32_t channel_count) {
  clap_noiserf *p = calloc(1, sizeof(*p));
  p->channel_count = channel_count;
  p->host = host;
  p->plugin.desc = desc;
  p->plugin.plugin_data = p;
  p->plugin.init = init;
  p->plugin.destroy = destroy;
  p->plugin.activate = activate;
  p->plugin.deactivate = deactivate;
  p->plugin.start_processing = start_processing;
  p->plugin.stop_processing = stop_processing;
  p->plugin.reset = reset;
  p->plugin.process = process;
  p->plugin.get_extension = get_extension;
  p->plugin.on_main_thread = on_main_thread;
  return &p->plugin;
}

clap_plugin_t *create_stereo(const clap_host_t *host) {
  return create(host, &s_desc_stereo, 2);
}

clap_plugin_t *create_mono(const clap_host_t *host) {
  return create(host, &s_desc_mono, 1);
}

/////////////////////////
// clap_plugin_factory //
/////////////////////////

static struct {
  const clap_plugin_descriptor_t *desc;
  clap_plugin_t *(*create)(const clap_host_t *host);
} s_plugins[] = {
    {
        .desc = &s_desc_stereo,
        .create = create_stereo,
    },
    {
        .desc = &s_desc_mono,
        .create = create_mono,
    },
};

static uint32_t
plugin_factory_get_plugin_count(const struct clap_plugin_factory *factory) {
  return sizeof(s_plugins) / sizeof(s_plugins[0]);
}

static const clap_plugin_descriptor_t *
plugin_factory_get_plugin_descriptor(const struct clap_plugin_factory *factory,
                                     uint32_t index) {
  return s_plugins[index].desc;
}

static const clap_plugin_t *
plugin_factory_create_plugin(const struct clap_plugin_factory *factory,
                             const clap_host_t *host, const char *plugin_id) {
  if (!clap_version_is_compatible(host->clap_version)) {
    return NULL;
  }

  const int N = sizeof(s_plugins) / sizeof(s_plugins[0]);
  for (int i = 0; i < N; ++i)
    if (!strcmp(plugin_id, s_plugins[i].desc->id))
      return s_plugins[i].create(host);

  return NULL;
}

const clap_plugin_factory_t s_plugin_factory = {
    .get_plugin_count = plugin_factory_get_plugin_count,
    .get_plugin_descriptor = plugin_factory_get_plugin_descriptor,
    .create_plugin = plugin_factory_create_plugin,
};
