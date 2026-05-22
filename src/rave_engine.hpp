#pragma once

// ── RAVE inference stub (RPi 5 / MEGALO_RAVE path only) ───────────────────
//
// RAVE = Realtime Audio Variational autoEncoder (Caillon & Esling, 2021).
// https://github.com/acids-ircam/RAVE
//
// This file defines the interface used by plugin.cpp when compiled with
// -DMEGALO_RAVE.  The actual inference backend must be wired here once
// a trained model and a runtime library are available.
//
// ── Integration roadmap ───────────────────────────────────────────────────
//
// 1. Train a pitch-conditioned RAVE model on target instrument sounds
//    (guitar, synth pads, …) using the acids-ircam/RAVE Python package.
//    Export with:  model.export_to_onnx("megalo_rave.onnx")
//
// 2. Choose a runtime:
//    • ONNX Runtime ≥ 1.16 (ARM64, available as .deb for RPi OS) — simplest.
//    • RTNeural (header-only, ARM NEON) — lighter, but needs manual porting
//      of RAVE's Conv1d / GRU layers; good long-term target for Dwarf.
//
// 3. Uncomment the relevant backend block below, link the library in
//    CMakeLists.txt / Makefile (MEGALO_RAVE section), and fill in the
//    encode() / decode() bodies.
//
// 4. Place the .onnx model alongside the LV2 bundle (megalo.lv2/).
//    The plugin will look for it relative to its own bundle path via
//    lv2:extensionData / LV2_State_Interface (TODO).
//
// ── RPi 5 build flag ─────────────────────────────────────────────────────
//   cmake -DMEGALO_RAVE=ON -DMEGALO_RAVE_MODEL_PATH=/path/to/model.onnx ..
//   make TARGET=rpi5 MEGALO_RAVE=1 MEGALO_RAVE_MODEL_PATH=...
//
// ── Runtime cost estimate (tiny RAVE, 8-dim latent, RPi 5) ───────────────
//   Encoder  : ~2 ms per 2048-sample block   (run once on LoopReady)
//   Decoder  : ~4 ms per 2048-sample block   (streaming, replaces AdditiveSynth)
//   Total decode overhead vs granular: +3–5 % CPU on Cortex-A76 @ 2.4 GHz

#ifdef MEGALO_RAVE

// ── Backend selection ─────────────────────────────────────────────────────
// Uncomment ONE of the following:
// #define RAVE_BACKEND_ONNX       // ONNX Runtime (easiest, ~50 MB footprint)
// #define RAVE_BACKEND_RTNEURAL   // RTNeural port (lighter, needs manual work)

static constexpr int RAVE_LATENT_DIM = 8;    // must match trained model
static constexpr int RAVE_HOP_SIZE   = 2048; // RPi 5 (use 1024 for Dwarf if ever ported)

class RaveEngine {
public:
    // Load model from path.  Returns false if backend not compiled in.
    bool load(const char* model_path) noexcept {
        (void)model_path;
        // TODO: instantiate Ort::Session / RTNeural model
        return false;
    }

    // Encode the captured loop into the RAVE latent space.
    // Called ONCE on LoopReady (not per-sample).
    bool encode(const float* loop, int loop_len, float sr) noexcept {
        (void)loop; (void)loop_len; (void)sr;
        // TODO: run RAVE encoder forward pass on loop[0..loop_len-1]
        // Store result in _latent[RAVE_LATENT_DIM].
        return false;
    }

    // Decode RAVE_HOP_SIZE samples into out[], applying pitch_ratio.
    // Called once per hop from the plugin's run() block loop.
    // pitch_ratio = 2^(semitones/12) — passed to the pitch-conditioning
    // input of the decoder (pitch-conditioned RAVE variant required).
    bool decode(float* out, float pitch_ratio) noexcept {
        (void)out; (void)pitch_ratio;
        // TODO: manipulate _latent for pitch, run RAVE decoder, OLA output
        return false;
    }

    bool valid() const noexcept { return _loaded; }

private:
    bool  _loaded                     = false;
    float _latent[RAVE_LATENT_DIM]    = {};
    float _out_buf[RAVE_HOP_SIZE * 2] = {}; // double-buffer for OLA
    int   _out_read                   = 0;
    int   _out_fill                   = 0;

    // TODO: Ort::Env env; Ort::Session session; ...
};

#endif // MEGALO_RAVE
