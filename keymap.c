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
    _SY,
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

uint8_t unpack_mods(uint16_t keycode) {
    const uint8_t mods = QK_MOD_TAP_GET_MODS(keycode);
    return mods & 0x10 ? (mods << 4) : mods;
}

bool pre_process_record_user(uint16_t keycode, keyrecord_t *record) {
    const uint16_t tap_part = 0xFF & keycode;
    if (record->event.pressed) {
        if (tap_part > KC_Z || IS_EXCEPTIONAL_INPUT(keycode, record)) {
            is_quick_succession_input = false;
            inter_keycode             = keycode;
        } else if (timer_elapsed(inter_record.event.time) > QUICK_TAP_TERM) {
            is_quick_succession_input = IS_QK_MOD_TAP(keycode) && (keycode & (QK_LALT | QK_LGUI));
            inter_keycode             = keycode;
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
    if (keycode == LT(_SY, keycode)) {
        keyball_set_scroll_mode(record->event.pressed);
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
        }
        tap_code(tap_part);
        within_word(poped_key);
        if (poped_key == keycode) {
            // Keep keys after `keycode` pending for the next input event.
            return;
        }
    }
}

#define IS_QK_COMBO(r) ((r)->event.key.row == 0 && (r)->event.key.col == 0)

void process_pended_keys(uint16_t keycode, keyrecord_t *record) {
    if (IS_UNILATERAL_INPUT(record, 0x88) || IS_QK_COMBO(record)) {
        set_mts_mods(&lmts);
        set_mts_mods(&rmts);
        is_quick_succession_input = false;
        return;
    }
    const bool is_left_side = IS_UNILATERAL_INPUT(record, 0x07);
    set_mts_mods(!is_left_side ? &lmts : &rmts);
    send_mts_taps(is_left_side ? &lmts : &rmts, keycode);
}

#define LCS_T(k) (MT(MOD_LCTL | MOD_LSFT, (k)))
#define LCG_T(k) (MT(MOD_LCTL | MOD_LGUI, (k)))
#define LCSA_T(k) (MT(MOD_LCTL | MOD_LSFT | MOD_LALT, (k)))
#define LCSG_T(k) (MT(MOD_LCTL | MOD_LSFT | MOD_LGUI, (k)))
#define LSAG_T(k) (MT(MOD_LSFT | MOD_LALT | MOD_LGUI, (k)))
#define RCG_T(k) (MT(MOD_RCTL | MOD_RGUI, (k)))
#define RCSA_T(k) (MT(MOD_RCTL | MOD_RSFT | MOD_RALT, (k)))
#define RCSG_T(k) (MT(MOD_RCTL | MOD_RSFT | MOD_RGUI, (k)))
#define RSAG_T(k) (MT(MOD_RSFT | MOD_RALT | MOD_RGUI, (k)))

static bool is_volkey_held;

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    static bool is_layer_SY_held;
    static bool is_fixed_swap_hands;
    static bool is_alternative_swap_hands;
    static bool oneshot_ignore_key_release;

    if (oneshot_ignore_key_release && !record->event.pressed) {
        oneshot_ignore_key_release = false;
        if (keycode == LT(_SY, keycode)) {
            is_layer_SY_held = false;
            if (!record->tap.count) {
                keyball_set_scroll_mode(true);
            }
        }
        return false;
    }

    if (IS_LAYER_ON(_NV)) {
        caps_word_off();
    } else if (is_layer_SY_held) {
        layer_on(_SY);
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
            if (record->tap.count) {
                if (!record->event.pressed) {
                    oneshot_ignore_key_release = true;
                }
            } else {
                is_volkey_held = record->event.pressed;
            }
            return false;
        case LT(_BS, 1):
            if (record->event.pressed) {
                if (record->tap.count == 1) {
                    if (is_fixed_swap_hands) {
                        is_fixed_swap_hands = false;
                        swap_hands_off();
                    } else if (is_alternative_swap_hands) {
                        swap_hands_toggle();
                    } else {
                        swap_hands_on();
                    }
                } else if (record->tap.count) {
                    is_fixed_swap_hands       = true;
                    is_alternative_swap_hands = false;
                    swap_hands_on();
                } else {
                    is_fixed_swap_hands = false;
                    is_alternative_swap_hands ^= true;
                    swap_hands_off();
                }
            }
            return false;
    }

    process_pended_keys(keycode, record);

    if (is_alternative_swap_hands) {
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

    switch (keycode) {
        case LT(_BS, KC_F1):
            nav.type = NAV_UNDO_REDO;
            return false;
        case LT(_BS, KC_F2)... LT(_BS, KC_F5): {
            static const uint8_t mods[4] = {
                MOD_LALT,
                MOD_LSFT | MOD_LCTL,
                MOD_LCTL,
                MOD_LCTL,
            };
            static const uint8_t codes[4] = {
                KC_APP,
                KC_F15,
                KC_F16,
                KC_C,
            };
            const uint8_t index      = keycode - LT(_BS, KC_F2);
            const uint8_t saved_mods = get_mods();
            keycode                  = index == 3 ? KC_V : KC_TAB;
            if (record->event.pressed) {
                clear_mods();
                if (record->tap.count) {
                    register_mods(mods[index]);
                    register_code(keycode);
                } else {
                    register_mods(index == 3 ? MOD_LCTL : 0);
                    tap_code(codes[index]);
                }
                // Keep Alt held for F2 so repeated F2 acts as Alt-Tab.
                if (index) {
                    set_mods(saved_mods);
                }
            } else if (record->tap.count) {
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
        case KC_INT4:
            if (record->event.pressed) {
                add_weak_mods(MOD_LGUI);
                register_code(KC_SLSH);
            } else {
                unregister_code(KC_SLSH);
            }
            return false;
        case KC_DOWN ... KC_UP:
            if (nav.type != NAV_WASD) {
                return true;
            }
        case KC_RGHT ... KC_LEFT:
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
                    case NAV_WASD:
                        switch (keycode) {
                            case KC_RGHT:
                                nav.keycode = is_swap_hands_on() ? KC_A : KC_D;
                                break;
                            case KC_LEFT:
                                nav.keycode = is_swap_hands_on() ? KC_D : KC_A;
                                break;
                            case KC_DOWN:
                                nav.keycode = KC_S;
                                break;
                            default:
                                nav.keycode = KC_W;
                        }
                        break;
                    default:
                        if (is_swap_hands_on()) {
                            nav.keycode = keycode == KC_LEFT ? KC_RGHT : KC_LEFT;
                            break;
                        }
                        return true;
                }
                register_code(nav.keycode);
                set_mods(saved_mods);
                nav.registered = true;
                return false;
            }
            break;
        case LT(_SY, KC_SPC):
            if (!record->tap.count) {
                is_layer_SY_held = record->event.pressed;
            }
        case LT(_FN, KC_BSPC):
            if (is_alternative_swap_hands && record->tap.count) {
                if (record->event.pressed) {
                    tap_code(keycode == LT(_SY, KC_SPC) ? KC_H : KC_F);
                }
                return false;
            }
            break;
        case LT(_GM, KC_NO):
            if (record->tap.count) {
                nav.type = record->tap.count > 1 ? (get_highest_layer(layer_state) == _GM ? NAV_WASD : NAV_TAB) : NAV_NONE;
                return false;
            }
        case LT(_NV, KC_H):
            layer_clear();
            if (!record->event.pressed && !record->tap.count) {
                unregister_mods(MOD_HYPR);
                nav.type = NAV_NONE;
            }
    }
    return true;
}

void post_process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed) {
        within_word(keycode);
    }
}

enum combos {
    CMB_INT4,
    CMB_VOL1,
    CMB_VOL2,
    CMB_SH_OS_TOGG1,
    CMB_SH_OS_TOGG2,
    CMB_LNG1,
    CMB_LNG2,
    CMB_OS_CTL,
    CMB_OS_SFT,
    CMB_OS_ALT,
    CMB_OS_GUI,
    CMB_MS_BTN1,
    CMB_MS_BTN2,
    CMB_MS_BTN3,
};

const uint16_t PROGMEM cmb_int4[]        = {LCAG_T(KC_Z), LCA_T(KC_K), COMBO_END};
const uint16_t PROGMEM cmb_vol1[]        = {LCAG_T(KC_Z), LSA_T(KC_M), COMBO_END};
const uint16_t PROGMEM cmb_vol2[]        = {KC_DOT, RCAG_T(KC_X), COMBO_END};
const uint16_t PROGMEM cmb_sh_os_togg1[] = {LSA_T(KC_M), LCA_T(KC_K), COMBO_END};
const uint16_t PROGMEM cmb_sh_os_togg2[] = {KC_COMMA, KC_DOT, COMBO_END};
const uint16_t PROGMEM cmb_lng1[]        = {LCTL_T(KC_S), LCS_T(KC_G), COMBO_END};
const uint16_t PROGMEM cmb_lng2[]        = {LT(_BS, KC_MINS), RCTL_T(KC_C), COMBO_END};
const uint16_t PROGMEM cmb_os_ctl[]      = {LSG_T(KC_D), LCG_T(KC_W), COMBO_END};
const uint16_t PROGMEM cmb_os_sft[]      = {LAG_T(KC_L), LCG_T(KC_W), COMBO_END};
const uint16_t PROGMEM cmb_os_alt[]      = {LAG_T(KC_L), LSG_T(KC_D), COMBO_END};
const uint16_t PROGMEM cmb_os_gui[]      = {KC_P, LAG_T(KC_L), COMBO_END};
const uint16_t PROGMEM cmb_ms_btn1[]     = {LSFT_T(KC_T), LCTL_T(KC_S), COMBO_END};
const uint16_t PROGMEM cmb_ms_btn2[]     = {LALT_T(KC_R), LSFT_T(KC_T), COMBO_END};
const uint16_t PROGMEM cmb_ms_btn3[]     = {LALT_T(KC_R), LCTL_T(KC_S), COMBO_END};

// clang-format off
combo_t key_combos[] = {
    [CMB_INT4]        = COMBO(cmb_int4, KC_INT4),
    [CMB_VOL1]        = COMBO(cmb_vol1, LT(_BS, KC_NO)),
    [CMB_VOL2]        = COMBO(cmb_vol2, LT(_BS, KC_NO)),
    [CMB_SH_OS_TOGG1] = COMBO(cmb_sh_os_togg1, LT(_BS, 1)),
    [CMB_SH_OS_TOGG2] = COMBO(cmb_sh_os_togg2, LT(_BS, 1)),
    [CMB_LNG1]        = COMBO(cmb_lng1, LT(_BS, KC_LNG1)),
    [CMB_LNG2]        = COMBO(cmb_lng2, LT(_BS, KC_LNG2)),
    [CMB_OS_CTL]      = COMBO(cmb_os_ctl, OSM(MOD_LCTL)),
    [CMB_OS_SFT]      = COMBO(cmb_os_sft, OSM(MOD_LSFT)),
    [CMB_OS_ALT]      = COMBO(cmb_os_alt, OSM(MOD_LALT)),
    [CMB_OS_GUI]      = COMBO(cmb_os_gui, OSM(MOD_LGUI)),
    [CMB_MS_BTN1]     = COMBO(cmb_ms_btn1, KC_MS_BTN1),
    [CMB_MS_BTN2]     = COMBO(cmb_ms_btn2, KC_MS_BTN2),
    [CMB_MS_BTN3]     = COMBO(cmb_ms_btn3, KC_MS_BTN3),
};
// clang-format on

bool combo_should_trigger(uint16_t combo_index, combo_t *combo, uint16_t keycode, keyrecord_t *record) {
    switch (combo_index) {
        case CMB_MS_BTN1 ... CMB_MS_BTN3:
            if (IS_LAYER_ON(_NV)) {
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
    if (is_volkey_held) {
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
    KC_P          ,LAG_T(KC_L)  ,LSG_T(KC_D)  ,LCG_T(KC_W)  ,LCSG_T(KC_Q)  ,                                     RCSG_T(KC_Q)   ,RCG_T(KC_Y)   ,RSG_T(KC_O)    ,RAG_T(KC_U)   ,RCSA_T(KC_J)   ,
    LGUI_T(KC_N)  ,LALT_T(KC_R) ,LSFT_T(KC_T) ,LCTL_T(KC_S) ,LCS_T(KC_G)   ,                                     LT(_BS,KC_MINS),RCTL_T(KC_C)  ,RSFT_T(KC_A)   ,RALT_T(KC_I)  ,RGUI_T(KC_E)   ,
    LSAG_T(KC_B)  ,LCAG_T(KC_Z) ,LSA_T(KC_M)  ,LCA_T(KC_K)  ,LCSA_T(KC_V)  ,                                     S(KC_MINS)     ,KC_COMM       ,KC_DOT         ,RCAG_T(KC_X)  ,RSAG_T(KC_SCLN),
    LT(_GM,KC_NO) ,LALT(KC_PSCR),LSFT(KC_PSCR),KC_ENT       ,LT(_NV,KC_H)  ,LT(_FN,KC_F),LT(_FN,KC_BSPC),LT(_SY,KC_SPC) ,KC_ENT        ,RSFT(KC_PSCR)  ,RALT(KC_PSCR) ,LT(_GM,KC_NO)) ,

  [_GM] = LAYOUT_universal(
    KC_P          ,KC_X         ,KC_K         ,KC_Z         ,KC_Q          ,                                     KC_Q           ,KC_Z          ,KC_UP          ,KC_X          ,KC_P           ,
    KC_E          ,KC_H         ,KC_J         ,KC_L         ,KC_G          ,                                     KC_G           ,KC_LEFT       ,KC_DOWN        ,KC_RGHT       ,KC_E           ,
    KC_B          ,KC_R         ,KC_M         ,KC_C         ,KC_V          ,                                     KC_V           ,KC_C          ,KC_M           ,KC_R          ,KC_B           ,
    _______       ,_______      ,_______      ,_______      ,LT(_NV,KC_SPC),_______     ,LT(_FN,KC_F)   ,KC_SPC         ,_______       ,_______        ,_______       ,_______)       ,

  [_NV] = LAYOUT_universal(
    KC_BSPC       ,KC_ESC       ,KC_UP        ,KC_ENT       ,KC_DEL        ,                                     KC_DEL         ,RCG_T(KC_LBRC),S(KC_QUOT)     ,RAG_T(KC_RBRC),KC_BSPC        ,
    KC_HOME       ,KC_LEFT      ,KC_DOWN      ,KC_RGHT      ,KC_END        ,                                     LT(_BS,KC_3)   ,RCTL_T(KC_9)  ,RSFT_T(KC_QUOT),RALT_T(KC_0)  ,RGUI_T(KC_SCLN),
    LT(_BS,KC_F1) ,LT(_BS,KC_F2),LT(_BS,KC_F3),LT(_BS,KC_F4),LT(_BS,KC_F5) ,                                     KC_BSLS        ,S(KC_LBRC)    ,KC_GRV         ,S(KC_RBRC)    ,S(KC_2)        ,
    _______       ,_______      ,_______      ,_______      ,_______       ,_______     ,LT(_FN,KC_BSPC),LT(_SY,KC_SPC) ,_______       ,_______        ,_______       ,_______)       ,

  [_FN] = LAYOUT_universal(
    KC_WBAK       ,KC_F1        ,KC_F2        ,KC_F3        ,KC_WFWD       ,                                     KC_WFWD        ,KC_PSCR       ,KC_PGUP        ,KC_PGDN       ,KC_WBAK        ,
    LGUI_T(KC_F10),LALT_T(KC_F4),LSFT_T(KC_F5),LCTL_T(KC_F6),KC_F11        ,                                     KC_F21         ,KC_MS_BTN1    ,KC_MS_BTN3     ,KC_MS_BTN2    ,KC_F20         ,
    KC_F12        ,KC_F7        ,KC_F8        ,KC_F9        ,LCTL(KC_W)    ,                                     KC_WHOM        ,KC_F17        ,KC_F18         ,KC_F19        ,KC_F22         ,
    _______       ,_______      ,_______      ,_______      ,_______       ,_______     ,_______        ,_______        ,_______       ,_______        ,_______       ,_______)       ,

  [_SY] = LAYOUT_universal(
    KC_BSPC       ,KC_1         ,KC_2         ,KC_3         ,KC_DEL        ,                                     KC_DEL         ,S(KC_COMM)    ,KC_EQL         ,S(KC_DOT)     ,KC_BSPC        ,
    KC_0          ,KC_4         ,KC_5         ,KC_6         ,S(KC_4)       ,                                     S(KC_7)        ,S(KC_EQL)     ,KC_SLSH        ,S(KC_8)       ,S(KC_5)        ,
    KC_COMM       ,KC_7         ,KC_8         ,KC_9         ,KC_DOT        ,                                     S(KC_BSLS)     ,S(KC_1)       ,S(KC_SLSH)     ,S(KC_GRV)     ,S(KC_6)        ,
    _______       ,_______      ,_______      ,_______      ,_______       ,_______     ,_______        ,_______        ,_______       ,_______        ,_______       ,_______)       ,
};
// clang-format on

layer_state_t layer_state_set_user(layer_state_t state) {
    // Auto enable scroll mode when the highest layer is _SY
    keyball_set_scroll_mode(get_highest_layer(state) == _SY);
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
