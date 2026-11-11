/* Focused invariants for the mu_id_pool primitive used by vk.c. */
#include "mu_id_pool.h"

#include <stdio.h>
#include <string.h>

#define CAPACITY 64u
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "id_pool check failed at %s:%d: %s\n", __FILE__, __LINE__, #x); \
    return 1; \
} } while (0)

static uint32_t next_random(uint32_t *state) {
    uint32_t x = *state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *state = x;
    return x;
}

int main(void) {
    mu_id_pool pool = {0};
    uint8_t live[CAPACITY] = {0};
    uint32_t i, state = 0x91e10da5u;

    mu_id_pool_init(&pool, CAPACITY);
    for (i = 0; i < CAPACITY; ++i) {
        uint32_t id = UINT32_MAX;
        CHECK(mu_id_pool_create_id(&pool, &id));
        CHECK(id == i);
        CHECK(!live[id]);
        live[id] = 1u;
    }
    {
        uint32_t id = 0;
        CHECK(!mu_id_pool_create_id(&pool, &id));
        CHECK(pool.used_ids == CAPACITY);
    }

    for (i = 0; i < 20000u; ++i) {
        uint32_t id = next_random(&state) % CAPACITY;
        if (live[id]) {
            CHECK(mu_id_pool_destroy_id(&pool, id));
            live[id] = 0u;
        } else {
            uint32_t got = UINT32_MAX;
            CHECK(mu_id_pool_create_id(&pool, &got));
            CHECK(got < CAPACITY && !live[got]);
            live[got] = 1u;
        }
        CHECK(pool.used_ids <= CAPACITY);
        for (uint32_t j = 0; j < CAPACITY; ++j)
            CHECK(mu_id_pool_is_id(&pool, j) == (live[j] != 0u));
    }

    for (i = 0; i < CAPACITY; ++i)
        if (live[i]) CHECK(mu_id_pool_destroy_id(&pool, i));
    CHECK(pool.used_ids == 0u);
    mu_id_pool_check_ranges(&pool);
    mu_id_pool_deinit(&pool);
    puts("mu_id_pool allocation, uniqueness, reuse, and teardown checks passed");
    return 0;
}
