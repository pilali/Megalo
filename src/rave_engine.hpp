#pragma once

// ── RAVE inference stub (RPi 5 / MEGALO_RAVE path only) ───────────────────
//
// RAVE = Realtime Audio Variational autoEncoder (Caillon & Esling, 2021).
// https://github.com/acids-ircam/RAVE
//
// Compile with -DMEGALO_RAVE to enable. Requires a trained model and either
// ONNX Runtime ≥ 1.16 (ARM64) or RTNeural (header-only, ARM NEON).
// See integration roadmap in the comments below.
//
// ── Integration roadmap ───────────────────────────────────────────────────
// 1. Train a pitch-conditioned RAVE model (acids-ircam/RAVE Python package).
//    Export: model.export_to_onnx("megalo_rave.onnx")
// 2. Choose runtime: ONNX Runtime (simplest) or RTNeural (lighter).
// 3. Uncomment the backend block below and link the library in Makefile/CMake.
// 4. Place the .onnx model alongside the LV2 bundle (megaloHN.lv2/).

#ifdef MEGALO_RAVE

static constexpr int RAVE_LATENT_DIM = 8;
static constexpr int RAVE_HOP_SIZE   = 2048;

class RaveEngine {
public:
    bool load(const char* model_path) noexcept {
        (void)model_path;
        return false;
    }

    bool encode(const float* loop, int loop_len, float sr) noexcept {
        (void)loop; (void)loop_len; (void)sr;
        return false;
    }

    bool decode(float* out, float pitch_ratio) noexcept {
        (void)out; (void)pitch_ratio;
        return false;
    }

    bool valid() const noexcept { return _loaded; }

private:
    bool  _loaded                     = false;
    float _latent[RAVE_LATENT_DIM]    = {};
    float _out_buf[RAVE_HOP_SIZE * 2] = {};
    int   _out_read                   = 0;
    int   _out_fill                   = 0;
};

#endif // MEGALO_RAVE
