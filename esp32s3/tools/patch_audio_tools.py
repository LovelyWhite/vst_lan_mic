"""Re-apply the LAN MIC mic-volume patch to the vendored audio-tools library.

Root cause: audio-tools v1.3.0 routes SET_CUR/GET_CUR of both Feature Units
(playback FU1 and microphone FU2) into one shared volume_/mute_ state, and
only the downlink (readBytes) applies it.  On macOS that makes the input
slider silently retoggle the output volume while never affecting the mic.

The patch gives the mic-path Feature Unit its own volume/mute state; the
uplink applies it via inputGain() in AudioTasks.cpp.  It also narrows the
Feature Unit GET_RANGE resolution to 1/256 dB so host volume-key steps are
not quantised away.  .pio/ is gitignored, so this extra_script re-applies the
edits after every fresh dependency install.

shortcut: anchors are exact-match against audio-tools v1.3.0 (pinned in
platformio.ini); if a build fails on "anchor mismatch" after bumping
lib_deps, re-derive EDITS from the new library sources.
"""

import glob
import os

STREAM_EDITS = [
    # Public API: separate mic-path state, inserted after setMute().
    (
        """  bool setMute(bool m, uint8_t channel = 0) {
    LOGW("setMute %s channel: %d", m ? "true" : "false", channel);
    if (channel >= mute_.size()) return false;
    mute_[channel] = m;
    if (mute_cb_) mute_cb_(m, channel);
    onMuteChanged(channel);
    return true;
  }
""",
        """  bool setMute(bool m, uint8_t channel = 0) {
    LOGW("setMute %s channel: %d", m ? "true" : "false", channel);
    if (channel >= mute_.size()) return false;
    mute_[channel] = m;
    if (mute_cb_) mute_cb_(m, channel);
    onMuteChanged(channel);
    return true;
  }

  /** @brief Effective capture gain for a per-channel index (1-based),
   *  combining the mic Feature Unit's master and per-channel state. */
  float inputGain(uint8_t channel) const {
    if (input_volume_.empty()) return 1.0f;
    if (input_mute_[0]) return 0.0f;
    float master = input_volume_[0];
    if (channel >= input_volume_.size()) return master;
    if (input_mute_[channel]) return 0.0f;
    return master * input_volume_[channel];
  }

  float inputVolume(uint8_t channel) const {
    return (channel < input_volume_.size()) ? input_volume_[channel] : 0.0f;
  }

  bool setInputVolume(float vol, uint8_t channel) {
    if (channel >= input_volume_.size()) return false;
    input_volume_[channel] = vol;
    return true;
  }

  bool isInputMute(uint8_t channel) const {
    return (channel < input_mute_.size()) ? input_mute_[channel] : false;
  }

  bool setInputMute(bool m, uint8_t channel) {
    if (channel >= input_mute_.size()) return false;
    input_mute_[channel] = m;
    return true;
  }
""",
    ),
    # State vectors.
    (
        """  std::vector<float> volume_;
  std::vector<bool> mute_;
""",
        """  std::vector<float> volume_;
  std::vector<bool> mute_;
  // Mic-path Feature Unit keeps its own state so host input sliders never
  // touch the playback volume used by readBytes().
  std::vector<float> input_volume_;
  std::vector<bool> input_mute_;
""",
    ),
    # Size the mic-path state alongside the playback state.
    (
        """    volume_.assign(vol_sz, 1.0f);
    mute_.assign(vol_sz, false);
""",
        """    volume_.assign(vol_sz, 1.0f);
    mute_.assign(vol_sz, false);
    input_volume_.assign(vol_sz, 1.0f);
    input_mute_.assign(vol_sz, false);
""",
    ),
    # True-dB volume curve: the fork's linear mapping (gain = 1 + dB/100)
    # saturates near the top of the slider, so macOS volume keys (small dB
    # steps near 0 dB) changed the gain imperceptibly while dragging (large
    # dB swings) changed it clearly.  USB Audio 2.0 defines the FU value as
    # real decibels, which is also what macOS's slider/key curve assumes:
    # gain = 10^(dB/20).
    (
        "#include <cstddef>\n#include <cstdint>\n",
        "#include <cmath>\n#include <cstddef>\n#include <cstdint>\n",
    ),
    (
        """  /** @brief Convert linear volume (0.0–1.0) to UAC2 int16 (1/256 dB).
   *  Linear mapping: 0.0 → -100 dB (min), 1.0 → 0 dB (max). */
  static int16_t floatToUac2(float vol) {
    if (vol <= 0.0f) return (int16_t)0x8000;
    if (vol >= 1.0f) return 0;
    return (int16_t)((1.0f - vol) * kVolumeMinDb256);
  }

  /** @brief Convert UAC2 int16 (1/256 dB) to linear volume (0.0–1.0).
   *  Linear mapping within the -100..0 dB range reported by GET_RANGE. */
  static float uac2ToFloat(int16_t v) {
    if (v == (int16_t)0x8000) return 0.0f;
    if (v >= 0) return 1.0f;
    if (v <= kVolumeMinDb256) return 0.0f;
    return 1.0f - (float)v / (float)kVolumeMinDb256;
  }
""",
        """  /** @brief Convert linear volume (0.0–1.0) to UAC2 int16 (1/256 dB),
   *  the exact inverse of uac2ToFloat() within the advertised range. */
  static int16_t floatToUac2(float vol) {
    if (vol <= 0.0f) return (int16_t)0x8000;
    if (vol >= 1.0f) return 0;
    float db256 = 20.0f * log10f(vol) * 256.0f;
    if (db256 <= (float)kVolumeMinDb256) return kVolumeMinDb256;
    return (int16_t)roundf(db256);
  }

  /** @brief Convert UAC2 int16 (1/256 dB, USB Audio 2.0 unit) to linear
   *  amplitude: gain = 10^(dB/20), so host keys and slider follow the same
   *  curve macOS models. */
  static float uac2ToFloat(int16_t v) {
    if (v == (int16_t)0x8000) return 0.0f;
    if (v >= 0) return 1.0f;
    if (v < kVolumeMinDb256) v = kVolumeMinDb256;  // keep GET_CUR on-grid
    return powf(10.0f, (float)v / (256.0f * 20.0f));
  }
""",
    ),
]

DEVICE_EDITS = [
    # Helper identifying the mic-path Feature Unit (chain 2, RXTX mode only).
    (
        """  bool isFeatureUnit(uint8_t id) const {
    return id == USBAudio2DescriptorBuilder::ENTITY_FU1 ||
           id == USBAudio2DescriptorBuilder::ENTITY_FU2;
  }
""",
        """  bool isFeatureUnit(uint8_t id) const {
    return id == USBAudio2DescriptorBuilder::ENTITY_FU1 ||
           id == USBAudio2DescriptorBuilder::ENTITY_FU2;
  }

  /** @brief True for the mic-path Feature Unit in RXTX mode, which keeps
   *  its own volume/mute state instead of sharing the playback one. */
  bool isInputFeatureUnit(uint8_t id) const {
    return config_.enable_ep_out && config_.enable_ep_in &&
           id == USBAudio2DescriptorBuilder::ENTITY_FU2;
  }
""",
    ),
    # SET_CUR: route mic Feature Unit writes to the separate state and log
    # them (the mic FU path had no log at all, so host key-vs-slider writes
    # could not be told apart on the serial console).
    (
        """            if (isFeatureUnit(entityID) &&
                p_request.bRequest == kAudioCsReqCur) {
              uint8_t channel = u16Low(p_request.wValue);
              if (ctrlSel == AUDIO_FU_CTRL_MUTE) {
                setMute(cb[0] != 0, channel);
              } else if (ctrlSel == AUDIO_FU_CTRL_VOLUME) {
                int16_t v;
                memcpy(&v, cb, 2);
                setVolume(uac2ToFloat(v), channel);
              }
            }
""",
        """            if (isFeatureUnit(entityID) &&
                p_request.bRequest == kAudioCsReqCur) {
              uint8_t channel = u16Low(p_request.wValue);
              const bool micFu = isInputFeatureUnit(entityID);
              if (ctrlSel == AUDIO_FU_CTRL_MUTE) {
                if (micFu) setInputMute(cb[0] != 0, channel);
                else setMute(cb[0] != 0, channel);
              } else if (ctrlSel == AUDIO_FU_CTRL_VOLUME) {
                int16_t v;
                memcpy(&v, cb, 2);
                LOGW("FU vol set mic=%d ch=%d dB256=%d", int(micFu), channel,
                     int(v));
                if (micFu) setInputVolume(uac2ToFloat(v), channel);
                else setVolume(uac2ToFloat(v), channel);
              }
            }
""",
    ),
    # GET_CUR mute: answer from the right state (entity is wIndex high byte).
    (
        """    uint8_t channel = u16Low(p_request.wValue);
    if (ctrlSel == AUDIO_FU_CTRL_MUTE &&
        p_request.bRequest == kAudioCsReqCur) {
      cb[0] = isMute(channel) ? 1 : 0;
""",
        """    uint8_t channel = u16Low(p_request.wValue);
    const bool micFu = isInputFeatureUnit(u16High(p_request.wIndex));
    if (ctrlSel == AUDIO_FU_CTRL_MUTE &&
        p_request.bRequest == kAudioCsReqCur) {
      cb[0] = (micFu ? isInputMute(channel) : isMute(channel)) ? 1 : 0;
""",
    ),
    # GET_CUR volume.
    (
        "        int16_t v = floatToUac2(volume(channel));\n",
        "        int16_t v = floatToUac2(micFu ? inputVolume(channel) : volume(channel));\n",
    ),
    # GET_RANGE: advertise 1/256 dB granularity instead of 1 dB. macOS volume
    # keys step by far less than 1 dB near 0 dB, so a 1 dB resolution quantised
    # every key press back to the value already set (no change); dragging the
    # slider crossed a dB boundary and worked. Fine steps also keep every
    # GET_CUR readback on the advertised grid.
    # The floor goes to -48 dB (-12288), what real UAC2 interfaces advertise:
    # macOS spreads both the key steps and the whole slider travel over the
    # advertised range, so the -100 dB default made one drag change the gain by
    # several times what a key press did.
    (
        "        int16_t vmin = -25600, vmax = 0, vres = 256;\n",
        "        int16_t vmin = -12288, vmax = 0, vres = 1;\n",
    ),
]

EDITS = {
    "USBAudioStreamBase.h": STREAM_EDITS,
    "USBAudioDeviceBase.h": DEVICE_EDITS,
}

USB_SUBPATH = os.path.join("src", "AudioTools", "Communication", "USB")


def patch_project(project_dir):
    """Apply every edit to each vendored audio-tools copy.

    Returns (patched, problems): paths freshly patched, and paths whose
    anchors no longer match (library drift or missing file)."""
    patched, problems = [], []
    pattern = os.path.join(project_dir, ".pio", "libdeps", "*", "audio-tools")
    for libdir in sorted(glob.glob(pattern)):
        for name, edits in EDITS.items():
            path = os.path.join(libdir, USB_SUBPATH, name)
            if not os.path.isfile(path):
                problems.append("%s (missing)" % path)
                continue
            with open(path, encoding="utf-8") as handle:
                text = handle.read()
            original = text
            for old, new in edits:
                if new in text:
                    continue  # edit already applied
                if old not in text:
                    problems.append("%s (anchor mismatch in %s)" % (path, name))
                    continue
                text = text.replace(old, new, 1)
            if text != original:
                with open(path, "w", encoding="utf-8") as handle:
                    handle.write(text)
                patched.append(path)
    return patched, problems


try:
    Import("env")  # SCons-provided when PlatformIO loads this extra_script
    HAVE_ENV = True
except NameError:
    HAVE_ENV = False

if HAVE_ENV:

    def _apply(target=None, source=None):
        libdirs = glob.glob(
            os.path.join(env.subst("$PROJECT_DIR"), ".pio", "libdeps", "*",
                         "audio-tools"))
        if not libdirs:
            return False
        patched, problems = patch_project(env.subst("$PROJECT_DIR"))
        for path in patched:
            print("audio-tools mic-volume patch applied: %s" % path)
        for problem in problems:
            print("audio-tools mic-volume patch FAILED: %s" % problem)
        if problems:
            env.Exit(1)
        return True

    if not _apply():
        # "pre" scripts can run before lib_deps are installed; retry once at
        # build start, before any source is compiled.
        env.AddPreAction("buildprog", _apply)
