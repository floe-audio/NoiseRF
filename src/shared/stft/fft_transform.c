/*
libspecbleach - A spectral processing library

Copyright 2022 Luciano Dato <lucianodato@gmail.com>

This library is free software; you can redistribute it and/or
modify it under the terms of the GNU Lesser General Public
License as published by the Free Software Foundation; either
version 2.1 of the License, or (at your option) any later version.

This library is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
Lesser General Public License for more details.

You should have received a copy of the GNU Lesser General Public
License along with this library; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301  USA
*/

#include "fft_transform.h"
#include "../configurations.h"
#include "../utils/general_utils.h"

#include "pffft.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void allocate_pffft(FftTransform *self);

struct FftTransform {
  PFFFT_Setup *setup;

  uint32_t fft_size;
  uint32_t frame_size;
  uint32_t zeropadding_amount;
  uint32_t copy_position;
  uint32_t padding_amount;
  float *input_fft_buffer;
  float *output_fft_buffer;
  float *work_buffer; // Work buffer for PFFFT
};

static uint32_t calculate_fft_size(FftTransform *self) {
  uint32_t next_power_of_two = get_next_power_two((int)self->frame_size);
  self->padding_amount = next_power_of_two - self->frame_size;
  return next_power_of_two < 32 ? 32 : next_power_of_two;
}

FftTransform *fft_transform_initialize(const uint32_t frame_size,
                                       const ZeroPaddingType padding_type,
                                       const uint32_t zeropadding_amount) {
  FftTransform *self = (FftTransform *)calloc(1U, sizeof(FftTransform));

  self->zeropadding_amount = zeropadding_amount;
  self->frame_size = frame_size;

  self->fft_size = calculate_fft_size(self);

  self->copy_position = (self->fft_size / 2U) - (self->frame_size / 2U);

  allocate_pffft(self);

  return self;
}

FftTransform *fft_transform_initialize_bins(const uint32_t fft_size) {
  FftTransform *self = (FftTransform *)calloc(1U, sizeof(FftTransform));

  self->fft_size = fft_size;
  self->frame_size = self->fft_size;

  allocate_pffft(self);

  return self;
}

static void allocate_pffft(FftTransform *self) {
  self->setup = pffft_new_setup(self->fft_size, PFFFT_REAL);

  assert(self->setup != NULL); // probably given an invalid fft size

  const size_t fft_size_bytes = self->fft_size * sizeof(float);

  self->input_fft_buffer = (float *)pffft_aligned_malloc(fft_size_bytes);
  self->output_fft_buffer = (float *)pffft_aligned_malloc(fft_size_bytes);

  memset(self->input_fft_buffer, 0, fft_size_bytes);
  memset(self->output_fft_buffer, 0, fft_size_bytes);

  self->work_buffer = (float *)pffft_aligned_malloc(fft_size_bytes);
}

void fft_transform_free(FftTransform *self) {
  if (!self) {
    return;
  }

  if (self->input_fft_buffer) {
    pffft_aligned_free(self->input_fft_buffer);
  }

  if (self->output_fft_buffer) {
    pffft_aligned_free(self->output_fft_buffer);
  }

  if (self->work_buffer) {
    pffft_aligned_free(self->work_buffer);
  }

  if (self->setup) {
    pffft_destroy_setup(self->setup);
  }

  free(self);
}

uint32_t get_fft_size(FftTransform *self) { return self->fft_size; }

uint32_t get_fft_real_spectrum_size(FftTransform *self) {
  return self->fft_size / 2U + 1U;
}

bool fft_load_input_samples(FftTransform *self, const float *input) {
  if (!self || !input) {
    return false;
  }

  // Clear input buffer first
  memset(self->input_fft_buffer, 0, self->fft_size * sizeof(float));

  // Copy centered values only
  for (uint32_t i = self->copy_position;
       i < (self->frame_size + self->copy_position); i++) {
    self->input_fft_buffer[i] = input[i - self->copy_position];
  }

  return true;
}

bool fft_get_output_samples(FftTransform *self, float *output) {
  if (!self || !output) {
    return false;
  }

  // Copy centered values only
  for (uint32_t i = self->copy_position;
       i < (self->frame_size + self->copy_position); i++) {
    output[i - self->copy_position] = self->input_fft_buffer[i];
  }

  return true;
}

bool compute_forward_fft(FftTransform *self) {
  if (!self || !self->setup) {
    return false;
  }

  pffft_transform_ordered(self->setup, self->input_fft_buffer,
                          self->output_fft_buffer, NULL, PFFFT_FORWARD);

  return true;
}

bool compute_backward_fft(FftTransform *self) {
  if (!self || !self->setup) {
    return false;
  }

  pffft_transform_ordered(self->setup, self->output_fft_buffer,
                          self->input_fft_buffer, NULL, PFFFT_BACKWARD);

  return true;
}

float *get_fft_input_buffer(FftTransform *self) {
  return self->input_fft_buffer;
}

float *get_fft_output_buffer(FftTransform *self) {
  return self->output_fft_buffer;
}
