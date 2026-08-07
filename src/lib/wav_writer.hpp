#pragma once
// RIFF WAVE / RF64 ファイル出力。
// int(8/16/24/32bit) は WAVE_FORMAT_PCM、float(-32/-64) は WAVE_FORMAT_IEEE_FLOAT。
//
// 2GB/4GB 問題への対応(EBU Tech 3306 の定石):
//   書き始めは "RIFF" + ダミーの "JUNK" チャンク(ds64 と同サイズ)で純 WAV として
//   書き、close 時にデータが 4GiB 境界を超えていたら "RF64" + "ds64" に昇格する。
//   4GiB 未満のファイルは従来どおりの純粋な WAV になり互換性を失わない。

#include <windows.h>

#include <cstdint>
#include <string>

namespace nokomo::detail {

class WavWriter {
public:
    WavWriter() = default;
    ~WavWriter() { close(); }
    WavWriter(const WavWriter&) = delete;
    WavWriter& operator=(const WavWriter&) = delete;

    // forceRf64: サイズによらず常に RF64 で確定する(既定は自動判定)
    bool open(const std::wstring& path, uint32_t sampleRate, uint32_t channels,
              int32_t bitsPerSample, bool forceRf64 = false);
    // 戻り値は書き込めたフレーム数(ディスクフル等で frames より小さくなり得る)
    size_t write(const void* data, size_t frames);
    bool close(); // ヘッダのサイズを確定する(必要なら RF64 へ昇格)

    uint64_t framesWritten() const { return framesWritten_; }
    bool wroteRf64() const { return wroteRf64_; } // close 後に有効

private:
    HANDLE h_ = INVALID_HANDLE_VALUE;
    uint32_t bytesPerFrame_ = 0;
    uint64_t dataBytes_ = 0;
    uint64_t framesWritten_ = 0;
    bool isFloat_ = false;
    bool forceRf64_ = false;
    bool wroteRf64_ = false;
    uint32_t junkOffset_ = 0;     // "JUNK"(→ds64) チャンクの位置
    uint32_t factOffset_ = 0;     // fact チャンクのサンプル数の位置(float時)
    uint32_t dataSizeOffset_ = 0; // "data" チャンクサイズの位置
};

} // namespace nokomo::detail
