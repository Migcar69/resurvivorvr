// Realistic VR hands from the SteamVR glove models (see vr_glove.cpp); vr_hands falls back to simple hands
// when they cannot be loaded.
#pragma once
#include <d3d11.h>

#include "vr_hands.h"

namespace vrglove {

bool init(ID3D11Device *dev, ID3D11DeviceContext *ctx);
bool ready();
// Where the index fingertip of a hand (0 left, 1 right) is for that hand state (LOCAL space)
bool index_tip(int hand, const vrhands::HandState &h, float out[3]);
// Where the index finger points when stretched out: from its tip, along the finger (unit vector, LOCAL space)
bool index_ray(int hand, const vrhands::HandState &h, float origin[3], float dir[3]);
void draw(const Mat4 &view_proj, const vrhands::HandState hands[2], bool srgb);

}  // namespace vrglove
