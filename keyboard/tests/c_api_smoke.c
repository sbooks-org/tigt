#include "pc_xt_keyboard.h"

#include <assert.h>
#include <stdint.h>

static size_t handle(void *keyboard, pc_xt_keyboard_v1_input_event input, uint8_t output[PC_XT_KEYBOARD_V1_EVENT_MAX_BYTES]) {
    return pc_xt_keyboard_v1_handle(
        keyboard,
        &input,
        output,
        PC_XT_KEYBOARD_V1_EVENT_MAX_BYTES
    );
}

static void navigation_wire_sequences(void) {
    void *keyboard = pc_xt_keyboard_v1_create(PC_XT_KEYBOARD_V1_AT_SET1);
    uint8_t output[PC_XT_KEYBOARD_V1_EVENT_MAX_BYTES];
    pc_xt_keyboard_v1_input_event left = {
        .key = { .kind = PC_XT_KEYBOARD_V1_KEY_LEFT },
        .kind = PC_XT_KEYBOARD_V1_PRESS,
    };
    assert(keyboard != NULL);
    assert(handle(keyboard, left, output) == 2);
    assert(output[0] == 0xE0 && output[1] == 0x4B);
    left.kind = PC_XT_KEYBOARD_V1_RELEASE;
    assert(handle(keyboard, left, output) == 2);
    assert(output[0] == 0xE0 && output[1] == 0xCB);
    left.kind = PC_XT_KEYBOARD_V1_PRESS;
    left.modifiers = PC_XT_KEYBOARD_V1_MOD_SUPER;
    assert(handle(keyboard, left, output) == 1 && output[0] == 0x4B);
    left.kind = PC_XT_KEYBOARD_V1_RELEASE;
    assert(handle(keyboard, left, output) == 1 && output[0] == 0xCB);
    pc_xt_keyboard_v1_destroy(keyboard);
}

static size_t handle_events(void *keyboard, pc_xt_keyboard_v1_input_event input,
                            pc_xt_keyboard_v1_key_event output[PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS]) {
    return pc_xt_keyboard_v1_handle_events(
        keyboard, &input, output, PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS
    );
}

static void physical_models(void) {
    for (uint32_t model = PC_XT_KEYBOARD_V1_XT_SET1; model <= PC_XT_KEYBOARD_V1_AT_SET1; ++model) {
        void *keyboard = pc_xt_keyboard_v1_create(model);
        pc_xt_keyboard_v1_key_event output[PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS];
        const struct {
            pc_xt_keyboard_v1_input_key input;
            uint16_t keys[2];
            size_t count;
        } cases[] = {
            { { .kind = PC_XT_KEYBOARD_V1_KEY_CHAR, .character = 'a' }, { 0x1E }, 1 },
            { { .kind = PC_XT_KEYBOARD_V1_KEY_LEFT },
              { model == PC_XT_KEYBOARD_V1_AT_SET1 ? 0x14B : 0x4B }, 1 },
            { { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 19 }, { 0x37 }, 1 },
            { { .kind = PC_XT_KEYBOARD_V1_KEY_PRINT_SCREEN },
              { model == PC_XT_KEYBOARD_V1_AT_SET1 ? 0x137 : 0x37 }, 1 },
            { { .kind = PC_XT_KEYBOARD_V1_KEY_PAUSE },
              { model == PC_XT_KEYBOARD_V1_AT_SET1 ? 0x145 : 0x1D, 0x45 },
              model == PC_XT_KEYBOARD_V1_AT_SET1 ? 1u : 2u },
        };
        assert(keyboard != NULL);
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
            pc_xt_keyboard_v1_input_event input = {
                .key = cases[i].input,
                .kind = PC_XT_KEYBOARD_V1_PRESS,
            };
            assert(handle_events(keyboard, input, output) == cases[i].count);
            for (size_t j = 0; j < cases[i].count; ++j) {
                assert(output[j].key == cases[i].keys[j] && output[j].down == 1);
            }
            input.kind = PC_XT_KEYBOARD_V1_RELEASE;
            assert(handle_events(keyboard, input, output) == cases[i].count);
            for (size_t j = 0; j < cases[i].count; ++j) {
                assert(output[j].key == cases[i].keys[j] && output[j].down == 0);
            }
        }
        pc_xt_keyboard_v1_destroy(keyboard);
    }
}

static void shared_modifiers_and_errors(void) {
    void *keyboard = pc_xt_keyboard_v1_create(PC_XT_KEYBOARD_V1_AT_SET1);
    pc_xt_keyboard_v1_key_event output[PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS];
    pc_xt_keyboard_v1_input_event a = {
        .key = { .kind = PC_XT_KEYBOARD_V1_KEY_CHAR, .character = 'a' },
        .modifiers = PC_XT_KEYBOARD_V1_MOD_CONTROL,
        .kind = PC_XT_KEYBOARD_V1_PRESS,
    };
    pc_xt_keyboard_v1_input_event b = a;
    b.key.character = 'b';
    assert(keyboard != NULL);
    assert(handle_events(keyboard, a, output) == 2);
    assert(output[0].key == 0x1D && output[0].down == 1);
    assert(output[1].key == 0x1E && output[1].down == 1);
    assert(handle_events(keyboard, b, output) == 1);
    assert(output[0].key == 0x30 && output[0].down == 1);
    b.kind = PC_XT_KEYBOARD_V1_REPEAT;
    assert(handle_events(keyboard, b, output) == 0);

    a.kind = PC_XT_KEYBOARD_V1_RELEASE;
    for (size_t i = 0; i < PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS; ++i) {
        output[i].key = 0xFFFF;
        output[i].down = 0xFF;
    }
    assert(pc_xt_keyboard_v1_handle_events(
        keyboard, &a, output, PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS - 1
    ) == PC_XT_KEYBOARD_V1_ERROR);
    assert(pc_xt_keyboard_v1_handle_events(
        keyboard, &a, NULL, PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS
    ) == PC_XT_KEYBOARD_V1_ERROR);
    pc_xt_keyboard_v1_input_event invalid = a;
    invalid.kind = 0xFF;
    assert(handle_events(keyboard, invalid, output) == PC_XT_KEYBOARD_V1_ERROR);
    for (size_t i = 0; i < PC_XT_KEYBOARD_V1_EVENT_MAX_KEYS; ++i) {
        assert(output[i].key == 0xFFFF && output[i].down == 0xFF);
    }
    assert(handle_events(keyboard, a, output) == 1);
    assert(output[0].key == 0x1E && output[0].down == 0);
    b.kind = PC_XT_KEYBOARD_V1_RELEASE;
    assert(handle_events(keyboard, b, output) == 2);
    assert(output[0].key == 0x1D && output[0].down == 0);
    assert(output[1].key == 0x30 && output[1].down == 0);
    pc_xt_keyboard_v1_destroy(keyboard);
}

static void bulk_release_capacity_retry(void) {
    void *keyboard = pc_xt_keyboard_v1_create(PC_XT_KEYBOARD_V1_AT_SET1);
    pc_xt_keyboard_v1_key_event output[64];
    unsigned char held[0x146] = { 0 };
    size_t held_count = 0;
    const pc_xt_keyboard_v1_input_key keys[] = {
        { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 1 },
        { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 2 },
        { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 3 },
        { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 4 },
        { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 5 },
        { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 6 },
        { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 7 },
        { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 8 },
        { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 9 },
        { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 10 },
        { .kind = PC_XT_KEYBOARD_V1_KEY_LEFT },
        { .kind = PC_XT_KEYBOARD_V1_KEY_RIGHT },
        { .kind = PC_XT_KEYBOARD_V1_KEY_UP },
        { .kind = PC_XT_KEYBOARD_V1_KEY_DOWN },
        { .kind = PC_XT_KEYBOARD_V1_KEY_HOME },
        { .kind = PC_XT_KEYBOARD_V1_KEY_END },
        { .kind = PC_XT_KEYBOARD_V1_KEY_PAGE_UP },
        { .kind = PC_XT_KEYBOARD_V1_KEY_PAGE_DOWN },
        { .kind = PC_XT_KEYBOARD_V1_KEY_DELETE },
        { .kind = PC_XT_KEYBOARD_V1_KEY_INSERT },
        { .kind = PC_XT_KEYBOARD_V1_KEY_KEYPAD_BEGIN },
        { .kind = PC_XT_KEYBOARD_V1_KEY_ESCAPE },
        { .kind = PC_XT_KEYBOARD_V1_KEY_PRINT_SCREEN },
        { .kind = PC_XT_KEYBOARD_V1_KEY_PAUSE },
        { .kind = PC_XT_KEYBOARD_V1_KEY_SCROLL_LOCK },
        { .kind = PC_XT_KEYBOARD_V1_KEY_NUM_LOCK },
        { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 16 },
        { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 17 },
        { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 18 },
        { .kind = PC_XT_KEYBOARD_V1_KEY_FUNCTION, .value = 19 },
        { .kind = PC_XT_KEYBOARD_V1_KEY_CHAR, .character = 'c' },
        { .kind = PC_XT_KEYBOARD_V1_KEY_CHAR, .character = 'z' },
    };
    assert(keyboard != NULL);
    for (size_t i = 0; i < sizeof(keys) / sizeof(keys[0]); ++i) {
        pc_xt_keyboard_v1_input_event input = {
            .key = keys[i],
            .modifiers = PC_XT_KEYBOARD_V1_MOD_SUPER,
            .kind = PC_XT_KEYBOARD_V1_PRESS,
        };
        size_t count = handle_events(keyboard, input, output);
        assert(count != PC_XT_KEYBOARD_V1_ERROR);
        for (size_t j = 0; j < count; ++j) {
            assert(output[j].down == 1 && output[j].key < sizeof(held));
            assert(!held[output[j].key]);
            held[output[j].key] = 1;
            ++held_count;
        }
    }
    assert(held_count == 33);
    pc_xt_keyboard_v1_input_event release = {
        .key = { .kind = PC_XT_KEYBOARD_V1_KEY_MODIFIER, .value = PC_XT_KEYBOARD_V1_LEFT_SUPER },
        .kind = PC_XT_KEYBOARD_V1_RELEASE,
    };
    for (size_t i = 0; i < 64; ++i) {
        output[i].key = 0xFFFF;
        output[i].down = 0xFF;
    }
    assert(pc_xt_keyboard_v1_handle_events(
        keyboard, &release, output, 32
    ) == PC_XT_KEYBOARD_V1_ERROR);
    for (size_t i = 0; i < 64; ++i) {
        assert(output[i].key == 0xFFFF && output[i].down == 0xFF);
    }
    assert(pc_xt_keyboard_v1_handle_events(keyboard, &release, output, 64) == held_count);
    for (size_t i = 0; i < held_count; ++i) {
        assert(output[i].down == 0 && output[i].key < sizeof(held));
        assert(held[output[i].key]);
        held[output[i].key] = 0;
    }
    assert(handle_events(keyboard, release, output) == 0);
    pc_xt_keyboard_v1_destroy(keyboard);
}

int main(void) {
    navigation_wire_sequences();
    physical_models();
    shared_modifiers_and_errors();
    bulk_release_capacity_retry();
    uint8_t output[PC_XT_KEYBOARD_V1_EVENT_MAX_BYTES];
    void *keyboard = pc_xt_keyboard_v1_create(PC_XT_KEYBOARD_V1_AT_SET1);
    assert(keyboard != 0);

    pc_xt_keyboard_v1_input_event a_press = {
        .key = { .kind = PC_XT_KEYBOARD_V1_KEY_CHAR, .character = 'a' },
        .kind = PC_XT_KEYBOARD_V1_PRESS,
    };
    assert(handle(keyboard, a_press, output) == 1 && output[0] == 0x1E);

    pc_xt_keyboard_v1_input_event a_release = a_press;
    a_release.kind = PC_XT_KEYBOARD_V1_RELEASE;
    assert(handle(keyboard, a_release, output) == 1 && output[0] == 0x9E);

    pc_xt_keyboard_v1_input_event print = {
        .key = { .kind = PC_XT_KEYBOARD_V1_KEY_PRINT_SCREEN },
        .kind = PC_XT_KEYBOARD_V1_PRESS,
    };
    assert(handle(keyboard, print, output) == 4);
    assert(output[0] == 0xE0 && output[1] == 0x2A && output[2] == 0xE0 && output[3] == 0x37);

    pc_xt_keyboard_v1_input_event pause = {
        .key = { .kind = PC_XT_KEYBOARD_V1_KEY_CHAR, .character = '\\' },
        .modifiers = PC_XT_KEYBOARD_V1_MOD_CONTROL | PC_XT_KEYBOARD_V1_MOD_SHIFT | PC_XT_KEYBOARD_V1_MOD_ALT | PC_XT_KEYBOARD_V1_MOD_SUPER,
        .kind = PC_XT_KEYBOARD_V1_PRESS,
    };
    assert(handle(keyboard, pause, output) == 6);
    assert(output[0] == 0xE1 && output[1] == 0x1D && output[2] == 0x45 && output[3] == 0xE1 && output[4] == 0x9D && output[5] == 0xC5);

    pc_xt_keyboard_v1_input_event sysrq = pause;
    sysrq.key.character = '`';
    assert(handle(keyboard, sysrq, output) == 1 && output[0] == 0x54);

    pc_xt_keyboard_v1_destroy(keyboard);
    return 0;
}
