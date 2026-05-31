# Codex Usage + Face Display Design

## Goal

Build a dual-mode screen experience on the ESP32 display:

- Show a cute StackChan-inspired face during idle periods.
- Show Codex usage data only when usage values change.
- Keep the usage card visible for up to 3 minutes after the most recent valid data change.
- Return to the face automatically if no relevant usage field changes for more than 3 minutes.
- Transition smoothly between face mode and usage mode instead of hard switching.

## Current Project Context

The current firmware already:

- Runs on an ESP32-C3 with a `240x240` ST7789 display.
- Uses Arduino with `Adafruit_ST7789` and `Adafruit_GFX`.
- Renders a Codex usage card from serial or HTTP JSON.
- Treats incoming JSON as the source of truth for usage fields.

The current display code lives in `cc-island-c3/src/main.cpp` and is a good base for adding a higher-level mode controller without replacing the existing connectivity logic.

## Product Behavior

### Modes

The screen has two visible modes:

1. `face`
2. `usage_card`

There is also a short-lived transition state between them.

### Active Usage Rule

Incoming payloads are considered meaningful activity only when at least one of these fields changes value:

- `window_pct`
- `week_pct`
- `reset_text`

When any of those fields changes:

- update the displayed usage values
- switch to `usage_card` mode if not already there
- extend the usage display deadline to `now + 3 minutes`

If no relevant field changes before the deadline expires:

- transition back to `face` mode

### Mixed Strategy Detail

This follows the user's preferred mixed strategy:

- usage mode is triggered by data changes
- usage mode stays alive for a fixed window after each change
- additional changes renew that window

This avoids needing an explicit upstream `active` flag while still behaving as if the display knows when Codex is "active enough" to deserve the usage card.

## Visual Design

### Face Mode

The idle face should use a black stage with floating facial features rather than a full head outline.

Visual direction:

- pure black background
- large rounded white eyes
- small colored highlights
- subtle mouth
- optional light blush for warm expressions

The face should feel close to StackChan in spirit:

- expressive through eye shape and gaze
- simple geometry
- high contrast
- cute rather than detailed

### Face Expressions

First version includes five expressions:

1. `neutral`
2. `happy`
3. `curious`
4. `sleepy`
5. `angry_pout`

### Face Animation

First version includes three small continuous animations:

1. random blink
2. small gaze drift
3. tiny breathing-style bob

All motion should stay restrained so the character feels alive without looking noisy.

### Usage Card Mode

The usage card should preserve the existing visual concept already present in the project rather than redesigning it from scratch.

Allowed refinements:

- cleaner redraw boundaries
- smoother entrance and exit
- minor spacing or contrast adjustments if needed for transition quality

Not in scope for this phase:

- mixing face and usage card on screen at the same time
- replacing the card with a new visual language

## Transitions

Transitions should feel calm and readable on the small display.

Recommended first implementation:

- `face -> usage_card`: short fade/cover or slide-up reveal in `250-400ms`
- `usage_card -> face`: soft fade-out or upward release in `250-400ms`

The transition system should:

- avoid large allocations
- avoid full-frame heavy effects that may cause visible stutter
- be implementable with simple stepped redraws

Preference order:

1. stepped wipe or slide
2. simple dither-like fade illusion
3. hard cut only as a fallback for debugging

## Architecture

### `UsageStateTracker`

Purpose:

- compare new payloads with the previously displayed values
- decide whether the update counts as activity
- track the usage display expiry timestamp

Responsibilities:

- own the latest usage data
- return whether a payload caused a meaningful change
- report whether usage mode should still be visible

### `ScreenModeController`

Purpose:

- decide which screen mode should be visible right now
- manage transition timing and direction

Responsibilities:

- hold current mode
- start transitions when target mode changes
- expose transition progress to renderers

### `FaceState`

Purpose:

- hold the current expression and animation parameters

Responsibilities:

- eye openness
- gaze offsets
- mouth parameters
- accent visibility and color

### `FaceAnimator`

Purpose:

- advance the idle character over time

Responsibilities:

- schedule blinks
- move gaze subtly
- update bobbing offsets
- optionally bias animation based on the current expression

### `FaceRenderer`

Purpose:

- draw the face based on `FaceState`

Responsibilities:

- clear face scene
- draw eyes
- draw pupils or eye slits
- draw highlights
- draw mouth and optional blush

### Existing Usage Card Renderer

The current card drawing logic can remain conceptually separate.

Plan:

- keep the existing static/dynamic card split where useful
- adapt it so it works under mode control and transition redraw boundaries

## Data Flow

1. Serial or HTTP fetch receives JSON.
2. JSON is parsed into candidate usage values.
3. `UsageStateTracker` compares candidate values with current values.
4. If meaningful usage fields changed, the tracker extends the active deadline.
5. `ScreenModeController` chooses `usage_card` while the deadline is valid, otherwise `face`.
6. The render loop updates animation and redraws the active mode or transition.

## Error Handling

- Invalid JSON should not affect the current display mode except for existing sync/error text behavior if retained.
- Repeated payloads with identical usage values should not retrigger the card timer.
- Wi-Fi disconnects or HTTP errors should not force the display into usage mode.
- If transition rendering proves too slow, the implementation may temporarily fall back to a simpler wipe while preserving the same mode behavior.

## Testing Strategy

### Manual Behavior Checks

Verify:

1. boot enters face mode
2. first meaningful usage update transitions to usage card
3. repeated identical payloads do not renew the timer
4. changed payloads renew the timer
5. after 3 minutes of no meaningful change, screen returns to face mode
6. another change after idle transitions back to usage card cleanly

### Interaction Checks

Verify the face remains animated during idle and freezes only if explicitly intended during transitions.

### Performance Checks

Verify:

- no objectionable flicker
- no obvious tearing from redraw strategy
- acceptable frame smoothness on the ESP32-C3

## Scope

### In Scope

- dual-mode display behavior
- StackChan-inspired geometric face
- 5 expressions
- 3 subtle idle animations
- meaningful-change-driven usage display with 3-minute renewal window
- smooth mode transitions

### Out of Scope

- 3D face rendering
- bitmap-based expression packs
- voice-synced mouth animation
- touch interaction
- upstream API contract changes
- concurrent face + usage composite layout

## Implementation Notes

- Prefer keeping network and parsing flow in `main.cpp` while extracting display responsibilities into focused modules.
- Favor small structs and deterministic redraws over complex scene systems.
- Keep memory usage modest and avoid framebuffers unless later testing proves they are necessary.

## Success Criteria

The feature is successful when:

- the screen feels alive and cute while idle
- usage information appears only when Codex usage actually changes
- the usage card remains visible while updates keep arriving
- the display returns to a pleasant idle face after 3 minutes of unchanged usage
- transitions feel intentional rather than abrupt
