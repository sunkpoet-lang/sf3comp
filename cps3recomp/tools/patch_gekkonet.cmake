# SPDX-License-Identifier: GPL-3.0-only
# (ForkAde) GekkoNet's state for a spectator who joins a match in progress, made fit for sf3comp's state (near 10 MB,
# the character RAM; a CPS game's is a few hundred KB): run by CMake on GekkoNet's sources once they are fetched.
#  - compressed (zlib, level 1: some 0.9 MB) when the host sets it, and put back when the spectator has it whole;
#  - sent a few chunks at a time (kChunksPerSend every kSendEveryMs) instead of every chunk at once every 200 ms, which
#    over a relay never arrived whole and sent gigabytes trying.
# The protocol's packets are the same; only what the state's bytes are changes, so both sides need this build (they
# are the same program: ForkAde's build check).
#   cmake -DSOURCE=<gekkonet source dir> -P patch_gekkonet.cmake
if(NOT SOURCE)
    message(FATAL_ERROR "SOURCE: GekkoNet's source folder")
endif()
set(backend_cpp "${SOURCE}/GekkoLib/src/backend.cpp")
set(backend_h "${SOURCE}/GekkoLib/include/private/backend.h")
file(READ "${backend_cpp}" cpp)
if(cpp MATCHES "ForkAde: sf3comp spectator state")
    message(STATUS "GekkoNet already patched for sf3comp's spectator state")
    return()
endif()
file(READ "${backend_h}" h)

function(swap var old new)
    string(FIND "${${var}}" "${old}" at)
    if(at EQUAL -1)
        message(FATAL_ERROR "patch_gekkonet: not found in GekkoNet: ${old}")
    endif()
    string(REPLACE "${old}" "${new}" result "${${var}}")
    set(${var} "${result}" PARENT_SCOPE)
endfunction()

# The cursor of the chunks sent
swap(h "        u64 last_spectator_state_send_time = 0;"
"        u64 last_spectator_state_send_time = 0;
        u32 spectator_state_cursor = 0; // (ForkAde: the next chunk to send)")

# The state packed (a header: GZS1 and its size) and unpacked
swap(cpp "void Gekko::MessageSystem::SetSpectatorState("
"// (ForkAde: sf3comp spectator state) The state as it is sent: GZS1, its size, then its bytes deflated
namespace {
    std::vector<u8> PackSpectatorState(const u8* state, u32 size) {
        uLongf packed = compressBound(size);
        std::vector<u8> out(8 + packed);
        const u8 head[4] = {'G', 'Z', 'S', '1'};
        std::memcpy(out.data(), head, 4);
        for (int i = 0; i < 4; i++) out[4 + i] = static_cast<u8>(size >> (8 * i));
        if (compress2(out.data() + 8, &packed, state, size, 1) != Z_OK) {
            return std::vector<u8>(state, state + size);
        }
        out.resize(8 + packed);
        return out;
    }
    bool UnpackSpectatorState(std::vector<u8>& state, u32 most) {
        if (state.size() < 8 || std::memcmp(state.data(), \"GZS1\", 4) != 0) {
            return true; // (sent as it is)
        }
        u32 size = 0;
        for (int i = 0; i < 4; i++) size |= static_cast<u32>(state[4 + i]) << (8 * i);
        if (size == 0 || size > most) return false;
        std::vector<u8> raw(size);
        uLongf got = size;
        if (uncompress(raw.data(), &got, state.data() + 8, static_cast<uLong>(state.size() - 8)) != Z_OK || got != size) {
            return false;
        }
        state = std::move(raw);
        return true;
    }
}

void Gekko::MessageSystem::SetSpectatorState(")
swap(cpp "        player->spectator_state.assign(state, state + state_size);"
"        player->spectator_state = PackSpectatorState(state, state_size); // (ForkAde)
        player->spectator_state_cursor = 0;")
swap(cpp "    frame = _incoming_spectator_state.frame;
    state = _incoming_spectator_state.state;"
"    frame = _incoming_spectator_state.frame;
    state = _incoming_spectator_state.state;
    if (!UnpackSpectatorState(state, _state_size)) { // (ForkAde)
        _incoming_spectator_state.ready = false;
        return false;
    }")

# Sent a few chunks at a time
swap(cpp "        if (player->last_spectator_state_send_time + NetStats::INPUT_RETRY_INTERVAL > now) {
            continue;
        }

        const u32 total_size = (u32)player->spectator_state.size();
        for (u32 offset = 0; offset < total_size; offset += SPECTATOR_STATE_CHUNK_SIZE) {"
"        // (ForkAde) kChunksPerSend chunks every kSendEveryMs, round and round until the spectator has them all
        constexpr u64 kSendEveryMs = 16;
        constexpr u32 kChunksPerSend = 32;
        if (player->last_spectator_state_send_time + kSendEveryMs > now) {
            continue;
        }

        const u32 total_size = (u32)player->spectator_state.size();
        const u32 chunks = 1 + (total_size - 1) / SPECTATOR_STATE_CHUNK_SIZE;
        for (u32 sent = 0; sent < kChunksPerSend && sent < chunks; sent++) {
            if (player->spectator_state_cursor >= chunks) player->spectator_state_cursor = 0;
            const u32 offset = player->spectator_state_cursor++ * SPECTATOR_STATE_CHUNK_SIZE;")

# zlib in GekkoNet
swap(cpp "#include \"backend.h\"" "#include \"backend.h\"
#include <zlib.h> // (ForkAde: sf3comp spectator state)
#include <cstring>")

file(WRITE "${backend_cpp}" "${cpp}")
file(WRITE "${backend_h}" "${h}")
message(STATUS "GekkoNet patched for sf3comp's spectator state")
