#include "wav_writer.hpp"

#include "nokomo/nokomo.hpp"

#include <cstring>
#include <vector>

namespace nokomo::detail {

namespace {

void put16(std::vector<uint8_t>& v, uint16_t x)
{
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)(x >> 8));
}

void put32(std::vector<uint8_t>& v, uint32_t x)
{
    v.push_back((uint8_t)(x & 0xFF));
    v.push_back((uint8_t)((x >> 8) & 0xFF));
    v.push_back((uint8_t)((x >> 16) & 0xFF));
    v.push_back((uint8_t)((x >> 24) & 0xFF));
}

void putTag(std::vector<uint8_t>& v, const char* tag)
{
    v.insert(v.end(), tag, tag + 4);
}

bool writeAt(HANDLE h, uint64_t offset, const void* data, DWORD size)
{
    LARGE_INTEGER pos;
    pos.QuadPart = (LONGLONG)offset;
    DWORD written = 0;
    return SetFilePointerEx(h, pos, nullptr, FILE_BEGIN) &&
           WriteFile(h, data, size, &written, nullptr) && written == size;
}

constexpr uint32_t kDs64BodySize = 28; // riffSize(8)+dataSize(8)+sampleCount(8)+tableLength(4)
constexpr uint64_t kSizeLimit32 = 0xFFFFFFFEull; // 32bit サイズフィールドに収まる上限

} // namespace

bool WavWriter::open(const std::wstring& path, uint32_t sampleRate, uint32_t channels,
                     int32_t bitsPerSample, uint32_t channelMask, bool forceRf64)
{
    close();
    isFloat_ = bitsPerSample < 0;
    forceRf64_ = forceRf64;
    wroteRf64_ = false;
    uint32_t bits = (uint32_t)(isFloat_ ? -bitsPerSample : bitsPerSample);
    bytesPerFrame_ = bits / 8 * channels;
    if (bytesPerFrame_ == 0 || sampleRate == 0 || channels > 0xFFFF) return false;
    const uint32_t stdMask = nokomo::defaultChannelMask(channels);
    if (channelMask == 0) channelMask = stdMask;
    const bool extensible = channels > 2 || channelMask != stdMask;

    h_ = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                     FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h_ == INVALID_HANDLE_VALUE) return false;

    std::vector<uint8_t> hdr;
    putTag(hdr, "RIFF");
    put32(hdr, 0); // close 時に確定
    putTag(hdr, "WAVE");
    // RF64 昇格用プレースホルダ。ds64 は WAVE 直後の先頭チャンクでなければならない
    junkOffset_ = (uint32_t)hdr.size();
    putTag(hdr, "JUNK");
    put32(hdr, kDs64BodySize);
    for (uint32_t i = 0; i < kDs64BodySize; ++i) hdr.push_back(0);
    putTag(hdr, "fmt ");
    if (extensible) {
        put32(hdr, 40);
        put16(hdr, 0xFFFE); // WAVE_FORMAT_EXTENSIBLE
    } else if (isFloat_) {
        put32(hdr, 18);
        put16(hdr, 3); // WAVE_FORMAT_IEEE_FLOAT
    } else {
        put32(hdr, 16);
        put16(hdr, 1); // WAVE_FORMAT_PCM
    }
    put16(hdr, (uint16_t)channels);
    put32(hdr, sampleRate);
    put32(hdr, sampleRate * bytesPerFrame_);
    put16(hdr, (uint16_t)bytesPerFrame_);
    put16(hdr, (uint16_t)bits);
    if (extensible) {
        put16(hdr, 22);                // cbSize
        put16(hdr, (uint16_t)bits);    // wValidBitsPerSample
        put32(hdr, channelMask);       // dwChannelMask
        // SubFormat: KSDATAFORMAT_SUBTYPE_PCM / _IEEE_FLOAT
        // {0000000X-0000-0010-8000-00AA00389B71}
        static const uint8_t kGuidTail[12] = {0x00, 0x00, 0x10, 0x00, 0x80, 0x00,
                                              0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        put32(hdr, isFloat_ ? 3 : 1);
        hdr.insert(hdr.end(), kGuidTail, kGuidTail + 12);
    } else if (isFloat_) {
        put16(hdr, 0); // cbSize
    }
    if (isFloat_) {
        putTag(hdr, "fact");
        put32(hdr, 4);
        factOffset_ = (uint32_t)hdr.size();
        put32(hdr, 0); // 総サンプル数(close 時に確定)
    }
    putTag(hdr, "data");
    dataSizeOffset_ = (uint32_t)hdr.size();
    put32(hdr, 0); // close 時に確定

    DWORD written = 0;
    if (!WriteFile(h_, hdr.data(), (DWORD)hdr.size(), &written, nullptr) ||
        written != hdr.size()) {
        close();
        return false;
    }
    dataBytes_ = 0;
    framesWritten_ = 0;
    return true;
}

size_t WavWriter::write(const void* data, size_t frames)
{
    if (h_ == INVALID_HANDLE_VALUE || frames == 0) return 0;
    // 一度の WriteFile は DWORD 上限があるので必要なら分割する
    const uint8_t* p = (const uint8_t*)data;
    uint64_t remain = (uint64_t)frames * bytesPerFrame_;
    while (remain > 0) {
        DWORD chunk = remain > 0x40000000 ? 0x40000000 : (DWORD)remain; // 1GiB 単位
        DWORD written = 0;
        if (!WriteFile(h_, p, chunk, &written, nullptr)) break;
        dataBytes_ += written;
        p += written;
        remain -= written;
        if (written < chunk) break; // ディスクフル等
    }
    uint64_t wroteBytes = (uint64_t)frames * bytesPerFrame_ - remain;
    size_t wroteFrames = (size_t)(wroteBytes / bytesPerFrame_);
    framesWritten_ += wroteFrames;
    return wroteFrames;
}

bool WavWriter::close()
{
    if (h_ == INVALID_HANDLE_VALUE) return true;
    bool ok = true;

    // RIFF チャンクはワード境界(dataBytes が奇数ならパディング 1 バイト)
    uint64_t pad = dataBytes_ & 1;
    if (pad) {
        uint8_t zero = 0;
        DWORD written = 0;
        ok = WriteFile(h_, &zero, 1, &written, nullptr) && written == 1;
    }

    const uint64_t headerBytes = dataSizeOffset_ + 4;
    const uint64_t riffSize = headerBytes + dataBytes_ + pad - 8;
    const bool rf64 = forceRf64_ || riffSize > kSizeLimit32 || dataBytes_ > kSizeLimit32;

    if (!rf64) {
        // 純 WAV として確定(JUNK チャンクはそのまま残す)
        uint32_t v = (uint32_t)riffSize;
        ok = writeAt(h_, 4, &v, 4) && ok;
        if (factOffset_) {
            v = (uint32_t)framesWritten_;
            ok = writeAt(h_, factOffset_, &v, 4) && ok;
        }
        v = (uint32_t)dataBytes_;
        ok = writeAt(h_, dataSizeOffset_, &v, 4) && ok;
    } else {
        // RF64 へ昇格: RIFF→RF64、32bit サイズは -1、JUNK→ds64 に実サイズを格納
        wroteRf64_ = true;
        std::vector<uint8_t> head;
        putTag(head, "RF64");
        put32(head, 0xFFFFFFFF);
        ok = writeAt(h_, 0, head.data(), (DWORD)head.size()) && ok;

        std::vector<uint8_t> ds64;
        putTag(ds64, "ds64");
        put32(ds64, kDs64BodySize);
        put32(ds64, (uint32_t)(riffSize & 0xFFFFFFFF));        // riffSize (64bit LE)
        put32(ds64, (uint32_t)(riffSize >> 32));
        put32(ds64, (uint32_t)(dataBytes_ & 0xFFFFFFFF));      // dataSize (64bit LE)
        put32(ds64, (uint32_t)(dataBytes_ >> 32));
        put32(ds64, (uint32_t)(framesWritten_ & 0xFFFFFFFF));  // sampleCount (64bit LE)
        put32(ds64, (uint32_t)(framesWritten_ >> 32));
        put32(ds64, 0);                                        // tableLength
        ok = writeAt(h_, junkOffset_, ds64.data(), (DWORD)ds64.size()) && ok;

        uint32_t v = 0xFFFFFFFF;
        if (factOffset_) ok = writeAt(h_, factOffset_, &v, 4) && ok;
        ok = writeAt(h_, dataSizeOffset_, &v, 4) && ok;
    }

    CloseHandle(h_);
    h_ = INVALID_HANDLE_VALUE;
    junkOffset_ = factOffset_ = dataSizeOffset_ = 0;
    return ok;
}

} // namespace nokomo::detail
