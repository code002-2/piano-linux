#ifndef UPERF_STATE_MACHINE_H
#define UPERF_STATE_MACHINE_H

#include <stdint.h>
#include <stdbool.h>
#include "config.h"

/* FSM scene states */
typedef enum {
    SCENE_IDLE = 0,
    SCENE_TOUCH,
    SCENE_TRIGGER,
    SCENE_GESTURE,
    SCENE_JUNK,
    SCENE_SWITCH,
    SCENE_BOOST,
    SCENE_NUM_STATES
} SceneState;

/* Event types that drive state transitions */
typedef enum {
    EVT_TOUCH_DOWN   = 0,   /* Finger pressed on screen */
    EVT_TOUCH_UP,            /* Finger lifted */
    EVT_SWIPE,               /* Swipe gesture detected */
    EVT_GESTURE,             /* Edge swipe / system gesture */
    EVT_HOLD,                /* Long press */
    EVT_JUNK,                /* Noise / accidental touch */
    EVT_WINDOW_SWITCH,       /* App/activity switch */
    EVT_HEAVY_LOAD_START,    /* HeavyLoad detected by detector */
    EVT_HEAVY_LOAD_END,      /* Load dropped below threshold */
    EVT_TIMEOUT,             /* State timer expired */
    EVT_MODE_CHANGE,         /* User changed power mode */
    EVT_NUM_TYPES
} EventType;

/* Hint configuration per state (from JSON) */
typedef struct {
    float max_duration;  /* Maximum duration in seconds before auto-transition */
} HintConfig;

/* Opaque state machine handle */
typedef struct StateMachine StateMachine;

/* Create a new state machine from config.
 * Returns NULL on failure. */
StateMachine *state_machine_new(const Config *cfg);

/* Free state machine resources. */
void state_machine_free(StateMachine *sm);

/* Advance the state machine by one tick.
 * Called periodically (e.g., every base_sample_time).
 * Handles timeout-based transitions. */
void state_machine_tick(StateMachine *sm);

/* Feed an event into the state machine.
 * May trigger immediate state transitions.
 * Returns the new scene state, or SCENE_NUM_STATES on error. */
SceneState state_machine_feed_event(StateMachine *sm, EventType evt);

/* Get the current scene state. */
SceneState state_machine_get_scene(const StateMachine *sm);

/* Get the current power mode. */
PowerMode state_machine_get_mode(const StateMachine *sm);

/* Change power mode. Triggers a state reset to idle in the new mode. */
void state_machine_set_mode(StateMachine *sm, PowerMode mode);

/* Get the action parameters for the current scene and mode.
 * Fills the provided ActionParams struct. */
void state_machine_get_actions(const StateMachine *sm, ActionParams *out);

/* Get the hint duration config for a given scene. */
float state_machine_get_hint_duration(const StateMachine *sm, SceneState scene);

/* Check if we should enter boost state based on current load. */
bool state_machine_needs_boost(const StateMachine *sm, float current_load,
                               float heavy_load_threshold);

/* Apply a thermal reduction factor (0.0-1.0) to the current action parameters.
 * Scales down frequency limits proportionally to the thermal severity.
 * reduction: 0.0 = no reduction, 1.0 = maximum reduction to minimum freq. */
void state_machine_apply_thermal_reduction(StateMachine *sm, float reduction);

/* Get the current thermal reduction factor. */
float state_machine_get_thermal_reduction(const StateMachine *sm);

#endif /* UPERF_STATE_MACHINE_H */
