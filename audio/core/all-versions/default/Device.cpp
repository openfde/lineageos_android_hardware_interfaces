/*
 * Copyright (C) 2018 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#define LOG_TAG "DeviceHAL"

#include "core/default/Device.h"
#include <HidlUtils.h>
#include "common/all-versions/default/EffectMap.h"
#include "core/default/Conversions.h"
#include "core/default/StreamIn.h"
#include "core/default/StreamOut.h"
#include "core/default/Util.h"

//#define LOG_NDEBUG 0

#include <inttypes.h>
#include <memory.h>
#include <string.h>
#include <algorithm>

#include <android/log.h>

namespace android {
namespace hardware {
namespace audio {
namespace CPP_VERSION {
namespace implementation {

using ::android::hardware::audio::common::CPP_VERSION::implementation::HidlUtils;

Device::Device(audio_hw_device_t* device) : mIsClosed(false), mDevice(device) {}

Device::~Device() {
    (void)doClose();
    mDevice = nullptr;
}

Result Device::analyzeStatus(const char* funcName, int status,
                             const std::vector<int>& ignoreErrors) {
    return util::analyzeStatus("Device", funcName, status, ignoreErrors);
}

void Device::closeInputStream(audio_stream_in_t* stream) {
    mDevice->close_input_stream(mDevice, stream);
    LOG_ALWAYS_FATAL_IF(mOpenedStreamsCount == 0, "mOpenedStreamsCount is already 0");
    --mOpenedStreamsCount;
}

void Device::closeOutputStream(audio_stream_out_t* stream) {
    mDevice->close_output_stream(mDevice, stream);
    LOG_ALWAYS_FATAL_IF(mOpenedStreamsCount == 0, "mOpenedStreamsCount is already 0");
    --mOpenedStreamsCount;
}

char* Device::halGetParameters(const char* keys) {
    return mDevice->get_parameters(mDevice, keys);
}

int Device::halSetParameters(const char* keysAndValues) {
    return mDevice->set_parameters(mDevice, keysAndValues);
}

// Methods from ::android::hardware::audio::CPP_VERSION::IDevice follow.
Return<Result> Device::initCheck() {
    return analyzeStatus("init_check", mDevice->init_check(mDevice));
}

Return<Result> Device::setMasterVolume(float volume) {
    if (mDevice->set_master_volume == NULL) {
        return Result::NOT_SUPPORTED;
    }
    if (!isGainNormalized(volume)) {
        ALOGW("Can not set a master volume (%f) outside [0,1]", volume);
        return Result::INVALID_ARGUMENTS;
    }
    return analyzeStatus("set_master_volume", mDevice->set_master_volume(mDevice, volume),
                         {ENOSYS} /*ignore*/);
}

Return<void> Device::getMasterVolume(getMasterVolume_cb _hidl_cb) {
    Result retval(Result::NOT_SUPPORTED);
    float volume = 0;
    if (mDevice->get_master_volume != NULL) {
        retval = analyzeStatus("get_master_volume", mDevice->get_master_volume(mDevice, &volume),
                               {ENOSYS} /*ignore*/);
    }
    _hidl_cb(retval, volume);
    return Void();
}

Return<Result> Device::setMicMute(bool mute) {
    return analyzeStatus("set_mic_mute", mDevice->set_mic_mute(mDevice, mute), {ENOSYS} /*ignore*/);
}

Return<void> Device::getMicMute(getMicMute_cb _hidl_cb) {
    bool mute = false;
    Result retval = analyzeStatus("get_mic_mute", mDevice->get_mic_mute(mDevice, &mute),
                                  {ENOSYS} /*ignore*/);
    _hidl_cb(retval, mute);
    return Void();
}

Return<Result> Device::setMasterMute(bool mute) {
    Result retval(Result::NOT_SUPPORTED);
    if (mDevice->set_master_mute != NULL) {
        retval = analyzeStatus("set_master_mute", mDevice->set_master_mute(mDevice, mute),
                               {ENOSYS} /*ignore*/);
    }
    return retval;
}

Return<void> Device::getMasterMute(getMasterMute_cb _hidl_cb) {
    Result retval(Result::NOT_SUPPORTED);
    bool mute = false;
    if (mDevice->get_master_mute != NULL) {
        retval = analyzeStatus("get_master_mute", mDevice->get_master_mute(mDevice, &mute),
                               {ENOSYS} /*ignore*/);
    }
    _hidl_cb(retval, mute);
    return Void();
}

Return<void> Device::getInputBufferSize(const AudioConfig& config, getInputBufferSize_cb _hidl_cb) {
    audio_config_t halConfig;
    HidlUtils::audioConfigToHal(config, &halConfig);
    size_t halBufferSize = mDevice->get_input_buffer_size(mDevice, &halConfig);
    Result retval(Result::INVALID_ARGUMENTS);
    uint64_t bufferSize = 0;
    if (halBufferSize != 0) {
        retval = Result::OK;
        bufferSize = halBufferSize;
    }
    _hidl_cb(retval, bufferSize);
    return Void();
}

std::tuple<Result, sp<IStreamOut>> Device::openOutputStreamImpl(int32_t ioHandle,
                                                                const DeviceAddress& device,
                                                                const AudioConfig& config,
                                                                AudioOutputFlagBitfield flags,
                                                                AudioConfig* suggestedConfig) {
    audio_config_t halConfig;
    HidlUtils::audioConfigToHal(config, &halConfig);
    audio_stream_out_t* halStream;
    ALOGV(
        "open_output_stream handle: %d devices: %x flags: %#x "
        "srate: %d format %#x channels %x address %s",
        ioHandle, static_cast<audio_devices_t>(device.device),
        static_cast<audio_output_flags_t>(flags), halConfig.sample_rate, halConfig.format,
        halConfig.channel_mask, deviceAddressToHal(device).c_str());
    int status =
        mDevice->open_output_stream(mDevice, ioHandle, static_cast<audio_devices_t>(device.device),
                                    static_cast<audio_output_flags_t>(flags), &halConfig,
                                    &halStream, deviceAddressToHal(device).c_str());
    ALOGV("open_output_stream status %d stream %p", status, halStream);
    sp<IStreamOut> streamOut;
    if (status == OK) {
        streamOut = new StreamOut(this, halStream);
        ++mOpenedStreamsCount;
    }
    status_t convertStatus = HidlUtils::audioConfigFromHal(halConfig, suggestedConfig);
    ALOGW_IF(convertStatus != OK, "%s: suggested config with incompatible fields", __func__);
    return {analyzeStatus("open_output_stream", status, {EINVAL} /*ignore*/), streamOut};
}

std::tuple<Result, sp<IStreamIn>> Device::openInputStreamImpl(
    int32_t ioHandle, const DeviceAddress& device, const AudioConfig& config,
    AudioInputFlagBitfield flags, AudioSource source, AudioConfig* suggestedConfig) {
    audio_config_t halConfig;
    HidlUtils::audioConfigToHal(config, &halConfig);
    audio_stream_in_t* halStream;
    ALOGV(
        "open_input_stream handle: %d devices: %x flags: %#x "
        "srate: %d format %#x channels %x address %s source %d",
        ioHandle, static_cast<audio_devices_t>(device.device),
        static_cast<audio_input_flags_t>(flags), halConfig.sample_rate, halConfig.format,
        halConfig.channel_mask, deviceAddressToHal(device).c_str(),
        static_cast<audio_source_t>(source));
    int status = mDevice->open_input_stream(
        mDevice, ioHandle, static_cast<audio_devices_t>(device.device), &halConfig, &halStream,
        static_cast<audio_input_flags_t>(flags), deviceAddressToHal(device).c_str(),
        static_cast<audio_source_t>(source));
    ALOGV("open_input_stream status %d stream %p", status, halStream);
    sp<IStreamIn> streamIn;
    if (status == OK) {
        streamIn = new StreamIn(this, halStream);
        ++mOpenedStreamsCount;
    }
    status_t convertStatus = HidlUtils::audioConfigFromHal(halConfig, suggestedConfig);
    ALOGW_IF(convertStatus != OK, "%s: suggested config with incompatible fields", __func__);
    return {analyzeStatus("open_input_stream", status, {EINVAL} /*ignore*/), streamIn};
}

#if MAJOR_VERSION == 2
Return<void> Device::openOutputStream(int32_t ioHandle, const DeviceAddress& device,
                                      const AudioConfig& config, AudioOutputFlagBitfield flags,
                                      openOutputStream_cb _hidl_cb) {
    AudioConfig suggestedConfig;
    auto [result, streamOut] =
        openOutputStreamImpl(ioHandle, device, config, flags, &suggestedConfig);
    _hidl_cb(result, streamOut, suggestedConfig);
    return Void();
}

Return<void> Device::openInputStream(int32_t ioHandle, const DeviceAddress& device,
                                     const AudioConfig& config, AudioInputFlagBitfield flags,
                                     AudioSource source, openInputStream_cb _hidl_cb) {
    AudioConfig suggestedConfig;
    auto [result, streamIn] =
        openInputStreamImpl(ioHandle, device, config, flags, source, &suggestedConfig);
    _hidl_cb(result, streamIn, suggestedConfig);
    return Void();
}

#elif MAJOR_VERSION >= 4
Return<void> Device::openOutputStream(int32_t ioHandle, const DeviceAddress& device,
                                      const AudioConfig& config, AudioOutputFlagBitfield flags,
                                      const SourceMetadata& sourceMetadata,
                                      openOutputStream_cb _hidl_cb) {
    AudioConfig suggestedConfig;
    auto [result, streamOut] =
        openOutputStreamImpl(ioHandle, device, config, flags, &suggestedConfig);
    if (streamOut) {
        streamOut->updateSourceMetadata(sourceMetadata);
    }
    _hidl_cb(result, streamOut, suggestedConfig);
    return Void();
}

Return<void> Device::openInputStream(int32_t ioHandle, const DeviceAddress& device,
                                     const AudioConfig& config, AudioInputFlagBitfield flags,
                                     const SinkMetadata& sinkMetadata,
                                     openInputStream_cb _hidl_cb) {
    if (sinkMetadata.tracks.size() == 0) {
        // This should never happen, the framework must not create as stream
        // if there is no client
        ALOGE("openInputStream called without tracks connected");
        _hidl_cb(Result::INVALID_ARGUMENTS, nullptr, AudioConfig());
        return Void();
    }
    // Pick the first one as the main.
    AudioSource source = sinkMetadata.tracks[0].source;
    AudioConfig suggestedConfig;
    auto [result, streamIn] =
        openInputStreamImpl(ioHandle, device, config, flags, source, &suggestedConfig);
    if (streamIn) {
        streamIn->updateSinkMetadata(sinkMetadata);
    }
    _hidl_cb(result, streamIn, suggestedConfig);
    return Void();
}
#endif /* MAJOR_VERSION */

Return<bool> Device::supportsAudioPatches() {
    return version() >= AUDIO_DEVICE_API_VERSION_3_0;
}

Return<void> Device::createAudioPatch(const hidl_vec<AudioPortConfig>& sources,
                                      const hidl_vec<AudioPortConfig>& sinks,
                                      createAudioPatch_cb _hidl_cb) {
    auto [retval, patch] = createOrUpdateAudioPatch(
            static_cast<AudioPatchHandle>(AudioHandleConsts::AUDIO_PATCH_HANDLE_NONE), sources,
            sinks);
    _hidl_cb(retval, patch);
    return Void();
}

std::tuple<Result, AudioPatchHandle> Device::createOrUpdateAudioPatch(
        AudioPatchHandle patch, const hidl_vec<AudioPortConfig>& sources,
        const hidl_vec<AudioPortConfig>& sinks) {
    Result retval(Result::NOT_SUPPORTED);
    if (version() >= AUDIO_DEVICE_API_VERSION_3_0) {
        std::unique_ptr<audio_port_config[]> halSources(HidlUtils::audioPortConfigsToHal(sources));
        std::unique_ptr<audio_port_config[]> halSinks(HidlUtils::audioPortConfigsToHal(sinks));
        audio_patch_handle_t halPatch = static_cast<audio_patch_handle_t>(patch);
        retval = analyzeStatus("create_audio_patch",
                               mDevice->create_audio_patch(mDevice, sources.size(), &halSources[0],
                                                           sinks.size(), &halSinks[0], &halPatch));
        if (retval == Result::OK) {
            patch = static_cast<AudioPatchHandle>(halPatch);
        }
    }
    return {retval, patch};
}

Return<Result> Device::releaseAudioPatch(int32_t patch) {
    if (version() >= AUDIO_DEVICE_API_VERSION_3_0) {
        return analyzeStatus(
            "release_audio_patch",
            mDevice->release_audio_patch(mDevice, static_cast<audio_patch_handle_t>(patch)));
    }
    return Result::NOT_SUPPORTED;
}

Return<void> Device::getAudioPort(const AudioPort& port, getAudioPort_cb _hidl_cb) {
    audio_port halPort;
    HidlUtils::audioPortToHal(port, &halPort);
    Result retval = analyzeStatus("get_audio_port", mDevice->get_audio_port(mDevice, &halPort));
    AudioPort resultPort = port;
    if (retval == Result::OK) {
        HidlUtils::audioPortFromHal(halPort, &resultPort);
    }
    _hidl_cb(retval, resultPort);
    return Void();
}

Return<Result> Device::setAudioPortConfig(const AudioPortConfig& config) {
    if (version() >= AUDIO_DEVICE_API_VERSION_3_0) {
        struct audio_port_config halPortConfig;
        HidlUtils::audioPortConfigToHal(config, &halPortConfig);
        return analyzeStatus("set_audio_port_config",
                             mDevice->set_audio_port_config(mDevice, &halPortConfig));
    }
    return Result::NOT_SUPPORTED;
}

#if MAJOR_VERSION == 2
Return<AudioHwSync> Device::getHwAvSync() {
    int halHwAvSync;
    Result retval = getParam(AudioParameter::keyHwAvSync, &halHwAvSync);
    return retval == Result::OK ? halHwAvSync : AUDIO_HW_SYNC_INVALID;
}
#elif MAJOR_VERSION >= 4
Return<void> Device::getHwAvSync(getHwAvSync_cb _hidl_cb) {
    int halHwAvSync;
    Result retval = getParam(AudioParameter::keyHwAvSync, &halHwAvSync);
    _hidl_cb(retval, halHwAvSync);
    return Void();
}
#endif

Return<Result> Device::setScreenState(bool turnedOn) {
    return setParam(AudioParameter::keyScreenState, turnedOn);
}

#if MAJOR_VERSION == 2
Return<void> Device::getParameters(const hidl_vec<hidl_string>& keys, getParameters_cb _hidl_cb) {
    getParametersImpl({}, keys, _hidl_cb);
    return Void();
}

Return<Result> Device::setParameters(const hidl_vec<ParameterValue>& parameters) {
    return setParametersImpl({} /* context */, parameters);
}
#elif MAJOR_VERSION >= 4
Return<void> Device::getParameters(const hidl_vec<ParameterValue>& context,
                                   const hidl_vec<hidl_string>& keys, getParameters_cb _hidl_cb) {
    getParametersImpl(context, keys, _hidl_cb);
    return Void();
}
Return<Result> Device::setParameters(const hidl_vec<ParameterValue>& context,
                                     const hidl_vec<ParameterValue>& parameters) {
    return setParametersImpl(context, parameters);
}
#endif

#if MAJOR_VERSION == 2
Return<void> Device::debugDump(const hidl_handle& fd) {
    return debug(fd, {});
}
#endif

Return<void> Device::debug(const hidl_handle& fd, const hidl_vec<hidl_string>& /* options */) {
    if (fd.getNativeHandle() != nullptr && fd->numFds == 1) {
        analyzeStatus("dump", mDevice->dump(mDevice, fd->data[0]));
    }
    return Void();
}

#if MAJOR_VERSION >= 4
Return<void> Device::getMicrophones(getMicrophones_cb _hidl_cb) {
    Result retval = Result::NOT_SUPPORTED;
    size_t actual_mics = AUDIO_MICROPHONE_MAX_COUNT;
    audio_microphone_characteristic_t mic_array[AUDIO_MICROPHONE_MAX_COUNT];

    hidl_vec<MicrophoneInfo> microphones;
    if (mDevice->get_microphones != NULL &&
        mDevice->get_microphones(mDevice, &mic_array[0], &actual_mics) == 0) {
        microphones.resize(actual_mics);
        for (size_t i = 0; i < actual_mics; ++i) {
            halToMicrophoneCharacteristics(&microphones[i], mic_array[i]);
        }
        retval = Result::OK;
    }
    _hidl_cb(retval, microphones);
    return Void();
}

Return<Result> Device::setConnectedState(const DeviceAddress& address, bool connected) {
    auto key = connected ? AudioParameter::keyDeviceConnect : AudioParameter::keyDeviceDisconnect;
    return setParam(key, address);
}
#endif

Result Device::doClose() {
    if (mIsClosed || mOpenedStreamsCount != 0) return Result::INVALID_STATE;
    mIsClosed = true;
    return analyzeStatus("close", audio_hw_device_close(mDevice));
}

#if MAJOR_VERSION >= 6
Return<Result> Device::close() {
    return doClose();
}

Return<Result> Device::addDeviceEffect(AudioPortHandle device, uint64_t effectId) {
    if (version() < AUDIO_DEVICE_API_VERSION_3_1 || mDevice->add_device_effect == nullptr) {
        return Result::NOT_SUPPORTED;
    }

    effect_handle_t halEffect = EffectMap::getInstance().get(effectId);
    if (halEffect != NULL) {
        return analyzeStatus("add_device_effect",
                             mDevice->add_device_effect(
                                     mDevice, static_cast<audio_port_handle_t>(device), halEffect));
    } else {
        ALOGW("%s Invalid effect ID passed from client: %" PRIu64 "", __func__, effectId);
        return Result::INVALID_ARGUMENTS;
    }
}

Return<Result> Device::removeDeviceEffect(AudioPortHandle device, uint64_t effectId) {
    if (version() < AUDIO_DEVICE_API_VERSION_3_1 || mDevice->remove_device_effect == nullptr) {
        return Result::NOT_SUPPORTED;
    }

    effect_handle_t halEffect = EffectMap::getInstance().get(effectId);
    if (halEffect != NULL) {
        return analyzeStatus("remove_device_effect",
                             mDevice->remove_device_effect(
                                     mDevice, static_cast<audio_port_handle_t>(device), halEffect));
    } else {
        ALOGW("%s Invalid effect ID passed from client: %" PRIu64 "", __func__, effectId);
        return Result::INVALID_ARGUMENTS;
    }
}

Return<void> Device::updateAudioPatch(int32_t previousPatch,
                                      const hidl_vec<AudioPortConfig>& sources,
                                      const hidl_vec<AudioPortConfig>& sinks,
                                      createAudioPatch_cb _hidl_cb) {
    if (previousPatch != static_cast<int32_t>(AudioHandleConsts::AUDIO_PATCH_HANDLE_NONE)) {
        auto [retval, patch] = createOrUpdateAudioPatch(previousPatch, sources, sinks);
        _hidl_cb(retval, patch);
    } else {
        _hidl_cb(Result::INVALID_ARGUMENTS, previousPatch);
    }
    return Void();
}

#endif

Return<void> Device::getDevs(bool input, getDevs_cb _hidl_cb) {
    Result retval(Result::NOT_SUPPORTED);
    hidl_string result;
    if (mDevice->get_devs != NULL) {
        char* halResult = mDevice->get_devs(mDevice, input);
        if (halResult != NULL) {
            result = halResult;
            free(halResult);
        }
        retval = Result::OK;
    }
    _hidl_cb(retval, result);
    return Void();
}

Return<Result> Device::setDevVolume(bool input, const hidl_string& devName, float volume) {
    Result retval(Result::NOT_SUPPORTED);
    if (mDevice->set_dev_volume != NULL) {
        retval = analyzeStatus("set_dev_volume", mDevice->set_dev_volume(mDevice, input, devName.c_str(), volume),
                               {ENOSYS} /*ignore*/);
    }
    return retval;
}

Return<Result> Device::setDevMute(bool input, const hidl_string& devName, bool mute) {
    Result retval(Result::NOT_SUPPORTED);
    if (mDevice->set_dev_mute != NULL) {
    retval = analyzeStatus("set_dev_mute", mDevice->set_dev_mute(mDevice, input, devName.c_str(), mute),
                           {ENOSYS} /*ignore*/);
    }
    return retval;
}

Return<void> Device::setDefaultDev(bool input, const hidl_string& devName, bool needInfo, setDefaultDev_cb _hidl_cb) {
    Result retval(Result::NOT_SUPPORTED);
    hidl_string result;
    if (mDevice->set_default_dev != NULL) {
        char* halResult = mDevice->set_default_dev(mDevice, input, devName.c_str(), needInfo);
        if (halResult != NULL) {
            result = halResult;
            free(halResult);
        }
        retval = Result::OK;
    }
    _hidl_cb(retval, result);
    return Void();
}

}  // namespace implementation
}  // namespace CPP_VERSION
}  // namespace audio
}  // namespace hardware
}  // namespace android
