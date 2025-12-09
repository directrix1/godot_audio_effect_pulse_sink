// audio_effect_pulse_sink.h

#pragma once

#include <godot_cpp/classes/audio_effect.hpp>
#include <godot_cpp/classes/audio_effect_instance.hpp>
#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <atomic>
#include <thread>
#include <vector>

struct pa_simple; // Forward declaration, actual header in .cpp

namespace godot {

class AudioEffectPulseSinkInstance;

class AudioEffectPulseSink : public AudioEffect {
	GDCLASS(AudioEffectPulseSink, AudioEffect);

	String sink_name;
	bool mute_bus = false;

protected:
	static void _bind_methods();

public:
	AudioEffectPulseSink();
	~AudioEffectPulseSink() = default;

	void set_sink_name(const String &p_name);
	String get_sink_name() const;

	void set_mute_bus(bool p_mute);
	bool get_mute_bus() const;

	Ref<AudioEffectInstance> _instantiate() const override;
};

class AudioEffectPulseSinkInstance : public AudioEffectInstance {
	GDCLASS(AudioEffectPulseSinkInstance, AudioEffectInstance);

	friend class AudioEffectPulseSink;

	Ref<AudioEffectPulseSink> base;

	String cached_sink_name;
	pa_simple *pa = nullptr;
	int pa_error = 0;

	// Audio config
	float mix_rate = 48000.0f;

	// ================= Ring buffer =================
	// Single-producer (audio thread), single-consumer (worker thread) lock-free ring.
	static constexpr size_t RING_CAPACITY_POW2 = 16;           // 2^16 = 65536 samples
	static constexpr size_t RING_CAPACITY = (1u << RING_CAPACITY_POW2);
	std::vector<float> ring_data;
	std::atomic<size_t> ring_head { 0 }; // write index
	std::atomic<size_t> ring_tail { 0 }; // read index

	std::vector<float> worker_buffer; // temp buffer used by worker thread for contiguous writes

	// Worker thread
	std::thread worker_thread;
	std::atomic<bool> thread_running { false };

	void _ensure_ring();
	void _ring_push_sample(float s);
	size_t _ring_pop_many(float *p_dst, size_t p_max_samples);

	void _worker_loop();

	void _ensure_stream();
	void _close_stream();
	void _start_thread();
	void _stop_thread();

protected:
	static void _bind_methods();

public:
	AudioEffectPulseSinkInstance();
	~AudioEffectPulseSinkInstance() override;

	void set_base(const Ref<AudioEffectPulseSink> &p_base);

	virtual void _process(const void *p_src_buffer,
	                      AudioFrame *p_dst_buffer,
	                      int32_t p_frame_count) override;

	virtual bool _process_silence() const override {
		// We always want to tap the bus, even on silence/mute.
		return true;
	}
};

} // namespace godot
