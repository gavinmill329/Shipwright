// Mario Mode - Mario (via libsm64) as the playable character in Ship of Harkinian.
//
// How it fits together:
//  * libsm64 is a shared library built from the Super Mario 64 decompilation. It is loaded at runtime and reads
//    Mario's textures, animations and sounds from the user's own SM64 (USA) ROM.
//  * Every game frame the scene's collision (static + moving "dynapoly" actors + water boxes) is mirrored into
//    libsm64, Mario is ticked with the controller input, and Link (invisible) is moved to wherever Mario went, so
//    loading zones, doors, NPCs, enemies, switches and the camera keep working on Link's side.
//  * z_player.c decides who is in control each frame (Mario, or Link for talking/doors/cutscenes/climbing...).
//  * Mario is drawn with the game's own display lists in place of Link's model.

#include <SDL2/SDL.h>
#include <spdlog/spdlog.h>
#include <libultraship/bridge/consolevariablebridge.h>
#include <ship/Context.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "soh/Enhancements/game-interactor/GameInteractor.h"
#include "soh/ShipInit.hpp"
#include "soh/OTRGlobals.h"

extern "C" {
#include "z64.h"
#include "macros.h"
#include "functions.h"
#include "variables.h"
extern PlayState* gPlayState;
}

#include "soh/frame_interpolation.h"

#include "MarioMode.h"

// ------------------------------------------------------------------------------------------------
// libsm64 API (subset of libsm64.h; the library is loaded dynamically so we only need the types)
// ------------------------------------------------------------------------------------------------
namespace {

struct SM64Surface {
    int16_t type;
    int16_t force;
    uint16_t terrain;
    int32_t vertices[3][3];
};

struct SM64MarioInputs {
    float camLookX, camLookZ;
    float stickX, stickY;
    uint8_t buttonA, buttonB, buttonZ;
};

struct SM64ObjectTransform {
    float position[3];
    float eulerRotation[3];
};

struct SM64SurfaceObject {
    SM64ObjectTransform transform;
    uint32_t surfaceCount;
    SM64Surface* surfaces;
};

struct SM64MarioState {
    float position[3];
    float velocity[3];
    float faceAngle;
    float forwardVelocity;
    int16_t health;
    uint32_t action;
    int32_t animID;
    int16_t animFrame;
    uint32_t flags;
    uint32_t particleFlags;
    int16_t invincTimer;
};

struct SM64MarioGeometryBuffers {
    float* position;
    float* normal;
    float* color;
    float* uv;
    uint16_t numTrianglesUsed;
};

constexpr int SM64_TEXTURE_WIDTH = 64 * 11;
constexpr int SM64_TEXTURE_HEIGHT = 64;
constexpr int SM64_GEO_MAX_TRIANGLES = 1024;
constexpr int SM64_NUM_TEXTURES = 11;

// SM64 constants (from the decomp's sm64.h / surface_terrains.h)
constexpr uint32_t ACT_GROUP_MASK = 0x000001C0;
constexpr uint32_t ACT_GROUP_SUBMERGED = 0x000000C0;
constexpr uint32_t ACT_FLAG_AIR = 1 << 11;
constexpr uint32_t ACT_IDLE = 0x0C400201;
constexpr uint32_t ACT_FREEFALL = 0x0100088C;
constexpr uint32_t ACT_DIVE = 0x0188088A;
constexpr uint32_t ACT_DIVE_SLIDE = 0x00880456;
constexpr uint32_t ACT_SLIDE_KICK = 0x018008AA;
constexpr uint32_t ACT_SLIDE_KICK_SLIDE = 0x0080045A;
constexpr uint32_t ACT_GROUND_POUND = 0x008008A9;
constexpr uint32_t ACT_GROUND_POUND_LAND = 0x0080023C;
constexpr uint32_t ACT_WATER_PUNCH = 0x300024E1;
constexpr uint32_t MARIO_PUNCHING = 0x00100000;
constexpr uint32_t MARIO_KICKING = 0x00200000;
constexpr uint32_t MARIO_TRIPPING = 0x00400000;
constexpr int16_t SURFACE_DEFAULT = 0x0000;
constexpr int16_t SURFACE_ICE = 0x002E;
constexpr uint16_t TERRAIN_GRASS = 0x0000;
constexpr uint16_t TERRAIN_STONE = 0x0001;
constexpr uint16_t TERRAIN_SNOW = 0x0002;
constexpr uint16_t TERRAIN_SAND = 0x0003;
constexpr uint16_t TERRAIN_WATER = 0x0005;
constexpr uint16_t MARIO_FULL_HEALTH = 0x0880;

struct Sm64Api {
    void (*global_init)(const uint8_t* rom, uint8_t* outTexture) = nullptr;
    void (*audio_init)(const uint8_t* rom) = nullptr;
    uint32_t (*audio_tick)(uint32_t numQueuedSamples, uint32_t numDesiredSamples, int16_t* audioBuffer) = nullptr;
    void (*static_surfaces_load)(const SM64Surface* surfaces, uint32_t count) = nullptr;
    int32_t (*mario_create)(float x, float y, float z) = nullptr;
    void (*mario_tick)(int32_t id, const SM64MarioInputs* in, SM64MarioState* out, SM64MarioGeometryBuffers* geo) =
        nullptr;
    void (*mario_delete)(int32_t id) = nullptr;
    void (*set_mario_action)(int32_t id, uint32_t action) = nullptr;
    void (*set_mario_position)(int32_t id, float x, float y, float z) = nullptr;
    void (*set_mario_faceangle)(int32_t id, float y) = nullptr;
    void (*set_mario_velocity)(int32_t id, float x, float y, float z) = nullptr;
    void (*set_mario_forward_velocity)(int32_t id, float vel) = nullptr;
    void (*set_mario_health)(int32_t id, uint16_t health) = nullptr;
    void (*set_mario_water_level)(int32_t id, signed int level) = nullptr;
    uint32_t (*surface_object_create)(const SM64SurfaceObject* obj) = nullptr;
    void (*surface_object_move)(uint32_t id, const SM64ObjectTransform* t) = nullptr;
    void (*surface_object_delete)(uint32_t id) = nullptr;
    void (*set_sound_volume)(float vol) = nullptr;                                  // optional
    void (*register_debug_print_function)(void (*fn)(const char*)) = nullptr; // optional
};

// ------------------------------------------------------------------------------------------------
// State
// ------------------------------------------------------------------------------------------------
#define CVAR_MARIO_ENABLED CVAR_ENHANCEMENT("MarioMode.Enabled")
#define CVAR_MARIO_SCALE CVAR_ENHANCEMENT("MarioMode.Scale")
#define CVAR_MARIO_VOLUME CVAR_ENHANCEMENT("MarioMode.Volume")
#define CVAR_MARIO_ROM_PATH CVAR_ENHANCEMENT("MarioMode.RomPath")

enum class LoadState { NotTried, Ready, Failed };

Sm64Api sApi;
void* sLibHandle = nullptr;
LoadState sLoadState = LoadState::NotTried;
std::string sStatus = "Not loaded yet. Turn the option on to load it.";

std::vector<uint8_t> sRom;
uint8_t sTextureAtlas[4 * SM64_TEXTURE_WIDTH * SM64_TEXTURE_HEIGHT];
alignas(8) uint8_t sTextures[SM64_NUM_TEXTURES][64 * 64 * 4];

// Mario
int32_t sMarioId = -1;
SM64MarioState sState{};
float sGeoPos[9 * SM64_GEO_MAX_TRIANGLES];
float sGeoNormal[9 * SM64_GEO_MAX_TRIANGLES];
float sGeoColor[9 * SM64_GEO_MAX_TRIANGLES];
float sGeoUv[6 * SM64_GEO_MAX_TRIANGLES];
SM64MarioGeometryBuffers sGeo{ sGeoPos, sGeoNormal, sGeoColor, sGeoUv, 0 };
float sTickAccumulator = 0.0f;
bool sFollowingLink = false; // geometry should be drawn at Link's position
bool sBounce = false;

// Scene tracking
PlayState* sLoadedPlay = nullptr;
CollisionHeader* sLoadedHeader = nullptr;
bool sSceneDirty = true;

struct DynaTrack {
    bool used = false;
    Actor* actor = nullptr;
    CollisionHeader* header = nullptr;
    Vec3f scale{};
    uint32_t objId = 0;
};
DynaTrack sDyna[BG_ACTOR_MAX];

// Audio
SDL_AudioDeviceID sAudioDev = 0;
std::thread sAudioThread;
std::atomic<bool> sAudioRunning{ false };

// ------------------------------------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------------------------------------
float Scale() {
    float s = CVarGetFloat(CVAR_MARIO_SCALE, 3.2f);
    return s < 1.0f ? 1.0f : (s > 8.0f ? 8.0f : s);
}

float TicksPerFrame() {
    // SM64 runs at 30 ticks per second, OoT gameplay at 60 / R_UPDATE_RATE frames per second (normally 20).
    s32 rate = R_UPDATE_RATE;
    if (rate <= 0) {
        rate = 3;
    }
    return 30.0f * rate / 60.0f;
}

int16_t RadiansToBinang(float r) {
    return (int16_t)(r / 3.14159f * 32768.0f);
}

float BinangToRadians(int16_t a) {
    return (float)a / 32768.0f * 3.14159f;
}

void DebugPrint(const char* msg) {
    SPDLOG_INFO("[libsm64] {}", msg);
}

template <typename T> bool LoadSym(T& out, const char* name, bool required = true) {
    out = reinterpret_cast<T>(SDL_LoadFunction(sLibHandle, name));
    if (out == nullptr && required) {
        sStatus = std::string("The Mario library is missing ") + name + ". Re-download the Mario build.";
        return false;
    }
    return true;
}

std::vector<std::string> SearchDirectories() {
    std::vector<std::string> dirs;
    auto add = [&](const std::string& d) {
        if (d.empty()) {
            return;
        }
        for (auto& existing : dirs) {
            if (existing == d) {
                return;
            }
        }
        dirs.push_back(d);
    };
    try {
        add(Ship::Context::GetAppBundlePath());
        add(Ship::Context::GetAppDirectoryPath(appShortName));
        add(std::filesystem::current_path().string());
    } catch (...) {}
    std::vector<std::string> withRoms = dirs;
    for (auto& d : dirs) {
        withRoms.push_back(d + "/roms");
        withRoms.push_back(d + "/mario");
    }
    return withRoms;
}

bool LoadSm64Library() {
#if defined(_WIN32)
    const char* libName = "sm64.dll";
#elif defined(__APPLE__)
    const char* libName = "libsm64.dylib";
#else
    const char* libName = "libsm64.so";
#endif
    for (auto& dir : SearchDirectories()) {
        std::string path = dir + "/" + libName;
        std::error_code ec;
        if (std::filesystem::exists(path, ec)) {
            sLibHandle = SDL_LoadObject(path.c_str());
            if (sLibHandle != nullptr) {
                SPDLOG_INFO("[MarioMode] Loaded {}", path);
                break;
            }
            SPDLOG_ERROR("[MarioMode] Failed to load {}: {}", path, SDL_GetError());
        }
    }
    if (sLibHandle == nullptr) {
        sStatus = std::string("Couldn't find ") + libName + ". It must sit next to the game's .exe.";
        return false;
    }

    return LoadSym(sApi.global_init, "sm64_global_init") && LoadSym(sApi.audio_init, "sm64_audio_init") &&
           LoadSym(sApi.audio_tick, "sm64_audio_tick") &&
           LoadSym(sApi.static_surfaces_load, "sm64_static_surfaces_load") &&
           LoadSym(sApi.mario_create, "sm64_mario_create") && LoadSym(sApi.mario_tick, "sm64_mario_tick") &&
           LoadSym(sApi.mario_delete, "sm64_mario_delete") && LoadSym(sApi.set_mario_action, "sm64_set_mario_action") &&
           LoadSym(sApi.set_mario_position, "sm64_set_mario_position") &&
           LoadSym(sApi.set_mario_faceangle, "sm64_set_mario_faceangle") &&
           LoadSym(sApi.set_mario_velocity, "sm64_set_mario_velocity") &&
           LoadSym(sApi.set_mario_forward_velocity, "sm64_set_mario_forward_velocity") &&
           LoadSym(sApi.set_mario_health, "sm64_set_mario_health") &&
           LoadSym(sApi.set_mario_water_level, "sm64_set_mario_water_level") &&
           LoadSym(sApi.surface_object_create, "sm64_surface_object_create") &&
           LoadSym(sApi.surface_object_move, "sm64_surface_object_move") &&
           LoadSym(sApi.surface_object_delete, "sm64_surface_object_delete") &&
           LoadSym(sApi.set_sound_volume, "sm64_set_sound_volume", false) &&
           LoadSym(sApi.register_debug_print_function, "sm64_register_debug_print_function", false);
}

// Converts .v64 (byte swapped) and .n64 (little endian) dumps to the big-endian .z64 layout libsm64 expects.
bool NormalizeRom(std::vector<uint8_t>& rom) {
    if (rom.size() < 0x40) {
        return false;
    }
    if (rom[0] == 0x80 && rom[1] == 0x37 && rom[2] == 0x12 && rom[3] == 0x40) {
        return true;
    }
    if (rom[0] == 0x37 && rom[1] == 0x80 && rom[2] == 0x40 && rom[3] == 0x12) {
        for (size_t i = 0; i + 1 < rom.size(); i += 2) {
            std::swap(rom[i], rom[i + 1]);
        }
        return true;
    }
    if (rom[0] == 0x40 && rom[1] == 0x12 && rom[2] == 0x37 && rom[3] == 0x80) {
        for (size_t i = 0; i + 3 < rom.size(); i += 4) {
            std::swap(rom[i], rom[i + 3]);
            std::swap(rom[i + 1], rom[i + 2]);
        }
        return true;
    }
    return false;
}

bool IsSm64UsRom(const std::vector<uint8_t>& rom) {
    // Internal name "SUPER MARIO 64" at 0x20, game code "NSME" (US) at 0x3B.
    return rom.size() >= 0x800000 && std::memcmp(&rom[0x20], "SUPER MARIO 64", 14) == 0 && rom[0x3B] == 'N' &&
           rom[0x3C] == 'S' && rom[0x3D] == 'M' && rom[0x3E] == 'E';
}

bool TryRomFile(const std::filesystem::path& path) {
    std::error_code ec;
    auto size = std::filesystem::file_size(path, ec);
    // SM64 is an 8 MiB cartridge; skip anything else (like the Ocarina of Time ROM sitting in the same folder).
    if (ec || size != 0x800000) {
        return false;
    }
    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return false;
    }
    std::vector<uint8_t> data((size_t)size);
    file.read(reinterpret_cast<char*>(data.data()), (std::streamsize)size);
    if (!file || !NormalizeRom(data) || !IsSm64UsRom(data)) {
        return false;
    }
    sRom = std::move(data);
    SPDLOG_INFO("[MarioMode] Using Super Mario 64 ROM {}", path.string());
    return true;
}

bool FindRom() {
    const char* custom = CVarGetString(CVAR_MARIO_ROM_PATH, "");
    if (custom != nullptr && custom[0] != '\0' && TryRomFile(custom)) {
        return true;
    }
    for (auto& dir : SearchDirectories()) {
        std::error_code ec;
        if (!std::filesystem::is_directory(dir, ec)) {
            continue;
        }
        for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
            if (!entry.is_regular_file(ec)) {
                continue;
            }
            std::string ext = entry.path().extension().string();
            for (auto& c : ext) {
                c = (char)tolower((unsigned char)c);
            }
            if ((ext == ".z64" || ext == ".n64" || ext == ".v64") && TryRomFile(entry.path())) {
                return true;
            }
        }
    }
    sStatus = "Couldn't find a Super Mario 64 (USA) ROM. Put it (.z64/.n64/.v64) next to the game's .exe.";
    return false;
}

void AudioThreadMain() {
    using clock = std::chrono::steady_clock;
    int16_t buffer[544 * 2 * 2];
    auto next = clock::now();
    while (sAudioRunning.load()) {
        uint32_t queued = SDL_GetQueuedAudioSize(sAudioDev) / 4;
        uint32_t samples = sApi.audio_tick(queued, 1100, buffer);
        if (queued < 6000) {
            SDL_QueueAudio(sAudioDev, buffer, samples * 2 * 4);
        }
        next += std::chrono::milliseconds(33);
        std::this_thread::sleep_until(next);
        if (clock::now() - next > std::chrono::milliseconds(500)) {
            next = clock::now(); // resync after a stall (e.g. window dragged)
        }
    }
}

void StartAudio() {
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        SPDLOG_WARN("[MarioMode] No audio for Mario: {}", SDL_GetError());
        return;
    }
    SDL_AudioSpec want, have;
    SDL_zero(want);
    want.freq = 32000;
    want.format = AUDIO_S16;
    want.channels = 2;
    want.samples = 512;
    sAudioDev = SDL_OpenAudioDevice(nullptr, 0, &want, &have, 0);
    if (sAudioDev == 0) {
        SPDLOG_WARN("[MarioMode] No audio device for Mario: {}", SDL_GetError());
        return;
    }
    SDL_PauseAudioDevice(sAudioDev, 0);
    sAudioRunning = true;
    sAudioThread = std::thread(AudioThreadMain);
    sAudioThread.detach();
}

bool EnsureLoaded() {
    if (sLoadState == LoadState::Ready) {
        return true;
    }
    if (sLoadState == LoadState::Failed) {
        return false;
    }
    sLoadState = LoadState::Failed;
    if (!LoadSm64Library() || !FindRom()) {
        SPDLOG_WARN("[MarioMode] {}", sStatus);
        return false;
    }
    if (sApi.register_debug_print_function != nullptr) {
        sApi.register_debug_print_function(DebugPrint);
    }
    sApi.global_init(sRom.data(), sTextureAtlas);

    // Split the 704x64 atlas into eleven 64x64 textures so each fits a single texture load.
    for (int t = 0; t < SM64_NUM_TEXTURES; t++) {
        for (int y = 0; y < 64; y++) {
            std::memcpy(&sTextures[t][y * 64 * 4], &sTextureAtlas[(y * SM64_TEXTURE_WIDTH + t * 64) * 4], 64 * 4);
        }
    }

    sApi.audio_init(sRom.data());
    StartAudio();

    sLoadState = LoadState::Ready;
    sStatus = "Ready. Mario will appear when you load a file.";
    return true;
}

// ------------------------------------------------------------------------------------------------
// Collision mirroring
// ------------------------------------------------------------------------------------------------
uint16_t TerrainForPoly(PlayState* play, CollisionPoly* poly, s32 bgId) {
    switch (SurfaceType_GetSfx(&play->colCtx, poly, bgId)) {
        case 0:  // dirt
        case 6:  // tall grass
        case 8:  // grass
        case 11: // packed dirt / bushes
            return TERRAIN_GRASS;
        case 1: // sand
            return TERRAIN_SAND;
        case 4: // shallow water
        case 5: // deep water
            return TERRAIN_WATER;
        case 13: // ice
            return TERRAIN_SNOW;
        default:
            return TERRAIN_STONE;
    }
}

// Converts one OoT collision triangle. verts are in the header's space, multiplied by mul (scale * SM64 units).
bool ConvertPoly(PlayState* play, CollisionHeader* header, CollisionPoly* poly, s32 bgId, const Vec3f& mul,
                 SM64Surface& out) {
    // Polys flagged as "ignored by entities" don't block Link, so they shouldn't block Mario either.
    if (COLPOLY_VIA_FLAG_TEST(poly->flags_vIA, 2) || SurfaceType_IsIgnoredByEntities(&play->colCtx, poly, bgId)) {
        return false;
    }
    u16 ids[3] = { (u16)COLPOLY_VTX_INDEX(poly->flags_vIA), (u16)COLPOLY_VTX_INDEX(poly->flags_vIB),
                   (u16)poly->vIC };
    float v[3][3];
    for (int i = 0; i < 3; i++) {
        if (ids[i] >= header->numVertices) {
            return false;
        }
        Vec3s* src = &header->vtxList[ids[i]];
        v[i][0] = src->x * mul.x;
        v[i][1] = src->y * mul.y;
        v[i][2] = src->z * mul.z;
    }

    // SM64 derives the facing from vertex order: n = (v2 - v1) x (v3 - v2). Make it agree with OoT's stored normal.
    float ax = v[1][0] - v[0][0], ay = v[1][1] - v[0][1], az = v[1][2] - v[0][2];
    float bx = v[2][0] - v[1][0], by = v[2][1] - v[1][1], bz = v[2][2] - v[1][2];
    float nx = ay * bz - az * by;
    float ny = az * bx - ax * bz;
    float nz = ax * by - ay * bx;
    if (nx * nx + ny * ny + nz * nz < 1e-6f) {
        return false; // degenerate
    }
    float dot = nx * poly->normal.x + ny * poly->normal.y + nz * poly->normal.z;
    int order[3] = { 0, 1, 2 };
    if (dot < 0.0f) {
        order[1] = 2;
        order[2] = 1;
    }

    out.type = (SurfaceType_GetFloorType(&play->colCtx, poly, bgId) == 5) ? SURFACE_ICE : SURFACE_DEFAULT;
    out.force = 0;
    out.terrain = TerrainForPoly(play, poly, bgId);
    for (int i = 0; i < 3; i++) {
        for (int c = 0; c < 3; c++) {
            out.vertices[i][c] = (int32_t)lroundf(v[order[i]][c]);
        }
    }
    return true;
}

void DeleteDyna(int i) {
    if (sDyna[i].used) {
        sApi.surface_object_delete(sDyna[i].objId);
    }
    sDyna[i] = DynaTrack{};
}

// Mario himself is kept alive across scenes: the SM64 sound engine keeps pointers into his object while sounds
// play, so deleting him mid-sound is unsafe. He is simply moved to Link when the next scene starts.
void UnloadScene() {
    for (int i = 0; i < BG_ACTOR_MAX; i++) {
        DeleteDyna(i);
    }
    sLoadedPlay = nullptr;
    sLoadedHeader = nullptr;
}

void LoadStaticCollision(PlayState* play) {
    CollisionHeader* header = play->colCtx.colHeader;
    std::vector<SM64Surface> surfaces;
    if (header != nullptr) {
        float s = Scale();
        Vec3f mul = { s, s, s };
        surfaces.reserve(header->numPolygons);
        for (u32 i = 0; i < header->numPolygons; i++) {
            SM64Surface surf;
            if (ConvertPoly(play, header, &header->polyList[i], BGCHECK_SCENE, mul, surf)) {
                surfaces.push_back(surf);
            }
        }
    }
    sApi.static_surfaces_load(surfaces.data(), (uint32_t)surfaces.size());
    sLoadedPlay = play;
    sLoadedHeader = header;
    SPDLOG_INFO("[MarioMode] Loaded {} collision triangles for scene {}", surfaces.size(), play->sceneNum);
}

SM64ObjectTransform DynaTransform(const BgActor& bg) {
    float s = Scale();
    SM64ObjectTransform t;
    t.position[0] = bg.curTransform.pos.x * s;
    t.position[1] = bg.curTransform.pos.y * s;
    t.position[2] = bg.curTransform.pos.z * s;
    // libsm64 takes degrees and negates them internally; this round-trips to OoT's binary angles.
    t.eulerRotation[0] = -(float)bg.curTransform.rot.x * 180.0f / 32768.0f;
    t.eulerRotation[1] = -(float)bg.curTransform.rot.y * 180.0f / 32768.0f;
    t.eulerRotation[2] = -(float)bg.curTransform.rot.z * 180.0f / 32768.0f;
    return t;
}

void UpdateDynaCollision(PlayState* play) {
    DynaCollisionContext& dyna = play->colCtx.dyna;
    float s = Scale();
    for (int i = 0; i < BG_ACTOR_MAX; i++) {
        BgActor& bg = dyna.bgActors[i];
        bool active = (dyna.bgActorFlags[i] & 1) && !(dyna.bgActorFlags[i] & 2) && bg.actor != nullptr &&
                      bg.colHeader != nullptr && bg.colHeader->numPolygons > 0;
        if (!active) {
            DeleteDyna(i);
            continue;
        }
        DynaTrack& track = sDyna[i];
        bool changed = !track.used || track.actor != bg.actor || track.header != bg.colHeader ||
                       track.scale.x != bg.curTransform.scale.x || track.scale.y != bg.curTransform.scale.y ||
                       track.scale.z != bg.curTransform.scale.z;
        if (changed) {
            DeleteDyna(i);
            std::vector<SM64Surface> surfaces;
            surfaces.reserve(bg.colHeader->numPolygons);
            Vec3f mul = { bg.curTransform.scale.x * s, bg.curTransform.scale.y * s, bg.curTransform.scale.z * s };
            for (u32 p = 0; p < bg.colHeader->numPolygons; p++) {
                SM64Surface surf;
                if (ConvertPoly(play, bg.colHeader, &bg.colHeader->polyList[p], i, mul, surf)) {
                    surfaces.push_back(surf);
                }
            }
            if (surfaces.empty()) {
                continue;
            }
            SM64SurfaceObject obj;
            obj.transform = DynaTransform(bg);
            obj.surfaceCount = (uint32_t)surfaces.size();
            obj.surfaces = surfaces.data();
            track.objId = sApi.surface_object_create(&obj);
            track.used = true;
            track.actor = bg.actor;
            track.header = bg.colHeader;
            track.scale = bg.curTransform.scale;
        } else {
            SM64ObjectTransform t = DynaTransform(bg);
            sApi.surface_object_move(track.objId, &t);
        }
    }
}

// Rebuilds everything when the scene (or scale) changes. Returns false if there is nothing to play in.
bool sNeedsTeleport = false;

bool SyncScene(PlayState* play) {
    static float sLoadedScale = 0.0f;
    if (play == nullptr) {
        return false;
    }
    if (sSceneDirty || play != sLoadedPlay || play->colCtx.colHeader != sLoadedHeader || sLoadedScale != Scale()) {
        UnloadScene();
        LoadStaticCollision(play);
        sLoadedScale = Scale();
        sSceneDirty = false;
        sNeedsTeleport = true;
    }
    UpdateDynaCollision(play);
    return true;
}

void TeleportToLink(Player* player, bool keepMomentum) {
    float s = Scale();
    float tpf = TicksPerFrame();
    sApi.set_mario_position(sMarioId, player->actor.world.pos.x * s, player->actor.world.pos.y * s,
                            player->actor.world.pos.z * s);
    sApi.set_mario_faceangle(sMarioId, BinangToRadians(player->actor.shape.rot.y));
    if (keepMomentum) {
        sApi.set_mario_velocity(sMarioId, player->actor.velocity.x * s / tpf, player->actor.velocity.y * s / tpf,
                                player->actor.velocity.z * s / tpf);
        sApi.set_mario_forward_velocity(sMarioId, player->linearVelocity * s / tpf);
    } else {
        sApi.set_mario_velocity(sMarioId, 0.0f, 0.0f, 0.0f);
        sApi.set_mario_forward_velocity(sMarioId, 0.0f);
    }
    bool onGround = (player->actor.bgCheckFlags & BGCHECKFLAG_GROUND) != 0;
    sApi.set_mario_action(sMarioId, onGround ? ACT_IDLE : ACT_FREEFALL);
    sState.position[0] = player->actor.world.pos.x * s;
    sState.position[1] = player->actor.world.pos.y * s;
    sState.position[2] = player->actor.world.pos.z * s;
    sNeedsTeleport = false;
}

bool EnsureMario(PlayState* play, Player* player) {
    if (sMarioId >= 0) {
        if (sNeedsTeleport) {
            TeleportToLink(player, false);
        }
        return true;
    }
    float s = Scale();
    // libsm64 refuses to create Mario without a floor below him, so start slightly above Link's feet.
    sMarioId = sApi.mario_create(player->actor.world.pos.x * s, player->actor.world.pos.y * s + 20.0f,
                                 player->actor.world.pos.z * s);
    if (sMarioId < 0) {
        return false;
    }
    sApi.set_mario_faceangle(sMarioId, BinangToRadians(player->actor.shape.rot.y));
    sApi.set_mario_action(sMarioId, ACT_IDLE);
    sNeedsTeleport = false;
    sTickAccumulator = 0.0f;
    if (sApi.set_sound_volume != nullptr) {
        sApi.set_sound_volume(CVarGetFloat(CVAR_MARIO_VOLUME, 1.0f));
    }
    return true;
}

void UpdateWaterLevel(PlayState* play) {
    float s = Scale();
    f32 ySurface = 0.0f;
    WaterBox* box = nullptr;
    float x = sState.position[0] / s;
    float z = sState.position[2] / s;
    if (WaterBox_GetSurface1(play, &play->colCtx, x, z, &ySurface, &box)) {
        sApi.set_mario_water_level(sMarioId, (signed int)(ySurface * s));
    } else {
        sApi.set_mario_water_level(sMarioId, -1000000);
    }
}

void Tick(PlayState* play, const SM64MarioInputs& in) {
    if (sApi.set_sound_volume != nullptr) {
        static float sLastVolume = -1.0f;
        float vol = CVarGetFloat(CVAR_MARIO_VOLUME, 1.0f);
        if (vol != sLastVolume) {
            sApi.set_sound_volume(vol);
            sLastVolume = vol;
        }
    }
    sTickAccumulator += TicksPerFrame();
    int ticks = 0;
    while (sTickAccumulator >= 1.0f && ticks < 4) {
        UpdateWaterLevel(play);
        // Link's hearts are the real health bar; Mario never dies on his own.
        sApi.set_mario_health(sMarioId, MARIO_FULL_HEALTH);
        sApi.mario_tick(sMarioId, &in, &sState, &sGeo);
        sTickAccumulator -= 1.0f;
        ticks++;
    }
    if (sTickAccumulator > 4.0f) {
        sTickAccumulator = 0.0f;
    }
}

} // namespace

// ------------------------------------------------------------------------------------------------
// Public API
// ------------------------------------------------------------------------------------------------
extern "C" int32_t MarioMode_IsActive(void) {
    static bool sWasEnabled = false;
    bool enabled = CVarGetInteger(CVAR_MARIO_ENABLED, 1) != 0;
    if (!enabled) {
        if (sWasEnabled) {
            if (sLoadState == LoadState::Ready) {
                UnloadScene();
                sSceneDirty = true;
            } else if (sLoadState == LoadState::Failed) {
                sLoadState = LoadState::NotTried; // let the user retry after fixing files
            }
        }
        sWasEnabled = false;
        return false;
    }
    sWasEnabled = true;
    return EnsureLoaded();
}

extern "C" int32_t MarioMode_HideLink(void) {
    return MarioMode_IsActive() && sMarioId >= 0;
}

extern "C" float MarioMode_GetScale(void) {
    return Scale();
}

extern "C" void MarioMode_TickFree(PlayState* play, Player* player, Input* input, int32_t suppressA,
                                   MarioFrameState* out) {
    std::memset(out, 0, sizeof(*out));
    if (!MarioMode_IsActive() || !SyncScene(play) || !EnsureMario(play, player)) {
        return;
    }

    float s = Scale();
    SM64MarioInputs in{};
    float camX = play->view.eye.x * s;
    float camZ = play->view.eye.z * s;
    in.camLookX = sState.position[0] - camX;
    in.camLookZ = sState.position[2] - camZ;
    if (in.camLookX == 0.0f && in.camLookZ == 0.0f) {
        in.camLookZ = 1.0f;
    }
    float sx = input->cur.stick_x / 80.0f;
    float sy = input->cur.stick_y / 80.0f;
    in.stickX = sx > 1.0f ? 1.0f : (sx < -1.0f ? -1.0f : sx);
    in.stickY = -(sy > 1.0f ? 1.0f : (sy < -1.0f ? -1.0f : sy)); // libsm64 wants "down is positive"
    in.buttonA = (!suppressA && (input->cur.button & BTN_A)) ? 1 : 0;
    in.buttonB = (input->cur.button & BTN_B) ? 1 : 0;
    in.buttonZ = (input->cur.button & (BTN_Z | BTN_R)) ? 1 : 0;

    if (sBounce) {
        sApi.set_mario_velocity(sMarioId, sState.velocity[0], 40.0f, sState.velocity[2]);
        sBounce = false;
    }

    Tick(play, in);
    sFollowingLink = false;

    float tpf = TicksPerFrame();
    out->valid = true;
    for (int i = 0; i < 3; i++) {
        out->pos[i] = sState.position[i] / s;
        out->vel[i] = sState.velocity[i] / s * tpf;
    }
    out->forwardVel = sState.forwardVelocity / s * tpf;
    out->faceYaw = RadiansToBinang(sState.faceAngle);
    out->action = sState.action;
    out->inWater = (sState.action & ACT_GROUP_MASK) == ACT_GROUP_SUBMERGED;
    out->grounded = !(sState.action & ACT_FLAG_AIR) && !out->inWater;

    uint32_t a = sState.action;
    if (a == ACT_GROUND_POUND_LAND || (a == ACT_GROUND_POUND && sState.velocity[1] < 0.0f)) {
        out->attack = MARIO_ATTACK_POUND;
    } else if ((sState.flags & (MARIO_PUNCHING | MARIO_KICKING | MARIO_TRIPPING)) || a == ACT_DIVE ||
               a == ACT_DIVE_SLIDE || a == ACT_SLIDE_KICK || a == ACT_SLIDE_KICK_SLIDE || a == ACT_WATER_PUNCH) {
        out->attack = MARIO_ATTACK_STRIKE;
    } else if ((a & ACT_FLAG_AIR) && sState.velocity[1] < -10.0f) {
        out->attack = MARIO_ATTACK_STOMP;
    } else {
        out->attack = MARIO_ATTACK_NONE;
    }
}

extern "C" void MarioMode_TickFollow(PlayState* play, Player* player) {
    if (!MarioMode_IsActive() || !SyncScene(play) || !EnsureMario(play, player)) {
        return;
    }
    float s = Scale();
    sApi.set_mario_position(sMarioId, player->actor.world.pos.x * s, player->actor.world.pos.y * s,
                            player->actor.world.pos.z * s);
    sApi.set_mario_velocity(sMarioId, 0.0f, 0.0f, 0.0f);
    sApi.set_mario_forward_velocity(sMarioId, 0.0f);
    sApi.set_mario_faceangle(sMarioId, BinangToRadians(player->actor.shape.rot.y));
    SM64MarioInputs in{};
    in.camLookZ = 1.0f;
    Tick(play, in);
    sFollowingLink = true;
}

extern "C" void MarioMode_TakeOverFromLink(PlayState* play, Player* player) {
    if (!MarioMode_IsActive() || !SyncScene(play) || !EnsureMario(play, player)) {
        return;
    }
    TeleportToLink(player, true);
}

extern "C" void MarioMode_Bounce(void) {
    sBounce = true;
}

// ------------------------------------------------------------------------------------------------
// Drawing
// ------------------------------------------------------------------------------------------------
namespace {
constexpr int kMaxVtx = 3 * SM64_GEO_MAX_TRIANGLES;
constexpr int kMaxGfx = 4096;
Vtx sVtx[2][kMaxVtx];
Gfx sGfx[2][kMaxGfx];
int sBuf = 0;

int TextureIndexForTriangle(int tri) {
    float u = (sGeoUv[tri * 6 + 0] + sGeoUv[tri * 6 + 2] + sGeoUv[tri * 6 + 4]) / 3.0f;
    int idx = (int)floorf(u * SM64_NUM_TEXTURES);
    return (idx >= 0 && idx < SM64_NUM_TEXTURES) ? idx : -1; // -1: untextured (libsm64 uses uv = 1,1)
}

// Mario's colors with SM64-style lighting baked in: rgb = material color, a = light amount (applied in cycle 2,
// so textures like his eyes and the M logo get lit too).
void FillVertex(Vtx& vtx, int tri, int corner, int texIdx, const float* origin, float invAnchorScale) {
    int vi = tri * 3 + corner;
    float lx = sGeoPos[vi * 3 + 0] - origin[0];
    float ly = sGeoPos[vi * 3 + 1] - origin[1];
    float lz = sGeoPos[vi * 3 + 2] - origin[2];
    (void)invAnchorScale;
    vtx.v.ob[0] = (short)lroundf(lx);
    vtx.v.ob[1] = (short)lroundf(ly);
    vtx.v.ob[2] = (short)lroundf(lz);
    vtx.v.flag = 0;

    if (texIdx >= 0) {
        float u = sGeoUv[vi * 2 + 0] * SM64_NUM_TEXTURES - texIdx;
        float v = sGeoUv[vi * 2 + 1];
        vtx.v.tc[0] = (short)lroundf(u * 64.0f * 32.0f);
        vtx.v.tc[1] = (short)lroundf(v * 64.0f * 32.0f);
    } else {
        vtx.v.tc[0] = 0;
        vtx.v.tc[1] = 0;
    }

    const float k = 0.57735f; // normalize(1, 1, 1)
    float nx = sGeoNormal[vi * 3 + 0], ny = sGeoNormal[vi * 3 + 1], nz = sGeoNormal[vi * 3 + 2];
    float d = nx * k + ny * k + nz * k;
    float light = 0.5f + 0.5f * (d < 0.0f ? 0.0f : (d > 1.0f ? 1.0f : d));
    for (int c = 0; c < 3; c++) {
        float col = sGeoColor[vi * 3 + c];
        col = col < 0.0f ? 0.0f : (col > 1.0f ? 1.0f : col);
        vtx.v.cn[c] = (unsigned char)lroundf(col * 255.0f);
    }
    vtx.v.cn[3] = (unsigned char)lroundf(light * 255.0f);
}
} // namespace

extern "C" void MarioMode_Draw(PlayState* play, Player* player) {
    if (!MarioMode_HideLink() || sGeo.numTrianglesUsed == 0) {
        return;
    }
    // Hidden in first person so he doesn't block the view while aiming.
    if ((player->stateFlags1 & PLAYER_STATE1_FIRST_PERSON) || player->unk_6AD == 2) {
        return;
    }

    float s = Scale();
    Vec3f anchor;
    if (sFollowingLink) {
        anchor = player->actor.world.pos;
    } else {
        anchor = { sState.position[0] / s, sState.position[1] / s, sState.position[2] / s };
    }
    const float* origin = sState.position;

    sBuf ^= 1;
    Vtx* vtx = sVtx[sBuf];
    Gfx* g = sGfx[sBuf];
    Gfx* gEnd = g + kMaxGfx - 16;
    int vtxUsed = 0;
    int numTris = sGeo.numTrianglesUsed;
    if (numTris > SM64_GEO_MAX_TRIANGLES) {
        numTris = SM64_GEO_MAX_TRIANGLES;
    }

    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_2CYCLE);
    gDPSetRenderMode(g++, G_RM_PASS, G_RM_AA_ZB_OPA_SURF2);
    gSPLoadGeometryMode(g++, G_ZBUFFER | G_SHADE | G_SHADING_SMOOTH);
    gDPSetTextureLUT(g++, G_TT_NONE);
    gDPSetTexturePersp(g++, G_TP_PERSP);
    gDPSetTextureFilter(g++, G_TF_BILERP);

    // Draw untextured triangles first, then each of the 11 textures.
    for (int group = -1; group < SM64_NUM_TEXTURES; group++) {
        bool any = false;
        for (int t = 0; t < numTris; t++) {
            if (TextureIndexForTriangle(t) == group) {
                any = true;
                break;
            }
        }
        if (!any) {
            continue;
        }

        gDPPipeSync(g++);
        if (group < 0) {
            gSPTexture(g++, 0, 0, 0, G_TX_RENDERTILE, G_OFF);
            gDPSetCombineLERP(g++, 0, 0, 0, SHADE, 0, 0, 0, 1, COMBINED, 0, SHADE_ALPHA, 0, 0, 0, 0, 1);
        } else {
            gSPTexture(g++, 0xFFFF, 0xFFFF, 0, G_TX_RENDERTILE, G_ON);
            // cycle 1: mix(shade, texel, texel alpha)   cycle 2: * light
            gDPSetCombineLERP(g++, TEXEL0, SHADE, TEXEL0_ALPHA, SHADE, 0, 0, 0, 1, COMBINED, 0, SHADE_ALPHA, 0, 0, 0,
                              0, 1);
            gDPLoadTextureBlock(g++, sTextures[group], G_IM_FMT_RGBA, G_IM_SIZ_32b, 64, 64, 0, G_TX_CLAMP, G_TX_CLAMP,
                                G_TX_NOMASK, G_TX_NOMASK, G_TX_NOLOD, G_TX_NOLOD);
        }

        // Upload in batches of up to 20 triangles (60 vertices; the renderer holds 64).
        int batchStart = vtxUsed;
        int batchTris = 0;
        auto flush = [&]() {
            if (batchTris == 0) {
                return;
            }
            gSPVertex(g++, (uintptr_t)&vtx[batchStart], batchTris * 3, 0);
            for (int i = 0; i < batchTris; i++) {
                gSP1Triangle(g++, i * 3 + 0, i * 3 + 1, i * 3 + 2, 0);
            }
            batchStart = vtxUsed;
            batchTris = 0;
        };
        for (int t = 0; t < numTris; t++) {
            if (TextureIndexForTriangle(t) != group) {
                continue;
            }
            if (vtxUsed + 3 > kMaxVtx || g + 32 >= gEnd) {
                break;
            }
            for (int c = 0; c < 3; c++) {
                FillVertex(vtx[vtxUsed++], t, c, group, origin, 1.0f / s);
            }
            batchTris++;
            if (batchTris == 20) {
                flush();
            }
        }
        flush();
    }

    gDPPipeSync(g++);
    gDPSetCycleType(g++, G_CYC_1CYCLE);
    gSPEndDisplayList(g++);

    OPEN_DISPS(play->state.gfxCtx);

    FrameInterpolation_RecordActorPosRotMatrix();
    Matrix_Translate(anchor.x, anchor.y, anchor.z, MTXMODE_NEW);
    Matrix_Scale(1.0f / s, 1.0f / s, 1.0f / s, MTXMODE_APPLY);
    gSPMatrix(POLY_OPA_DISP++, MATRIX_NEWMTX(play->state.gfxCtx), G_MTX_NOPUSH | G_MTX_LOAD | G_MTX_MODELVIEW);
    gSPDisplayList(POLY_OPA_DISP++, sGfx[sBuf]);

    CLOSE_DISPS(play->state.gfxCtx);
}

// ------------------------------------------------------------------------------------------------
// Hooks & menu status
// ------------------------------------------------------------------------------------------------
const char* MarioMode_GetStatusText() {
    if (CVarGetInteger(CVAR_MARIO_ENABLED, 1) == 0) {
        return "Off.";
    }
    if (sLoadState == LoadState::Ready && sMarioId >= 0) {
        return "Mario is in Hyrule!";
    }
    return sStatus.c_str();
}

static RegisterShipInitFunc initFunc([]() {
    COND_HOOK(OnSceneInit, true, [](int16_t) { sSceneDirty = true; });
    COND_HOOK(OnPlayDestroy, true, []() {
        if (sLoadState == LoadState::Ready) {
            UnloadScene();
        }
        sSceneDirty = true;
    });
});
