/*
 * Copyright (C) 2019 The Android Open Source Project
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
//#define LOG_NDEBUG 0

#include "Sensor.h"

#include <hardware/sensors.h>
#include <utils/SystemClock.h>

#include <cmath>
#include <log/log.h>
#include <random>

namespace android {
namespace hardware {
namespace sensors {
namespace V2_1 {
namespace subhal {
namespace implementation {

using ::android::hardware::sensors::V1_0::MetaDataEventType;
using ::android::hardware::sensors::V1_0::OperationMode;
using ::android::hardware::sensors::V1_0::RateLevel;
using ::android::hardware::sensors::V1_0::Result;
using ::android::hardware::sensors::V1_0::SensorFlagBits;
using ::android::hardware::sensors::V1_0::SensorStatus;
using ::android::hardware::sensors::V2_1::Event;
using ::android::hardware::sensors::V2_1::SensorInfo;
using ::android::hardware::sensors::V2_1::SensorType;

enum class DevicePrivateSensorType : int32_t {
  TEMPERATURE = static_cast<int32_t>(SensorType::DEVICE_PRIVATE_BASE) + 1,
  CAMERA_V_SYNC,
  COLOR,
  HALL_EFFECT,
  MOTION_DETECT,
  STATIONARY_DETECT,
  BINNED_BRIGHTNESS,
  AUTO_BRIGHTNESS,
};

Sensor::Sensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : mIsEnabled(false),
      mSamplingPeriodNs(0),
      mLastSampleTimeNs(0),
      mCallback(callback),
      mMode(OperationMode::NORMAL) {
    mSensorInfo.sensorHandle = sensorHandle;
    mSensorInfo.vendor = "Google";
    mSensorInfo.version = 1;
    mSensorInfo.maxRange = 1.0f;
    mSensorInfo.resolution = 1.0f;
    mSensorInfo.power = 0.001f;  // mA
    mSensorInfo.minDelay = 0;
    mSensorInfo.maxDelay = 1000 * 1000;
    mSensorInfo.fifoReservedEventCount = 0;
    mSensorInfo.fifoMaxEventCount = 0;
    mSensorInfo.requiredPermission = "";
    mSensorInfo.flags = 0;
    mRunThread = std::thread(startThread, this);
}

Sensor::~Sensor() {
    // Ensure that lock is unlocked before calling mRunThread.join() or a
    // deadlock will occur.
    {
        std::unique_lock<std::mutex> lock(mRunMutex);
        mStopThread = true;
        mIsEnabled = false;
        mWaitCV.notify_all();
    }
    mRunThread.join();
}

const SensorInfo& Sensor::getSensorInfo() const {
    return mSensorInfo;
}

void Sensor::batch(int64_t samplingPeriodNs) {
    if (mSensorInfo.minDelay != 0 && mSensorInfo.maxDelay != 0) {
        samplingPeriodNs = std::clamp(samplingPeriodNs,
                                      static_cast<int64_t>(mSensorInfo.minDelay) * 1000,
                                      static_cast<int64_t>(mSensorInfo.maxDelay) * 1000);
    }
    std::unique_lock<std::mutex> lock(mRunMutex);
    if (mSamplingPeriodNs != samplingPeriodNs) {
        mSamplingPeriodNs = samplingPeriodNs;
        // Wake up the 'run' thread to check if a new event should be generated now
        mWaitCV.notify_all();
    }
}

void Sensor::activate(bool enable) {
    if (mIsEnabled != enable) {
        std::unique_lock<std::mutex> lock(mRunMutex);
        mIsEnabled = enable;
        mWaitCV.notify_all();
    }
}

Result Sensor::flush() {
    // Only generate a flush complete event if the sensor is enabled and if the sensor is not a
    // one-shot sensor.
    if (!mIsEnabled || (mSensorInfo.flags & static_cast<uint32_t>(SensorFlagBits::ONE_SHOT_MODE))) {
        return Result::BAD_VALUE;
    }

    // Note: If a sensor supports batching, write all of the currently batched events for the sensor
    // to the Event FMQ prior to writing the flush complete event.
    Event ev;
    ev.sensorHandle = mSensorInfo.sensorHandle;
    ev.sensorType = SensorType::META_DATA;
    ev.u.meta.what = MetaDataEventType::META_DATA_FLUSH_COMPLETE;
    std::vector<Event> evs{ev};
    mCallback->postEvents(evs, isWakeUpSensor());

    return Result::OK;
}

void Sensor::startThread(Sensor* sensor) {
    sensor->run();
}

void Sensor::run() {
    std::unique_lock<std::mutex> runLock(mRunMutex);
    constexpr int64_t kNanosecondsInSeconds = 1000 * 1000 * 1000;

    while (!mStopThread) {
        if (!mIsEnabled || mMode == OperationMode::DATA_INJECTION) {
            mWaitCV.wait(runLock, [&] {
                return ((mIsEnabled && mMode == OperationMode::NORMAL) || mStopThread);
            });
        } else {
            timespec curTime;
            clock_gettime(CLOCK_BOOTTIME, &curTime);
            int64_t now = (curTime.tv_sec * kNanosecondsInSeconds) + curTime.tv_nsec;
            int64_t nextSampleTime = mLastSampleTimeNs + mSamplingPeriodNs;

            if (now >= nextSampleTime) {
                mLastSampleTimeNs = now;
                nextSampleTime = mLastSampleTimeNs + mSamplingPeriodNs;
                mCallback->postEvents(readEvents(), isWakeUpSensor());
            }

            mWaitCV.wait_for(runLock, std::chrono::nanoseconds(nextSampleTime - now));
        }
    }
}

bool Sensor::isWakeUpSensor() {
    return mSensorInfo.flags & static_cast<uint32_t>(SensorFlagBits::WAKE_UP);
}

std::vector<Event> Sensor::readEvents() {
    std::vector<Event> events;
    Event event;
    event.sensorHandle = mSensorInfo.sensorHandle;
    event.sensorType = mSensorInfo.type;
    event.timestamp = ::android::elapsedRealtimeNano();
    static std::random_device rd;
    static std::mt19937 gen(rd());
    int32_t type = static_cast<int32_t>(mSensorInfo.type);
    switch (type) {
        case static_cast<int32_t>(SensorType::ACCELEROMETER): {
            static std::uniform_real_distribution<float> dis(-0.1f, 0.1f);
            event.u.vec3.x = dis(gen);
            event.u.vec3.y = dis(gen);
            event.u.vec3.z = 9.8f + dis(gen);
            break;
        }
        case static_cast<int32_t>(SensorType::MAGNETIC_FIELD): {
            static std::uniform_real_distribution<float> dis(-2.0f, 2.0f);
            event.u.vec3.x = 44.0 + dis(gen);
            event.u.vec3.y = -2.0 + dis(gen);
            event.u.vec3.z = -70.0 + dis(gen);
            break;
        }
        case static_cast<int32_t>(SensorType::ORIENTATION): {
            static std::uniform_real_distribution<float> dis(-0.3f, 0.3f);
            event.u.vec3.x = 265.0 + dis(gen);
            event.u.vec3.y = -0.5 + dis(gen);
            event.u.vec3.z = -0.7 + dis(gen);
            break;
        }
        case static_cast<int32_t>(SensorType::GYROSCOPE): {
            static std::uniform_real_distribution<float> dis(-0.00053f, 0.00053f);
            event.u.vec3.x = dis(gen);
            event.u.vec3.y = dis(gen);
            event.u.vec3.z = dis(gen);
            break;
        }
        case static_cast<int32_t>(SensorType::LIGHT): {
            static std::uniform_real_distribution<float> dis(-3.0f, 3.0f);
            event.u.scalar = 416.0f + dis(gen);
            break;
        }
        case static_cast<int32_t>(SensorType::GRAVITY): {
            static std::uniform_real_distribution<float> dis(-0.1f, 0.1f);
            event.u.vec3.x = dis(gen);
            event.u.vec3.y = dis(gen);
            event.u.vec3.z = 9.8f + dis(gen);
            break;
        }
        case static_cast<int32_t>(SensorType::LINEAR_ACCELERATION): {
            static std::uniform_real_distribution<float> dis(-0.001f, 0.001f);
            event.u.vec3.x = 0.0072f + dis(gen);
            event.u.vec3.y = 0.0224f + dis(gen);
            event.u.vec3.z = 0.0572f + dis(gen);
            break;
        }
        case static_cast<int32_t>(SensorType::ROTATION_VECTOR): {
            static std::uniform_real_distribution<float> dis(-0.0003f, 0.0003f);
            event.u.data[0] = -0.0016f + dis(gen);
            event.u.data[1] = 0.0076f + dis(gen);
            event.u.data[2] = 0.7366f + dis(gen);
            event.u.data[3] = 0.6762f + dis(gen);
            event.u.data[4] = 1.5708f + dis(gen);
            break;
        }
        case static_cast<int32_t>(SensorType::MAGNETIC_FIELD_UNCALIBRATED): {
            static std::uniform_real_distribution<float> dis(-0.002f, 0.002f);
            event.u.uncal.x = 43.6760f + dis(gen);
            event.u.uncal.y = -2.5864f + dis(gen);
            event.u.uncal.z = 69.8084f + dis(gen);
            event.u.uncal.x_bias = 0;
            event.u.uncal.y_bias = 0;
            event.u.uncal.z_bias = 0;
            break;
        }
        case static_cast<int32_t>(SensorType::GAME_ROTATION_VECTOR): {
            static std::uniform_real_distribution<float> dis(-0.0002f, 0.0002f);
            event.u.vec4.x = 0.0047f + dis(gen);
            event.u.vec4.y = 0.0064f + dis(gen);
            event.u.vec4.z = 0.0002f + dis(gen);
            event.u.vec4.w = 1.0f;
            break;
        }
        case static_cast<int32_t>(SensorType::GYROSCOPE_UNCALIBRATED): {
            static std::uniform_real_distribution<float> dis(-0.0002f, 0.0002f);
            event.u.uncal.x = 0.0002f + dis(gen);
            event.u.uncal.y = 0.0009f + dis(gen);
            event.u.uncal.z = 0.005f + dis(gen);
            event.u.uncal.x_bias = 0.0001f + dis(gen);
            event.u.uncal.y_bias = 0.0006f + dis(gen);
            event.u.uncal.z_bias = 0.0047f + dis(gen);
            break;
        }
        case static_cast<int32_t>(SensorType::SIGNIFICANT_MOTION):
        case static_cast<int32_t>(SensorType::STEP_DETECTOR):
        case static_cast<int32_t>(SensorType::TILT_DETECTOR):
        case static_cast<int32_t>(SensorType::PICK_UP_GESTURE): {
            event.u.scalar = 1.0f;
            break;
        }
        case static_cast<int32_t>(SensorType::STEP_COUNTER): {
            event.u.stepCount = 1;
            break;
        }
        case static_cast<int32_t>(SensorType::GEOMAGNETIC_ROTATION_VECTOR): {
            static std::uniform_real_distribution<float> dis(-0.0003f, 0.0003f);
            event.u.data[0] = -0.0016f + dis(gen);
            event.u.data[1] = 0.0076f + dis(gen);
            event.u.data[2] = 0.7366f + dis(gen);
            event.u.data[3] = 0.6762f + dis(gen);
            event.u.data[4] = 1.5708f + dis(gen);
            break;
        }
        case static_cast<int32_t>(SensorType::DEVICE_ORIENTATION): {
            event.u.scalar = 0;
            break;
        }
        case static_cast<int32_t>(SensorType::ACCELEROMETER_UNCALIBRATED): {
            static std::uniform_real_distribution<float> dis(-0.0002f, 0.0002f);
            event.u.uncal.x = 0.0002f + dis(gen);
            event.u.uncal.y = 0.0009f + dis(gen);
            event.u.uncal.z = 0.005f + dis(gen);
            event.u.uncal.x_bias = 0.0001f + dis(gen);
            event.u.uncal.y_bias = 0.0006f + dis(gen);
            event.u.uncal.z_bias = 0.0047f + dis(gen);
            break;
        }
        case static_cast<int32_t>(DevicePrivateSensorType::TEMPERATURE): {
            static std::uniform_real_distribution<float> dis(-0.5f, 0.5f);
            event.u.data[0] = 24.0f + dis(gen);
            break;
        }
        case static_cast<int32_t>(DevicePrivateSensorType::CAMERA_V_SYNC):
        case static_cast<int32_t>(DevicePrivateSensorType::HALL_EFFECT): {
            break;
        }
        case static_cast<int32_t>(DevicePrivateSensorType::COLOR): {
            static std::uniform_real_distribution<float> disFloat(-2000.0f, 2000.0f);
            static std::uniform_int_distribution<int> disInt(-2000, 2000);
            event.u.data[0] = 3691.13f + disFloat(gen);
            event.u.data[1] = 96012.0f + disInt(gen);
            event.u.data[2] = 50202.24f + disFloat(gen);
            event.u.data[3] = 31164.0f + disInt(gen);
            event.u.data[5] = 175092.0f + disInt(gen);
            event.u.data[6] = 10.0f;
            event.u.data[7] = 0.01f;
            break;
        }

        case static_cast<int32_t>(DevicePrivateSensorType::MOTION_DETECT):
        case static_cast<int32_t>(DevicePrivateSensorType::STATIONARY_DETECT): {
            event.u.data[0] = 1.0f;
            break;
        }
        case static_cast<int32_t>(DevicePrivateSensorType::BINNED_BRIGHTNESS): {
            static std::uniform_int_distribution<int> dis(-10, 10);
            event.u.data[0] = 3.0f;
            event.u.data[1] = 377.0f + dis(gen);
            event.u.data[2] = 255.0f;
            break;
        }
        case static_cast<int32_t>(DevicePrivateSensorType::AUTO_BRIGHTNESS): {
            static std::uniform_real_distribution<float> dis(-1.0f, 1.0f);
            event.u.data[0] = 377.96f + dis(gen);
            event.u.data[1] = 377.96f + dis(gen);
            break;
        }
        default:
            ALOGE("unsupported sensorType(%d)", static_cast<int32_t>(mSensorInfo.type));
            break;
    }
    event.u.vec3.status = SensorStatus::ACCURACY_HIGH;
    events.push_back(event);
    return events;
}

void Sensor::setOperationMode(OperationMode mode) {
    if (mMode != mode) {
        std::unique_lock<std::mutex> lock(mRunMutex);
        mMode = mode;
        mWaitCV.notify_all();
    }
}

bool Sensor::supportsDataInjection() const {
    return mSensorInfo.flags & static_cast<uint32_t>(SensorFlagBits::DATA_INJECTION);
}

Result Sensor::injectEvent(const Event& event) {
    Result result = Result::OK;
    if (event.sensorType == SensorType::ADDITIONAL_INFO) {
        // When in OperationMode::NORMAL, SensorType::ADDITIONAL_INFO is used to push operation
        // environment data into the device.
    } else if (!supportsDataInjection()) {
        result = Result::INVALID_OPERATION;
    } else if (mMode == OperationMode::DATA_INJECTION) {
        mCallback->postEvents(std::vector<Event>{event}, isWakeUpSensor());
    } else {
        result = Result::BAD_VALUE;
    }
    return result;
}

OnChangeSensor::OnChangeSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : Sensor(sensorHandle, callback), mPreviousEventSet(false) {
    mSensorInfo.flags |= SensorFlagBits::ON_CHANGE_MODE;
}

void OnChangeSensor::activate(bool enable) {
    Sensor::activate(enable);
    if (!enable) {
        mPreviousEventSet = false;
    }
}

std::vector<Event> OnChangeSensor::readEvents() {
    std::vector<Event> events = Sensor::readEvents();
    std::vector<Event> outputEvents;

    for (auto iter = events.begin(); iter != events.end(); ++iter) {
        Event ev = *iter;
        if (ev.u.vec3 != mPreviousEvent.u.vec3 || !mPreviousEventSet) {
            outputEvents.push_back(ev);
            mPreviousEvent = ev;
            mPreviousEventSet = true;
        }
    }
    return outputEvents;
}

ContinuousSensor::ContinuousSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : Sensor(sensorHandle, callback) {
    mSensorInfo.flags |= SensorFlagBits::CONTINUOUS_MODE;
}

SpecialReportingSensor::SpecialReportingSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.flags &= ~static_cast<uint32_t>(SensorFlagBits::ON_CHANGE_MODE);
    mSensorInfo.flags |= SensorFlagBits::SPECIAL_REPORTING_MODE;
}

OneShotSensor::OneShotSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : Sensor(sensorHandle, callback) {
    mSensorInfo.flags |= SensorFlagBits::ONE_SHOT_MODE;
}

std::vector<Event> OneShotSensor::readEvents() {
    std::vector<Event> events = Sensor::readEvents();
    Sensor::activate(false);
    return events;
}

AccelerometerSensor::AccelerometerSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "LSM6DSR Accelerometer";
    mSensorInfo.vendor = "STMicro";
    mSensorInfo.type = SensorType::ACCELEROMETER;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_ACCELEROMETER;
    mSensorInfo.maxRange = 78.4f;  // +/- 8g
    mSensorInfo.resolution = 1.52e-5;
    mSensorInfo.minDelay = 2404;  // microseconds
    mSensorInfo.maxDelay = 615385;
    mSensorInfo.flags |= SensorFlagBits::ADDITIONAL_INFO | SensorFlagBits::DIRECT_CHANNEL_GRALLOC
        | (static_cast<int32_t>(RateLevel::VERY_FAST) << __builtin_ctz(static_cast<uint32_t>(SensorFlagBits::MASK_DIRECT_REPORT)));
}

MagnetometerSensor::MagnetometerSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "MMC56X3X Magnetometer";
    mSensorInfo.vendor = "MEMSIC";
    mSensorInfo.type = SensorType::MAGNETIC_FIELD;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_MAGNETIC_FIELD;
    mSensorInfo.maxRange = 3198.0f;
    mSensorInfo.resolution = 0.1f;
    mSensorInfo.minDelay = 10 * 1000;
    mSensorInfo.maxDelay = 800 * 1000;
    mSensorInfo.flags |= SensorFlagBits::DIRECT_CHANNEL_GRALLOC | (static_cast<int32_t>(RateLevel::NORMAL) << __builtin_ctz(static_cast<uint32_t>(SensorFlagBits::MASK_DIRECT_REPORT)));
}

OrientationSensor::OrientationSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "Orientation Sensor";
    mSensorInfo.type = SensorType::ORIENTATION;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_ORIENTATION;
    mSensorInfo.maxRange = 360.0f;
    mSensorInfo.resolution = 1.0e-5;
    mSensorInfo.minDelay = 5 * 1000;
    mSensorInfo.maxDelay = 200 * 1000;
}

GyroscopeSensor::GyroscopeSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "LSM6DSR Gyroscope";
    mSensorInfo.vendor = "STMicro";
    mSensorInfo.type = SensorType::GYROSCOPE;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_GYROSCOPE;
    mSensorInfo.maxRange = 34.9f;
    mSensorInfo.resolution = 0.001;
    mSensorInfo.minDelay = 2404;
    mSensorInfo.maxDelay = 615385;
    mSensorInfo.flags |= SensorFlagBits::ADDITIONAL_INFO | SensorFlagBits::DIRECT_CHANNEL_GRALLOC
        | (static_cast<int32_t>(RateLevel::VERY_FAST) << __builtin_ctz(static_cast<uint32_t>(SensorFlagBits::MASK_DIRECT_REPORT)));
}

LightSensor::LightSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.name = "TCS3410 Ambient Light";
    mSensorInfo.vendor = "AMS";
    mSensorInfo.type = SensorType::LIGHT;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_LIGHT;
    mSensorInfo.maxRange = 1000000.0f;
    mSensorInfo.resolution = 0.01f;
    mSensorInfo.maxDelay = 100 * 1000;
}

GravitySensor::GravitySensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "Gravity Sensor";
    mSensorInfo.type = SensorType::GRAVITY;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_GRAVITY;
    mSensorInfo.maxRange = 9.81f;
    mSensorInfo.resolution = 1.0e-5;
    mSensorInfo.minDelay = 5 * 1000;
    mSensorInfo.maxDelay = 200 * 1000;
    mSensorInfo.flags |= SensorFlagBits::DIRECT_CHANNEL_GRALLOC | (static_cast<int32_t>(RateLevel::FAST) << __builtin_ctz(static_cast<uint32_t>(SensorFlagBits::MASK_DIRECT_REPORT)));
}

LinearAccelSensor::LinearAccelSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "Linear Acceleration Sensor";
    mSensorInfo.type = SensorType::LINEAR_ACCELERATION;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_LINEAR_ACCELERATION;
    mSensorInfo.maxRange = 78.4f;
    mSensorInfo.resolution = 1.52e-5;
    mSensorInfo.minDelay = 20 * 1000;
    mSensorInfo.maxDelay = 200 * 1000;
}

RotationVectorSensor::RotationVectorSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "Rotation Vector Sensor";
    mSensorInfo.type = SensorType::ROTATION_VECTOR;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_ROTATION_VECTOR;
    mSensorInfo.maxRange = 1.0f;
    mSensorInfo.resolution = 1.0e-5;
    mSensorInfo.minDelay = 5 * 1000;
    mSensorInfo.maxDelay = 200 * 1000;
}

MagneticUncalibratedSensor::MagneticUncalibratedSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "MMC56X3X Magnetometer-Uncalibrated";
    mSensorInfo.vendor = "MEMSIC";
    mSensorInfo.type = SensorType::MAGNETIC_FIELD_UNCALIBRATED;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_MAGNETIC_FIELD_UNCALIBRATED;
    mSensorInfo.maxRange = 3200.0f;
    mSensorInfo.resolution = 0.1f;
    mSensorInfo.minDelay = 10 * 1000;
    mSensorInfo.maxDelay = 800 * 1000;
    mSensorInfo.flags |= SensorFlagBits::DIRECT_CHANNEL_GRALLOC | (static_cast<int32_t>(RateLevel::NORMAL) << __builtin_ctz(static_cast<uint32_t>(SensorFlagBits::MASK_DIRECT_REPORT)));
}

GameRotationVectorSensor::GameRotationVectorSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "Game Rotation Vector Sensor";
    mSensorInfo.type = SensorType::GAME_ROTATION_VECTOR;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_GAME_ROTATION_VECTOR;
    mSensorInfo.maxRange = 1.0f;
    mSensorInfo.resolution = 1.0e-5;
    mSensorInfo.minDelay = 5 * 1000;
    mSensorInfo.maxDelay = 200 * 1000;
}

GyroUncalibratedSensor::GyroUncalibratedSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "LSM6DSR Gyroscope-Uncalibrated";
    mSensorInfo.vendor = "STMicro";
    mSensorInfo.type = SensorType::GYROSCOPE_UNCALIBRATED;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_GYROSCOPE_UNCALIBRATED;
    mSensorInfo.maxRange = 35.0f;
    mSensorInfo.resolution = 0.001f;
    mSensorInfo.minDelay = 2404;
    mSensorInfo.maxDelay = 615385;
    mSensorInfo.flags |= SensorFlagBits::ADDITIONAL_INFO | SensorFlagBits::DIRECT_CHANNEL_GRALLOC
        | (static_cast<int32_t>(RateLevel::VERY_FAST) << __builtin_ctz(static_cast<uint32_t>(SensorFlagBits::MASK_DIRECT_REPORT)));
}

SignificantMotionSensor::SignificantMotionSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OneShotSensor(sensorHandle, callback) {
    mSensorInfo.name = "Significant Motion(wake-up)";
    mSensorInfo.type = SensorType::SIGNIFICANT_MOTION;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_SIGNIFICANT_MOTION;
    mSensorInfo.minDelay = -1;
    mSensorInfo.maxDelay = 0;
    mSensorInfo.flags |= SensorFlagBits::WAKE_UP;
}

StepDetectorSensor::StepDetectorSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : SpecialReportingSensor(sensorHandle, callback) {
    mSensorInfo.name = "Step Detector";
    mSensorInfo.type = SensorType::STEP_DETECTOR;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_STEP_DETECTOR;
    mSensorInfo.maxDelay = 0;
}

StepCounterSensor::StepCounterSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.name = "Step Counter";
    mSensorInfo.type = SensorType::STEP_COUNTER;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_STEP_COUNTER;
    mSensorInfo.maxRange = 3.4f;
    mSensorInfo.resolution = 1.0f;
}

GeomagneticRotationVectorSensor::GeomagneticRotationVectorSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "Geomagnetic Rotation Vector Sensor";
    mSensorInfo.type = SensorType::GEOMAGNETIC_ROTATION_VECTOR;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_GEOMAGNETIC_ROTATION_VECTOR;
    mSensorInfo.maxRange = 1.0f;
    mSensorInfo.resolution = 1.0e-5;
    mSensorInfo.minDelay = 5 * 1000;
    mSensorInfo.maxDelay = 200 * 1000;
}

TiltDetectorSensor::TiltDetectorSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : SpecialReportingSensor(sensorHandle, callback) {
    mSensorInfo.name = "Tilt Sensor(wake-up)";
    mSensorInfo.type = SensorType::TILT_DETECTOR;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_TILT_DETECTOR;
    mSensorInfo.minDelay = 0;
    mSensorInfo.maxDelay = 0;
    mSensorInfo.flags |= SensorFlagBits::WAKE_UP;
}

PickUpGestureSensor::PickUpGestureSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OneShotSensor(sensorHandle, callback) {
    mSensorInfo.name = "Device Pickup Sensor(wake-up)";
    mSensorInfo.vendor = "STMicro";
    mSensorInfo.type = SensorType::PICK_UP_GESTURE;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_PICK_UP_GESTURE;
    mSensorInfo.minDelay = -1;
    mSensorInfo.maxDelay = 0;
    mSensorInfo.flags |= SensorFlagBits::WAKE_UP;
}

DeviceOrientationSensor::DeviceOrientationSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.name = "Device Orientation";
    mSensorInfo.type = SensorType::DEVICE_ORIENTATION;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_DEVICE_ORIENTATION;
    mSensorInfo.maxRange = 3.0f;
    mSensorInfo.resolution = 1.0f;
}

AccelUncalibratedSensor::AccelUncalibratedSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "LSM6DSR Accelerometer-Uncalibrated";
    mSensorInfo.vendor = "STMicro";
    mSensorInfo.type = SensorType::ACCELEROMETER_UNCALIBRATED;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_ACCELEROMETER_UNCALIBRATED;
    mSensorInfo.maxRange = 78.4f;
    mSensorInfo.resolution = 0.005f;
    mSensorInfo.minDelay = 2404;
    mSensorInfo.maxDelay = 615385;
    mSensorInfo.flags |= SensorFlagBits::ADDITIONAL_INFO | SensorFlagBits::DIRECT_CHANNEL_GRALLOC
        | (static_cast<int32_t>(RateLevel::VERY_FAST) << __builtin_ctz(static_cast<uint32_t>(SensorFlagBits::MASK_DIRECT_REPORT)));
}

TemperatureSensor::TemperatureSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "LSM6DSR Temperature";
    mSensorInfo.vendor = "STMicro";
    mSensorInfo.type = static_cast<SensorType>(static_cast<int32_t>(DevicePrivateSensorType::TEMPERATURE));
    mSensorInfo.typeAsString = "com.google.sensor.gyro_temperature";
    mSensorInfo.maxRange = 85.0f;
    mSensorInfo.resolution = 0.004f;
    mSensorInfo.minDelay = 19231;
    mSensorInfo.maxDelay = 615385;
}

CameraVSync0Sensor::CameraVSync0Sensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.name = "Camera V-Sync 0";
    mSensorInfo.type = static_cast<SensorType>(static_cast<int32_t>(DevicePrivateSensorType::CAMERA_V_SYNC));
    mSensorInfo.typeAsString = "com.google.sensor.camera_vsync";
    mSensorInfo.maxRange = 3.4e38f;
    mSensorInfo.resolution = 0.0f;
}

CameraVSync1Sensor::CameraVSync1Sensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.name = "Camera V-Sync 1";
    mSensorInfo.type = static_cast<SensorType>(static_cast<int32_t>(DevicePrivateSensorType::CAMERA_V_SYNC));
    mSensorInfo.typeAsString = "com.google.sensor.camera_vsync";
    mSensorInfo.maxRange = 3.4e38f;
    mSensorInfo.resolution = 0.0f;
}

ColorSensor::ColorSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.name = "TCS3410 Color";
    mSensorInfo.vendor = "AMS";
    mSensorInfo.type = static_cast<SensorType>(static_cast<int32_t>(DevicePrivateSensorType::COLOR));
    mSensorInfo.typeAsString = "com.google.sensor.color";
    mSensorInfo.maxRange = 1000000.0f;
    mSensorInfo.resolution = 0.01f;
    mSensorInfo.power = 0.001f;
    mSensorInfo.minDelay = 0;
    mSensorInfo.maxDelay = 100 * 1000;
}

HallEffect0Sensor::HallEffect0Sensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.name = "DRV5032 Hall Effect Sensor 0(wake-up)";
    mSensorInfo.vendor = "Texas Instruments";
    mSensorInfo.type = static_cast<SensorType>(static_cast<int32_t>(DevicePrivateSensorType::HALL_EFFECT));
    mSensorInfo.typeAsString = "com.google.sensor.hall_effect";
    mSensorInfo.flags |= SensorFlagBits::WAKE_UP;
}

HallEffect1Sensor::HallEffect1Sensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.name = "DRV5032 Hall Effect Sensor 1(wake-up)";
    mSensorInfo.vendor = "Texas Instruments";
    mSensorInfo.type = static_cast<SensorType>(static_cast<int32_t>(DevicePrivateSensorType::HALL_EFFECT));
    mSensorInfo.typeAsString = "com.google.sensor.hall_effect";
    mSensorInfo.flags |= SensorFlagBits::WAKE_UP;
}

HallEffect2Sensor::HallEffect2Sensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.name = "DRV5032 Hall Effect Sensor 2(wake-up)";
    mSensorInfo.vendor = "Texas Instruments";
    mSensorInfo.type = static_cast<SensorType>(static_cast<int32_t>(DevicePrivateSensorType::HALL_EFFECT));
    mSensorInfo.typeAsString = "com.google.sensor.hall_effect";
    mSensorInfo.flags |= SensorFlagBits::WAKE_UP;
}

MotionDetectSensor::MotionDetectSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OneShotSensor(sensorHandle, callback) {
    mSensorInfo.name = "LSM6DSR Motion Detect";
    mSensorInfo.vendor = "STMicro";
    mSensorInfo.type = static_cast<SensorType>(static_cast<int32_t>(DevicePrivateSensorType::MOTION_DETECT));
    mSensorInfo.typeAsString = "com.google.sensor.motion_detect";
    mSensorInfo.maxRange = 1.0f;
    mSensorInfo.resolution = 0.0f;
    mSensorInfo.minDelay = -1;
    mSensorInfo.maxDelay = 0;
}

StationaryDetectSensor::StationaryDetectSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OneShotSensor(sensorHandle, callback) {
    mSensorInfo.name = "LSM6DSR Stationary Detect";
    mSensorInfo.vendor = "STMicro";
    mSensorInfo.type = static_cast<SensorType>(static_cast<int32_t>(DevicePrivateSensorType::STATIONARY_DETECT));
    mSensorInfo.typeAsString = "com.google.sensor.stationary_detect";
    mSensorInfo.maxRange = 1.0f;
    mSensorInfo.resolution = 0.0f;
    mSensorInfo.minDelay = -1;
    mSensorInfo.maxDelay = 0;
}

BinnedBrightnessSensor::BinnedBrightnessSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.name = "Binned Brightness(wake-up)";
    mSensorInfo.type = static_cast<SensorType>(static_cast<int32_t>(DevicePrivateSensorType::BINNED_BRIGHTNESS));
    mSensorInfo.typeAsString = "com.google.sensor.binned_brightness";
    mSensorInfo.maxRange = 255.0f;
    mSensorInfo.resolution = 1.0f;
    mSensorInfo.flags |= SensorFlagBits::WAKE_UP;
}

AutoBrightnessSensor::AutoBrightnessSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.name = "Auto Brightness";
    mSensorInfo.type = static_cast<SensorType>(static_cast<int32_t>(DevicePrivateSensorType::AUTO_BRIGHTNESS));
    mSensorInfo.typeAsString = "com.google.sensor.auto_brightness";
    mSensorInfo.maxRange = 1000000.0f;
    mSensorInfo.resolution = 0.001f;
}

AccelSensor::AccelSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "Accel Sensor";
    mSensorInfo.type = SensorType::ACCELEROMETER;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_ACCELEROMETER;
    mSensorInfo.maxRange = 78.4f;  // +/- 8g
    mSensorInfo.resolution = 1.52e-5;
    mSensorInfo.power = 0.001f;        // mA
    mSensorInfo.minDelay = 20 * 1000;  // microseconds
    mSensorInfo.flags |= SensorFlagBits::DATA_INJECTION;
}

std::vector<Event> AccelSensor::readEvents() {
    std::vector<Event> events;
    Event event;
    event.sensorHandle = mSensorInfo.sensorHandle;
    event.sensorType = mSensorInfo.type;
    event.timestamp = ::android::elapsedRealtimeNano();
    event.u.vec3.x = 0;
    event.u.vec3.y = 0;
    event.u.vec3.z = 9.815;
    event.u.vec3.status = SensorStatus::ACCURACY_HIGH;
    events.push_back(event);
    return events;
}

PressureSensor::PressureSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "Pressure Sensor";
    mSensorInfo.type = SensorType::PRESSURE;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_PRESSURE;
    mSensorInfo.maxRange = 1100.0f;     // hPa
    mSensorInfo.resolution = 0.005f;    // hPa
    mSensorInfo.power = 0.001f;         // mA
    mSensorInfo.minDelay = 100 * 1000;  // microseconds
}

ProximitySensor::ProximitySensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.name = "Proximity Sensor";
    mSensorInfo.type = SensorType::PROXIMITY;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_PROXIMITY;
    mSensorInfo.maxRange = 5.0f;
    mSensorInfo.resolution = 1.0f;
    mSensorInfo.power = 0.012f;         // mA
    mSensorInfo.minDelay = 200 * 1000;  // microseconds
    mSensorInfo.flags |= SensorFlagBits::WAKE_UP;
}

GyroSensor::GyroSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : ContinuousSensor(sensorHandle, callback) {
    mSensorInfo.name = "Gyro Sensor";
    mSensorInfo.type = SensorType::GYROSCOPE;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_GYROSCOPE;
    mSensorInfo.maxRange = 1000.0f * M_PI / 180.0f;
    mSensorInfo.resolution = 1000.0f * M_PI / (180.0f * 32768.0f);
    mSensorInfo.power = 0.001f;
    mSensorInfo.minDelay = 2.5f * 1000;  // microseconds
}

std::vector<Event> GyroSensor::readEvents() {
    std::vector<Event> events;
    Event event;
    event.sensorHandle = mSensorInfo.sensorHandle;
    event.sensorType = mSensorInfo.type;
    event.timestamp = ::android::elapsedRealtimeNano();
    event.u.vec3.x = 0;
    event.u.vec3.y = 0;
    event.u.vec3.z = 0;
    event.u.vec3.status = SensorStatus::ACCURACY_HIGH;
    events.push_back(event);
    return events;
}

AmbientTempSensor::AmbientTempSensor(int32_t sensorHandle, ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.name = "Ambient Temp Sensor";
    mSensorInfo.type = SensorType::AMBIENT_TEMPERATURE;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_AMBIENT_TEMPERATURE;
    mSensorInfo.maxRange = 80.0f;
    mSensorInfo.resolution = 0.01f;
    mSensorInfo.power = 0.001f;
    mSensorInfo.minDelay = 40 * 1000;  // microseconds
}

RelativeHumiditySensor::RelativeHumiditySensor(int32_t sensorHandle,
                                               ISensorsEventCallback* callback)
    : OnChangeSensor(sensorHandle, callback) {
    mSensorInfo.name = "Relative Humidity Sensor";
    mSensorInfo.type = SensorType::RELATIVE_HUMIDITY;
    mSensorInfo.typeAsString = SENSOR_STRING_TYPE_RELATIVE_HUMIDITY;
    mSensorInfo.maxRange = 100.0f;
    mSensorInfo.resolution = 0.1f;
    mSensorInfo.power = 0.001f;
    mSensorInfo.minDelay = 40 * 1000;  // microseconds
}

}  // namespace implementation
}  // namespace subhal
}  // namespace V2_1
}  // namespace sensors
}  // namespace hardware
}  // namespace android
