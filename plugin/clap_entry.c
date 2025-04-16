#include <clap/clap.h>
#include <string.h>

extern const clap_plugin_factory_t s_plugin_factory;

static bool entry_init(const char *plugin_path) { return true; }

static void entry_deinit(void) {}

static const void *entry_get_factory(const char *factory_id) {
  if (!strcmp(factory_id, CLAP_PLUGIN_FACTORY_ID))
    return &s_plugin_factory;
  return NULL;
}

CLAP_EXPORT const clap_plugin_entry_t clap_entry = {
    .clap_version = CLAP_VERSION_INIT,
    .init = entry_init,
    .deinit = entry_deinit,
    .get_factory = entry_get_factory,
};
