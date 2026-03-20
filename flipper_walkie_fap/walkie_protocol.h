#pragma once

#include <stdint.h>

#define WALKIE_NODE_COUNT 2u
#define WALKIE_SAMPLE_RATE_HZ 8000u
#define WALKIE_SAMPLE_PERIOD_US (1000000u / WALKIE_SAMPLE_RATE_HZ)
#define WALKIE_AUDIO_PAYLOAD 24u
#define WALKIE_RX_BUF_SIZE 1024u
#define WALKIE_PWM_CARRIER_HZ 160000u

/*
 * Keep this set to 1 to match walkie_oled.ino.
 * Set it to 0 only if you want to talk to the simpler walkie_node0/node1 pair.
 */
#define WALKIE_ENABLE_SIMPLE_CRYPT 1u

typedef struct {
    uint8_t seq;
    uint8_t nonce;
    uint8_t pcm[WALKIE_AUDIO_PAYLOAD];
} AudioPacket;

static const uint8_t walkie_addrs[WALKIE_NODE_COUNT][5] = {
    {'W', '0', 'L', 'K', '0'},
    {'W', '0', 'L', 'K', '1'},
};

static inline uint8_t walkie_prng8(uint8_t* state) {
    *state ^= (uint8_t)(*state << 3);
    *state ^= (uint8_t)(*state >> 5);
    *state ^= (uint8_t)(*state << 1);
    return *state;
}

static inline void
walkie_crypt_buffer(uint8_t* data, uint8_t len, uint8_t node_id, uint8_t seq, uint8_t nonce) {
#if WALKIE_ENABLE_SIMPLE_CRYPT
    static const uint8_t key[8] = {0x41, 0x72, 0x44, 0x75, 0x69, 0x6E, 0x6F, 0x21};
    uint8_t state = (uint8_t)(key[node_id & 7u] ^ seq ^ nonce ^ 0xA5u);
    for(uint8_t i = 0; i < len; i++) {
        state = walkie_prng8(&state);
        data[i] ^= (uint8_t)(state ^ key[i & 7u]);
    }
#else
    (void)data;
    (void)len;
    (void)node_id;
    (void)seq;
    (void)nonce;
#endif
}
