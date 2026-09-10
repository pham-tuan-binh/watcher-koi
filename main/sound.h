#pragma once

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SOUND_DROP = 0,   /*!< finger hits the water: bright, close, wet */
    SOUND_SURFACE,    /*!< a koi noses the surface: lower and softer */
    SOUND_DISTANT,    /*!< something falls in over by the far bank */
    SOUND_TICK,       /*!< knob detent: a dry low knock, no reverb */
    SOUND_COUNT
} sound_t;

/// Start the audio mixer task. Safe to call before anything is played.
void sound_init(void);

/// Queue a sound. Non-blocking; overlapping sounds are mixed.
void sound_play(sound_t which);

/// Silence the pond. Muted, queued sounds are dropped and anything already
/// sounding is faded out within a block — the speaker is millimetres from
/// the mic, so a recording needs the pond to stop talking over it.
void sound_set_muted(bool muted);

#ifdef __cplusplus
}
#endif
