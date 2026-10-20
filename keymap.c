/*
Copyright 2022 @Yowkees
Copyright 2022 MURAOKA Taro (aka KoRoN, @kaoriya)

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, either version 2 of the License, or
(at your option) any later version.

This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.

You should have received a copy of the GNU General Public License
along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include QMK_KEYBOARD_H // IWYU pragma: keep
#include "quantum.h"

enum keymap_layers {
    _BS,
    _GM,
    _NV,
    _FN,
    _SM,
};

#define IS_UNILATERAL_INPUT(r, m) ((m) & (1U << (r)->event.key.row))
#define IS_EXCEPTIONAL_INPUT(k, r) (IS_UNILATERAL_INPUT(r, 0x88) || ((0x1000 | (k)) == RSFT_T(k) && get_highest_layer(layer_state) == _BS))

typedef struct {
    uint8_t index;
    uint8_t bitmask;
} tap_bit_t;

#define TAP_BIT_FROM_KEYCODE(k) ((tap_bit_t){.index = QK_MOD_TAP_GET_TAP_KEYCODE((k)) / 8, .bitmask = (1U << QK_MOD_TAP_GET_TAP_KEYCODE((k)) % 8)})

static uint8_t pressed_keys[32];

static bool        is_quick_succession_input;
static uint16_t    inter_keycode;
static keyrecord_t inter_record;

static bool is_keyball_set_volume_mode;

uint8_t unpack_mods(uint16_t keycode) {
    const uint8_t mods = QK_MOD_TAP_GET_MODS(keycode);
    return mods & 0x10 ? (mods << 4) : mods;
}

bool pre_process_record_user(uint16_t keycode, keyrecord_t *record) {
    static bool    is_after_ag_input;
    static bool    is_left_side_held;
    const uint16_t tap_part = 0xFF & keycode;

    if (record->event.pressed) {
        if (is_after_ag_input) {
            is_after_ag_input = false;
            if ((is_left_side_held && IS_UNILATERAL_INPUT(record, 0x70)) || (!is_left_side_held && IS_UNILATERAL_INPUT(record, 0x07))) {
                is_quick_succession_input = true;
            }
        }
        const bool is_expired = timer_elapsed(inter_record.event.time) > QUICK_TAP_TERM;
        if (tap_part > KC_Z || IS_EXCEPTIONAL_INPUT(keycode, record) || is_expired) {
            is_quick_succession_input = false;
            inter_keycode             = keycode;
            if (is_expired && IS_QK_MOD_TAP(keycode) && (keycode & (QK_LALT | QK_LGUI))) {
                is_after_ag_input = true;
                is_left_side_held = IS_UNILATERAL_INPUT(record, 0x07);
            }
        }
        inter_record = *record;
    } else {
        if (get_highest_layer(layer_state)) {
            is_quick_succession_input = false;
        } else if (tap_part <= KC_Z && keycode == inter_keycode) {
            is_quick_succession_input = true;
        }
        tap_bit_t tap = TAP_BIT_FROM_KEYCODE(keycode);
        if (pressed_keys[tap.index] & tap.bitmask) {
            pressed_keys[tap.index] &= ~tap.bitmask;
            unregister_mods(unpack_mods(keycode));
            record->tap.count++;
        }
    }
    if (keycode == LT(_SM, keycode) && IS_LAYER_OFF(_SM)) {
        keyball_set_scroll_mode(record->event.pressed);
    }
    if (keycode == LT(_GM, keycode)) {
        is_keyball_set_volume_mode = record->event.pressed;
    }
    return true;
}

uint16_t get_quick_tap_term(uint16_t keycode, keyrecord_t *record) {
    if (keycode == LT(_GM, keycode) || keycode == LT(_FN, keycode)) {
        return QUICK_TAP_TERM;
    }
    return IS_EXCEPTIONAL_INPUT(keycode, record) ? 0 : QUICK_TAP_TERM;
}

bool get_hold_on_other_key_press(uint16_t keycode, keyrecord_t *record) {
    if (is_quick_succession_input) {
        tap_bit_t tap = TAP_BIT_FROM_KEYCODE(keycode);
        pressed_keys[tap.index] |= tap.bitmask;
        record->tap.interrupted = false;
        record->tap.count++;
        return true;
    }
    return false;
}

enum navkey_types {
    NAV_NONE,
    NAV_UNDO_REDO,
    NAV_TAB,
    NAV_HJKL,
    NAV_WASD,
};

typedef struct {
    uint8_t           keycode;
    enum navkey_types type;
    bool              registered;
} morph_key_t;

static morph_key_t nav;

void within_word(uint16_t keycode) {
    // clang-format off
    static const uint16_t brcts[][2] = {
        {S(KC_QUOT), S(KC_QUOT)},
        {S(KC_LBRC), S(KC_RBRC)},
        {S(KC_COMM), S(KC_DOT)},
        {S(KC_9), S(KC_0)},
        {KC_QUOT, KC_QUOT},
        {KC_LBRC, KC_RBRC},
        {KC_GRV, KC_GRV},
        {0, -1},
    };
    // clang-format on
    static const uint8_t null_id      = ARRAY_SIZE(brcts) - 1;
    static uint8_t       reception_id = null_id;
    const uint8_t        saved_mods   = get_mods();
    keycode &= 0xFF;

    if (is_caps_word_on()) {
        caps_word_press_user(keycode);
    }
    if ((saved_mods | get_weak_mods()) & MOD_LSFT) {
        keycode |= QK_LSFT;
    }
    clear_weak_mods();
    if (keycode == brcts[reception_id][1]) {
        clear_mods();
        tap_code(KC_LEFT);
        set_mods(saved_mods);
        reception_id = null_id;
        return;
    }
    for (reception_id = 0; reception_id < null_id; reception_id++) {
        if (keycode == brcts[reception_id][0]) {
            return;
        }
    }
}

#define BUFFER_SIZE 16

typedef struct {
    uint16_t buffer[BUFFER_SIZE];
    uint8_t  front;
    uint8_t  rear;
    uint8_t  count;
} mt_queue_t;

bool enqueue(mt_queue_t *buf, uint16_t data) {
    if (buf->count >= BUFFER_SIZE) {
        return false;
    }
    buf->buffer[buf->rear] = data;
    buf->rear              = (buf->rear + 1) % BUFFER_SIZE;
    buf->count++;
    return true;
}

bool dequeue(mt_queue_t *buf, uint16_t *data) {
    if (buf->count <= 0) {
        return false;
    }
    *data      = buf->buffer[buf->front];
    buf->front = (buf->front + 1) % BUFFER_SIZE;
    buf->count--;
    return true;
}

static mt_queue_t lmts, rmts;

void set_mts_mods(mt_queue_t *mts) {
    uint16_t poped_key;
    uint8_t  pended_mods = 0;
    while (dequeue(mts, &poped_key)) {
        pended_mods |= unpack_mods(poped_key);
    }
    register_mods(pended_mods);
}

void send_mts_taps(mt_queue_t *mts, uint16_t keycode) {
    uint16_t poped_key;
    while (dequeue(mts, &poped_key)) {
        const uint8_t tap_part = QK_MOD_TAP_GET_TAP_KEYCODE(poped_key);
        if (is_caps_word_on()) {
            caps_word_press_user(tap_part);
        } else if (poped_key == S(tap_part)) {
            add_weak_mods(MOD_LSFT);
        }
        tap_code(tap_part);
        within_word(poped_key);
        if (poped_key == keycode) {
            // Keep keys after `keycode` pending for the next input event.
            return;
        }
    }
}

void process_pended_keys(uint16_t keycode, keyrecord_t *record) {
    if (IS_UNILATERAL_INPUT(record, 0x88) || (keycode >= KC_RGHT && keycode <= KC_UP)) {
        set_mts_mods(&lmts);
        set_mts_mods(&rmts);
        return;
    }
    const bool is_left_side = IS_UNILATERAL_INPUT(record, 0x07);
    set_mts_mods(!is_left_side ? &lmts : &rmts);
    send_mts_taps(is_left_side ? &lmts : &rmts, keycode);
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    static bool is_layer_SM_held;
    static bool is_fixed_swap_hands;
    static bool is_alternating_swap_hands;
    static bool oneshot_ignore_key_release;

    if (oneshot_ignore_key_release && keycode != LT(_BS, 1) && !record->event.pressed) {
        is_layer_SM_held           = false;
        oneshot_ignore_key_release = false;
        return false;
    }

    if (IS_LAYER_ON(_NV)) {
        caps_word_off();
    } else if (is_layer_SM_held) {
        layer_on(_SM);
    }

    switch (keycode) {
        case LT(_BS, KC_3):
        case LT(_BS, KC_MINS):
            if (!record->tap.count) {
                nav.type = record->event.pressed ? NAV_TAB : NAV_NONE;
            }
        case QK_MOD_TAP ... QK_MOD_TAP_MAX:
            if (IS_LAYER_ON(_NV)) {
                const uint8_t saved_mods = get_mods();
                caps_word_on();
                set_mods(saved_mods);
            }
            if (record->event.pressed && !record->tap.count) {
                mt_queue_t *mts = IS_UNILATERAL_INPUT(record, 0x0F) ? &lmts : &rmts;
                enqueue(mts, keycode); // discard when it overflows.
                return false;
            }
            break;
        case LT(_BS, KC_NO):
            if (record->event.pressed) {
                if (record->tap.count <= 1) {
                    if (is_fixed_swap_hands) {
                        is_fixed_swap_hands = false;
                        swap_hands_off();
                    } else if (is_alternating_swap_hands) {
                        swap_hands_toggle();
                    } else {
                        swap_hands_on();
                    }
                } else {
                    is_fixed_swap_hands       = true;
                    is_alternating_swap_hands = false;
                    swap_hands_on();
                }
            }
            return false;
        case LT(_BS, 1):
            if (record->event.pressed) {
                oneshot_ignore_key_release = true;
            }
            return false;
    }

    if (is_alternating_swap_hands) {
        if (record->event.pressed && get_highest_layer(layer_state) == _BS && IS_UNILATERAL_INPUT(record, 0x77)) {
            mt_queue_t *mts = IS_UNILATERAL_INPUT(record, 0x0F) ? &lmts : &rmts;
            enqueue(mts, keycode);
            return false;
        }
        if (IS_UNILATERAL_INPUT(record, 0x8F)) {
            swap_hands_on();
            if (get_highest_layer(layer_state)) {
                swap_hands_off();
            }
        } else {
            swap_hands_off();
        }
    } else if (!is_fixed_swap_hands) {
        swap_hands_off();
    }

    process_pended_keys(keycode, record);

    switch (keycode) {
        case LT(_BS, KC_F1):
            nav.type = record->tap.count <= 1 ? NAV_UNDO_REDO : NAV_NONE;
            return false;
        case LT(_BS, KC_F2)... LT(_BS, KC_F5): {
            static const uint8_t mods[2][4] = {
                {MOD_LALT, MOD_LSFT | MOD_LCTL, MOD_LCTL, MOD_LCTL},
                {0, 0, 0, MOD_LCTL},
            };
            static const uint8_t codes[4]   = {KC_APP, KC_F15, KC_F16, KC_C};
            const uint8_t        index      = keycode - LT(_BS, KC_F2);
            const uint8_t        saved_mods = get_mods();
            const uint8_t        mod        = mods[!record->tap.count][index];
            keycode                         = record->tap.count ? (index == 3 ? KC_V : KC_TAB) : codes[index];
            if (record->event.pressed) {
                clear_mods();
                register_mods(mod);
                register_code(keycode);
                if (mod != MOD_LALT) {
                    set_mods(saved_mods);
                }
            } else {
                unregister_code(keycode);
            }
            return false;
        }
        case LT(_BS, KC_LNG1):
        case LT(_BS, KC_LNG2):
            if (record->event.pressed) {
                if (record->tap.count <= 1) {
                    tap_code(keycode == LT(_BS, KC_LNG2) ? KC_F13 : KC_F14);
                    if (!record->tap.count) {
                        add_oneshot_mods(MOD_LSFT);
                    }
                    caps_word_off();
                } else {
                    caps_word_on();
                }
            }
            return false;
        case LT(_BS, KC_INT4):
            if (record->event.pressed) {
                register_mods(MOD_LGUI);
                register_code(KC_SLSH);
                unregister_mods(MOD_LGUI);
            } else {
                unregister_code(KC_SLSH);
            }
            return false;
        case KC_RGHT ... KC_LEFT:
        case KC_H:
        case KC_J:
        case KC_K:
        case KC_L:
        case KC_W:
        case KC_A:
        case KC_S:
        case KC_D:
            // Only one synthesized navigation key may be held at a time.
            if (nav.registered) {
                nav.registered = false;
                unregister_code(nav.keycode);
                if (!record->event.pressed) {
                    return false;
                }
            }
            if (record->event.pressed) {
                const uint8_t saved_mods = get_mods();
                switch (nav.type) {
                    case NAV_UNDO_REDO:
                        // Send Ctrl+Z / Ctrl+Y regardless of currently held modifiers.
                        clear_mods();
                        register_mods(MOD_LCTL);
                        nav.keycode = keycode == KC_LEFT ? KC_Z : KC_Y;
                        break;
                    case NAV_TAB:
                        if (keycode == KC_LEFT) {
                            register_mods(MOD_LSFT);
                        }
                        nav.keycode = KC_TAB;
                        break;
                    case NAV_HJKL:
                        switch (keycode) {
                            case KC_H:
                                nav.keycode = KC_LEFT;
                                break;
                            case KC_J:
                                nav.keycode = KC_DOWN;
                                break;
                            case KC_K:
                                nav.keycode = KC_UP;
                                break;
                            case KC_L:
                                nav.keycode = KC_RGHT;
                                break;
                            default:
                                return true;
                        }
                        break;
                    case NAV_WASD:
                        switch (keycode) {
                            case KC_W:
                                nav.keycode = KC_UP;
                                break;
                            case KC_A:
                                nav.keycode = KC_LEFT;
                                break;
                            case KC_S:
                                nav.keycode = KC_DOWN;
                                break;
                            case KC_D:
                                nav.keycode = KC_RGHT;
                                break;
                            default:
                                return true;
                        }
                        break;
                    default:
                        return true;
                }
                register_code(nav.keycode);
                set_mods(saved_mods);
                nav.registered = true;
                return false;
            }
            break;
        case LT(_SM, KC_SPC):
            if (!record->tap.count) {
                is_layer_SM_held = record->event.pressed;
            }
        case LT(_FN, KC_BSPC):
            if (is_alternating_swap_hands && record->tap.count) {
                if (record->event.pressed) {
                    tap_code(keycode == LT(_SM, KC_SPC) ? KC_H : KC_P);
                }
                return false;
            }
            break;
        case LT(_GM, KC_NO):
            is_fixed_swap_hands       = false;
            is_alternating_swap_hands = false;
            nav.type                  = NAV_NONE;
            if (record->tap.count > 1) {
                if (IS_LAYER_ON(_GM)) {
                    nav.type = record->event.key.row == 3 ? NAV_HJKL : NAV_WASD;
                } else {
                    is_alternating_swap_hands = true;
                }
            }
            swap_hands_off();
        case LT(_NV, KC_H):
            if (!record->tap.count) {
                layer_clear();
                if (!record->event.pressed) {
                    unregister_mods(MOD_HYPR);
                    nav.type = NAV_NONE;
                }
            }
    }
    return true;
}

void post_process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed) {
        within_word(keycode);
    }
}

#define LCS_T(k) (MT(MOD_LCTL | MOD_LSFT, (k)))

enum combos {
    CMB_OS_CTL1,
    CMB_OS_SFT1,
    CMB_OS_ALT1,
    CMB_OS_GUI1,
    CMB_OS_CTL2,
    CMB_OS_SFT2,
    CMB_OS_ALT2,
    CMB_OS_GUI2,
    CMB_PSCR1,
    CMB_PSCR2,
    CMB_MS_BTN1,
    CMB_MS_BTN2,
    CMB_MS_BTN3,
    CMB_LNG1,
    CMB_LNG2,
    CMB_INT4,
    CMB_OS_SH1,
    CMB_OS_SH2,
    CMB_IGNR_RL1,
    CMB_IGNR_RL2,
};

const uint16_t PROGMEM cmb_os_ctl1[]  = {KC_D, KC_W, COMBO_END};
const uint16_t PROGMEM cmb_os_sft1[]  = {KC_L, KC_W, COMBO_END};
const uint16_t PROGMEM cmb_os_alt1[]  = {KC_L, KC_D, COMBO_END};
const uint16_t PROGMEM cmb_os_gui1[]  = {KC_L, KC_D, KC_W, COMBO_END};
const uint16_t PROGMEM cmb_os_ctl2[]  = {KC_J, KC_O, COMBO_END};
const uint16_t PROGMEM cmb_os_sft2[]  = {KC_J, KC_U, COMBO_END};
const uint16_t PROGMEM cmb_os_alt2[]  = {KC_O, KC_U, COMBO_END};
const uint16_t PROGMEM cmb_os_gui2[]  = {KC_J, KC_O, KC_U, COMBO_END};
const uint16_t PROGMEM cmb_pscr1[]    = {KC_W, KC_Q, COMBO_END};
const uint16_t PROGMEM cmb_pscr2[]    = {KC_Q, KC_J, COMBO_END};
const uint16_t PROGMEM cmb_ms_btn1[]  = {LSFT_T(KC_T), LCTL_T(KC_S), COMBO_END};
const uint16_t PROGMEM cmb_ms_btn2[]  = {LALT_T(KC_R), LSFT_T(KC_T), COMBO_END};
const uint16_t PROGMEM cmb_ms_btn3[]  = {LALT_T(KC_R), LCTL_T(KC_S), COMBO_END};
const uint16_t PROGMEM cmb_lng1[]     = {LCTL_T(KC_S), LCS_T(KC_G), COMBO_END};
const uint16_t PROGMEM cmb_lng2[]     = {LT(_BS, KC_MINS), RCTL_T(KC_Y), COMBO_END};
const uint16_t PROGMEM cmb_int4[]     = {KC_Z, KC_K, COMBO_END};
const uint16_t PROGMEM cmb_os_sh1[]   = {KC_M, KC_K, COMBO_END};
const uint16_t PROGMEM cmb_os_sh2[]   = {KC_C, KC_DOT, COMBO_END};
const uint16_t PROGMEM cmb_ignr_rl1[] = {KC_Z, KC_M, COMBO_END};
const uint16_t PROGMEM cmb_ignr_rl2[] = {KC_DOT, KC_COMM, COMBO_END};

// clang-format off
combo_t key_combos[] = {
    [CMB_OS_CTL1]     = COMBO(cmb_os_ctl1, OSM(MOD_LCTL)),
    [CMB_OS_SFT1]     = COMBO(cmb_os_sft1, OSM(MOD_LSFT)),
    [CMB_OS_ALT1]     = COMBO(cmb_os_alt1, OSM(MOD_LALT)),
    [CMB_OS_GUI1]     = COMBO(cmb_os_gui1, OSM(MOD_LGUI)),
    [CMB_OS_CTL2]     = COMBO(cmb_os_ctl2, OSM(MOD_RCTL)),
    [CMB_OS_SFT2]     = COMBO(cmb_os_sft2, OSM(MOD_RSFT)),
    [CMB_OS_ALT2]     = COMBO(cmb_os_alt2, OSM(MOD_RALT)),
    [CMB_OS_GUI2]     = COMBO(cmb_os_gui2, OSM(MOD_RGUI)),
    [CMB_PSCR1]       = COMBO(cmb_pscr1, KC_PSCR),
    [CMB_PSCR2]       = COMBO(cmb_pscr2, KC_PSCR),
    [CMB_MS_BTN1]     = COMBO(cmb_ms_btn1, KC_MS_BTN1),
    [CMB_MS_BTN2]     = COMBO(cmb_ms_btn2, KC_MS_BTN2),
    [CMB_MS_BTN3]     = COMBO(cmb_ms_btn3, KC_MS_BTN3),
    [CMB_LNG1]        = COMBO(cmb_lng1, LT(_BS, KC_LNG1)),
    [CMB_LNG2]        = COMBO(cmb_lng2, LT(_BS, KC_LNG2)),
    [CMB_INT4]        = COMBO(cmb_int4, LT(_BS, KC_INT4)),
    [CMB_OS_SH1]      = COMBO(cmb_os_sh1, LT(_BS, KC_NO)),
    [CMB_OS_SH2]      = COMBO(cmb_os_sh2, LT(_BS, KC_NO)),
    [CMB_IGNR_RL1]    = COMBO(cmb_ignr_rl1, LT(_BS, 1)),
    [CMB_IGNR_RL2]    = COMBO(cmb_ignr_rl2, LT(_BS, 1)),
};
// clang-format on

bool combo_should_trigger(uint16_t combo_index, combo_t *combo, uint16_t keycode, keyrecord_t *record) {
    layer_state_t layer = get_highest_layer(layer_state);
    switch (combo_index) {
        case CMB_OS_CTL2 ... CMB_OS_GUI2:
            if (layer != _FN) {
                return false;
            }
            break;
        case CMB_MS_BTN1 ... CMB_MS_BTN3:
            if (layer == _NV) {
                return false;
            }
    }
    return true;
}

uint8_t combo_ref_from_layer(uint8_t layer) {
    return _BS;
}

bool caps_word_press_user(uint16_t keycode) {
    switch (keycode) {
        case KC_A ... KC_Z:
            add_weak_mods(MOD_LSFT);
            return true;
        case KC_QUOT:
            if (IS_LAYER_ON(_NV)) {
                return false;
            }
        case KC_1 ... KC_0:
        case KC_SCLN:
            if (IS_LAYER_ON(_NV)) {
                add_weak_mods(MOD_LSFT);
            }
        case KC_EQL:
        case KC_BSPC:
        case KC_SLSH:
        case KC_MINS:
        case S(KC_MINS):
            return true;
    }
    return false;
}

report_mouse_t pointing_device_task_kb(report_mouse_t mouse_report) {
    if (is_keyball_set_volume_mode) {
        const uint16_t keycode = mouse_report.x > 1 ? KC_VOLU : mouse_report.x < -1 ? KC_VOLD : KC_NO;
        register_code(keycode);
        unregister_code(keycode);
        mouse_report = (report_mouse_t){};
    }
    return mouse_report;
}

// clang-format off
#ifdef SWAP_HANDS_ENABLE
__attribute__((weak)) const keypos_t PROGMEM hand_swap_config[MATRIX_ROWS][MATRIX_COLS] = {
    {{0, 4}, {1, 4}, {2, 4}, {3, 4}, {4, 4}, {5, 4}},
    {{0, 5}, {1, 5}, {2, 5}, {3, 5}, {4, 5}, {5, 5}},
    {{0, 6}, {1, 6}, {2, 6}, {3, 6}, {4, 6}, {5, 6}},
    {{0, 7}, {1, 7}, {2, 7}, {3, 7}, {4, 7}, {5, 7}},
    {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}},
    {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}},
    {{0, 2}, {1, 2}, {2, 2}, {3, 2}, {4, 2}, {5, 2}},
    {{0, 3}, {1, 3}, {2, 3}, {3, 3}, {4, 3}, {5, 3}},
};
#endif

const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
  // keymap for VIA
  [_BS] = LAYOUT_universal(
    KC_F           ,KC_L         ,KC_D         ,KC_W         ,KC_Q          ,                                     KC_Q           ,KC_J          ,KC_O           ,KC_U          ,KC_X           ,
    LGUI_T(KC_N)   ,LALT_T(KC_R) ,LSFT_T(KC_T) ,LCTL_T(KC_S) ,LCS_T(KC_G)   ,                                     LT(_BS,KC_MINS),RCTL_T(KC_Y)  ,RSFT_T(KC_A)   ,RALT_T(KC_I)  ,RGUI_T(KC_E)   ,
    KC_B           ,KC_Z         ,KC_M         ,KC_K         ,KC_V          ,                                     S(KC_MINS)     ,KC_C          ,KC_DOT         ,KC_COMM       ,KC_SCLN        ,
    LT(_GM,KC_NO)  ,LALT(KC_PSCR),LSFT(KC_PSCR),KC_ENT       ,LT(_NV,KC_H)  ,LT(_FN,KC_P),LT(_FN,KC_BSPC),LT(_SM,KC_SPC) ,KC_ENT        ,RSFT(KC_PSCR)  ,RALT(KC_PSCR) ,LT(_GM,KC_NO)) ,

  [_GM] = LAYOUT_universal(
    KC_F           ,KC_X         ,KC_K         ,KC_Z         ,KC_Q          ,                                     KC_Q           ,KC_Z          ,KC_W           ,KC_X          ,KC_F           ,
    KC_E           ,KC_H         ,KC_J         ,KC_L         ,KC_G          ,                                     KC_G           ,KC_A          ,KC_S           ,KC_D          ,KC_E           ,
    KC_B           ,KC_R         ,KC_M         ,KC_C         ,KC_V          ,                                     KC_V           ,KC_C          ,KC_M           ,KC_R          ,KC_B           ,
    _______        ,_______      ,_______      ,_______      ,LT(_NV,KC_SPC),_______     ,LT(_FN,KC_P)   ,KC_SPC         ,_______       ,_______        ,_______       ,_______)       ,

  [_NV] = LAYOUT_universal(
    KC_BSPC        ,KC_ESC       ,KC_UP        ,KC_ENT       ,KC_DEL        ,                                     KC_DEL         ,KC_LBRC       ,S(KC_QUOT)     ,KC_RBRC       ,KC_BSPC        ,
    LGUI_T(KC_HOME),KC_LEFT      ,KC_DOWN      ,KC_RGHT      ,LCS_T(KC_END) ,                                     LT(_BS,KC_3)   ,RCTL_T(KC_9)  ,RSFT_T(KC_QUOT),RALT_T(KC_0)  ,RGUI_T(KC_SCLN),
    LT(_BS,KC_F1)  ,LT(_BS,KC_F2),LT(_BS,KC_F3),LT(_BS,KC_F4),LT(_BS,KC_F5) ,                                     KC_BSLS        ,S(KC_LBRC)    ,KC_GRV         ,S(KC_RBRC)    ,S(KC_2)        ,
    _______        ,_______      ,_______      ,_______      ,_______       ,_______     ,LT(_FN,KC_BSPC),LT(_SM,KC_SPC) ,_______       ,_______        ,_______       ,_______)       ,

  [_FN] = LAYOUT_universal(
    KC_WBAK        ,KC_F1        ,KC_F2        ,KC_F3        ,KC_WFWD       ,                                     KC_WFWD        ,KC_PGDN       ,KC_WHOM        ,KC_PGUP       ,KC_WBAK        ,
    LGUI_T(KC_F10) ,LALT_T(KC_F4),LSFT_T(KC_F5),LCTL_T(KC_F6),KC_F11        ,                                     KC_F21         ,KC_MS_BTN1    ,KC_MS_BTN3     ,KC_MS_BTN2    ,KC_F20         ,
    KC_F12         ,KC_F7        ,KC_F8        ,KC_F9        ,LCTL(KC_W)    ,                                     KC_ENT         ,KC_F17        ,KC_F18         ,KC_F19        ,KC_F22         ,
    _______        ,_______      ,_______      ,_______      ,_______       ,_______     ,KC_P           ,_______        ,_______       ,_______        ,_______       ,_______)       ,

  [_SM] = LAYOUT_universal(
    KC_BSPC        ,KC_1         ,KC_2         ,KC_3         ,KC_DEL        ,                                     KC_DEL         ,S(KC_COMM)    ,KC_EQL         ,S(KC_DOT)     ,KC_BSPC        ,
    KC_0           ,KC_4         ,KC_5         ,KC_6         ,S(KC_4)       ,                                     S(KC_7)        ,S(KC_EQL)     ,KC_SLSH        ,S(KC_8)       ,S(KC_5)        ,
    KC_COMM        ,KC_7         ,KC_8         ,KC_9         ,KC_DOT        ,                                     S(KC_BSLS)     ,S(KC_1)       ,S(KC_SLSH)     ,S(KC_GRV)     ,S(KC_6)        ,
    _______        ,_______      ,_______      ,_______      ,_______       ,_______     ,_______        ,_______        ,_______       ,_______        ,_______       ,_______)       ,
};
// clang-format on

layer_state_t layer_state_set_user(layer_state_t state) {
    // Auto enable scroll mode when the highest layer is _SM
    keyball_set_scroll_mode(get_highest_layer(state) == _SM);
    return state;
}

#ifdef OLED_ENABLE

#    include "lib/oledkit/oledkit.h"

void oledkit_render_info_user(void) {
    keyball_oled_render_keyinfo();
    keyball_oled_render_ballinfo();
    keyball_oled_render_layerinfo();
}
#endif
