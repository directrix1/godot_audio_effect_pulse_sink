// audio_effect_pulse_sink.cpp

#include "audio_effect_pulse_sink.h"

#include <godot_cpp/classes/audio_server.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <pulse/simple.h>
#include <pulse/error.h>

#include <chrono>

using namespace godot;

// =================== AudioEffectPulseSink ===================

AudioEffectPulseSink::AudioEffectPulseSink() {
	sink_name = String(); // empty by default, uses PA default sink if not set
}

void AudioEffectPulseSink::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_sink_name", "name"), &AudioEffectPulseSink::set_sink_name);
	ClassDB::bind_method(D_METHOD("get_sink_name"), &AudioEffectPulseSink::get_sink_name);

	ClassDB::bind_method(D_METHOD("set_mute_bus", "mute"), &AudioEffectPulseSink::set_mute_bus);
	ClassDB::bind_method(D_METHOD("get_mute_bus"), &AudioEffectPulseSink::get_mute_bus);

	ADD_PROPERTY(PropertyInfo(Variant::STRING, "sink_name"), "set_sink_name", "get_sink_name");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "mute_bus"), "set_mute_bus", "get_mute_bus");
}

void AudioEffectPulseSink::set_sink_name(const String &p_name) {
	sink_name = p_name;
}

String AudioEffectPulseSink::get_sink_name() const {
	return sink_name;
}

void AudioEffectPulseSink::set_mute_bus(bool p_mute) {
	mute_bus = p_mute;
}

bool AudioEffectPulseSink::get_mute_bus() const {
	return mute_bus;
}

Ref<AudioEffectInstance> AudioEffectPulseSink::_instantiate() const {
	Ref<AudioEffectPulseSinkInstance> inst;
	inst.instantiate();
	inst->set_base(Ref<AudioEffectPulseSink>(const_cast<AudioEffectPulseSink *>(this)));
	return inst;
}

// =================== AudioEffectPulseSinkInstance ===================

AudioEffectPulseSinkInstance::AudioEffectPulseSinkInstance() {
	if (AudioServer::get_singleton()) {
		mix_rate = AudioServer::get_singleton()->get_mix_rate();
	}
	_ensure_ring();
}

AudioEffectPulseSinkInstance::~AudioEffectPulseSinkInstance() {
	_stop_thread();
	_close_stream();
}

void AudioEffectPulseSinkInstance::_bind_methods() {
	// No script API for the instance itself
}

void AudioEffectPulseSinkInstance::set_base(const Ref<AudioEffectPulseSink> &p_base) {
	base = p_base;
	cached_sink_name = String(); // force re-open on first process
}

// =================== Ring buffer helpers ===================

void AudioEffectPulseSinkInstance::_ensure_ring() {
	if (ring_data.size() != RING_CAPACITY) {
		ring_data.resize(RING_CAPACITY);
	}
	if (worker_buffer.size() != RING_CAPACITY) {
		worker_buffer.resize(RING_CAPACITY);
	}
	ring_head.store(0, std::memory_order_relaxed);
	ring_tail.store(0, std::memory_order_relaxed);
}

void AudioEffectPulseSinkInstance::_ring_push_sample(float s) {
	// Single producer: audio thread only.
	size_t head = ring_head.load(std::memory_order_relaxed);
	size_t tail = ring_tail.load(std::memory_order_acquire);

	size_t next_head = (head + 1) & (RING_CAPACITY - 1);
	if (next_head == tail) {
		// Buffer full, drop sample (avoid blocking audio thread).
		return;
	}

	ring_data[head] = s;
	ring_head.store(next_head, std::memory_order_release);
}

size_t AudioEffectPulseSinkInstance::_ring_pop_many(float *p_dst, size_t p_max_samples) {
	// Single consumer: worker thread only.
	size_t head = ring_head.load(std::memory_order_acquire);
	size_t tail = ring_tail.load(std::memory_order_relaxed);

	if (head == tail) {
		return 0; // empty
	}

	size_t count = 0;
	while (tail != head && count < p_max_samples) {
		p_dst[count++] = ring_data[tail];
		tail = (tail + 1) & (RING_CAPACITY - 1);
	}

	ring_tail.store(tail, std::memory_order_release);
	return count;
}

// =================== Worker thread ===================

void AudioEffectPulseSinkInstance::_worker_loop() {
	using namespace std::chrono_literals;

	while (thread_running.load(std::memory_order_acquire)) {
		// Read as many samples as we currently have, up to worker_buffer size.
		size_t available = _ring_pop_many(worker_buffer.data(), worker_buffer.size());

		if (available == 0) {
			std::this_thread::sleep_for(1ms);
			continue;
		}

		// pa is only created / destroyed with the worker stopped, so safe to read here.
		if (!pa) {
			// No active stream; drop data.
			continue;
		}

		const size_t bytes = available * sizeof(float);
		if (pa_simple_write(pa, worker_buffer.data(), bytes, &pa_error) < 0) {
			// Can't use Godot logging from non-main threads safely; just stop on error.
			thread_running.store(false, std::memory_order_release);
			break;
		}
	}

	// Optional: final drain on exit (done in audio thread when closing stream).
}

// =================== PulseAudio stream lifecycle ===================

void AudioEffectPulseSinkInstance::_stop_thread() {
	if (thread_running.exchange(false, std::memory_order_acq_rel)) {
		if (worker_thread.joinable()) {
			worker_thread.join();
		}
	}
}

void AudioEffectPulseSinkInstance::_start_thread() {
	// Only start if not already running.
	if (thread_running.load(std::memory_order_acquire)) {
		return;
	}
	thread_running.store(true, std::memory_order_release);
	worker_thread = std::thread(&AudioEffectPulseSinkInstance::_worker_loop, this);
}

void AudioEffectPulseSinkInstance::_close_stream() {
	if (pa) {
		// We're on the audio/main thread here; worker is already stopped.
		pa_simple_drain(pa, &pa_error);
		pa_simple_free(pa);
		pa = nullptr;
	}
}

void AudioEffectPulseSinkInstance::_ensure_stream() {
	if (!base.is_valid()) {
		return;
	}

	String current_sink = base->get_sink_name();
	if (current_sink == cached_sink_name && pa != nullptr) {
		// No change, already open.
		return;
	}

	// Sink changed or no stream yet: stop worker, recreate stream.
	_stop_thread();
	_close_stream();

	cached_sink_name = current_sink;

	pa_sample_spec ss;
	ss.format = PA_SAMPLE_FLOAT32LE;
	ss.rate = (uint32_t)mix_rate;
	ss.channels = 2; // Godot AudioFrame is stereo

	const char *server = nullptr; // default server

	CharString sink_cs;
	const char *sink_cstr = nullptr;
	if (!current_sink.is_empty()) {
		sink_cs = current_sink.utf8();
		sink_cstr = sink_cs.get_data(); // non-default sink name
	}

	CharString app_name_cs = String("GodotPulseSink").utf8();
	const char *app_name = app_name_cs.get_data();

	CharString stream_name_cs = String("Godot bus tap").utf8();
	const char *stream_name = stream_name_cs.get_data();

	pa = pa_simple_new(server,
	                   app_name,
	                   PA_STREAM_PLAYBACK,
	                   sink_cstr,
	                   stream_name,
	                   &ss,
	                   nullptr,
	                   nullptr,
	                   &pa_error);

	if (!pa) {
		UtilityFunctions::push_warning(
			"AudioEffectPulseSink: Failed to open PulseAudio stream (sink: ",
			current_sink,
			") error: ",
			String::num_int64(pa_error)
		);
		return;
	}

	UtilityFunctions::print("AudioEffectPulseSink: Connected to PulseAudio sink: ", current_sink);

	// Reset ring buffer to avoid sending stale samples to a new sink.
	_ensure_ring();

	// Start worker thread that drains ring buffer into pa_simple_write().
	_start_thread();
}

// =================== Audio processing ===================

void AudioEffectPulseSinkInstance::_process(const void *p_src_buffer,
                                            AudioFrame *p_dst_buffer,
                                            int32_t p_frame_count) {
	if (!p_src_buffer || p_frame_count <= 0) {
		return;
	}

	const AudioFrame *src = static_cast<const AudioFrame *>(p_src_buffer);

	// First, ensure PA stream is ready (or re-created if sink changed).
	_ensure_stream();

	bool mute = base.is_valid() ? base->get_mute_bus() : false;

	// Bus output:
	if (mute) {
		for (int32_t i = 0; i < p_frame_count; i++) {
			p_dst_buffer[i] = AudioFrame(0.0f, 0.0f);
		}
	} else {
		for (int32_t i = 0; i < p_frame_count; i++) {
			p_dst_buffer[i] = src[i]; // pass-through
		}
	}

	// Mirror into ring buffer as interleaved float32 [L, R, L, R, ...].
	// This is non-blocking; overflow will just drop samples.
	for (int32_t i = 0; i < p_frame_count; i++) {
		_ring_push_sample(src[i].left);
		_ring_push_sample(src[i].right);
	}
}
