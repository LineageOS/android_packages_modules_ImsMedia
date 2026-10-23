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
#include <algorithm>

#include <ImsMediaDefine.h>
#include <ImsMediaTrace.h>
#include <ImsMediaTimer.h>
#include <ImsMediaAudioUtil.h>
#include <ImsMediaAudioPlayer.h>
#include <utils/Errors.h>

#define AAUDIO_STATE_TIMEOUT_NANO (100 * 1000000L)
#define DEFAULT_SAMPLING_RATE     (8000)
#define CODEC_TIMEOUT_NANO        (100000)
#define PLAYBACK_TARGET_DELAY_MS  (40)
#define PLAYBACK_MAX_DELAY_MS     (80)

using namespace android;

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
    mAudioStreamRestartTimeUs = 0;
    mFifoReadIndex = 0;
    mFifoWriteIndex = 0;
    mFifoTargetFrames = 0;
    mFifoMaxFrames = 0;
    mFifoPrerollFramesRemaining = 0;
    mFifoWaitingForData = true;
}

ImsMediaAudioPlayer::~ImsMediaAudioPlayer()
{
    Stop();
}

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
    if (!stop_l())
        return false;

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

    if (!openAudioStream())
    {
        IMLOGE0("[Start] create audio stream failed");
        stop_l();
        return false;
    }

    IMLOGD1("[Start] Creating codec[%s]", kMimeType);

    if (mCodecType == kAudioCodecAmr || mCodecType == kAudioCodecAmrWb)
    {
        mFormat = AMediaFormat_new();
        if (mFormat == nullptr)
        {
            stop_l();
            return false;
        }
        AMediaFormat_setString(mFormat, AMEDIAFORMAT_KEY_MIME, kMimeType);
        AMediaFormat_setInt32(mFormat, AMEDIAFORMAT_KEY_SAMPLE_RATE, mSamplingRate);
        AMediaFormat_setInt32(mFormat, AMEDIAFORMAT_KEY_CHANNEL_COUNT, 1);

        mCodec = AMediaCodec_createDecoderByType(kMimeType);

        if (mCodec == nullptr)
        {
            IMLOGE1("[Start] unable to create %s codec instance", kMimeType);
            stop_l();
            return false;
        }

        IMLOGD0("[Start] configure codec");
        media_status_t codecResult = AMediaCodec_configure(mCodec, mFormat, nullptr, nullptr, 0);

        if (codecResult != AMEDIA_OK)
        {
            IMLOGE2("[Start] unable to configure[%s] codec - err[%d]", kMimeType, codecResult);
            stop_l();
            return false;
        }
    }

    aaudio_stream_state_t inputState = AAUDIO_STREAM_STATE_STARTING;
    aaudio_stream_state_t nextState = AAUDIO_STREAM_STATE_UNINITIALIZED;
    auto result = AAudioStream_requestStart(mAudioStream);

    if (result != AAUDIO_OK)
    {
        IMLOGE1("[Start] Error start stream[%s]", AAudio_convertResultToText(result));
        stop_l();
        return false;
    }

    result = AAudioStream_waitForStateChange(
            mAudioStream, inputState, &nextState, AAUDIO_STATE_TIMEOUT_NANO);

    if (result != AAUDIO_OK || nextState != AAUDIO_STREAM_STATE_STARTED)
    {
        IMLOGE2("[Start] Unable to start stream: result[%d], state[%d]", result, nextState);
        stop_l();
        return false;
    }

    IMLOGI1("[Start] start stream state[%s]", AAudio_convertStreamStateToText(nextState));

    if (mCodecType == kAudioCodecAmr || mCodecType == kAudioCodecAmrWb)
    {
        media_status_t codecResult = AMediaCodec_start(mCodec);
        if (codecResult != AMEDIA_OK)
        {
            IMLOGE1("[Start] unable to start codec - err[%d]", codecResult);
            stop_l();
            return false;
        }
    }

    mAudioStreamRestartTimeUs = 0;
    IMLOGD0("[Start] exit");
    return true;
}

void ImsMediaAudioPlayer::Stop()
{
    IMLOGD0("[Stop] enter");
    ImsMediaMutex::Autolock lock(mMutex);
    stop_l();
    IMLOGD0("[Stop] exit");
}

bool ImsMediaAudioPlayer::stop_l()
{
    mAudioStreamRestartTimeUs = 0;
    if (!closeAudioStream_l())
        return false;

    if (mCodec != nullptr)
    {
        AMediaCodec_stop(mCodec);
        AMediaCodec_delete(mCodec);
        mCodec = nullptr;
    }
    if (mFormat != nullptr)
    {
        AMediaFormat_delete(mFormat);
        mFormat = nullptr;
    }
    return true;
}

bool ImsMediaAudioPlayer::closeAudioStream_l()
{
    if (mAudioStream == nullptr)
        return true;

    const aaudio_result_t result = AAudioStream_close(mAudioStream);
    if (result != AAUDIO_OK)
    {
        IMLOGE1("[closeAudioStream] Error closing stream[%s]", AAudio_convertResultToText(result));
        return false;
    }
    mAudioStream = nullptr;
    return true;
}

bool ImsMediaAudioPlayer::onDataFrame(uint8_t* buffer, uint32_t size, FrameType frameType,
        bool /*hasNextFrame*/, uint8_t /*nextFrameByte*/)
{
    ImsMediaMutex::Autolock lock(mMutex);

    uint8_t noDataFrame = 0x7c;  // AMR NO_DATA (FT=15).
    if ((mCodecType == kAudioCodecAmr || mCodecType == kAudioCodecAmrWb) &&
            (frameType == NO_DATA || frameType == LOST) && buffer == nullptr && size == 0)
    {
        buffer = &noDataFrame;
        size = sizeof(noDataFrame);
    }

    if (size == 0 || buffer == nullptr)
    {
        return false;
    }

    aaudio_stream_state_t state = mAudioStream == nullptr
            ? AAUDIO_STREAM_STATE_UNINITIALIZED : AAudioStream_getState(mAudioStream);
    if (state == AAUDIO_STREAM_STATE_DISCONNECTED || mAudioStreamRestartTimeUs != 0)
    {
        const uint64_t now = ImsMediaTimer::GetTimeInMicroSeconds();
        if (now < mAudioStreamRestartTimeUs)
            return false;
        mAudioStreamRestartTimeUs = now + 500000;
        if (!restartAudioStream_l())
            return false;
        mAudioStreamRestartTimeUs = 0;
        state = AAudioStream_getState(mAudioStream);
    }
    if (state != AAUDIO_STREAM_STATE_STARTED)
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
        if (size < MAX_PCM_SIZE * sizeof(int16_t))
            return false;
        memcpy(mBuffer, buffer, (MAX_PCM_SIZE * 2));
        queueFrames(reinterpret_cast<int16_t*>(mBuffer), MAX_PCM_SIZE);
    }
    return false;
}

bool ImsMediaAudioPlayer::queueFrames(const int16_t* data, uint32_t numFrames)
{
    if (numFrames == 0)
        return true;
    if (data == nullptr)
        return false;

    const uint32_t writeIndex = mFifoWriteIndex.load(std::memory_order_relaxed);
    const uint32_t queuedFrames = writeIndex - mFifoReadIndex.load(std::memory_order_acquire);
    if (queuedFrames > kFifoSize || numFrames > kFifoSize - queuedFrames)
    {
        return false;
    }

    const uint32_t offset = writeIndex % kFifoSize;
    const uint32_t first = std::min(numFrames, kFifoSize - offset);
    memcpy(mFifo + offset, data, first * sizeof(int16_t));
    memcpy(mFifo, reinterpret_cast<const uint8_t*>(data) + first * sizeof(int16_t),
            (numFrames - first) * sizeof(int16_t));
    mFifoWriteIndex.store(writeIndex + numFrames, std::memory_order_release);
    return true;
}

aaudio_data_callback_result_t ImsMediaAudioPlayer::audioDataCallback(
        AAudioStream* /*stream*/, void* userData, void* audioData, int32_t numFrames)
{
    auto* player = static_cast<ImsMediaAudioPlayer*>(userData);
    auto* output = static_cast<int16_t*>(audioData);
    if (numFrames <= 0)
        return AAUDIO_CALLBACK_RESULT_CONTINUE;

    memset(output, 0, numFrames * sizeof(int16_t));
    uint32_t readIndex = player->mFifoReadIndex.load(std::memory_order_relaxed);
    const uint32_t writeIndex = player->mFifoWriteIndex.load(std::memory_order_acquire);
    uint32_t queuedFrames = writeIndex - readIndex;
    if (queuedFrames > player->mFifoMaxFrames)
    {
        // Only the consumer discards old samples, after they are no longer in use.
        readIndex = writeIndex - player->mFifoTargetFrames;
        queuedFrames = player->mFifoTargetFrames;
        player->mFifoPrerollFramesRemaining = 0;
        player->mFifoWaitingForData = false;
    }

    if (player->mFifoWaitingForData && queuedFrames > 0)
    {
        player->mFifoPrerollFramesRemaining = player->mFifoTargetFrames;
        player->mFifoWaitingForData = false;
    }

    // Count rendered silence so a short fragment never waits for more input.
    const uint32_t silenceFrames = std::min<uint32_t>(
            player->mFifoPrerollFramesRemaining, numFrames);
    player->mFifoPrerollFramesRemaining -= silenceFrames;
    const uint32_t framesToCopy = std::min<uint32_t>(queuedFrames, numFrames - silenceFrames);
    for (uint32_t i = 0; i < framesToCopy; i++)
    {
        output[silenceFrames + i] = player->mFifo[(readIndex + i) % kFifoSize];
    }
    player->mFifoReadIndex.store(readIndex + framesToCopy, std::memory_order_release);
    if (framesToCopy < static_cast<uint32_t>(numFrames) - silenceFrames)
    {
        player->mFifoWaitingForData = true;
    }
    return AAUDIO_CALLBACK_RESULT_CONTINUE;
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
            if (buf != nullptr && info.size % sizeof(int16_t) == 0 &&
                    static_cast<size_t>(info.size) <= buffCapacity)
            {
                queueFrames(reinterpret_cast<const int16_t*>(buf), info.size / 2);
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

    queueFrames(reinterpret_cast<int16_t*>(output), decodeSize / 2);
    memset(output, 0, PCM_BUFFER_SIZE);

    return true;
}

bool ImsMediaAudioPlayer::openAudioStream()
{
    AAudioStreamBuilder* builder = nullptr;
    aaudio_result_t result = AAudio_createStreamBuilder(&builder);

    if (result != AAUDIO_OK)
    {
        IMLOGE1("[openAudioStream] Error creating stream builder[%s]",
                AAudio_convertResultToText(result));
        return false;
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
    AAudioStreamBuilder_setDataCallback(builder, audioDataCallback, this);

    // open stream
    result = AAudioStreamBuilder_openStream(builder, &mAudioStream);
    AAudioStreamBuilder_delete(builder);

    if (result != AAUDIO_OK || mAudioStream == nullptr)
    {
        IMLOGE1("[openAudioStream] Failed to openStream. Error[%s]",
                AAudio_convertResultToText(result));
        closeAudioStream_l();
        return false;
    }
    const int32_t sampleRate = AAudioStream_getSampleRate(mAudioStream);
    if (sampleRate <= 0 || AAudioStream_getChannelCount(mAudioStream) != 1 ||
            AAudioStream_getFormat(mAudioStream) != AAUDIO_FORMAT_PCM_I16)
    {
        IMLOGE0("[openAudioStream] Invalid PCM configuration");
        closeAudioStream_l();
        return false;
    }

    mFifoMaxFrames = std::clamp<uint64_t>(
            static_cast<uint64_t>(sampleRate) * PLAYBACK_MAX_DELAY_MS / 1000, 2, kFifoSize / 2);
    mFifoTargetFrames = std::clamp<uint64_t>(
            static_cast<uint64_t>(sampleRate) * PLAYBACK_TARGET_DELAY_MS / 1000, 1,
            mFifoMaxFrames / 2);
    mFifoReadIndex = 0;
    mFifoWriteIndex = 0;
    mFifoPrerollFramesRemaining = 0;
    mFifoWaitingForData = true;
    IMLOGI3("[openAudioStream] PCM queue rate[%d] target[%u] maximum[%u]", sampleRate,
            mFifoTargetFrames, mFifoMaxFrames);
    return true;
}

bool ImsMediaAudioPlayer::restartAudioStream_l()
{
    if (!closeAudioStream_l() || !openAudioStream())
        return false;

    aaudio_stream_state_t nextState = AAUDIO_STREAM_STATE_UNINITIALIZED;
    aaudio_result_t result = AAudioStream_requestStart(mAudioStream);
    if (result == AAUDIO_OK)
    {
        result = AAudioStream_waitForStateChange(mAudioStream, AAUDIO_STREAM_STATE_STARTING,
                &nextState, 3 * AAUDIO_STATE_TIMEOUT_NANO);
    }
    if (result == AAUDIO_OK && nextState == AAUDIO_STREAM_STATE_STARTED)
    {
        IMLOGI1("[restartAudioStream] start stream state[%s]",
                AAudio_convertStreamStateToText(nextState));
        return true;
    }

    IMLOGE2("[restartAudioStream] Unable to start stream: result[%d], state[%d]",
            result, nextState);
    closeAudioStream_l();
    return false;
}
