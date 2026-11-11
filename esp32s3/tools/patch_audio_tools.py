"""Re-apply the LAN MIC mic-volume patch to the vendored audio-tools library.

Root cause: audio-tools v1.3.0 routes SET_CUR/GET_CUR of both Feature Units
(playback FU1 and microphone FU2) into one shared volume_/mute_ state, and
only the downlink (readBytes) applies it.  On macOS that makes the input
slider silently retoggle the output volume while never affecting the mic.

The patch gives the mic-path Feature Unit its own volume/mute state; the
uplink applies it via inputGain() in AudioTasks.cpp.  .pio/ is gitignored,
so this extra_script re-applies the edits after every fresh dependency
install.

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
    # SET_CUR: route mic Feature Unit writes to the separate state.
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
