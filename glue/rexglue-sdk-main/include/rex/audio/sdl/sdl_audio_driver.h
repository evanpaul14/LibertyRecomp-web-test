/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2020 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#pragma once

#include <chrono>
#include <mutex>
#include <queue>
#include <stack>

#include <rex/audio/audio_driver.h>
#include <rex/thread.h>

#include <SDL3/SDL.h>

namespace rex::audio::sdl {

class SDLAudioDriver : public AudioDriver {
 public:
  SDLAudioDriver(memory::Memory* memory, rex::thread::Semaphore* semaphore);
  ~SDLAudioDriver() override;

  bool Initialize();
  void SubmitFrame(uint32_t frame_ptr) override;
  void Pause() override;
  void Resume() override;
  void Flush() override;
  void Shutdown();

 protected:
  static void SDLCallback(void* userdata, SDL_AudioStream* stream, int additional_amount,
                          int total_amount);
  void FlushQueuedFrames();

  rex::thread::Semaphore* semaphore_ = nullptr;

  SDL_AudioStream* sdl_stream_ = nullptr;
  bool sdl_initialized_ = false;
  bool silent_fallback_ = false;
  uint8_t sdl_device_channels_ = 0;

  static const uint32_t frame_frequency_ = 48000;
  static const uint32_t frame_channels_ = 6;
  static const uint32_t channel_samples_ = 256;
  static const uint32_t frame_samples_ = frame_channels_ * channel_samples_;
  static const uint32_t frame_size_ = sizeof(float) * frame_samples_;
  // When the silent fallback should release its next frame.
  std::chrono::steady_clock::time_point silent_deadline_{};
  std::queue<float*> frames_queued_ = {};
  std::stack<float*> frames_unused_ = {};
  std::mutex frames_mutex_ = {};

  // After the queue runs dry, play silence until this many frames are queued
  // again (--audio_refill_frames), so one late guest frame costs one gap
  // instead of a crackle per callback. Guarded by frames_mutex_.
  bool refilling_ = true;
  // Underrun statistics, logged every 5 s while they occur (web only).
  struct Stats {
    uint64_t start_ms = 0;
    uint32_t callbacks = 0, played = 0, silent = 0, underruns = 0;
    size_t min_queued = SIZE_MAX, max_queued = 0;
  } stats_;
};

}  // namespace rex::audio::sdl
