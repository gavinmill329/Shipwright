#pragma once

// Mario Mode: play Ocarina of Time as Mario, using libsm64 (Super Mario 64 decomp as a library).
//
// libsm64 is loaded at runtime (sm64.dll / libsm64.so / libsm64.dylib next to the executable) and reads
// Mario's model, animations and sounds from the user's own Super Mario 64 (USA) ROM, which is looked for
// next to the executable. Nothing from either game is shipped with this code.
//
// Include z64.h before this header.

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct MarioFrameState {
    int32_t valid;     // Mario exists and was ticked this frame
    float pos[3];      // OoT world units (feet position)
    float vel[3];      // OoT world units per game frame
    float forwardVel;  // OoT world units per game frame
    int16_t faceYaw;   // OoT binary angle
    int32_t grounded;  // standing / walking on a floor
    int32_t inWater;   // swimming
    int32_t attack;    // MARIO_ATTACK_*
    uint32_t action;   // raw SM64 action id
} MarioFrameState;

enum {
    MARIO_ATTACK_NONE,
    MARIO_ATTACK_STRIKE, // punch, kick, dive, slide kick
    MARIO_ATTACK_STOMP,  // falling onto something
    MARIO_ATTACK_POUND,  // ground pound
};

// True when the option is on and libsm64 + an SM64 ROM were found. Safe to call every frame.
int32_t MarioMode_IsActive(void);
// True when Link's model should be hidden because Mario is drawn instead.
int32_t MarioMode_HideLink(void);

// Mario is in control: ticks Mario with the player's input and reports where he ended up.
// suppressA: the A button press belongs to Link this frame (talk, open, climb...).
void MarioMode_TickFree(PlayState* play, Player* player, Input* input, int32_t suppressA, MarioFrameState* out);
// Link is in control (cutscene, talking, climbing, knockback...): Mario is kept on Link's position.
void MarioMode_TickFollow(PlayState* play, Player* player);
// Hand control back to Mario at Link's current position and speed.
void MarioMode_TakeOverFromLink(PlayState* play, Player* player);
// Mario stomped something this frame: bounce him up a little.
void MarioMode_Bounce(void);
// SM64 units per OoT unit.
float MarioMode_GetScale(void);

void MarioMode_Draw(PlayState* play, Player* player);

#ifdef __cplusplus
}
#endif
