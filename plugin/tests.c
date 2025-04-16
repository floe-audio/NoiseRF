#include "debug.h"
#include "utest.h"
#include <clap/clap.h>
#include <math.h>

UTEST_MAIN()

extern const clap_plugin_factory_t s_plugin_factory;

const void *host_get_extension(const struct clap_host *host,
                               const char *extension_id) {
  return NULL;
}
void host_request_restart(const struct clap_host *host) {}
void request_process(const struct clap_host *host) {}
void request_callback(const struct clap_host *host) {}

clap_host_t const test_host = {
    .clap_version = CLAP_VERSION,
    .host_data = NULL,
    .name = "Test Host",
    .vendor = "Test Vendor",
    .url = "",
    .version = "1",
    .get_extension = host_get_extension,
    .request_restart = host_request_restart,
    .request_process = request_process,
    .request_callback = request_callback,
};

struct plugin_test_fixture {
  const clap_plugin_t *plugin;
};

UTEST_F_SETUP(plugin_test_fixture) {
  const clap_plugin_factory_t *f = &s_plugin_factory;

  ASSERT_TRUE(f->get_plugin_count != NULL);
  ASSERT_TRUE(f->get_plugin_descriptor != NULL);
  ASSERT_TRUE(f->create_plugin != NULL);

  const uint32_t plugin_count = f->get_plugin_count(f);
  ASSERT_GT(plugin_count, 0);

  const clap_plugin_descriptor_t *desc = f->get_plugin_descriptor(f, 0);
  ASSERT_TRUE(desc != NULL);

  utest_fixture->plugin = f->create_plugin(f, &test_host, desc->id);
}

UTEST_F_TEARDOWN(plugin_test_fixture) {
  utest_fixture->plugin->destroy(utest_fixture->plugin);
}

// Just test the plugin being created and then destroyed, with no callbacks
// inbetween.
UTEST_F(plugin_test_fixture, close_instantly) {}

UTEST_F(plugin_test_fixture, init) {
  const clap_plugin_t *p = utest_fixture->plugin;

  ASSERT_TRUE(p->init(p));
}

#define TEST_PROCESS_BLOCK_SIZE 64

uint32_t host_process_in_event_size(const clap_input_events_t *events) {
  return 0;
}
const clap_event_header_t *
host_process_in_event_get(const clap_input_events_t *events, uint32_t index) {
  return NULL;
}
bool host_process_out_event_try_push(const clap_output_events_t *events,
                                     const clap_event_header_t *event) {
  return false;
}

bool approx(float a, float b, float epsilon) {
  const double diff = fabs(a - b);
  if (diff > epsilon || isnan(diff)) {
    return false;
  }
  return true;
}

UTEST_F(plugin_test_fixture, correct_latency) {
  const clap_plugin_t *p = utest_fixture->plugin;

  ASSERT_TRUE(p->init(p));

  const double test_sample_rates[] = {44100.0, 48000.0, 96000.0};
  const size_t num_sample_rates =
      sizeof(test_sample_rates) / sizeof(test_sample_rates[0]);

  for (size_t sample_rate_index = 0; sample_rate_index < num_sample_rates;
       ++sample_rate_index) {
    ASSERT_TRUE(p->activate(p, test_sample_rates[sample_rate_index],
                            TEST_PROCESS_BLOCK_SIZE, TEST_PROCESS_BLOCK_SIZE));

    const clap_plugin_latency_t *latency_extension =
        p->get_extension(p, CLAP_EXT_LATENCY);
    ASSERT_TRUE(latency_extension != NULL);

    const uint32_t latency = latency_extension->get(p);
    DEBUG_PRINT("Latency: %u\n", latency);

    float inputs[2][TEST_PROCESS_BLOCK_SIZE];
    float outputs[2][TEST_PROCESS_BLOCK_SIZE];
    memset(inputs, 0, sizeof(inputs));
    memset(outputs, 0, sizeof(outputs));
    float *input_channels[2] = {inputs[0], inputs[1]};
    float *output_channels[2] = {outputs[0], outputs[1]};

    clap_audio_buffer_t input_buffer = {};
    input_buffer.channel_count = 2;
    input_buffer.data32 = input_channels;

    clap_audio_buffer_t output_buffer = {};
    output_buffer.channel_count = 2;
    output_buffer.data32 = output_channels;

    clap_input_events_t const in_events = {
        .ctx = NULL,
        .size = host_process_in_event_size,
        .get = host_process_in_event_get,
    };

    clap_output_events_t const out_events = {
        .ctx = NULL,
        .try_push = host_process_out_event_try_push,
    };

    clap_process_t process = {};
    process.frames_count = TEST_PROCESS_BLOCK_SIZE;
    process.steady_time = -1;
    process.transport = NULL;
    process.audio_inputs = &input_buffer;
    process.audio_inputs_count = 1;
    process.audio_outputs = &output_buffer;
    process.audio_outputs_count = 1;
    process.in_events = &in_events;
    process.out_events = &out_events;

    uint32_t blocks_needed =
        (latency + TEST_PROCESS_BLOCK_SIZE - 1) / TEST_PROCESS_BLOCK_SIZE;
    // We process more blocks than is needed for the latency because we
    // are looking for problems, and any problem could occur beyond the
    // technically correct range - we want to capture that too.
    blocks_needed *= 2;

    for (uint32_t block = 0; block < blocks_needed; ++block) {
      // We send a single sample in the first block and then detect if the
      // latency is correct.
      if (block == 0) {
        inputs[0][0] = 1.0f;
        inputs[1][0] = 1.0f;
      } else {
        inputs[0][0] = 0.0f;
        inputs[1][0] = 0.0f;
      }

      const clap_process_status status = p->process(p, &process);
      ASSERT_NE(status, CLAP_PROCESS_ERROR);

      for (uint32_t frame = 0; frame < TEST_PROCESS_BLOCK_SIZE; ++frame) {
        const uint32_t overall_frame = block * TEST_PROCESS_BLOCK_SIZE + frame;

        // WARNING: the plugin is not behaving perfectly. It sends non-zero
        // samples when the input is all zeros. It's adding noise, albeit
        // incredibly low noise - values such as 0.00000000000124.

        for (uint32_t channel = 0; channel < 2; ++channel) {
          if (approx(outputs[channel][frame], 1.0f, 0.02f)) {
            EXPECT_EQ_MSG(overall_frame, latency,
                          "value 1.0 should be the latency frame");
          } else if (approx(outputs[channel][frame], 0.0f, 0.000001f)) {
            EXPECT_NE_MSG(overall_frame, latency,
                          "value 0.0 should not be the latency frame");
          } else {
            DEBUG_PRINT("Unexpected output value at frame %u: %.16f\n",
                        overall_frame, outputs[channel][frame]);
            EXPECT_TRUE_MSG(0, "Unexpected output value");
          }
        }
      }
    }

    p->deactivate(p);
  }
}
