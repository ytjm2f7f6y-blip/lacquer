#pragma once
// The space types (from long player's parameter table).
namespace lp {
enum SpaceType { sClean, sStudio, sPlate, sSpring, sChamber, sRoom, kNumSpaceTypes };
inline const char* spaceTypeName(int t) {
    static const char* n[kNumSpaceTypes] = {"clean", "studio", "plate", "spring", "chamber", "room"};
    return n[t];
}
}  // namespace lp
