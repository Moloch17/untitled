#pragma once

#include <cstdint>

namespace net {

// Bump whenever the wire format changes incompatibly. Both ends check it during
// the handshake so a stale client fails with a clear error instead of decoding
// garbage.
constexpr uint32_t kProtocolVersion = 6;

// Tags the first four bytes of every UDP datagram. UDP sockets receive whatever
// the internet sends them; this discards obvious noise before parsing.
constexpr uint32_t kUdpMagic = 0x554E5431;  // "UNT1"

// Server-authoritative simulation rate. Snapshots go out at this rate and the
// client extrapolates between them.
constexpr int kServerTickHz = 60;

// What an account is allowed to do. Every privileged action is authorised
// against the level on the account behind the caller's session, so there is no
// shared secret to leak, and every action is attributable to a person.
enum class PermissionLevel : uint8_t {
    Player = 0,
    GameMaster = 1,
    Admin = 2,
};

// ---------------------------------------------------------------------------
// Client <-> auth server (TCP)
// ---------------------------------------------------------------------------
enum class AuthMessage : uint16_t {
    LoginRequest = 1,   // {version:u32, username:str, password:str}
    // {result:u8, token:str, worldHost:str, worldPort:u16, accountId:u64,
    //  permissionLevel:u8}
    LoginResponse = 2,
    RegisterRequest = 3,
    RegisterResponse = 4,

    // Administrative commands from the console. Each carries the caller's
    // session token; the server resolves it to an account and checks that
    // account's permission level.
    AdminAccountCreateRequest = 5,   // {token:str, username:str, password:str, level:u8}
    AdminAccountCreateResponse = 6,  // {result:u8}
    AdminAccountDeleteRequest = 7,   // {token:str, username:str}
    AdminAccountDeleteResponse = 8,  // {result:u8}
    AdminSetLevelRequest = 9,        // {token:str, username:str, level:u8}
    AdminSetLevelResponse = 10,      // {result:u8}
};

enum class AuthResult : uint8_t {
    Success = 0,
    InvalidCredentials = 1,
    VersionMismatch = 2,
    AccountExists = 3,
    ServerError = 4,
    MalformedRequest = 5,
    NotAuthorised = 6,
    NoSuchAccount = 7,
    // The caller is who they say they are, but their account isn't allowed.
    InsufficientPermission = 8,
    // The session has expired or was already redeemed; log in again.
    SessionExpired = 9,
};

const char* toString(AuthResult result);

// ---------------------------------------------------------------------------
// Auth/world server <-> database server (TCP RPC)
//
// Only the database server links libpq and talks to Postgres; everything else
// goes through these calls. Each request carries a caller-chosen id that the
// reply echoes back, so a connection can have several calls in flight.
// ---------------------------------------------------------------------------
enum class DbMessage : uint16_t {
    AccountLookupRequest = 100,   // {requestId:u32, username:str}
    // {requestId:u32, found:u8, accountId:u64, passwordHash:str, level:u8}
    AccountLookupResponse = 101,
    AccountCreateRequest = 102,   // {requestId:u32, username:str, passwordHash:str}
    AccountCreateResponse = 103,  // {requestId:u32, result:u8, accountId:u64}
    SessionCreateRequest = 104,   // {requestId:u32, accountId:u64, token:str, ttlSeconds:u32}
    SessionCreateResponse = 105,  // {requestId:u32, result:u8}
    SessionLookupRequest = 106,   // {requestId:u32, token:str}
    // {requestId:u32, found:u8, accountId:u64, username:str, level:u8}
    SessionLookupResponse = 107,
    SessionDeleteRequest = 108,   // {requestId:u32, token:str}
    SessionDeleteResponse = 109,  // {requestId:u32, result:u8}
    AccountDeleteRequest = 110,   // {requestId:u32, username:str}
    AccountDeleteResponse = 111,  // {requestId:u32, result:u8}
    AccountSetLevelRequest = 112,   // {requestId:u32, username:str, level:u8}
    AccountSetLevelResponse = 113,  // {requestId:u32, result:u8}
    AccountCreateWithLevelRequest = 114,  // {requestId:u32, username:str, hash:str, level:u8}
};

enum class DbResult : uint8_t {
    Ok = 0,
    NotFound = 1,
    Conflict = 2,
    Error = 3,
};

// ---------------------------------------------------------------------------
// Client <-> world server
//
// TCP carries anything that must not be lost or reordered: joining with a
// session token, and logging out. UDP carries the tick stream, where a dropped
// packet is better ignored than retransmitted -- by the time a retransmission
// arrived it would already be stale.
// ---------------------------------------------------------------------------
enum class WorldMessage : uint16_t {
    // TCP
    JoinRequest = 200,   // {version:u32, token:str}
    JoinResponse = 201,  // {result:u8, playerId:u64, udpPort:u16, username:str, entityId:u32}
    LogoutRequest = 202,
    LogoutResponse = 203,
    Disconnect = 204,  // {reason:u8} -- server-initiated

    // UDP
    ClientHello = 210,  // {playerId:u64, token:str} -- binds this UDP address to the session
    Snapshot = 211,     // see below
    // {playerId:u64, sequence:u32, moveForward:f32, moveRight:f32, yaw:f32, buttons:u8}
    //
    // The client sends intent, never position: movement is resolved by the
    // server's physics so a modified client can't teleport.
    ClientInput = 212,

    // Administrative, from the console tool. Gated on ADMIN_SECRET like the
    // auth server's equivalents, because they act on the running world.
    AdminSetTimeRequest = 220,   // {token:str, mode:u8, timeOfDay:f32}
    AdminSetTimeResponse = 221,  // {result:u8, timeOfDay:f32}
    AdminStatusRequest = 222,    // {token:str}
    AdminStatusResponse = 223,   // {result:u8, timeOfDay:f32, players:u16, tick:u32}
};

// How the world clock should run, for AdminSetTimeRequest.
enum class TimeMode : uint8_t {
    Set = 0,      // jump to the given time and carry on from there
    FollowReal = 1,  // drop any offset and track the wall clock again
};

enum class JoinResult : uint8_t {
    Success = 0,
    InvalidSession = 1,
    VersionMismatch = 2,
    ServerFull = 3,
    ServerError = 4,
};

// The world clock is server-owned and replicated like everything else, so two
// clients can never disagree about what time it is. One full day is
// DAY_LENGTH_SECONDS on the world server (60 by default).
//
// Snapshot payload:
//   {tick:u32, serverTimeMs:u64, entityCount:u16,
//    entities:[{id:u32, posX/Y/Z:f32, rotX/Y/Z/W:f32,
//               velX/Y/Z:f32, angVelX/Y/Z:f32}]}
//
// Velocities are sent alongside the transform so the client can extrapolate
// forward from the newest snapshot rather than lagging behind to interpolate.
//
// Each player entity also carries the sequence number of the last input the
// server actually consumed for it. That acknowledgement is what makes
// reconciliation possible: the client can tell which of its predicted inputs
// the authoritative state already accounts for, and replay only the rest.
constexpr uint16_t kMaxEntitiesPerSnapshot = 512;

// Tells the client what to draw for a replicated entity.
enum class EntityType : uint8_t {
    Cube = 0,
    Player = 1,
};

// Bit flags in ClientInput's `buttons` field.
constexpr uint8_t kInputJump = 1 << 0;
constexpr uint8_t kInputSprint = 1 << 1;

}  // namespace net
