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

// =================== Ring buffer helpers (frames) ===================

void AudioEffectPulseSinkInstance::_ensure_ring() {
	if (ring_data.size() != RING_CAPACITY_FRAMES) {
		ring_data.resize(RING_CAPACITY_FRAMES);
	}
	if (worker_frame_buffer.size() != RING_CAPACITY_FRAMES) {
		worker_frame_buffer.resize(RING_CAPACITY_FRAMES);
	}
	if (worker_interleaved_buf.size() != RING_CAPACITY_FRAMES * 2) {
		worker_interleaved_buf.resize(RING_CAPACITY_FRAMES * 2);
	}

	ring_head.store(0, std::memory_order_relaxed);
	ring_tail.store(0, std::memory_order_relaxed);
}

void AudioEffectPulseSinkInstance::_ring_push_frames(const AudioFrame *p_src, size_t p_frame_count) {
	if (p_frame_count == 0) {
		return;
	}

	const size_t capacity = RING_CAPACITY_FRAMES;

	// Single producer: audio thread only.
	size_t head = ring_head.load(std::memory_order_relaxed);
	size_t tail = ring_tail.load(std::memory_order_acquire);

	// Compute free space in ring (one slot kept empty to distinguish full vs empty).
	size_t used;
	if (head >= tail) {
		used = head - tail;
	} else {
		used = (capacity - tail) + head;
	}
	size_t free = capacity - used - 1;

	if (free == 0) {
		// No space at all, drop whole block.
		return;
	}

	// We can only push up to `free` frames.
	size_t to_write = (p_frame_count > free) ? free : p_frame_count;
	if (to_write == 0) {
		return;
	}

	// First contiguous segment before wrap.
	size_t first_chunk = to_write;
	size_t space_till_end = capacity - head;
	if (first_chunk > space_till_end) {
		first_chunk = space_till_end;
	}

	// Copy first chunk.
	std::memcpy(&ring_data[head],
	            p_src,
	            first_chunk * sizeof(AudioFrame));

	// Copy second chunk if we wrapped around.
	size_t remaining = to_write - first_chunk;
	if (remaining > 0) {
		std::memcpy(&ring_data[0],
		            p_src + first_chunk,
		            remaining * sizeof(AudioFrame));
	}

	// Publish new head once.
	size_t new_head = (head + to_write) & (capacity - 1);
	ring_head.store(new_head, std::memory_order_release);
}

size_t AudioEffectPulseSinkInstance::_ring_pop_many(AudioFrame *p_dst, size_t p_max_frames) {
	if (!p_dst || p_max_frames == 0) {
		return 0;
	}

	// Single consumer thread.
	size_t head = ring_head.load(std::memory_order_acquire);
	size_t tail = ring_tail.load(std::memory_order_relaxed);

	if (head == tail) {
		return 0; // empty
	}

	const size_t capacity = RING_CAPACITY_FRAMES;

	// Calculate how many frames are available.
	size_t available = (head >= tail)
		? (head - tail)
		: (capacity - tail + head);

	if (available == 0) {
		return 0;
	}

	size_t to_read = (available > p_max_frames) ? p_max_frames : available;

	// First contiguous segment until end of ring.
	size_t first_chunk = to_read;
	size_t space_till_end = capacity - tail;
	if (first_chunk > space_till_end) {
		first_chunk = space_till_end;
	}

	// Copy the first contiguous block.
	std::memcpy(p_dst,
	            &ring_data[tail],
	            first_chunk * sizeof(AudioFrame));

	// Copy the second block (wrapped) if needed.
	size_t remaining = to_read - first_chunk;
	if (remaining > 0) {
		std::memcpy(p_dst + first_chunk,
		            &ring_data[0],
		            remaining * sizeof(AudioFrame));
	}

	// Publish new consumer index.
	ring_tail.store((tail + to_read) & (capacity - 1), std::memory_order_release);

	return to_read;
}

// =================== Worker thread ===================

void AudioEffectPulseSinkInstance::_worker_loop() {
    using namespace std::chrono_literals;

    // Ensure AudioFrame is exactly two floats stored contiguously.
    static_assert(sizeof(AudioFrame) == sizeof(float) * 2,
        "AudioFrame must be exactly two floats for direct streaming.");

    while (thread_running.load(std::memory_order_acquire)) {
        // Pop as many frames as available into worker_frame_buffer.
        size_t frames = _ring_pop_many(worker_frame_buffer.data(),
                                       worker_frame_buffer.size());

        if (frames == 0) {
            std::this_thread::sleep_for(1ms);
            continue;
        }

        if (!pa) {
            // Stream not ready—drop the frames.
            continue;
        }

        // AudioFrame* → float* (interleaved L,R,L,R,...)
        const float *samples = reinterpret_cast<const float *>(worker_frame_buffer.data());

        // Number of bytes to send to PulseAudio.
        const size_t bytes = frames * sizeof(AudioFrame);

        // Write directly from worker_frame_buffer.
        if (pa_simple_write(pa, samples, bytes, &pa_error) < 0) {
            // Can't call Godot logging here; just stop.
            thread_running.store(false, std::memory_order_release);
            break;
        }
    }

    // Drain handled by main thread when closing PA stream.
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
	if (thread_running.load(std::memory_order_acquire)) {
		return;
	}
	thread_running.store(true, std::memory_order_release);
	worker_thread = std::thread(&AudioEffectPulseSinkInstance::_worker_loop, this);
}

void AudioEffectPulseSinkInstance::_close_stream() {
	if (pa) {
		// Worker is stopped before we call this.
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

	// Reset ring buffer to avoid sending stale frames to a new sink.
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

	// Ensure PA stream is ready (or re-created if sink changed).
	_ensure_stream();

	const bool mute = base.is_valid() ? base->get_mute_bus() : false;
	const size_t byte_count = static_cast<size_t>(p_frame_count) * sizeof(AudioFrame);

	// Bus output: fast path with memset/memcpy.
	if (mute) {
		std::memset(p_dst_buffer, 0, byte_count);
	} else {
		std::memcpy(p_dst_buffer, src, byte_count);
	}

	// Mirror into ring buffer (bulk push of frames).
	_ring_push_frames(src, static_cast<size_t>(p_frame_count));
}
