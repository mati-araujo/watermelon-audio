#include "DelayLine.h"

DelayLine::DelayLine(float maxDelayMs, float sampleRate)
    : mSampleRate(sampleRate) {
    setMaxDelay(maxDelayMs);
}

// Move constructor
DelayLine::DelayLine(DelayLine&& other) noexcept
    : mBuffer(std::move(other.mBuffer)),
      mWritePos(other.mWritePos.load(std::memory_order_acquire)),
      mSampleRate(other.mSampleRate) {
    // Reset the source object
    other.mWritePos.store(0, std::memory_order_release);
    other.mSampleRate = 48000.0f;
}

// Move assignment operator
DelayLine& DelayLine::operator=(DelayLine&& other) noexcept {
    if (this != &other) {
        mBuffer = std::move(other.mBuffer);
        mWritePos.store(other.mWritePos.load(std::memory_order_acquire), std::memory_order_release);
        mSampleRate = other.mSampleRate;

        // Reset the source object
        other.mWritePos.store(0, std::memory_order_release);
        other.mSampleRate = 48000.0f;
    }
    return *this;
}

void DelayLine::setSampleRate(float sampleRate) {
    if (sampleRate <= 0.0f) {
        return;
    }

    float maxDelayMs = getMaxDelayMs();
    mSampleRate = sampleRate;

    // Recalculate buffer size based on new sample rate
    setMaxDelay(maxDelayMs);
}

void DelayLine::setMaxDelay(float maxDelayMs) {
    if (maxDelayMs <= 0.0f) {
        return;
    }

    int newSize = DSPMath::msToSamples(maxDelayMs, mSampleRate);
    newSize = std::max(1, newSize);  // Minimum 1 sample

    mBuffer.clear();
    mBuffer.resize(newSize, 0.0f);
    mWritePos.store(0, std::memory_order_release);
}

void DelayLine::write(float input) {
    int bufferSize = static_cast<int>(mBuffer.size());
    if (bufferSize == 0) {
        return;
    }

    // Get current write position
    int pos = mWritePos.load(std::memory_order_acquire);

    // Write sample
    mBuffer[pos] = input;

    // Increment and wrap write position
    int newPos = (pos + 1) % bufferSize;
    mWritePos.store(newPos, std::memory_order_release);
}

float DelayLine::read(int delaySamples) const {
    int bufferSize = static_cast<int>(mBuffer.size());
    if (bufferSize == 0 || delaySamples < 0) {
        return 0.0f;
    }

    // Clamp delay to buffer size
    delaySamples = std::min(delaySamples, bufferSize - 1);

    int readPos = calculateReadPos(delaySamples);
    return getSample(readPos);
}

float DelayLine::readInterpolated(float delaySamples, Interpolation interpolation) const {
    int bufferSize = static_cast<int>(mBuffer.size());
    if (bufferSize == 0 || delaySamples < 0.0f) {
        return 0.0f;
    }

    // Clamp delay to buffer size
    delaySamples = std::min(delaySamples, static_cast<float>(bufferSize - 1));

    switch (interpolation) {
        case Interpolation::NONE: {
            // Round to nearest integer
            int delay = static_cast<int>(delaySamples + 0.5f);
            return read(delay);
        }

        case Interpolation::LINEAR: {
            // Linear interpolation between two samples
            int delay1 = static_cast<int>(std::floor(delaySamples));
            int delay2 = delay1 + 1;
            float frac = delaySamples - static_cast<float>(delay1);

            // Clamp delay2
            if (delay2 >= bufferSize) {
                delay2 = bufferSize - 1;
                frac = 0.0f;
            }

            float sample1 = read(delay1);
            float sample2 = read(delay2);

            return DSPMath::lerp(sample1, sample2, frac);
        }

        case Interpolation::CUBIC: {
            // Cubic interpolation (Hermite spline) - 4 points
            int delay1 = static_cast<int>(std::floor(delaySamples));
            float frac = delaySamples - static_cast<float>(delay1);

            // Get 4 samples: y0 (n-1), y1 (n), y2 (n+1), y3 (n+2)
            int delay0 = delay1 - 1;
            int delay2 = delay1 + 1;
            int delay3 = delay1 + 2;

            // Clamp to valid range
            delay0 = std::max(0, delay0);
            delay1 = std::max(0, delay1);
            delay2 = std::min(delay2, bufferSize - 1);
            delay3 = std::min(delay3, bufferSize - 1);

            float y0 = read(delay0);
            float y1 = read(delay1);
            float y2 = read(delay2);
            float y3 = read(delay3);

            return DSPMath::cubicInterpolate(y0, y1, y2, y3, frac);
        }

        default:
            return 0.0f;
    }
}

float DelayLine::readMs(float delayMs, Interpolation interpolation) const {
    float delaySamples = DSPMath::msToSamples(delayMs, mSampleRate);
    return readInterpolated(delaySamples, interpolation);
}

float DelayLine::readMultiTap(const int* delaySamples, const float* gains, int numTaps) const {
    float output = 0.0f;

    for (int i = 0; i < numTaps; ++i) {
        output += read(delaySamples[i]) * gains[i];
    }

    return output;
}

float DelayLine::process(float input, float delaySamples, Interpolation interpolation) {
    float output = readInterpolated(delaySamples, interpolation);
    write(input);
    return output;
}

void DelayLine::clear() {
    std::fill(mBuffer.begin(), mBuffer.end(), 0.0f);
}

float DelayLine::getMaxDelayMs() const {
    return DSPMath::samplesToMs(static_cast<int>(mBuffer.size()), mSampleRate);
}

int DelayLine::calculateReadPos(int delaySamples) const {
    int bufferSize = static_cast<int>(mBuffer.size());
    if (bufferSize == 0) {
        return 0;
    }

    int writePos = mWritePos.load(std::memory_order_acquire);

    // Calculate read position: writePos - delay
    int readPos = writePos - delaySamples;

    // Wrap to valid range using modulo
    readPos = DSPMath::wrapIndex(readPos, bufferSize);

    return readPos;
}

float DelayLine::getSample(int position) const {
    int bufferSize = static_cast<int>(mBuffer.size());
    if (bufferSize == 0) {
        return 0.0f;
    }

    // Bounds check
    if (position < 0 || position >= bufferSize) {
        position = DSPMath::wrapIndex(position, bufferSize);
    }

    return mBuffer[position];
}
