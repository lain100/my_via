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

#define IS_UNILATERAL_INPUT(r, m) ((m) & (1U << (r)->event.key.row))

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
        if (tap_part > KC_Z || IS_UNILATERAL_INPUT(record, 0x88) || timer_elapsed(inter_record.event.time) > QUICK_TAP_TERM) {
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
    if (IS_QK_LAYER_TAP(keycode) && QK_LAYER_TAP_GET_LAYER(keycode) == 4) {
        keyball_set_scroll_mode(record->event.pressed);
    }
    return true;
}

uint16_t get_quick_tap_term(uint16_t keycode, keyrecord_t *record) {
    if (IS_UNILATERAL_INPUT(record, 0x88) || (IS_QK_MOD_TAP(keycode) && (keycode & QK_LSFT) && get_highest_layer(layer_state) == 0)) {
        return 0;
    }
    return QUICK_TAP_TERM;
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

enum navkey_types { NAV_UndR = 1, NAV_Tab };

typedef struct {
    uint8_t  keycode;
    uint8_t  type;
    uint8_t  phase;
    uint16_t timer;
    bool     registered;
} morph_key_t;

const uint16_t del_rep_delay[33] = {
    400, 99, 79, 65, 57, 49, 43, 40, 35, 33, 30, 28, 26, 25, 23, 22, 20, 20, 19, 18, 17, 16, 15, 15, 14, 14, 13, 13, 12, 12, 11, 11, 10,
};

static morph_key_t nav, del;

void repeat_keys(void) {
    if (del.registered && timer_elapsed(del.timer) > del_rep_delay[del.phase]) {
        tap_code(del.keycode);
        del.timer = timer_read();
        if (del.phase < ARRAY_SIZE(del_rep_delay) - 1) {
            del.phase++;
        }
    }
}

void within_word(uint16_t keycode) {
    static const uint16_t brcts[][2] = {
        {S(KC_QUOT), S(KC_QUOT)}, {S(KC_LBRC), S(KC_RBRC)}, {S(KC_COMM), S(KC_DOT)}, {S(KC_9), S(KC_0)}, {KC_QUOT, KC_QUOT}, {KC_LBRC, KC_RBRC}, {KC_GRV, KC_GRV}, {0, -1},
    };
    static const uint8_t null_id      = ARRAY_SIZE(brcts) - 1;
    static uint8_t       reception_id = null_id;
    keycode &= 0xFF;

    if (is_caps_word_on()) {
        caps_word_press_user(keycode);
    }
    if (get_weak_mods() & MOD_LSFT) {
        keycode |= QK_LSFT;
    }
    clear_weak_mods();
    if (keycode == brcts[reception_id][1]) {
        const uint8_t saved_mods = get_mods();
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
            return;
        }
    }
}

#define IS_QK_COMBO(r) ((r)->event.key.row == 0 && (r)->event.key.col == 0)

void procoss_pended_keys(uint16_t keycode, keyrecord_t *record) {
    if (IS_UNILATERAL_INPUT(record, 0x88) || IS_QK_COMBO(record)) {
        set_mts_mods(&lmts);
        set_mts_mods(&rmts);
        is_quick_succession_input = false;
        return;
    }
    const bool is_row_0_to_2 = IS_UNILATERAL_INPUT(record, 0x07);
    set_mts_mods(!is_row_0_to_2 ? &lmts : &rmts);
    send_mts_taps(is_row_0_to_2 ? &lmts : &rmts, keycode);
}

bool process_record_user(uint16_t keycode, keyrecord_t *record) {
    static bool layer4_is_held;
    if (IS_LAYER_ON(2)) {
        caps_word_off();
    } else if (layer4_is_held) {
        layer_on(4);
    }

    static bool is_swap_hands_toggle;
    switch (keycode) {
        case LT(0, KC_3):
        case LT(0, KC_X):
            if (!record->tap.count) {
                nav.type = record->event.pressed ? NAV_Tab : 0;
            }
        case QK_MOD_TAP ... QK_MOD_TAP_MAX:
            if (IS_LAYER_ON(2)) {
                const uint8_t saved_mods = get_mods();
                caps_word_on();
                set_mods(saved_mods);
            }
            if (record->event.pressed && !record->tap.count) {
                mt_queue_t *mts = IS_UNILATERAL_INPUT(record, 0x0F) ? &lmts : &rmts;
                enqueue(mts, keycode);
                return false;
            }
            break;
        case LT(0, 1):
            if (record->event.pressed) {
                if (record->tap.count == 1) {
                    if (is_swap_hands_toggle) {
                        is_swap_hands_toggle = false;
                        swap_hands_off();
                    } else {
                        swap_hands_on();
                    }
                } else if (record->tap.count) {
                    is_swap_hands_toggle = true;
                    swap_hands_on();
                } else {
                    is_swap_hands_toggle = false;
                    swap_hands_off();
                    nav.type = NAV_Tab;
                }
            }
            return false;
    }

    procoss_pended_keys(keycode, record);
    if (!is_swap_hands_toggle) {
        swap_hands_off();
    }

    switch (keycode) {
        case LT(0, KC_NO):
            if (record->event.pressed) {
                const uint8_t current_layer = get_highest_layer(layer_state) % 4;
                layer_move(record->tap.count ? (current_layer + 1) : 0);
            }
            return false;
        case LT(0, KC_F1):
            nav.type = NAV_UndR;
            return false;
        case LT(0, KC_F2)... LT(0, KC_F5): {
            static const uint8_t mods[4]    = {MOD_LALT, MOD_LSFT | MOD_LCTL, MOD_LCTL, MOD_LCTL};
            static const uint8_t codes[4]   = {KC_APP, KC_F15, KC_F16, KC_C};
            const uint8_t        index      = keycode - LT(0, KC_F2);
            const uint8_t        saved_mods = get_mods();
            keycode                         = index == 3 ? KC_V : KC_TAB;
            if (record->event.pressed) {
                clear_mods();
                if (record->tap.count) {
                    register_mods(mods[index]);
                    register_code(keycode);
                } else {
                    register_mods(index == 3 ? MOD_LCTL : 0);
                    tap_code(codes[index]);
                }
                if (index) {
                    set_mods(saved_mods);
                }
            } else if (record->tap.count) {
                unregister_code(keycode);
            }
            return false;
        }
        case LT(0, KC_LNG1):
        case LT(0, KC_LNG2):
            if (record->event.pressed) {
                if (record->tap.count <= 1) {
                    tap_code(keycode == LT(0, KC_LNG2) ? KC_F13 : KC_F14);
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
        case KC_RGHT ... KC_LEFT:
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
                    case NAV_UndR:
                        clear_mods();
                        register_mods(MOD_LCTL);
                        nav.keycode = keycode == KC_LEFT ? KC_Z : KC_Y;
                        break;
                    case NAV_Tab:
                        switch (keycode) {
                            case KC_LEFT:
                                register_mods(MOD_LSFT);
                            default:
                                nav.keycode = KC_TAB;
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
        case KC_BSPC:
        case KC_DEL:
            if (record->event.pressed) {
                tap_code(keycode);
                del = (morph_key_t){
                    .keycode    = keycode,
                    .timer      = timer_read(),
                    .registered = true,
                };
            } else {
                del.registered = false;
            }
            return false;
        case LT(2, KC_H):
            if (!record->tap.count) {
                if (!record->event.pressed) {
                    unregister_mods(MOD_HYPR);
                    nav.type = 0;
                }
                if (IS_LAYER_OFF(1)) {
                    layer_clear();
                }
            }
            break;
        case LT(4, KC_SPC):
            if (!record->tap.count) {
                layer4_is_held = record->event.pressed;
            }
    }
    return true;
}

void post_process_record_user(uint16_t keycode, keyrecord_t *record) {
    if (record->event.pressed) {
        within_word(keycode);
    }
}

void matrix_scan_user(void) {
    repeat_keys();
}

enum combos {
    CMB_INT4,
    CMB_SH_OS_TOGG1,
    CMB_DEL1,
    CMB_DEL2,
    CMB_SH_OS_TOGG2,
    CMB_LNG1,
    CMB_LNG2,
    CMB_PSCR,
    CMB_OS_CTL,
    CMB_OS_SFT,
    CMB_OS_ALT,
    CMB_OS_GUI,
    CMB_MS_BTN1,
    CMB_MS_BTN2,
    CMB_MS_BTN3,
};

#define RCA_T(k) (MT(MOD_RCTL | MOD_RALT, (k)))

const uint16_t PROGMEM cmb_int4[]        = {KC_Z, KC_K, COMBO_END};
const uint16_t PROGMEM cmb_sh_os_togg1[] = {KC_M, KC_K, COMBO_END};
const uint16_t PROGMEM cmb_del1[]        = {KC_Z, KC_M, COMBO_END};
const uint16_t PROGMEM cmb_del2[]        = {KC_DOT, KC_MINS, COMBO_END};
const uint16_t PROGMEM cmb_sh_os_togg2[] = {RCA_T(KC_C), KC_DOT, COMBO_END};
const uint16_t PROGMEM cmb_lng1[]        = {LCTL_T(KC_S), KC_G, COMBO_END};
const uint16_t PROGMEM cmb_lng2[]        = {LT(0, KC_X), RCTL_T(KC_Y), COMBO_END};
const uint16_t PROGMEM cmb_pscr[]        = {KC_L, KC_D, KC_W, COMBO_END};
const uint16_t PROGMEM cmb_os_ctl[]      = {KC_D, KC_W, COMBO_END};
const uint16_t PROGMEM cmb_os_sft[]      = {KC_L, KC_W, COMBO_END};
const uint16_t PROGMEM cmb_os_alt[]      = {KC_L, KC_D, COMBO_END};
const uint16_t PROGMEM cmb_os_gui[]      = {KC_Q, KC_L, COMBO_END};
const uint16_t PROGMEM cmb_ms_btn1[]     = {LSFT_T(KC_T), LCTL_T(KC_S), COMBO_END};
const uint16_t PROGMEM cmb_ms_btn2[]     = {LALT_T(KC_R), LSFT_T(KC_T), COMBO_END};
const uint16_t PROGMEM cmb_ms_btn3[]     = {LALT_T(KC_R), LCTL_T(KC_S), COMBO_END};

combo_t key_combos[] = {
    [CMB_INT4] = COMBO(cmb_int4, KC_INT4), [CMB_SH_OS_TOGG1] = COMBO(cmb_sh_os_togg1, LT(0, 1)), [CMB_DEL1] = COMBO(cmb_del1, KC_DEL), [CMB_DEL2] = COMBO(cmb_del2, KC_DEL), [CMB_SH_OS_TOGG2] = COMBO(cmb_sh_os_togg2, LT(0, 1)), [CMB_LNG1] = COMBO(cmb_lng1, LT(0, KC_LNG1)), [CMB_LNG2] = COMBO(cmb_lng2, LT(0, KC_LNG2)), [CMB_PSCR] = COMBO(cmb_pscr, KC_PSCR), [CMB_OS_CTL] = COMBO(cmb_os_ctl, OSM(MOD_LCTL)), [CMB_OS_SFT] = COMBO(cmb_os_sft, OSM(MOD_LSFT)), [CMB_OS_ALT] = COMBO(cmb_os_alt, OSM(MOD_LALT)), [CMB_OS_GUI] = COMBO(cmb_os_gui, OSM(MOD_LGUI)), [CMB_MS_BTN1] = COMBO(cmb_ms_btn1, KC_MS_BTN1), [CMB_MS_BTN2] = COMBO(cmb_ms_btn2, KC_MS_BTN2), [CMB_MS_BTN3] = COMBO(cmb_ms_btn3, KC_MS_BTN3),
};

bool combo_should_trigger(uint16_t combo_index, combo_t *combo, uint16_t keycode, keyrecord_t *record) {
    switch (combo_index) {
        case CMB_MS_BTN1 ... CMB_MS_BTN3:
            if (IS_LAYER_ON(2)) {
                return false;
            }
    }
    return true;
}

uint8_t combo_ref_from_layer(uint8_t layer) {
    return 0;
}

bool caps_word_press_user(uint16_t keycode) {
    switch (keycode) {
        case KC_A ... KC_Z:
        case KC_MINS:
            add_weak_mods(MOD_LSFT);
            return true;
        case KC_QUOT:
            if (IS_LAYER_ON(2)) {
                return false;
            }
        case KC_1 ... KC_0:
        case KC_SCLN:
            if (IS_LAYER_ON(2)) {
                add_weak_mods(MOD_LSFT);
            }
        case KC_EQL:
        case KC_DEL:
        case KC_BSPC:
        case KC_SLSH:
            return true;
    }
    return false;
}

#define VOL_TENSION_THRESHOLD 5

report_mouse_t pointing_device_task_kb(report_mouse_t mouse_report) {
    if (del.registered) {
        const uint16_t keycode = mouse_report.x > VOL_TENSION_THRESHOLD ? KC_VOLU : mouse_report.x < -VOL_TENSION_THRESHOLD ? KC_VOLD : 0;
        if (keycode) {
            del.keycode = 0;
        }
        register_code(keycode);
        unregister_code(keycode);
        mouse_report = (report_mouse_t){};
    }
    return mouse_report;
}

#ifdef SWAP_HANDS_ENABLE
__attribute__((weak)) const keypos_t PROGMEM hand_swap_config[MATRIX_ROWS][MATRIX_COLS] = {
    {{0, 4}, {1, 4}, {2, 4}, {3, 4}, {4, 4}, {5, 4}}, {{0, 5}, {1, 5}, {2, 5}, {3, 5}, {4, 5}, {5, 5}}, {{0, 6}, {1, 6}, {2, 6}, {3, 6}, {4, 6}, {5, 6}}, {{0, 7}, {1, 7}, {2, 7}, {3, 7}, {4, 7}, {5, 7}}, {{0, 0}, {1, 0}, {2, 0}, {3, 0}, {4, 0}, {5, 0}}, {{0, 1}, {1, 1}, {2, 1}, {3, 1}, {4, 1}, {5, 1}}, {{0, 2}, {1, 2}, {2, 2}, {3, 2}, {4, 2}, {5, 2}}, {{0, 3}, {1, 3}, {2, 3}, {3, 3}, {4, 3}, {5, 3}},
};
#endif

// clang-format off
const uint16_t PROGMEM keymaps[][MATRIX_ROWS][MATRIX_COLS] = {
  // keymap for default (VIA)
  [0] = LAYOUT_universal(
    KC_Q     , KC_W     , KC_E     , KC_R     , KC_T     ,                            KC_Y     , KC_U     , KC_I     , KC_O     , KC_P     ,
    KC_A     , KC_S     , KC_D     , KC_F     , KC_G     ,                            KC_H     , KC_J     , KC_K     , KC_L     , KC_MINS  ,
    KC_Z     , KC_X     , KC_C     , KC_V     , KC_B     ,                            KC_N     , KC_M     , KC_COMM  , KC_DOT   , KC_SLSH  ,
    KC_LCTL  , KC_LGUI  , KC_LALT  ,LSFT_T(KC_LNG2),LT(1,KC_SPC),LT(3,KC_LNG1),KC_BSPC,LT(2,KC_ENT),LSFT_T( KC_LNG2),KC_RALT,KC_RGUI, KC_RSFT),

  [1] = LAYOUT_universal(
    KC_F1    , KC_F2    , KC_F3    , KC_F4    , KC_RBRC  ,                            KC_F6    , KC_F7    , KC_F8    , KC_F9    , KC_F10   ,
    KC_F5    , KC_EXLM  , S(KC_6)  ,S(KC_INT3), S(KC_8)  ,                           S(KC_INT1), KC_BTN1  , KC_PGUP  , KC_BTN2  , KC_SCLN  ,
    S(KC_EQL),S(KC_LBRC),S(KC_7)   , S(KC_2)  ,S(KC_RBRC),                            KC_LBRC  , KC_DLR   , KC_PGDN  , KC_BTN3  , KC_F11   ,
    KC_INT1  , KC_EQL   , S(KC_3)  , _______  , _______  , _______  ,      TO(2)    , TO(0)    , _______  , KC_RALT  , KC_RGUI  , KC_F12),

  [2] = LAYOUT_universal(
    KC_TAB   , KC_7     , KC_8     , KC_9     , KC_MINS  ,                            KC_NUHS  , _______  , KC_BTN3  , _______  , KC_BSPC  ,
   S(KC_QUOT), KC_4     , KC_5     , KC_6     ,S(KC_SCLN),                            S(KC_9)  , KC_BTN1  , KC_UP    , KC_BTN2  , KC_QUOT  ,
    KC_SLSH  , KC_1     , KC_2     , KC_3     ,S(KC_MINS),                           S(KC_NUHS), KC_LEFT  , KC_DOWN  , KC_RGHT  , _______  ,
    KC_ESC   , KC_0     , KC_DOT   , KC_DEL   , KC_ENT   , KC_BSPC  ,      _______  , _______  , _______  , _______  , _______  , _______),

  [3] = LAYOUT_universal(
    RGB_TOG  , AML_TO   , AML_I50  , AML_D50  , _______  ,                            _______  , _______  , SSNP_HOR , SSNP_VRT , SSNP_FRE ,
    RGB_MOD  , RGB_HUI  , RGB_SAI  , RGB_VAI  , SCRL_DVI ,                            _______  , _______  , _______  , _______  , _______  ,
    RGB_RMOD , RGB_HUD  , RGB_SAD  , RGB_VAD  , SCRL_DVD ,                            CPI_D1K  , CPI_D100 , CPI_I100 , CPI_I1K  , KBC_SAVE ,
    QK_BOOT  , KBC_RST  , _______  , _______  , _______  , _______  ,      _______  , _______  , _______  , _______  , KBC_RST  , QK_BOOT),
};
// clang-format on

layer_state_t layer_state_set_user(layer_state_t state) {
    // Auto enable scroll mode when the highest layer is 4
    keyball_set_scroll_mode(get_highest_layer(state) == 4);
#ifdef POINTING_DEVICE_AUTO_MOUSE_ENABLE
    keyball_handle_auto_mouse_layer_change(state);
#endif
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
