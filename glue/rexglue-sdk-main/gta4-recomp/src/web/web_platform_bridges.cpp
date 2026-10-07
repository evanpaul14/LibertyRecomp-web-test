/**
 ******************************************************************************
 * @file        web_platform_bridges.cpp
 * @brief       Web (Emscripten) implementations of the macOS-only bridges.
 *
 * The macOS build implements these in Objective-C++ against Game Center,
 * AVFoundation and the TCC microphone prompt. None of those exist in a
 * browser yet, so the web build reports each feature as unavailable.
 ******************************************************************************
 */

#include <achievement_bridge_gc.h>
#include <input/user_music_player.h>
#include <network/gta4_microphone_permission.h>

namespace gta4::game_center {

void Initialize() {}

void SubmitAchievement(uint32_t) {}

}  // namespace gta4::game_center

namespace gta4::input {

// No playlist is ever discovered, so every request is declined and the guest
// vehicle radio is never muted on the web.
struct UserMusicPlayer::Impl {};

UserMusicPlayer::UserMusicPlayer(std::filesystem::path) : impl_(std::make_unique<Impl>()) {}

UserMusicPlayer::~UserMusicPlayer() = default;

bool UserMusicPlayer::Next() {
  return false;
}

bool UserMusicPlayer::Previous() {
  return false;
}

bool UserMusicPlayer::Stop() {
  return false;
}

bool UserMusicPlayer::HasTracks() const {
  return false;
}

void PublishUserMusicPlayer(UserMusicPlayer*) {}

bool IsUserMusicAvailable() {
  return false;
}

bool RequestUserMusicNext() {
  return false;
}

bool RequestUserMusicPrevious() {
  return false;
}

bool RequestUserMusicStop() {
  return false;
}

}  // namespace gta4::input

namespace gta4::voice {

MicrophonePermissionStatus GetMicrophonePermissionStatus() noexcept {
  return MicrophonePermissionStatus::kRestricted;
}

void RequestMicrophonePermission(std::function<void()> completion) {
  // There is no prompt to wait for; callers re-query and see kRestricted.
  if (completion) {
    completion();
  }
}

}  // namespace gta4::voice
