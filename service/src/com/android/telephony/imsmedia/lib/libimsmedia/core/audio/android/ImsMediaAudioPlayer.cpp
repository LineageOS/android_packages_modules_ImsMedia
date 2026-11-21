/**
 * Copyright (C) 2022 The Android Open Source Project
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

#include <stdio.h>
#include <sys/time.h>
// #include <sys/timeb.h>
#include <fcntl.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <memory>
#include <thread>
#include <vector>

#include <ImsMediaDefine.h>
#include <ImsMediaTrace.h>
#include <ImsMediaTimer.h>
#include <ImsMediaAudioUtil.h>
#include <ImsMediaAudioPlayer.h>
#include <utils/Errors.h>

#define AAUDIO_STATE_TIMEOUT_NANO (100 * 1000000L)
#define DEFAULT_SAMPLING_RATE     (8000)
#define CODEC_TIMEOUT_NANO        (100000)

using namespace android;

namespace
{
aaudio_result_t waitForStreamStart(AAudioStream* stream, int64_t timeoutNanos)
{
    aaudio_stream_state_t state = AAUDIO_STREAM_STATE_UNINITIALIZED;
    const auto result = AAudioStream_waitForStateChange(
            stream, AAUDIO_STREAM_STATE_STARTING, &state, timeoutNanos);
    if (result != AAUDIO_OK)
    {
        return result;
    }
    if (state == AAUDIO_STREAM_STATE_DISCONNECTED)
    {
        return AAUDIO_ERROR_DISCONNECTED;
    }
    return state == AAUDIO_STREAM_STATE_STARTED ? AAUDIO_OK : AAUDIO_ERROR_INVALID_STATE;
}

aaudio_result_t primeLegacyStream(AAudioStream* stream)
{
    // AudioTrack may require a full buffer before its playback position advances.
    // One burst is not necessarily enough to reach this start threshold.
    const int32_t frames = AAudioStream_getBufferSizeInFrames(stream);
    const int32_t channels = AAudioStream_getChannelCount(stream);
    if (frames <= 0 || channels <= 0)
    {
        return AAUDIO_ERROR_INVALID_STATE;
    }
    const std::vector<int16_t> silence(static_cast<size_t>(frames) * channels, 0);
    // The legacy path treats any positive timeout as a blocking AudioTrack write.
    const auto written = AAudioStream_write(stream, silence.data(), frames, 0);
    return written < 0 ? written : AAUDIO_OK;
}
}  // namespace

ImsMediaAudioPlayer::ImsMediaAudioPlayer()
{
    mAudioStream = nullptr;
    mCodec = nullptr;
    mFormat = nullptr;
    mCodecType = 0;
    mCodecMode = 0;
    mSamplingRate = DEFAULT_SAMPLING_RATE;
    mEvsChAwOffset = 0;
    mEvsBandwidth = kEvsBandwidthNone;
    memset(mBuffer, 0, sizeof(mBuffer));
    mEvsBitRate = 0;
    mEvsCodecHeaderMode = kRtpPayloadHeaderModeEvsHeaderFull;
    mIsFirstFrame = false;
    mIsEvsInitialized = false;
    mIsOctetAligned = false;
    mIsDtxEnabled = false;
}

ImsMediaAudioPlayer::~ImsMediaAudioPlayer() {}

void ImsMediaAudioPlayer::SetCodec(int32_t type)
{
    IMLOGD_PACKET1(IM_PACKET_LOG_AUDIO, "[SetCodec] type[%d]", type);
    mCodecType = type;
}

void ImsMediaAudioPlayer::SetEvsBitRate(int32_t bitRate)
{
    mEvsBitRate = bitRate;
    IMLOGD_PACKET1(IM_PACKET_LOG_AUDIO, "[SetEvsBitRate] mEvsBitRate[%d]", mEvsBitRate);
}

void ImsMediaAudioPlayer::SetEvsChAwOffset(int32_t offset)
{
    mEvsChAwOffset = offset;
}

void ImsMediaAudioPlayer::SetSamplingRate(int32_t samplingRate)
{
    mSamplingRate = samplingRate;
}

void ImsMediaAudioPlayer::SetEvsBandwidth(int32_t evsBandwidth)
{
    mEvsBandwidth = (kEvsBandwidth)evsBandwidth;
}

void ImsMediaAudioPlayer::SetEvsPayloadHeaderMode(int32_t EvsPayloadHeaderMode)
{
    mEvsCodecHeaderMode = (kRtpPayloadHeaderMode)EvsPayloadHeaderMode;
}

void ImsMediaAudioPlayer::SetCodecMode(uint32_t mode)
{
    IMLOGD1("[SetCodecMode] mode[%d]", mode);
    mCodecMode = mode;
}

void ImsMediaAudioPlayer::SetDtxEnabled(bool isDtxEnabled)
{
    mIsDtxEnabled = isDtxEnabled;
}

void ImsMediaAudioPlayer::SetOctetAligned(bool isOctetAligned)
{
    mIsOctetAligned = isOctetAligned;
}

void ImsMediaAudioPlayer::ProcessCmr(const uint32_t cmr)
{
    IMLOGD1("[ProcessCmr] cmr[%d]", cmr);

    mCodecMode = cmr;
    Stop();
    Start();
}

bool ImsMediaAudioPlayer::Start()
{
    ImsMediaMutex::Autolock lock(mMutex);
    char kMimeType[128] = {'\0'};
    switch (mCodecType)
    {
        case kAudioCodecAmr:
            sprintf(kMimeType, "audio/3gpp");
            break;
        case kAudioCodecAmrWb:
            sprintf(kMimeType, "audio/amr-wb");
            break;
        case kAudioCodecEvs:
            // TODO: Integration with libEVS is required.
            sprintf(kMimeType, "audio/evs");
            break;
        case kAudioCodecL16:
            sprintf(kMimeType, "audio/l16");
            break;
        default:
            return false;
    }

    openAudioStream();

    if (mAudioStream == nullptr)
    {
        IMLOGE0("[Start] create audio stream failed");
        return false;
    }

    IMLOGD1("[Start] Creating codec[%s]", kMimeType);

    std::unique_ptr<AMediaFormat, decltype(&AMediaFormat_delete)> format(
            nullptr, AMediaFormat_delete);
    std::unique_ptr<AMediaCodec, decltype(&AMediaCodec_delete)> codec(nullptr, AMediaCodec_delete);
    if (mCodecType == kAudioCodecAmr || mCodecType == kAudioCodecAmrWb)
    {
        format.reset(AMediaFormat_new());
        if (format == nullptr)
        {
            IMLOGE0("[Start] unable to create codec format");
            closeAudioStream();
            return false;
        }
        AMediaFormat_setString(format.get(), AMEDIAFORMAT_KEY_MIME, kMimeType);
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_SAMPLE_RATE, mSamplingRate);
        AMediaFormat_setInt32(format.get(), AMEDIAFORMAT_KEY_CHANNEL_COUNT, 1);

        codec.reset(AMediaCodec_createDecoderByType(kMimeType));
        if (codec == nullptr)
        {
            IMLOGE1("[Start] unable to create %s codec instance", kMimeType);
            closeAudioStream();
            return false;
        }

        IMLOGD0("[Start] configure codec");
        const auto result = AMediaCodec_configure(codec.get(), format.get(), nullptr, nullptr, 0);
        if (result != AMEDIA_OK)
        {
            IMLOGE2("[Start] unable to configure[%s] codec - err[%d]", kMimeType, result);
            closeAudioStream();
            return false;
        }
    }

    if (!startAudioStream(AAUDIO_STATE_TIMEOUT_NANO))
    {
        return false;
    }

    if (codec != nullptr)
    {
        const auto result = AMediaCodec_start(codec.get());
        if (result != AMEDIA_OK)
        {
            IMLOGE1("[Start] unable to start codec - err[%d]", result);
            closeAudioStream();
            return false;
        }
        mCodec = codec.release();
        mFormat = format.release();
    }

    IMLOGD0("[Start] exit");
    return true;
}

void ImsMediaAudioPlayer::Stop()
{
    IMLOGD0("[Stop] enter");
    ImsMediaMutex::Autolock lock(mMutex);
    if ((mCodecType == kAudioCodecAmr || mCodecType == kAudioCodecAmrWb) && (mCodec != nullptr))
    {
        AMediaCodec_stop(mCodec);
        AMediaCodec_delete(mCodec);
        mCodec = nullptr;
    }

    if ((mCodecType == kAudioCodecAmr || mCodecType == kAudioCodecAmrWb) && (mFormat != nullptr))
    {
        AMediaFormat_delete(mFormat);
        mFormat = nullptr;
    }

    if (mAudioStream != nullptr)
    {
        aaudio_stream_state_t inputState = AAUDIO_STREAM_STATE_STOPPING;
        aaudio_stream_state_t nextState = AAUDIO_STREAM_STATE_UNINITIALIZED;
        aaudio_result_t result = AAudioStream_requestStop(mAudioStream);

        if (result != AAUDIO_OK)
        {
            IMLOGE1("[Stop] Error stop stream[%s]", AAudio_convertResultToText(result));
        }

        // TODO: if it causes extra delay in stop, optimize later
        result = AAudioStream_waitForStateChange(
                mAudioStream, inputState, &nextState, AAUDIO_STATE_TIMEOUT_NANO);

        if (result != AAUDIO_OK)
        {
            IMLOGE1("[Stop] Error stop stream[%s]", AAudio_convertResultToText(result));
        }

        IMLOGI1("[Stop] stream state[%s]", AAudio_convertStreamStateToText(nextState));

        closeAudioStream();
    }

    IMLOGD0("[Stop] exit ");
}

bool ImsMediaAudioPlayer::onDataFrame(uint8_t* buffer, uint32_t size, FrameType /*frameType*/,
        bool /*hasNextFrame*/, uint8_t /*nextFrameByte*/)
{
    ImsMediaMutex::Autolock lock(mMutex);

    if (size == 0 || buffer == nullptr || mAudioStream == nullptr ||
            AAudioStream_getState(mAudioStream) != AAUDIO_STREAM_STATE_STARTED)
    {
        return false;
    }

    if (mCodecType == kAudioCodecAmr || mCodecType == kAudioCodecAmrWb)
    {
        if (mCodec == nullptr)
        {
            return false;
        }
        else
        {
            return decodeAmr(buffer, size);
        }
    }
    else if (mCodecType == kAudioCodecEvs)
    {
        // TODO:Integration with libEVS is required.
        return decodeEvs(buffer, size);
    }
    else if (mCodecType == kAudioCodecL16)
    {
        memcpy(mBuffer, buffer, (MAX_PCM_SIZE * 2));
        // call audio write
        AAudioStream_write(mAudioStream, mBuffer, MAX_PCM_SIZE, 0);
    }
    return false;
}

bool ImsMediaAudioPlayer::decodeAmr(uint8_t* buffer, uint32_t size)
{
    auto index = AMediaCodec_dequeueInputBuffer(mCodec, CODEC_TIMEOUT_NANO);

    if (index >= 0)
    {
        size_t bufferSize = 0;
        uint8_t* inputBuffer = AMediaCodec_getInputBuffer(mCodec, index, &bufferSize);
        if (inputBuffer != nullptr)
        {
            memcpy(inputBuffer, buffer, size);
            IMLOGD_PACKET2(IM_PACKET_LOG_AUDIO,
                    "[decodeAmr] queue input buffer index[%d], size[%d]", index, size);

            auto err = AMediaCodec_queueInputBuffer(
                    mCodec, index, 0, size, ImsMediaTimer::GetTimeInMicroSeconds(), 0);
            if (err != AMEDIA_OK)
            {
                IMLOGE1("[decodeAmr] Unable to queue input buffers - err[%d]", err);
            }
        }
    }
    else
    {
        IMLOGE1("[decodeAmr] Unable to get input buffers - err[%d]", index);
    }

    AMediaCodecBufferInfo info;
    index = AMediaCodec_dequeueOutputBuffer(mCodec, &info, CODEC_TIMEOUT_NANO);

    if (index >= 0)
    {
        IMLOGD_PACKET5(IM_PACKET_LOG_AUDIO,
                "[decodeAmr] index[%d], size[%d], offset[%d], time[%ld], flags[%d]", index,
                info.size, info.offset, info.presentationTimeUs, info.flags);

        if (info.size > 0)
        {
            size_t buffCapacity;
            uint8_t* buf = AMediaCodec_getOutputBuffer(mCodec, index, &buffCapacity);
            if (buf != nullptr && buffCapacity > 0)
            {
                memcpy(mBuffer, buf, info.size);
                // call audio write
                AAudioStream_write(mAudioStream, mBuffer, info.size / 2, 0);
            }
        }

        AMediaCodec_releaseOutputBuffer(mCodec, index, false);
    }
    else if (index == AMEDIACODEC_INFO_OUTPUT_BUFFERS_CHANGED)
    {
        IMLOGD0("[decodeAmr] output buffer changed");
    }
    else if (index == AMEDIACODEC_INFO_OUTPUT_FORMAT_CHANGED)
    {
        if (mFormat != nullptr)
        {
            AMediaFormat_delete(mFormat);
        }

        mFormat = AMediaCodec_getOutputFormat(mCodec);
        IMLOGD1("[decodeAmr] format changed, format[%s]", AMediaFormat_toString(mFormat));
    }
    else if (index == AMEDIACODEC_INFO_TRY_AGAIN_LATER)
    {
        IMLOGD0("[decodeAmr] no output buffer");
    }
    else
    {
        IMLOGD1("[decodeAmr] unexpected index[%d]", index);
    }

    return true;
}

// TODO: Integration with libEVS is required.
bool ImsMediaAudioPlayer::decodeEvs(uint8_t* buffer, uint32_t size)
{
    uint16_t output[PCM_BUFFER_SIZE];
    int decodeSize = 0;

    // TODO: Integration with libEVS is required to decode buffer data.
    (void)buffer;
    (void)size;

    if (!mIsEvsInitialized)
    {
        IMLOGD0("[decodeEvs] Decoder has been initialised");
        mIsEvsInitialized = true;
    }

    if (!mIsFirstFrame)
    {
        IMLOGD0("[decodeEvs] First frame has been decoded");
        mIsFirstFrame = true;
    }

    AAudioStream_write(mAudioStream, output, (decodeSize / 2), 0);
    memset(output, 0, PCM_BUFFER_SIZE);

    return true;
}

void ImsMediaAudioPlayer::openAudioStream()
{
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);

    if (result != AAUDIO_OK)
    {
        IMLOGE1("[openAudioStream] Error creating stream builder[%s]",
                AAudio_convertResultToText(result));
        return;
    }

    // setup builder
    AAudioStreamBuilder_setInputPreset(builder, AAUDIO_INPUT_PRESET_VOICE_COMMUNICATION);
    AAudioStreamBuilder_setDirection(builder, AAUDIO_DIRECTION_OUTPUT);
    AAudioStreamBuilder_setFormat(builder, AAUDIO_FORMAT_PCM_I16);
    AAudioStreamBuilder_setChannelCount(builder, 1);
    AAudioStreamBuilder_setSampleRate(builder, mSamplingRate);
    AAudioStreamBuilder_setSharingMode(builder, AAUDIO_SHARING_MODE_EXCLUSIVE);
    AAudioStreamBuilder_setPerformanceMode(builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
    AAudioStreamBuilder_setUsage(builder, AAUDIO_USAGE_VOICE_COMMUNICATION);
    AAudioStreamBuilder_setErrorCallback(builder, audioErrorCallback, this);

    // open stream
    result = AAudioStreamBuilder_openStream(builder, &mAudioStream);
    AAudioStreamBuilder_delete(builder);

    if (result != AAUDIO_OK)
    {
        IMLOGE1("[openAudioStream] Failed to openStream. Error[%s]",
                AAudio_convertResultToText(result));
        if (mAudioStream != nullptr)
        {
            AAudioStream_close(mAudioStream);
        }
        mAudioStream = nullptr;
    }
}

bool ImsMediaAudioPlayer::startAudioStream(int64_t timeoutNanos)
{
    for (int attempt = 0; mAudioStream != nullptr; ++attempt)
    {
        const bool isLegacy = !AAudioStream_isMMapUsed(mAudioStream);
        auto result = AAudioStream_requestStart(mAudioStream);
        if (result == AAUDIO_OK)
        {
            // Preserve the normal start path. On a fresh legacy retry, probe state
            // without waiting again before providing the PCM needed to make progress.
            result = waitForStreamStart(mAudioStream, attempt == 0 || !isLegacy ? timeoutNanos : 0);
            if (result == AAUDIO_ERROR_TIMEOUT && isLegacy)
            {
                IMLOGD0("[startAudioStream] priming legacy output");
                result = primeLegacyStream(mAudioStream);
                if (result == AAUDIO_OK)
                {
                    result = waitForStreamStart(mAudioStream, AAUDIO_STATE_TIMEOUT_NANO);
                }
            }
        }
        if (result == AAUDIO_OK)
        {
            IMLOGI0("[startAudioStream] stream started");
            return true;
        }

        IMLOGE1("[startAudioStream] Error starting stream[%s]", AAudio_convertResultToText(result));
        closeAudioStream();
        if (attempt != 0 || !isLegacy || result != AAUDIO_ERROR_TIMEOUT)
        {
            break;
        }
        // Retry a stalled legacy start once on a fresh track, retaining the codec.
        openAudioStream();
    }
    return false;
}

void ImsMediaAudioPlayer::closeAudioStream()
{
    AAudioStream_close(mAudioStream);
    mAudioStream = nullptr;
}

void ImsMediaAudioPlayer::restartAudioStream()
{
    ImsMediaMutex::Autolock lock(mMutex);

    if (mAudioStream == nullptr)
    {
        return;
    }

    AAudioStream_requestStop(mAudioStream);
    closeAudioStream();
    openAudioStream();
    startAudioStream(3 * AAUDIO_STATE_TIMEOUT_NANO);
}

void ImsMediaAudioPlayer::audioErrorCallback(
        AAudioStream* stream, void* userData, aaudio_result_t error)
{
    if (stream == nullptr || userData == nullptr)
    {
        return;
    }

    aaudio_stream_state_t streamState = AAudioStream_getState(stream);
    IMLOGW2("[errorCallback] error[%s], state[%d]", AAudio_convertResultToText(error), streamState);

    if (error == AAUDIO_ERROR_DISCONNECTED)
    {
        // Handle stream restart on a separate thread
        std::thread streamRestartThread(&ImsMediaAudioPlayer::restartAudioStream,
                reinterpret_cast<ImsMediaAudioPlayer*>(userData));
        streamRestartThread.detach();
    }
}
