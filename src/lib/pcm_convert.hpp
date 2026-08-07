#pragma once
// PCM サンプル形式ユーティリティ。
// KPI のビット数表現(8/16/24/32=int, -32/-64=float)を扱う。
// 8bit は WAV と同じく unsigned(中心 128)。

#include <cstdint>
#include <cstring>

namespace nokomo::detail {

inline double readSampleAsDouble(const uint8_t* p, int32_t bits)
{
    switch (bits) {
    case 8:
        return ((int)*p - 128) / 128.0;
    case 16: {
        int16_t v;
        memcpy(&v, p, 2);
        return v / 32768.0;
    }
    case 24: {
        int32_t v = (int32_t)((uint32_t)p[0] << 8 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 24) >> 8;
        return v / 8388608.0;
    }
    case 32: {
        int32_t v;
        memcpy(&v, p, 4);
        return v / 2147483648.0;
    }
    case -32: {
        float v;
        memcpy(&v, p, 4);
        return v;
    }
    case -64: {
        double v;
        memcpy(&v, p, 8);
        return v;
    }
    default:
        return 0.0;
    }
}

inline void writeSampleFromDouble(uint8_t* p, int32_t bits, double v)
{
    switch (bits) {
    case 8: {
        double s = v * 128.0 + 128.0;
        if (s < 0.0) s = 0.0;
        if (s > 255.0) s = 255.0;
        *p = (uint8_t)s;
        break;
    }
    case 16: {
        double s = v * 32768.0;
        if (s < -32768.0) s = -32768.0;
        if (s > 32767.0) s = 32767.0;
        int16_t o = (int16_t)s;
        memcpy(p, &o, 2);
        break;
    }
    case 24: {
        double s = v * 8388608.0;
        if (s < -8388608.0) s = -8388608.0;
        if (s > 8388607.0) s = 8388607.0;
        int32_t o = (int32_t)s;
        p[0] = (uint8_t)(o & 0xFF);
        p[1] = (uint8_t)((o >> 8) & 0xFF);
        p[2] = (uint8_t)((o >> 16) & 0xFF);
        break;
    }
    case 32: {
        double s = v * 2147483648.0;
        if (s < -2147483648.0) s = -2147483648.0;
        if (s > 2147483647.0) s = 2147483647.0;
        int32_t o = (int32_t)s;
        memcpy(p, &o, 4);
        break;
    }
    case -32: {
        float o = (float)v;
        memcpy(p, &o, 4);
        break;
    }
    case -64: {
        memcpy(p, &v, 8);
        break;
    }
    default:
        break;
    }
}

inline uint32_t bytesPerSampleOf(int32_t bits)
{
    return (uint32_t)(bits < 0 ? -bits : bits) / 8;
}

// サンプル列の形式変換(サンプル数 = フレーム数 × チャネル数)
inline void convertSamples(uint8_t* dst, int32_t dstBits,
                           const uint8_t* src, int32_t srcBits, size_t samples)
{
    const uint32_t sb = bytesPerSampleOf(srcBits);
    const uint32_t db = bytesPerSampleOf(dstBits);
    for (size_t i = 0; i < samples; ++i) {
        writeSampleFromDouble(dst + i * db, dstBits, readSampleAsDouble(src + i * sb, srcBits));
    }
}

// ゲイン適用(フェードアウト用)。インターリーブされた frames × channels を処理する。
// gain は各フレームで gainStart + gainStep * frameIndex。
inline void applyGainFrames(uint8_t* buf, int32_t bits, uint32_t channels,
                            size_t frames, double gainStart, double gainStep)
{
    const uint32_t bs = bytesPerSampleOf(bits);
    for (size_t f = 0; f < frames; ++f) {
        double gain = gainStart + gainStep * (double)f;
        if (gain < 0.0) gain = 0.0;
        uint8_t* p = buf + f * bs * channels;
        for (uint32_t c = 0; c < channels; ++c, p += bs) {
            writeSampleFromDouble(p, bits, readSampleAsDouble(p, bits) * gain);
        }
    }
}

} // namespace nokomo::detail
