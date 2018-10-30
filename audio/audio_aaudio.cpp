/* Copyright (c) 2017-2018 Hans-Kristian Arntzen
 *
 * Permission is hereby granted, free of charge, to any person obtaining
 * a copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, sublicense, and/or sell copies of the Software, and to
 * permit persons to whom the Software is furnished to do so, subject to
 * the following conditions:
 *
 * The above copyright notice and this permission notice shall be
 * included in all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,
 * EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.
 * IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY
 * CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,
 * TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE
 * SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "audio_aaudio.hpp"
#include "dsp/dsp.hpp"
#include "util.hpp"
#include <stdint.h>
#include <aaudio/AAudio.h>
#include <time.h>
#include <cmath>
#include <vector>
#include <algorithm>

namespace Granite
{
namespace Audio
{
static aaudio_data_callback_result_t aaudio_callback(AAudioStream *stream, void *userData, void *audioData, int32_t numFrames);

struct AAudioBackend : Backend
{
	AAudioBackend(BackendCallback &callback)
			: Backend(callback)
	{
	}

	~AAudioBackend();
	bool init(float target_sample_rate, unsigned channels);

	const char *get_backend_name() override
	{
		return "AAudio";
	}

	float get_sample_rate() override
	{
		return sample_rate;
	}

	unsigned get_num_channels() override
	{
		return num_channels;
	}

	bool start() override;
	bool stop() override;

	void thread_callback(void *data, int32_t num_frames) noexcept;

	AAudioStream *stream = nullptr;

	std::vector<float> mix_buffers[Backend::MaxAudioChannels];
	float *mix_buffers_ptr[Backend::MaxAudioChannels] = {};

	float sample_rate = 0.0f;
	double inv_sample_rate = 0.0;
	unsigned num_channels = 0;
	int64_t frame_count = 0;
	int32_t frames_per_callback = 0;
	aaudio_format_t format;
	bool is_active = false;

	double last_latency = 0.0;
};

bool AAudioBackend::init(float, unsigned channels)
{
	aaudio_result_t res;
	AAudioStreamBuilder *builder = nullptr;
	if ((res = AAudio_createStreamBuilder(&builder)) != AAUDIO_OK)
		return false;

	AAudioStreamBuilder_setChannelCount(builder, channels);
	//AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
	AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
	AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
	AAudioStreamBuilder_setDataCallback(builder, aaudio_callback, this);

	if ((res = AAudioStreamBuilder_openStream(builder, &stream)) != AAUDIO_OK) {
		AAudioStreamBuilder_delete(builder);
		return false;
	}

	sample_rate = float(AAudioStream_getSampleRate(stream));
	inv_sample_rate = 1.0 / sample_rate;
	num_channels = AAudioStream_getChannelCount(stream);
	format = AAudioStream_getFormat(stream);

	int32_t burst_frames = AAudioStream_getFramesPerBurst(stream);
	int32_t max_frames = AAudioStream_getBufferCapacityInFrames(stream);

	// Target 20 ms latency.
	int32_t target_blocks = int(std::ceil(20.0f * sample_rate / (1000.0f * burst_frames)));
	target_blocks = std::max(target_blocks, 2);

	if (AAudioStream_setBufferSizeInFrames(stream, std::min(max_frames, target_blocks * burst_frames)) < 0)
		return false;

	frames_per_callback = AAudioStream_getFramesPerDataCallback(stream);
	if (frames_per_callback == AAUDIO_UNSPECIFIED)
		frames_per_callback = burst_frames;

	for (unsigned c = 0; c < num_channels; c++)
	{
		mix_buffers[c].resize(frames_per_callback);
		mix_buffers_ptr[c] = mix_buffers[c].data();
	}

	callback.set_backend_parameters(sample_rate, num_channels, size_t(frames_per_callback));

	// Set initial latency estimate.
	last_latency = double(AAudioStream_getBufferSizeInFrames(stream)) * inv_sample_rate;
	callback.set_latency_usec(uint32_t(last_latency * 1e6));

	return true;
}

void AAudioBackend::thread_callback(void *data, int32_t numFrames) noexcept
{
	// Update measured latency.
	int64_t frame_position, time_ns;
	if (AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &frame_position, &time_ns) == AAUDIO_OK)
	{
		timespec ts;
		if (clock_gettime(CLOCK_MONOTONIC, &ts) == 0)
		{
			int64_t current_ns = ts.tv_sec * 1000000000 + ts.tv_nsec;

			// Extrapolate play counter based on timestamp.
			double playing_time = double(frame_position) * inv_sample_rate;
			playing_time += 1e-9 * double(current_ns - time_ns);
			double pushed_time = double(frame_count) * inv_sample_rate;
			double latency = pushed_time - playing_time;
			if (latency < 0.0)
				latency = 0.0;

			// Interpolate latency over time for a smoother result.
			last_latency = 0.95 * last_latency + 0.05 * latency;
			callback.set_latency_usec(uint32_t(last_latency * 1e6));

			LOGI("Measured latency: %.3f ms\n", last_latency * 1000.0);
		}
	}

	frame_count += numFrames;

	union
	{
		int16_t *i16;
		float *f32;
		void *data;
	} u;
	u.data = data;

	while (numFrames)
	{
		auto to_render = std::min(numFrames, frames_per_callback);

		callback.mix_samples(mix_buffers_ptr, size_t(to_render));

		if (format == AAUDIO_FORMAT_PCM_FLOAT && num_channels == 2)
		{
			DSP::interleave_stereo_f32(u.f32, mix_buffers_ptr[0], mix_buffers_ptr[1], size_t(to_render));
			u.f32 += to_render * 2;
		}
		else if (format == AAUDIO_FORMAT_PCM_FLOAT)
		{
			for (int f = 0; f < to_render; f++)
				for (unsigned c = 0; c < num_channels; c++)
					*u.f32++ = mix_buffers[c][f];
		}
		else if (format == AAUDIO_FORMAT_PCM_I16 && num_channels == 2)
		{
			DSP::interleave_stereo_f32_i16(u.i16, mix_buffers_ptr[0], mix_buffers_ptr[1], size_t(to_render));
			u.i16 += to_render * 2;
		}
		else
		{
			for (int f = 0; f < to_render; f++)
				for (unsigned c = 0; c < num_channels; c++)
					*u.i16++ = DSP::f32_to_i16(mix_buffers[c][f]);
		}

		numFrames -= to_render;
	}
}

static aaudio_data_callback_result_t aaudio_callback(AAudioStream *, void *userData, void *audioData, int32_t numFrames)
{
	auto *backend = static_cast<AAudioBackend *>(userData);
	backend->thread_callback(audioData, numFrames);
	return AAUDIO_CALLBACK_RESULT_CONTINUE;
}

bool AAudioBackend::start()
{
	if (is_active)
		return false;

	callback.on_backend_start();
	frame_count = 0;

	aaudio_result_t res;
	if ((res = AAudioStream_requestStart(stream)) != AAUDIO_OK)
		return false;

#if 0
	aaudio_stream_state_t current_state = AAudioStream_getState(stream);
	aaudio_stream_state_t input_state = current_state;
	while (res == AAUDIO_OK && current_state != AAUDIO_STREAM_STATE_STARTED)
	{
		res = AAudioStream_waitForStateChange(stream, input_state, &current_state, 1000000000);
		input_state = current_state;
	}

	if (input_state != AAUDIO_STREAM_STATE_STARTED)
		return false;
#endif

	is_active = true;
	return true;
}

bool AAudioBackend::stop()
{
	if (!is_active)
		return false;

	aaudio_result_t res;
	if ((res = AAudioStream_requestStop(stream)) != AAUDIO_OK)
		return false;

#if 0
	aaudio_stream_state_t current_state = AAudioStream_getState(stream);
	aaudio_stream_state_t input_state = current_state;
	while (res == AAUDIO_OK && current_state != AAUDIO_STREAM_STATE_STOPPED)
	{
		res = AAudioStream_waitForStateChange(stream, input_state, &current_state, 10000000);
		input_state = current_state;
	}

	if (input_state != AAUDIO_STREAM_STATE_STOPPED)
		return false;
#endif

	callback.on_backend_stop();
	is_active = false;
	return true;
}

AAudioBackend::~AAudioBackend()
{
	stop();
	if (stream)
		AAudioStream_close(stream);
}

Backend *create_aaudio_backend(BackendCallback &callback, float sample_rate, unsigned channels)
{
	auto *aa = new AAudioBackend(callback);
	if (!aa->init(sample_rate, channels))
	{
		delete aa;
		return nullptr;
	}
	return aa;
}
}
}