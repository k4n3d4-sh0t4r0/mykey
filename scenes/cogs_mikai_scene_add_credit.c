#include "../cogs_mikai.h"
#include <furi_hal_rtc.h>

enum {
    AddCreditSceneEventInput,
    AddCreditSceneEventWrite,
    AddCreditSceneEventSave,
    AddCreditSceneEventDiscard,
    AddCreditSceneEventPopupDone,
};

static bool cogs_mikai_scene_add_credit_validator(const char* text, FuriString* error, void* context) {
    UNUSED(context);

    if(strlen(text) == 0) {
        return true;
    }

    // digits and at most one decimal point
    bool has_decimal = false;
    for(size_t i = 0; text[i] != '\0'; i++) {
        if(text[i] == '.' || text[i] == ',') {
            if(has_decimal) {
                furi_string_set(error, "Only one decimal point");
                return false;
            }
            has_decimal = true;
        } else if(text[i] < '0' || text[i] > '9') {
            furi_string_set(error, "Only numbers and '.'");
            return false;
        }
    }

    return true;
}

// Validate a date entered as DD/MM/YYYY (also accepts '.' or '-' as separators)
static bool cogs_mikai_scene_add_credit_date_validator(const char* text, FuriString* error, void* context) {
    UNUSED(context);

    if(strlen(text) == 0) {
        return true;
    }

    for(size_t i = 0; text[i] != '\0'; i++) {
        char c = text[i];
        bool is_digit = (c >= '0' && c <= '9');
        bool is_sep = (c == '/' || c == '.' || c == '-');
        if(!is_digit && !is_sep) {
            furi_string_set(error, "Numbers and '/' only");
            return false;
        }
    }

    return true;
}

// Parse DD/MM/YYYY (or DD/MM/YY). year_out is returned as full year (e.g. 2026).
static bool parse_date(const char* text, uint8_t* day, uint8_t* month, uint16_t* year) {
    if(!text || !day || !month || !year) return false;

    uint32_t parts[3] = {0, 0, 0};
    uint8_t digits[3] = {0, 0, 0};
    uint8_t idx = 0;

    for(size_t i = 0; text[i] != '\0'; i++) {
        char c = text[i];
        if(c >= '0' && c <= '9') {
            if(idx > 2) return false;
            parts[idx] = parts[idx] * 10 + (c - '0');
            digits[idx]++;
        } else { // separator
            idx++;
            if(idx > 2) return false;
        }
    }

    // Need exactly day, month and year
    if(digits[0] == 0 || digits[1] == 0 || digits[2] == 0) return false;

    uint32_t d = parts[0];
    uint32_t m = parts[1];
    uint32_t y = parts[2];

    // Accept two-digit year as 20xx
    if(digits[2] <= 2) y += 2000;

    if(d < 1 || d > 31) return false;
    if(m < 1 || m > 12) return false;
    if(y < 2000 || y > 2127) return false; // year stored on 7 bits (year - 2000)

    *day = (uint8_t)d;
    *month = (uint8_t)m;
    *year = (uint16_t)y;
    return true;
}

static bool parse_euros_to_cents(const char* text, uint16_t* cents) {
    if(!text || !cents) return false;

    uint32_t integer_part = 0;
    uint32_t decimal_part = 0;
    uint32_t decimal_digits = 0;
    bool in_decimal = false;

    for(size_t i = 0; text[i] != '\0'; i++) {
        if(text[i] == '.' || text[i] == ',') {
            in_decimal = true;
        } else if(text[i] >= '0' && text[i] <= '9') {
            if(in_decimal) {
                if(decimal_digits < 2) {
                    decimal_part = decimal_part * 10 + (text[i] - '0');
                    decimal_digits++;
                }
            } else {
                integer_part = integer_part * 10 + (text[i] - '0');
            }
        }
    }
    if(decimal_digits == 1) {
        decimal_part *= 10;
    }

    uint32_t total_cents = integer_part * 100 + decimal_part;
    if(total_cents > 99999) return false; // Max 999.99 EUR

    *cents = (uint16_t)total_cents;
    return true;
}

static void cogs_mikai_scene_add_credit_text_input_callback(void* context) {
    COGSMyKaiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, AddCreditSceneEventInput);
}

static void cogs_mikai_scene_add_credit_popup_callback(void* context) {
    COGSMyKaiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, AddCreditSceneEventPopupDone);
}

void cogs_mikai_scene_add_credit_on_enter(void* context) {
    COGSMyKaiApp* app = context;

    if(!app->mykey.is_loaded) {
        Popup* popup = app->popup;
        popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
        popup_set_text(popup, "No card loaded\nRead a card first", 64, 25, AlignCenter, AlignTop);
        popup_set_callback(popup, cogs_mikai_scene_add_credit_popup_callback);
        popup_set_context(popup, app);
        popup_set_timeout(popup, 2000);
        popup_enable_timeout(popup);
        view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);
        return;
    }

    app->add_credit_step = 0;
    app->add_credit_cents = 0;

    TextInput* text_input = app->text_input;
    snprintf(app->text_buffer, sizeof(app->text_buffer), "5.00");

    text_input_set_header_text(text_input, "Add Credit (EUR)");
    text_input_set_validator(text_input, cogs_mikai_scene_add_credit_validator, NULL);
    text_input_set_result_callback(
        text_input,
        cogs_mikai_scene_add_credit_text_input_callback,
        app,
        app->text_buffer,
        sizeof(app->text_buffer),
        false);

    view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewTextInput);
}

// Comparable key YYYYMMDD from a (year-2000, month, day) triple
static uint32_t date_key(uint16_t full_year, uint8_t month, uint8_t day) {
    return (uint32_t)full_year * 10000u + (uint32_t)month * 100u + day;
}

// Switch the text input to the date entry step, pre-filled with the oldest date
// that is still allowed: the last transaction date if there is history (that is
// the minimum a new transaction may use), otherwise today's date.
static void cogs_mikai_scene_add_credit_show_date_input(COGSMyKaiApp* app) {
    DateTime datetime;
    furi_hal_rtc_get_datetime(&datetime);

    uint16_t def_year = datetime.year;
    uint8_t def_month = datetime.month;
    uint8_t def_day = datetime.day;

    uint8_t ld = 0, lm = 0, ly = 0;
    if(mykey_get_last_transaction_date(&app->mykey, &ld, &lm, &ly)) {
        def_year = (uint16_t)ly + 2000;
        def_month = lm;
        def_day = ld;
    }

    snprintf(
        app->text_buffer,
        sizeof(app->text_buffer),
        "%02u/%02u/%04u",
        def_day,
        def_month,
        def_year);

    TextInput* text_input = app->text_input;
    text_input_reset(text_input);
    text_input_set_header_text(text_input, "Date (DD/MM/YYYY)");
    text_input_set_validator(text_input, cogs_mikai_scene_add_credit_date_validator, NULL);
    text_input_set_result_callback(
        text_input,
        cogs_mikai_scene_add_credit_text_input_callback,
        app,
        app->text_buffer,
        sizeof(app->text_buffer),
        false);

    app->add_credit_step = 1;
    view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewTextInput);
}

bool cogs_mikai_scene_add_credit_on_event(void* context, SceneManagerEvent event) {
    COGSMyKaiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == AddCreditSceneEventInput) {
            if(app->text_buffer[0] == '\0') {
                FURI_LOG_W(TAG, "Ignoring empty text_buffer (already processed)");
                consumed = true;
                return consumed;
            }

            if(app->add_credit_step == 0) {
                // Step 1: parse the amount, then ask for the date
                FURI_LOG_I(TAG, "Add credit input: '%s'", app->text_buffer);
                uint16_t cents = 0;
                bool parse_ok = parse_euros_to_cents(app->text_buffer, &cents);
                FURI_LOG_I(TAG, "Parse result: %d, cents: %d", parse_ok, cents);

                if(parse_ok && cents > 0) {
                    FURI_LOG_I(TAG, "Valid amount: %d cents", cents);
                    app->add_credit_cents = cents;
                    cogs_mikai_scene_add_credit_show_date_input(app);
                } else {
                    FURI_LOG_E(TAG, "Invalid amount: parse_ok=%d, cents=%d", parse_ok, cents);
                    Popup* popup = app->popup;
                    popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
                    popup_set_text(
                        popup, "Invalid amount\nEnter 0.01-999.99", 64, 25, AlignCenter, AlignTop);
                    popup_set_callback(popup, cogs_mikai_scene_add_credit_popup_callback);
                    popup_set_context(popup, app);
                    popup_set_timeout(popup, 2000);
                    popup_enable_timeout(popup);
                    view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);
                }
                consumed = true;
                return consumed;
            }

            // Step 2: parse the date and apply the credit
            FURI_LOG_I(TAG, "Add credit date input: '%s'", app->text_buffer);
            uint8_t day = 0, month = 0;
            uint16_t year = 0;
            bool date_ok = parse_date(app->text_buffer, &day, &month, &year);
            uint16_t cents = app->add_credit_cents;
            FURI_LOG_I(TAG, "Date parse: %d -> %02u/%02u/%04u", date_ok, day, month, year);

            // The history must stay chronological: refuse a date older than the
            // most recent transaction already on the card.
            bool date_too_old = false;
            if(date_ok) {
                uint8_t ld = 0, lm = 0, ly = 0;
                if(mykey_get_last_transaction_date(&app->mykey, &ld, &lm, &ly)) {
                    uint16_t last_full_year = (uint16_t)ly + 2000;
                    if(date_key(year, month, day) <
                       date_key(last_full_year, lm, ld)) {
                        date_too_old = true;
                        FURI_LOG_E(
                            TAG,
                            "Date %02u/%02u/%04u is older than last txn %02u/%02u/%04u",
                            day, month, year, ld, lm, last_full_year);
                    }
                }
            }

            if(date_ok && !date_too_old && cents > 0) {
                bool success = mykey_add_cents(
                    &app->mykey,
                    cents,
                    day,
                    month,
                    (uint8_t)(year - 2000));

                if(success) {
                    // cache updated credit
                    app->mykey.current_credit = mykey_get_current_credit(&app->mykey);

                    // reset text input to prevent any further callbacks
                    text_input_reset(app->text_input);

                    // clear text buffer to prevent re-triggering
                    memset(app->text_buffer, 0, sizeof(app->text_buffer));

                    Popup* popup = app->popup;
                    popup_set_header(popup, "Credit Added!", 64, 10, AlignCenter, AlignTop);
                    popup_set_text(popup, "Saved in memory\nUse 'Write to Card'", 64, 25, AlignCenter, AlignTop);
                    popup_set_callback(popup, cogs_mikai_scene_add_credit_popup_callback);
                    popup_set_context(popup, app);
                    popup_set_timeout(popup, 2000);
                    popup_enable_timeout(popup);
                    view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);
                    notification_message(app->notifications, &sequence_success);
                } else {
                    Popup* popup = app->popup;
                    popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
                    popup_set_text(popup, "Failed to add credit", 64, 25, AlignCenter, AlignTop);
                    popup_set_callback(popup, cogs_mikai_scene_add_credit_popup_callback);
                    popup_set_context(popup, app);
                    popup_set_timeout(popup, 2000);
                    popup_enable_timeout(popup);
                    view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);
                    notification_message(app->notifications, &sequence_error);
                }
            } else {
                FURI_LOG_E(
                    TAG,
                    "Rejected date: date_ok=%d, too_old=%d, cents=%d",
                    date_ok, date_too_old, cents);
                Popup* popup = app->popup;
                popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
                popup_set_text(
                    popup,
                    date_too_old ? "Date too old\nKeep history order" :
                                   "Invalid date\nUse DD/MM/YYYY",
                    64,
                    25,
                    AlignCenter,
                    AlignTop);
                popup_set_callback(popup, cogs_mikai_scene_add_credit_popup_callback);
                popup_set_context(popup, app);
                popup_set_timeout(popup, 2000);
                popup_enable_timeout(popup);
                view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);
            }
            consumed = true;
        } else {
            scene_manager_search_and_switch_to_previous_scene(app->scene_manager, COGSMyKaiSceneStart);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(app->mykey.is_modified) {
            scene_manager_search_and_switch_to_previous_scene(app->scene_manager, COGSMyKaiSceneStart);
            consumed = true;
        }
    }

    return consumed;
}

void cogs_mikai_scene_add_credit_on_exit(void* context) {
    COGSMyKaiApp* app = context;
    text_input_reset(app->text_input);
    popup_reset(app->popup);
}
