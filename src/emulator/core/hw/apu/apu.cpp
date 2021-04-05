/*
 * Copyright (C) 2020 fleroviux
 *
 * Licensed under GPLv3 or any later version.
 * Refer to the included LICENSE file.
 */

#include <cmath>
#include <common/dsp/resampler/blep.hpp>
#include <common/dsp/resampler/cosine.hpp>
#include <common/dsp/resampler/cubic.hpp>
#include <common/dsp/resampler/nearest.hpp>
#include <common/dsp/resampler/windowed-sinc.hpp>

#include "apu.hpp"

// FIXME
#include <fstream>
static std::ofstream dump{"audio_out.bin", std::ios::binary};

namespace nba::core {

// See callback.cpp for implementation
void AudioCallback(APU* apu, std::int16_t* stream, int byte_len);

APU::APU(
  Scheduler& scheduler,
  DMA& dma,
  arm::MemoryBase& memory,
  std::shared_ptr<Config> config
)   : mmio(scheduler)
    , scheduler(scheduler)
    , memory(memory)
    , dma(dma)
    , config(config) {
}

void APU::Reset() {
  using namespace common::dsp;

  mmio.fifo[0].Reset();
  mmio.fifo[1].Reset();
  mmio.psg1.Reset();
  mmio.psg2.Reset();
  mmio.psg3.Reset();
  mmio.psg4.Reset();
  mmio.soundcnt.Reset();
  mmio.bias.Reset();

  resolution_old = 0;
  scheduler.Add(mmio.bias.GetSampleInterval(), this, &APU::StepMixer);
  scheduler.Add(BaseChannel::s_cycles_per_step, this, &APU::StepSequencer);

  auto audio_dev = config->audio_dev;
  audio_dev->Close();
  audio_dev->Open(this, (AudioDevice::Callback)AudioCallback);

  using Interpolation = Config::Audio::Interpolation;

  buffer = std::make_shared<common::dsp::StereoRingBuffer<float>>(audio_dev->GetBlockSize() * 4, true);

  switch (config->audio.interpolation) {
    case Interpolation::Cosine:
      resampler = std::make_unique<CosineStereoResampler<float>>(buffer);
      break;
    case Interpolation::Cubic:
      resampler = std::make_unique<CubicStereoResampler<float>>(buffer);
      break;
    case Interpolation::Sinc_32:
      resampler = std::make_unique<SincStereoResampler<float, 32>>(buffer);
      break;
    case Interpolation::Sinc_64:
      resampler = std::make_unique<SincStereoResampler<float, 64>>(buffer);
      break;
    case Interpolation::Sinc_128:
      resampler = std::make_unique<SincStereoResampler<float, 128>>(buffer);
      break;
    case Interpolation::Sinc_256:
      resampler = std::make_unique<SincStereoResampler<float, 256>>(buffer);
      break;
  }

  // TODO: use cubic interpolation or better if M4A samplerate hack is active.
  if (config->audio.interpolate_fifo) {
    for (int fifo = 0; fifo < 2; fifo++) {
      fifo_buffer[fifo] = std::make_shared<RingBuffer<float>>(16, true);
      fifo_resampler[fifo] = std::make_unique<BlepResampler<float>>(fifo_buffer[fifo]);
      fifo_samplerate[fifo] = 0;
    }
  }

  resampler->SetSampleRates(mmio.bias.GetSampleRate(), audio_dev->GetSampleRate());
}

void APU::OnTimerOverflow(int timer_id, int times, int samplerate) {
  auto const& soundcnt = mmio.soundcnt;

  if (!soundcnt.master_enable) {
    return;
  }

  constexpr DMA::Occasion occasion[2] = { DMA::Occasion::FIFO0, DMA::Occasion::FIFO1 };

  for (int fifo_id = 0; fifo_id < 2; fifo_id++) {
    if (soundcnt.dma[fifo_id].timer_id == timer_id) {
      auto& fifo = mmio.fifo[fifo_id];
      for (int time = 0; time < times - 1; time++) {
        fifo.Read();
      }
      if (config->audio.interpolate_fifo) {
        if (samplerate != fifo_samplerate[fifo_id]) {
          fifo_resampler[fifo_id]->SetSampleRates(samplerate, mmio.bias.GetSampleRate());
          fifo_samplerate[fifo_id] = samplerate;
        }
        fifo_resampler[fifo_id]->Write(fifo.Read() / 128.0);
      } else {
        latch[fifo_id] = fifo.Read();
      }
      if (fifo.Count() <= 16) {
        dma.Request(occasion[fifo_id]);
      }
    }
  }
}

void APU::OnSoundDriverMainCalled(M4ASoundInfo* soundinfo, bool start) {
  // This is the M4A/MP2K HLE audio mixer

  // Target sample rate it 65kHz, SoundMain() is called 60 times per second.
  static constexpr int kSampleCount = 65536 / 60;

  using Access = arm::MemoryBase::Access;

  // TODO: move this to the member variables, nerd.
  static struct {
    bool forward_loop;
    std::uint32_t frequency;
    std::uint32_t loop_sample_index;
    std::uint32_t number_of_samples;
    std::uint32_t data_address;
    float current_sample_index;
  } channel_cache[kM4AMaxDirectSoundChannels];

  if (soundinfo->magic & 1) {
    return;
  }

  if (start) {
    for (int i = 0; i < kM4AMaxDirectSoundChannels; i++) {
      if (soundinfo->channels[i].status == 0x80) {
        auto wav_address = soundinfo->channels[i].wav;
        
        channel_cache[i].forward_loop = memory.ReadHalf(wav_address + 2, Access::Debug) != 0;
        channel_cache[i].frequency = memory.ReadWord(wav_address + 4, Access::Debug);
        channel_cache[i].loop_sample_index = memory.ReadWord(wav_address + 8, Access::Debug);
        channel_cache[i].number_of_samples = memory.ReadWord(wav_address + 12, Access::Debug);
        channel_cache[i].data_address = wav_address + 16;
        channel_cache[i].current_sample_index = 0;

        // LOG_ERROR("[{0}] {1} {2}", i, channel_cache[i].frequency, std::pow(2, (180 - soundinfo->channels[i].ky) / 12.0));
        LOG_ERROR("[{0}] {1} Hz", i, channel_cache[i].frequency / 1024.0);
      }
    }
  } else {
    for (int i = 0; i < kSampleCount; i++) {
      float samples[2] { 0.0 };

      // TODO: reverse the order of the loops and write to a temporary buffer instead.
      for (int j = 0; j < kM4AMaxDirectSoundChannels; j++) {
        auto& channel = soundinfo->channels[j];

        // For now let's ignore channels that are definitely off.
        if (channel.status == 0) {
          continue;
        }

        // Let's ignore percussive channels for now.
        // if (channel.type == 8) {
        //   continue;
        // }

        auto& cache = channel_cache[j];

        if (cache.frequency == 0) {
          // Welp, not sure what to do in that case.
          continue;
        }
        // auto angular_step = 0;

        // TODO: interpolate between two samples based on the fractional portion of current_sample_index
        // auto sample = std::int8_t(memory.ReadByte(cache.data_address + int(cache.current_sample_index), Access::Debug)) / 128.0;
        // auto frequency = std::pow(2, (180 - soundinfo->channels[j].ky) / 12.0);
        // auto sample = std::sin(2 * M_PI * frequency * i / 65536.0);
        auto sample_rate = cache.frequency / 1024.0;
        auto note_freq = (std::uint64_t(channel.freq) << 32) / cache.frequency / 16384.0;
        auto angular_step = note_freq / 256.0 * (sample_rate / 65536.0);

        // if (channel.status == 8) {
        //   angular_step = (sample_rate / 65536.0);
        // }

        auto sample = std::int8_t(memory.ReadByte(cache.data_address + int(cache.current_sample_index), Access::Debug)) / 128.0;

        samples[0] += sample * channel.leftVolume  / 255.0;
        samples[1] += sample * channel.rightVolume / 255.0;

        cache.current_sample_index += angular_step;

        if (cache.current_sample_index >= cache.number_of_samples) {
          if (cache.forward_loop) {
            // TODO: properly wrap around, respecting the fractional offset
            // TODO: also use the actual loop point
            cache.current_sample_index = cache.loop_sample_index;
          } else {
            cache.current_sample_index = cache.number_of_samples;
          }
        }
      }

      dump.write((char*)samples, sizeof(samples));
    }
  }
}

void APU::StepMixer(int cycles_late) {
  auto& bias = mmio.bias;

  if (bias.resolution != resolution_old) {
    resampler->SetSampleRates(bias.GetSampleRate(),
      config->audio_dev->GetSampleRate());
    resolution_old = mmio.bias.resolution;
    if (config->audio.interpolate_fifo) {
      for (int fifo = 0; fifo < 2; fifo++) {
        fifo_resampler[fifo]->SetSampleRates(fifo_samplerate[fifo], mmio.bias.GetSampleRate());
      }
    }
  }

  common::dsp::StereoSample<std::int16_t> sample { 0, 0 };

  constexpr int psg_volume_tab[4] = { 1, 2, 4, 0 };
  constexpr int dma_volume_tab[2] = { 2, 4 };

  auto& psg = mmio.soundcnt.psg;
  auto& dma = mmio.soundcnt.dma;

  auto psg_volume = psg_volume_tab[psg.volume];

  if (config->audio.interpolate_fifo) {
    for (int fifo = 0; fifo < 2; fifo++) {
      latch[fifo] = std::int8_t(fifo_buffer[fifo]->Read() * 127.0);
    }
  }

  for (int channel = 0; channel < 2; channel++) {
    std::int16_t psg_sample = 0;

    if (psg.enable[channel][0]) psg_sample += mmio.psg1.GetSample();
    if (psg.enable[channel][1]) psg_sample += mmio.psg2.GetSample();
    if (psg.enable[channel][2]) psg_sample += mmio.psg3.GetSample();
    if (psg.enable[channel][3]) psg_sample += mmio.psg4.GetSample();

    sample[channel] += psg_sample * psg_volume * psg.master[channel] / 28;

    for (int fifo = 0; fifo < 2; fifo++) {
      if (dma[fifo].enable[channel]) {
        sample[channel] += latch[fifo] * dma_volume_tab[dma[fifo].volume];
      }
    }

    sample[channel] += mmio.bias.level;
    sample[channel]  = std::clamp(sample[channel], std::int16_t(0), std::int16_t(0x3FF));
    sample[channel] -= 0x200;
  }

  buffer_mutex.lock();
  resampler->Write({ sample[0] / float(0x200), sample[1] / float(0x200) });
  buffer_mutex.unlock();

  scheduler.Add(mmio.bias.GetSampleInterval() - cycles_late, this, &APU::StepMixer);
}

void APU::StepSequencer(int cycles_late) {
  mmio.psg1.Tick();
  mmio.psg2.Tick();
  mmio.psg3.Tick();
  mmio.psg4.Tick();

  scheduler.Add(BaseChannel::s_cycles_per_step - cycles_late, this, &APU::StepSequencer);
}

} // namespace nba::core
