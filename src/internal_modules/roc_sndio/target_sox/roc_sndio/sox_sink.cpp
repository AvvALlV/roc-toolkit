/*
 * Copyright (c) 2015 Roc Streaming authors
 *
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at http://mozilla.org/MPL/2.0/.
 */

#include "roc_sndio/sox_sink.h"
#include "roc_core/log.h"
#include "roc_core/panic.h"
#include "roc_sndio/backend_map.h"

namespace roc {
namespace sndio {

SoxSink::SoxSink(core::IArena& arena, const Config& config, DriverType type)
    : output_(NULL)
    , buffer_(arena)
    , buffer_size_(0)
    , is_file_(false)
    , valid_(false) {
    BackendMap::instance();

    if (config.latency != 0) {
        roc_log(LogError, "sox sink: setting io latency not supported by sox backend");
        return;
    }

    sample_spec_ = config.sample_spec;

    if (type == DriverType_File) {
        sample_spec_.use_defaults(audio::Sample_RawFormat, audio::ChanLayout_Surround,
                                  audio::ChanOrder_Smpte, audio::ChanMask_Surround_Stereo,
                                  44100);
    } else {
        sample_spec_.use_defaults(audio::Sample_RawFormat, audio::ChanLayout_Surround,
                                  audio::ChanOrder_Smpte, audio::ChanMask_Surround_Stereo,
                                  0);
    }

    if (!sample_spec_.is_raw()) {
        roc_log(LogError, "sox sink: sample format can be only \"-\" or \"%s\"",
                audio::pcm_format_to_str(audio::Sample_RawFormat));
        return;
    }

    frame_length_ = config.frame_length;

    if (frame_length_ == 0) {
        roc_log(LogError, "sox sink: frame length is zero");
        return;
    }

    memset(&out_signal_, 0, sizeof(out_signal_));
    out_signal_.rate = (sox_rate_t)sample_spec_.sample_rate();
    out_signal_.channels = (unsigned)sample_spec_.num_channels();
    out_signal_.precision = SOX_SAMPLE_PRECISION;

    valid_ = true;
}

SoxSink::~SoxSink() {
    close_();
}

bool SoxSink::is_valid() const {
    return valid_;
}

bool SoxSink::open(const char* driver, const char* path) {
    roc_panic_if(!valid_);

    roc_log(LogDebug, "sox sink: opening: driver=%s path=%s", driver, path);

    if (buffer_.size() != 0 || output_) {
        roc_panic("sox sink: can't call open() more than once");
    }

    if (!open_(driver, path)) {
        return false;
    }

    if (!setup_buffer_()) {
        return false;
    }

    return true;
}

ISink* SoxSink::to_sink() {
    return this;
}

ISource* SoxSink::to_source() {
    return NULL;
}

DeviceType SoxSink::type() const {
    return DeviceType_Sink;
}

DeviceState SoxSink::state() const {
    return DeviceState_Active;
}

void SoxSink::pause() {
    // no-op
}

bool SoxSink::resume() {
    return true;
}

bool SoxSink::restart() {
    return true;
}

audio::SampleSpec SoxSink::sample_spec() const {
    roc_panic_if(!valid_);

    if (!output_) {
        roc_panic("sox sink: not opened");
    }

    return sample_spec_;
}

core::nanoseconds_t SoxSink::latency() const {
    roc_panic_if(!valid_);

    if (!output_) {
        roc_panic("sox sink: not opened");
    }

    return 0;
}

bool SoxSink::has_latency() const {
    roc_panic_if(!valid_);

    if (!output_) {
        roc_panic("sox sink: not opened");
    }

    return false;
}

bool SoxSink::has_clock() const {
    roc_panic_if(!valid_);

    if (!output_) {
        roc_panic("sox sink: not opened");
    }

    return !is_file_;
}

status::StatusCode SoxSink::write(audio::Frame& frame) {
    roc_panic_if(!valid_);

    const audio::sample_t* frame_data = frame.raw_samples();
    size_t frame_size = frame.num_raw_samples();

    sox_sample_t* buffer_data = buffer_.data();
    size_t buffer_pos = 0;

    SOX_SAMPLE_LOCALS;

    size_t clips = 0;

    while (frame_size > 0) {
        for (; buffer_pos < buffer_size_ && frame_size > 0; buffer_pos++) {
            buffer_data[buffer_pos] = SOX_FLOAT_32BIT_TO_SAMPLE(*frame_data, clips);
            frame_data++;
            frame_size--;
        }

        if (buffer_pos == buffer_size_) {
            write_(buffer_data, buffer_pos);
            buffer_pos = 0;
        }
    }

    return write_(buffer_data, buffer_pos);
}

bool SoxSink::setup_buffer_() {
    buffer_size_ = sample_spec_.ns_2_samples_overall(frame_length_);
    if (buffer_size_ == 0) {
        roc_log(LogError, "sox sink: buffer size is zero");
        return false;
    }
    if (!buffer_.resize(buffer_size_)) {
        roc_log(LogError, "sox sink: can't allocate sample buffer");
        return false;
    }

    return true;
}

bool SoxSink::open_(const char* driver, const char* path) {
    output_ = sox_open_write(path, &out_signal_, NULL, driver, NULL, NULL);
    if (!output_) {
        roc_log(LogDebug, "sox sink: can't open: driver=%s path=%s", driver, path);
        return false;
    }

    is_file_ = !(output_->handler.flags & SOX_FILE_DEVICE);

    const unsigned long requested_rate = (unsigned long)out_signal_.rate;
    const unsigned long actual_rate = (unsigned long)output_->signal.rate;

    if (requested_rate != 0 && requested_rate != actual_rate) {
        roc_log(LogError,
                "sox sink:"
                " can't open output file or device with the requested sample rate:"
                " required_by_output=%lu requested_by_user=%lu",
                actual_rate, requested_rate);
        return false;
    }

    const unsigned long requested_chans = (unsigned long)out_signal_.channels;
    const unsigned long actual_chans = (unsigned long)output_->signal.channels;

    if (requested_chans != 0 && requested_chans != actual_chans) {
        roc_log(LogError,
                "sox sink:"
                " can't open output file or device with the requested channel count:"
                " required_by_output=%lu requested_by_user=%lu",
                actual_chans, requested_chans);
        return false;
    }

    sample_spec_.set_sample_rate(actual_rate);
    sample_spec_.channel_set().set_layout(audio::ChanLayout_Surround);
    sample_spec_.channel_set().set_order(audio::ChanOrder_Smpte);
    sample_spec_.channel_set().set_count(actual_chans);

    roc_log(LogInfo,
            "sox sink:"
            " opened: bits=%lu rate=%lu req_rate=%lu chans=%lu req_chans=%lu is_file=%d",
            (unsigned long)output_->encoding.bits_per_sample, actual_rate, requested_rate,
            actual_chans, requested_chans, (int)is_file_);

    return true;
}

status::StatusCode SoxSink::write_(const sox_sample_t* samples, size_t n_samples) {
    if (n_samples > 0) {
        if (sox_write(output_, samples, n_samples) != n_samples) {
            roc_log(LogError, "sox sink: failed to write output buffer");
            return is_file_ ? status::StatusErrFile : status::StatusErrDevice;
        }
    }

    return status::StatusOK;
}

void SoxSink::close_() {
    if (!output_) {
        return;
    }

    roc_log(LogDebug, "sox sink: closing output");

    int err = sox_close(output_);
    if (err != SOX_SUCCESS) {
        roc_panic("sox sink: can't close output: %s", sox_strerror(err));
    }

    output_ = NULL;
}

} // namespace sndio
} // namespace roc
