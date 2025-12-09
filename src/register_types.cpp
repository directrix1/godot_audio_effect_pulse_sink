// register_types.cpp

#include <godot_cpp/godot.hpp>
#include <godot_cpp/core/class_db.hpp>

#include "audio_effect_pulse_sink.h"

using namespace godot;

void initialize_pulse_sink_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	ClassDB::register_class<AudioEffectPulseSink>();
	ClassDB::register_class<AudioEffectPulseSinkInstance>();
}

void uninitialize_pulse_sink_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}
	// Nothing to clean up
}

extern "C" {

GDExtensionBool GDE_EXPORT pulse_sink_library_init(
	GDExtensionInterfaceGetProcAddress p_get_proc_address,
	GDExtensionClassLibraryPtr p_library,
	GDExtensionInitialization *r_initialization
) {
	GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_pulse_sink_module);
	init_obj.register_terminator(uninitialize_pulse_sink_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}

} // extern "C"
