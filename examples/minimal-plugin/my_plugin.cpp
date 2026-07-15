// Minimal Speculor plugin — a frame-in / frame-out filter with one float
// parameter. Build it against an extracted SDK bundle; see README.md.
#include <speculor/plugin_helpers.h>

struct MyPluginState {
    spc::HostServices host;
    float gain = 1.0f;
};

SPC_PLUGIN(MyPluginState, host)

SPC_PLUGIN_DESCRIPTOR(
    spc::DescriptorBuilder("my_plugin", "My Plugin", "Filters")
        .author("You").version("0.1.0")
        .description("A minimal example plugin")
        .float_param("gain", "Gain", 0.0f, 10.0f, 1.0f, 0.1f)
        .input("in", "Input", SPC_DATA_FRAME)
        .output("out", "Output", SPC_DATA_FRAME)
)

SPC_PLUGIN_AUTO_PARAMS(MyPluginState,
    SPC_BIND_FLOAT(MyPluginState, "gain", gain)
)

static int process(SpcPluginInstance* inst, const SpcData* inputs,
                   uint32_t input_count, SpcData* outputs,
                   uint32_t output_count) {
    // Read inputs, write outputs. A real filter would scale pixels by `gain`.
    (void)inst; (void)inputs; (void)input_count;
    (void)outputs; (void)output_count;
    return 0;
}

SPC_PLUGIN_VTABLE(
    .get_descriptor    = get_descriptor,
    .create_instance   = create_instance,
    .destroy_instance  = destroy_instance,
    .set_parameter     = set_parameter,
    .get_parameter     = get_parameter,
    .process           = process,
    .set_host_services = set_host_services
)
