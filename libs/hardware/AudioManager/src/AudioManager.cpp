#include "AudioManager.h"

#include <BoardConfig.h>

// Capability-gated like FrontlightManager: devices without FREEINK_CAP_AUDIO
// compile the stub bodies at the bottom and link no I2S/codec code.
#if FREEINK_CAP_AUDIO

#include <Wire.h>
#include <driver/i2s_std.h>

#include <memory>

// Opt-in MP3 decode (see AudioManager.h and BoardConfig.h's FREEINK_CAP_MP3).
// mp3dec.h is the classic Helix fixed-point decoder's public C API, as
// bundled by libhelix-mp3 ports (e.g. ESP8266Audio and its forks) — add one
// to lib_deps to build with -DFREEINK_MP3_HELIX=1.
#if FREEINK_CAP_MP3
#include <mp3dec.h>
#endif

namespace freeink {

namespace {

constexpr uint32_t CODEC_I2C_HZ = 100000;  // OEM bus speed (shared with touch)
constexpr size_t READ_CHUNK = 1024;        // mono source bytes per loop pass

// ES8388 playback init recovered from the Murphy OEM firmware — exact register
// order matters (staged mute -> clocks/format -> mixers -> power -> unmute).
struct RegVal {
  uint8_t reg, val;
};
constexpr RegVal ES8388_INIT[] = {
    {0x19, 0x04}, {0x01, 0x50}, {0x02, 0x00}, {0x08, 0x00}, {0x04, 0x3e}, {0x00, 0x12},
    {0x17, 0x18}, {0x18, 0x02}, {0x26, 0x1b}, {0x27, 0x90}, {0x2a, 0x90}, {0x2b, 0x80},
    {0x2d, 0x00}, {0x1b, 0x00}, {0x1a, 0x00}, {0x03, 0xff}, {0x09, 0x88}, {0x0a, 0xf0},
    {0x0b, 0x80}, {0x0c, 0x0e}, {0x0d, 0x02}, {0x10, 0x20}, {0x11, 0x20}, {0x2e, 0x1e},
    {0x2f, 0x1e}, {0x30, 0x1e}, {0x31, 0x1e}, {0x04, 0x3c}, {0x19, 0x00},
};

constexpr uint8_t ES8388_VOL_MAX_REG = 0x21;  // register full-scale for OUT volumes

// ES8311 playback init, mirroring M5Unified's PaperColor speaker bring-up
// (_speaker_enabled_cb_papercolor). The codec derives its internal MCLK from
// BCLK (reg 0x01 bit7 + reg 0x02 MULT_PRE=3, i.e. 32*fs * 8 = 256*fs), so no
// MCLK line is needed and the same init covers every sample rate. The 16-bit
// I2S SDP format (0x09) is set explicitly — M5Unified relies on the default.
constexpr RegVal ES8311_INIT[] = {
    {0x00, 0x80},  // RESET: CSM power on, slave mode
    {0x01, 0xB5},  // CLK_MANAGER: internal MCLK from BCLK pin, clocks on
    {0x02, 0x18},  // CLK_MANAGER: MULT_PRE x8 -> internal MCLK = 256*fs
    {0x09, 0x0C},  // SDP-in: I2S format, 16-bit
    {0x0D, 0x01},  // SYSTEM: power up analog circuitry
    {0x12, 0x00},  // SYSTEM: power up DAC
    {0x13, 0x10},  // SYSTEM: enable output to HP drive
    {0x32, 0xCF},  // DAC volume +16 dB (M5's default for the 1W speaker)
    {0x37, 0x08},  // DAC: bypass equalizer
};

constexpr uint8_t ES8311_VOL_MAX_REG = 0xCF;  // +16 dB, 0.5 dB/step (0xBF = 0 dB)

uint32_t readLE32(const uint8_t* p) {
  return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
uint16_t readLE16(const uint8_t* p) { return (uint16_t)p[0] | ((uint16_t)p[1] << 8); }

}  // namespace

bool AudioManager::present() const {
  return BoardConfig::ACTIVE.audio.output == BoardConfig::AudioOutput::I2sEs8388 ||
         BoardConfig::ACTIVE.audio.output == BoardConfig::AudioOutput::I2sEs8311 ||
         BoardConfig::ACTIVE.audio.output == BoardConfig::AudioOutput::I2sDac;
}

bool AudioManager::codecWrite(uint8_t reg, uint8_t value) {
  const auto& cfg = BoardConfig::ACTIVE.audio;
  Wire.beginTransmission(cfg.codecAddr);
  Wire.write(reg);
  Wire.write(value);
  return Wire.endTransmission() == 0;
}

bool AudioManager::codecInit() {
  const auto& cfg = BoardConfig::ACTIVE.audio;
  if (cfg.codecAddr == 0) return true;  // plain I2S DAC, nothing to configure

  const RegVal* seq;
  size_t seqLen;
  if (cfg.output == BoardConfig::AudioOutput::I2sEs8311) {
    seq = ES8311_INIT;
    seqLen = sizeof(ES8311_INIT) / sizeof(ES8311_INIT[0]);
  } else {
    seq = ES8388_INIT;
    seqLen = sizeof(ES8388_INIT) / sizeof(ES8388_INIT[0]);
  }

  Wire.begin(cfg.codecSda, cfg.codecScl, CODEC_I2C_HZ);

  // The OEM firmware retries until the codec ACKs; three attempts is plenty
  // for a codec already powered.
  for (int attempt = 0; attempt < 3; ++attempt) {
    bool ok = true;
    for (size_t i = 0; i < seqLen; ++i) {
      if (!codecWrite(seq[i].reg, seq[i].val)) {
        ok = false;
        break;
      }
    }
    if (ok) return true;
    delay(100);
  }
  return false;
}

bool AudioManager::begin() {
  if (begun_) return true;
  const auto& cfg = BoardConfig::ACTIVE.audio;
  if (!present()) return false;

  if (cfg.ampEnable != BoardConfig::PIN_UNASSIGNED) {
    pinMode(cfg.ampEnable, OUTPUT);
    digitalWrite(cfg.ampEnable, LOW);  // amp comes up only during playback
  }
  if (cfg.enable != BoardConfig::PIN_UNASSIGNED) {
    pinMode(cfg.enable, OUTPUT);
    digitalWrite(cfg.enable, cfg.enableActiveHigh ? HIGH : LOW);
    delay(10);  // codec rail ramp before the first I2C access
  }

  if (!codecInit()) {
    log_e("audio codec init failed");
    return false;
  }
  begun_ = true;
  return true;
}

void AudioManager::setVolume(uint8_t percent) {
  if (percent > 100) percent = 100;
  const auto& cfg = BoardConfig::ACTIVE.audio;
  if (cfg.codecAddr == 0) return;
  if (cfg.output == BoardConfig::AudioOutput::I2sEs8311) {
    // Single DAC volume register, 0.5 dB/step; full scale matches the init
    // value M5 ships for this speaker (+16 dB).
    codecWrite(0x32, (uint8_t)((uint16_t)percent * ES8311_VOL_MAX_REG / 100));
    return;
  }
  const uint8_t reg = (uint8_t)((uint16_t)percent * ES8388_VOL_MAX_REG / 100);
  // OUT1 (0x2e/0x2f) and OUT2 (0x30/0x31) pairs, like the OEM volume path.
  codecWrite(0x2e, reg);
  codecWrite(0x2f, reg);
  codecWrite(0x30, reg);
  codecWrite(0x31, reg);
}

// DAC mute between alarms so nothing residual reaches the output.
void AudioManager::codecMute(bool mute) {
  const auto& cfg = BoardConfig::ACTIVE.audio;
  if (cfg.codecAddr == 0) return;
  if (cfg.output == BoardConfig::AudioOutput::I2sEs8311) {
    codecWrite(0x31, mute ? 0x60 : 0x00);  // DAC_MUTE bits
  } else {
    codecWrite(0x19, mute ? 0x04 : 0x00);  // ES8388 DACCONTROL3 soft mute
  }
}

void AudioManager::setAmp(bool on) {
  const auto& cfg = BoardConfig::ACTIVE.audio;
  if (cfg.ampEnable == BoardConfig::PIN_UNASSIGNED) return;
  digitalWrite(cfg.ampEnable, on ? HIGH : LOW);
}

void AudioManager::powerDown() {
  const auto& cfg = BoardConfig::ACTIVE.audio;
  if (!begun_) return;
  stop();
  if (cfg.output == BoardConfig::AudioOutput::I2sEs8311) {
    // Like M5Unified's disable path: drop the amp and the codec rail.
    setAmp(false);
    if (cfg.enable != BoardConfig::PIN_UNASSIGNED) {
      digitalWrite(cfg.enable, cfg.enableActiveHigh ? LOW : HIGH);
    }
  } else if (cfg.codecAddr != 0) {
    codecWrite(0x02, 0xff);  // ES8388 CHIPPOWER: everything off
  }
  begun_ = false;
}

bool AudioManager::parseWavHeader(const WavSource& source, WavInfo& info) {
  uint8_t hdr[12];
  if (!source.seek(0)) return false;
  if (source.read(hdr, 12) != 12) return false;
  if (memcmp(hdr, "RIFF", 4) != 0 || memcmp(hdr + 8, "WAVE", 4) != 0) return false;

  size_t pos = 12;
  bool haveFmt = false;
  for (int guard = 0; guard < 32; ++guard) {
    uint8_t chunk[8];
    if (!source.seek(pos) || source.read(chunk, 8) != 8) return false;
    const uint32_t size = readLE32(chunk + 4);
    pos += 8;

    if (memcmp(chunk, "fmt ", 4) == 0) {
      uint8_t fmt[16];
      if (size < 16 || source.read(fmt, 16) != 16) return false;
      const uint16_t audioFormat = readLE16(fmt);
      info.channels = readLE16(fmt + 2);
      info.sampleRate = readLE32(fmt + 4);
      info.bitsPerSample = readLE16(fmt + 14);
      if (audioFormat != 1) return false;  // PCM only
      haveFmt = true;
    } else if (memcmp(chunk, "data", 4) == 0) {
      info.dataStart = pos;
      info.dataLength = size;
      break;
    }
    pos += size + (size & 1);  // chunks are word-aligned
  }

  return haveFmt && info.dataStart != 0 && info.bitsPerSample == 16 &&
         (info.channels == 1 || info.channels == 2) && info.sampleRate >= 8000 &&
         info.sampleRate <= 48000;
}

bool AudioManager::ensureI2s(uint32_t sampleRate) {
  const auto& cfg = BoardConfig::ACTIVE.audio;
  i2s_chan_handle_t tx = (i2s_chan_handle_t)txChan_;

  if (tx && currentRate_ == sampleRate) {
    // Channel exists but was disabled when the last playback drained.
    if (!chanEnabled_) {
      if (i2s_channel_enable(tx) != ESP_OK) return false;
      chanEnabled_ = true;
    }
    return true;
  }

  if (tx) {
    if (chanEnabled_) i2s_channel_disable(tx);
    chanEnabled_ = false;
    i2s_std_clk_config_t clk = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate);
    clk.mclk_multiple = I2S_MCLK_MULTIPLE_256;  // codec runs 256x MCLK/LRCK
    if (i2s_channel_reconfig_std_clock(tx, &clk) != ESP_OK) return false;
    if (i2s_channel_enable(tx) != ESP_OK) return false;
    chanEnabled_ = true;
    currentRate_ = sampleRate;
    return true;
  }

  i2s_chan_config_t chanCfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_0, I2S_ROLE_MASTER);
  // Without auto_clear the DMA replays its last buffers on underrun — heard
  // as a looping stutter after playback stops.
  chanCfg.auto_clear = true;
  if (i2s_new_channel(&chanCfg, &tx, nullptr) != ESP_OK) return false;

  i2s_std_config_t std = {};
  std.clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sampleRate);
  std.clk_cfg.mclk_multiple = I2S_MCLK_MULTIPLE_256;
  std.slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO);
  std.gpio_cfg.mclk = cfg.mclk == BoardConfig::PIN_UNASSIGNED ? I2S_GPIO_UNUSED
                                                              : (gpio_num_t)cfg.mclk;
  std.gpio_cfg.bclk = (gpio_num_t)cfg.bclk;
  std.gpio_cfg.ws = (gpio_num_t)cfg.lrclk;
  std.gpio_cfg.dout = (gpio_num_t)cfg.dout;
  std.gpio_cfg.din = I2S_GPIO_UNUSED;

  if (i2s_channel_init_std_mode(tx, &std) != ESP_OK) {
    i2s_del_channel(tx);
    return false;
  }
  if (i2s_channel_enable(tx) != ESP_OK) {
    i2s_del_channel(tx);
    return false;
  }
  txChan_ = tx;
  chanEnabled_ = true;
  currentRate_ = sampleRate;
  return true;
}

void AudioManager::teardownI2s() {
  if (!txChan_) return;
  i2s_chan_handle_t tx = (i2s_chan_handle_t)txChan_;
  if (chanEnabled_) i2s_channel_disable(tx);
  i2s_del_channel(tx);
  txChan_ = nullptr;
  chanEnabled_ = false;
  currentRate_ = 0;
}

bool AudioManager::play(const WavSource& source, bool loop) {
  if (!begun_ && !begin()) return false;
  stop();

  WavInfo info;
  if (!parseWavHeader(source, info)) {
    log_e("unsupported WAV (need 16-bit PCM, 1-2ch, 8-48 kHz)");
    return false;
  }
  if (!ensureI2s(info.sampleRate)) {
    log_e("i2s setup failed");
    return false;
  }
  if (!source.seek(info.dataStart)) return false;

  // Unmute the DAC (stop() mutes it). Codec writes stay on the caller's core
  // so the shared I2C bus is never touched from the audio task. The speaker
  // amp comes up in the playback task once silence is flowing — enabling it
  // against an idle I2S line is an audible pop.
  codecMute(false);

  source_ = source;
  wav_ = info;
  loop_ = loop;
  format_ = Format::Pcm;
  stopRequested_ = false;
  playing_ = true;

  // Same shape as the OEM "musicTask" (high priority, core 0 — the Arduino
  // loop owns core 1); 8K stack covers the on-stack sample buffers.
  if (xTaskCreatePinnedToCore(taskEntry, "audio_play", 8192, this, 10, &task_, 0) != pdPASS) {
    playing_ = false;
    task_ = nullptr;
    return false;
  }
  return true;
}

bool AudioManager::playBuffer(const uint8_t* data, size_t len, bool loop) {
  // Shared offset state lives in the lambdas; play() copies them.
  auto offset = std::make_shared<size_t>(0);
  WavSource src;
  src.read = [data, len, offset](uint8_t* dst, size_t want) -> int {
    const size_t left = len - *offset;
    const size_t n = want < left ? want : left;
    memcpy(dst, data + *offset, n);
    *offset += n;
    return (int)n;
  };
  src.seek = [len, offset](size_t pos) {
    if (pos > len) return false;
    *offset = pos;
    return true;
  };
  return play(src, loop);
}

void AudioManager::stop() {
  if (!playing_) return;
  stopRequested_ = true;
  // The task deletes itself; wait for it to drain (bounded).
  for (int i = 0; i < 200 && playing_; ++i) {
    vTaskDelay(pdMS_TO_TICKS(10));
  }
  // Drop the amp and mute the DAC so nothing residual reaches the output
  // between alarms.
  setAmp(false);
  codecMute(true);
}

void AudioManager::taskEntry(void* self) { static_cast<AudioManager*>(self)->taskLoop(); }

void AudioManager::taskLoop() {
#if FREEINK_CAP_MP3
  if (format_ == Format::Mp3) {
    mp3TaskLoop();
    return;
  }
#endif
  pcmTaskLoop();
}

// Prime the line with silence, then raise the amp: the AW8737A (and similar
// class-D amps) pop loudly when enabled against an idle or just-started I2S
// line. Shared by pcmTaskLoop() and mp3TaskLoop() so both formats get the
// same pop-free start.
void AudioManager::primeSilenceAndAmpUp() {
  i2s_chan_handle_t tx = (i2s_chan_handle_t)txChan_;
  int16_t silence[READ_CHUNK] = {};
  for (int i = 0; i < 2; ++i) {
    size_t written = 0;
    if (i2s_channel_write(tx, silence, sizeof(silence), &written, pdMS_TO_TICKS(200)) != ESP_OK) break;
  }
  setAmp(true);
}

// Flush silence through every DMA descriptor, then stop the channel entirely
// — a merely-idle channel replays stale DMA contents (heard as a stutter).
void AudioManager::flushAndDisable() {
  i2s_chan_handle_t tx = (i2s_chan_handle_t)txChan_;
  int16_t silence[READ_CHUNK] = {};
  for (int i = 0; i < 6; ++i) {
    size_t written = 0;
    if (i2s_channel_write(tx, silence, sizeof(silence), &written, pdMS_TO_TICKS(200)) != ESP_OK) break;
  }
  i2s_channel_disable(tx);
  chanEnabled_ = false;
}

void AudioManager::pcmTaskLoop() {
  i2s_chan_handle_t tx = (i2s_chan_handle_t)txChan_;
  uint8_t inBuf[READ_CHUNK];
  // Mono is duplicated into both slots, so the out buffer is 2x.
  int16_t outBuf[READ_CHUNK];

  primeSilenceAndAmpUp();

  size_t consumed = 0;
  while (!stopRequested_) {
    size_t want = READ_CHUNK;
    if (wav_.dataLength > 0) {
      const size_t left = wav_.dataLength - consumed;
      if (left == 0) {
        if (loop_ && source_.seek(wav_.dataStart)) {
          consumed = 0;
          continue;
        }
        break;
      }
      if (want > left) want = left;
    }
    want &= ~(size_t)3;  // keep sample alignment (16-bit stereo frames)
    if (want < 4) want = 4;

    const int n = source_.read(inBuf, want);
    if (n <= 0) {
      if (loop_ && source_.seek(wav_.dataStart)) {
        consumed = 0;
        continue;
      }
      break;
    }
    consumed += n;

    const int16_t* samples = (const int16_t*)inBuf;
    size_t outBytes;
    if (wav_.channels == 1) {
      const int frames = n / 2;
      for (int i = 0; i < frames; ++i) {
        outBuf[i * 2] = samples[i];
        outBuf[i * 2 + 1] = samples[i];
      }
      outBytes = (size_t)frames * 4;
    } else {
      memcpy(outBuf, inBuf, n);
      outBytes = (size_t)n;
    }

    size_t written = 0;
    if (i2s_channel_write(tx, outBuf, outBytes, &written, pdMS_TO_TICKS(1000)) != ESP_OK) break;
  }

  flushAndDisable();

  playing_ = false;
  task_ = nullptr;
  vTaskDelete(nullptr);
}

#if FREEINK_CAP_MP3

namespace {
// MPEG1 Layer III's worst case: 2 channels x 1152 samples/frame (2 granules
// x 576 samples), interleaved.
constexpr size_t MP3_MAX_FRAME_SAMPLES = 1152 * 2;
// Compressed bytes buffered ahead of the decoder. Generous relative to a
// typical frame (a few hundred bytes) so MP3FindSyncWord() reliably has a
// full frame to work with after each refill.
constexpr size_t MP3_IN_BUF = 4096;
}  // namespace

void AudioManager::mp3TaskLoop() {
  HMP3Decoder dec = MP3InitDecoder();
  if (!dec) {
    playing_ = false;
    task_ = nullptr;
    vTaskDelete(nullptr);
    return;
  }

  uint8_t inBuf[MP3_IN_BUF];
  size_t inFill = 0;
  bool sourceEof = false;
  bool ampUp = false;
  int16_t pcm[MP3_MAX_FRAME_SAMPLES];
  int16_t outBuf[MP3_MAX_FRAME_SAMPLES];

  while (!stopRequested_) {
    // Keep the compressed buffer topped up unless the source is exhausted.
    if (!sourceEof && inFill < MP3_IN_BUF) {
      const int n = source_.read(inBuf + inFill, MP3_IN_BUF - inFill);
      if (n > 0) {
        inFill += (size_t)n;
      } else {
        sourceEof = true;
      }
    }
    if (inFill == 0) {
      if (loop_ && source_.seek(0)) {
        sourceEof = false;
        continue;
      }
      break;
    }

    unsigned char* p = inBuf;
    int bytesLeft = (int)inFill;
    const int sync = MP3FindSyncWord(p, bytesLeft);
    if (sync < 0) {
      // No frame header in the buffered bytes. At EOF that means no more
      // frames; otherwise keep whatever tail might be a partial sync word
      // and let the next refill complete it.
      if (sourceEof) {
        if (loop_ && source_.seek(0)) {
          inFill = 0;
          sourceEof = false;
          continue;
        }
        break;
      }
      if (inFill >= MP3_IN_BUF) {
        // No sync word anywhere in a full buffer (e.g. a large ID3v2 tag
        // ahead of the first frame) — drop half of it to make room so the
        // next refill brings in new bytes instead of re-scanning the same
        // unchanging buffer forever.
        const size_t drop = inFill / 2;
        memmove(inBuf, inBuf + drop, inFill - drop);
        inFill -= drop;
      }
      continue;
    }
    p += sync;
    bytesLeft -= sync;
    const int bytesLeftBefore = bytesLeft;

    const int err = MP3Decode(dec, &p, &bytesLeft, pcm, 0);
    if (err != 0) {
      // Corrupt/unsupported frame: drop the sync byte and resync on the next
      // pass rather than aborting the whole stream over one bad frame.
      memmove(inBuf, inBuf + sync + 1, inFill - (size_t)sync - 1);
      inFill -= (size_t)sync + 1;
      continue;
    }

    // Shift whatever the decoder didn't consume to the front of inBuf.
    const size_t frameBytes = (size_t)(bytesLeftBefore - bytesLeft);
    const size_t consumedTotal = (size_t)sync + frameBytes;
    memmove(inBuf, inBuf + consumedTotal, inFill - consumedTotal);
    inFill -= consumedTotal;

    MP3FrameInfo info;
    MP3GetLastFrameInfo(dec, &info);
    if (info.outputSamps <= 0 || info.nChans < 1 || info.nChans > 2 || info.samprate < 8000 ||
        info.samprate > 48000) {
      continue;
    }
    if (!ensureI2s((uint32_t)info.samprate)) break;
    if (!ampUp) {
      primeSilenceAndAmpUp();
      ampUp = true;
    }

    const int frames = info.outputSamps / info.nChans;
    size_t outBytes;
    if (info.nChans == 1) {
      for (int i = 0; i < frames; ++i) {
        outBuf[i * 2] = pcm[i];
        outBuf[i * 2 + 1] = pcm[i];
      }
      outBytes = (size_t)frames * 4;
    } else {
      memcpy(outBuf, pcm, (size_t)frames * 2 * sizeof(int16_t));
      outBytes = (size_t)frames * 4;
    }

    size_t written = 0;
    if (i2s_channel_write((i2s_chan_handle_t)txChan_, outBuf, outBytes, &written, pdMS_TO_TICKS(1000)) != ESP_OK) {
      break;
    }
  }

  if (ampUp) flushAndDisable();
  MP3FreeDecoder(dec);

  playing_ = false;
  task_ = nullptr;
  vTaskDelete(nullptr);
}

#endif  // FREEINK_CAP_MP3

bool AudioManager::playMp3(const WavSource& source, bool loop) {
#if FREEINK_CAP_MP3
  if (!begun_ && !begin()) return false;
  stop();
  if (!source.seek(0)) return false;

  // Unmute now; the amp itself comes up in mp3TaskLoop() once the first
  // frame's sample rate is known and silence is already flowing (same
  // pop-avoidance contract as play()).
  codecMute(false);

  source_ = source;
  loop_ = loop;
  format_ = Format::Mp3;
  stopRequested_ = false;
  playing_ = true;

  // Larger stack than the WAV path: the Helix decoder's working buffers plus
  // this class's own compressed/PCM staging buffers all live on-stack.
  if (xTaskCreatePinnedToCore(taskEntry, "audio_mp3", 16384, this, 10, &task_, 0) != pdPASS) {
    playing_ = false;
    task_ = nullptr;
    return false;
  }
  return true;
#else
  (void)source;
  (void)loop;
  log_e("playMp3() requires -DFREEINK_MP3_HELIX=1 (see AudioManager.h)");
  return false;
#endif
}

}  // namespace freeink

#else  // !FREEINK_CAP_AUDIO — stubs so callers need no #ifdefs

namespace freeink {
bool AudioManager::present() const { return false; }
bool AudioManager::begin() { return false; }
void AudioManager::setVolume(uint8_t) {}
bool AudioManager::play(const WavSource&, bool) { return false; }
bool AudioManager::playBuffer(const uint8_t*, size_t, bool) { return false; }
bool AudioManager::playMp3(const WavSource&, bool) { return false; }
void AudioManager::stop() {}
void AudioManager::powerDown() {}
bool AudioManager::parseWavHeader(const WavSource&, WavInfo&) { return false; }
bool AudioManager::ensureI2s(uint32_t) { return false; }
void AudioManager::teardownI2s() {}
bool AudioManager::codecInit() { return false; }
bool AudioManager::codecWrite(uint8_t, uint8_t) { return false; }
void AudioManager::codecMute(bool) {}
void AudioManager::setAmp(bool) {}
void AudioManager::taskEntry(void*) {}
void AudioManager::taskLoop() {}
void AudioManager::pcmTaskLoop() {}
void AudioManager::mp3TaskLoop() {}
void AudioManager::primeSilenceAndAmpUp() {}
void AudioManager::flushAndDisable() {}
}  // namespace freeink

#endif  // FREEINK_CAP_AUDIO
