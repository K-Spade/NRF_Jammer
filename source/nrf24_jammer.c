#include <furi.h>
#include <furi_hal.h>
#include <furi_hal_power.h>

#include <expansion/expansion.h>
#include <gui/gui.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/submenu.h>
#include <gui/modules/text_box.h>
#include <gui/modules/text_input.h>
#include <gui/modules/variable_item_list.h>

#include <stdio.h>
#include <string.h>

#define TAG "NRF24Jammer"
#define UART_BAUD 115200U
#define RX_BUFFER_SIZE 2048U
#define LOG_BUFFER_SIZE 4096U
#define RAW_COMMAND_SIZE 64U
#define POWER_BOOT_WAIT_MS 3000U
#define UART_RX_BATCH_MS 50U
#define LOG_REFRESH_MIN_MS 100U
#define LOG_REFRESH_MIN_MS 100U

typedef enum {
    ViewMain,
    ViewPresets,
    ViewSettings,
    ViewDevice,
    ViewLog,
    ViewRawCommand,
} AppView;

typedef enum {
    MainStatus,
    MainStart,
    MainStop,
    MainPresets,
    MainSettings,
    MainScan,
    MainDevice,
    MainRawCommand,
    MainHelp,
} MainMenuIndex;

typedef enum {
    PresetBle,
    PresetBt,
    PresetWifi,
    PresetDrone,
    PresetAll,
} PresetIndex;

typedef enum {
    DeviceAutostartOn,
    DeviceAutostartOff,
    DeviceQuietOn,
    DeviceQuietOff,
    DeviceEchoOn,
    DeviceEchoOff,
    DeviceSave,
    DeviceReset,
    DeviceReboot,
} DeviceIndex;

typedef enum {
    SettingMode,
    SettingPa,
    SettingRate,
    SettingRadios,
    SettingChannel,
    SettingRangeLow,
    SettingRangeHigh,
    SettingDuration,
    SettingApplyRange,
} SettingIndex;

typedef enum {
    WorkerEventReserved = (1U << 0),
    WorkerEventStop = (1U << 1),
    WorkerEventRxData = (1U << 2),
    WorkerEventRxError = (1U << 3),
} WorkerEvent;

typedef enum {
    CustomEventUartData = 100,
    CustomEventUartError,
    CustomEventLogRefresh,
} CustomEvent;

typedef struct {
    Gui* gui;
    Expansion* expansion;
    ViewDispatcher* dispatcher;
    Submenu* main_menu;
    Submenu* preset_menu;
    Submenu* device_menu;
    VariableItemList* settings;
    TextBox* log_box;
    FuriTimer* log_timer;
    TextInput* raw_input;

    FuriHalSerialHandle* serial;
    FuriThread* worker;
    FuriStreamBuffer* rx_stream;
    bool uart_ready;
    bool rx_started;

    AppView current_view;
    AppView log_return_view;
    char log_text[LOG_BUFFER_SIZE];
    size_t log_length;
    bool log_dirty;
    char raw_command[RAW_COMMAND_SIZE];
    VariableItem* mode_item;
    VariableItem* pa_item;
    VariableItem* rate_item;
    VariableItem* radios_item;
    VariableItem* channel_item;
    VariableItem* range_low_item;
    VariableItem* range_high_item;
    VariableItem* duration_item;

    uint8_t rx_log_pending;
    uint8_t power_boot_waits;
    uint8_t range_low;
    uint8_t range_high;
    uint16_t duration_seconds;
} JammerApp;

static const char* const mode_names[] = {"Random", "Sweep", "List", "Fixed"};
static const char* const mode_commands[] = {"random", "sweep", "list", "fixed"};
static const char* const pa_names[] = {"MIN", "LOW", "HIGH", "MAX"};
static const char* const pa_commands[] = {"min", "low", "high", "max"};
static const char* const rate_names[] = {"1 Mbps", "2 Mbps", "250 kbps"};
static const char* const rate_commands[] = {"1m", "2m", "250k"};
static const char* const radio_names[] = {"Both", "HP", "SP"};
static const char* const radio_commands[] = {"both", "hp", "sp"};
static const uint16_t duration_values[] = {0, 10, 30, 60, 120, 300};
static const char* const duration_names[] = {"Unlimited", "10 s", "30 s", "60 s", "120 s", "300 s"};

static void app_raw_input_done(void* context);

static void app_switch_view(JammerApp* app, AppView view) {
    app->current_view = view;
    view_dispatcher_switch_to_view(app->dispatcher, view);
}

// Show the serial log; pressing Back from it goes to `return_view`.
static void app_show_log(JammerApp* app, AppView return_view) {
    app->log_return_view = return_view;
    app_switch_view(app, ViewLog);
}

static void app_refresh_log(JammerApp* app) {
    app->log_dirty = false;
    text_box_set_text(app->log_box, app->log_text);
    text_box_set_focus(app->log_box, TextBoxFocusEnd);
}

static void app_log_timer_callback(void* context) {
    JammerApp* app = context;
    if(!app->log_dirty) return;
    view_dispatcher_send_custom_event(app->dispatcher, CustomEventLogRefresh);
}

static void app_schedule_log_refresh(JammerApp* app) {
    // Coalesce bursts: redraw at most every LOG_REFRESH_MIN_MS so a full
    // status dump cannot flood the GUI thread and stall the UI.
    app->log_dirty = true;
    if(!app->log_timer) {
        app->log_timer = furi_timer_alloc(app_log_timer_callback, FuriTimerTypeOnce, app);
    }
    furi_timer_start(app->log_timer, furi_ms_to_ticks(LOG_REFRESH_MIN_MS));
}

static void app_log_append(JammerApp* app, const char* text) {
    const size_t incoming = strlen(text);
    if((incoming + 1U) >= LOG_BUFFER_SIZE) return;

    if((app->log_length + incoming + 1U) >= LOG_BUFFER_SIZE) {
        const char* marker = "[older log cleared]\n";
        strcpy(app->log_text, marker);
        app->log_length = strlen(marker);
    }

    memcpy(app->log_text + app->log_length, text, incoming);
    app->log_length += incoming;
    app->log_text[app->log_length] = '\0';
    app_refresh_log(app);
}

static void app_log_rx_byte(JammerApp* app, uint8_t byte) {
    if(byte == '\r') return;
    if((byte != '\n') && ((byte < 0x20U) || (byte > 0x7eU))) return;

    if((app->log_length + 2U) >= LOG_BUFFER_SIZE) {
        const char* marker = "[older log cleared]\n";
        strcpy(app->log_text, marker);
        app->log_length = strlen(marker);
    }

    app->log_text[app->log_length++] = (char)byte;
    app->log_text[app->log_length] = '\0';
    app->log_dirty = true;
}

static bool app_send_command(JammerApp* app, const char* command) {
    char display[RAW_COMMAND_SIZE + 8U];
    snprintf(display, sizeof(display), "> %s\n", command);
    app_log_append(app, display);

    if(!app->uart_ready || !app->serial) {
        app_log_append(app, "[!] UART unavailable\n");
        return false;
    }

    if(app->power_boot_waits < 3U && !furi_hal_power_is_otg_enabled()) {
        // The target was just powered up and may not be listening yet.
        app->power_boot_waits++;
        furi_delay_ms(POWER_BOOT_WAIT_MS);
    }

    furi_hal_serial_tx(app->serial, (const uint8_t*)command, strlen(command));
    furi_hal_serial_tx(app->serial, (const uint8_t*)"\n", 1U);
    furi_hal_serial_tx_wait_complete(app->serial);
    return true;
}

static void app_show_command_result(JammerApp* app, const char* command) {
    app_send_command(app, command);
    app_switch_view(app, ViewLog);
}

static void app_uart_irq(
    FuriHalSerialHandle* handle,
    FuriHalSerialRxEvent event,
    void* context) {
    JammerApp* app = context;
    uint32_t flags = 0;

    if(event & FuriHalSerialRxEventData) {
        const uint8_t byte = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(app->rx_stream, &byte, 1U, 0U);
        flags |= WorkerEventRxData;
    }

    if(event & (FuriHalSerialRxEventFrameError | FuriHalSerialRxEventNoiseError |
                FuriHalSerialRxEventOverrunError | FuriHalSerialRxEventParityError)) {
        flags |= WorkerEventRxError;
    }

    if(flags && app->worker) {
        furi_thread_flags_set(furi_thread_get_id(app->worker), flags);
    }
}

static bool app_power_5v_set(bool enabled) {
    if(furi_hal_power_is_otg_enabled() == enabled) return true;

    if(enabled) {
        furi_hal_power_enable_otg();
    } else {
        furi_hal_power_disable_otg();
    }

    return furi_hal_power_is_otg_enabled() == enabled;
}

static int32_t app_uart_worker(void* context) {
    JammerApp* app = context;
    const uint32_t mask = WorkerEventStop | WorkerEventRxData | WorkerEventRxError;

    while(true) {
        const uint32_t events =
            furi_thread_flags_wait(mask, FuriFlagWaitAny, FuriWaitForever);
        if(events & WorkerEventStop) break;
        if(events & WorkerEventRxData) {
            // Throttle: one GUI refresh per burst instead of one per byte.
            // Keeps the UI responsive when a lot of data arrives.
            view_dispatcher_send_custom_event(app->dispatcher, CustomEventUartData);
            furi_delay_ms(UART_RX_BATCH_MS);
        }
        if(events & WorkerEventRxError) {
            view_dispatcher_send_custom_event(app->dispatcher, CustomEventUartError);
        }
    }

    return 0;
}

static bool app_custom_event(void* context, uint32_t event) {
    JammerApp* app = context;

    if(event == CustomEventUartData) {
        uint8_t bytes[128];
        size_t count;
        do {
            count = furi_stream_buffer_receive(app->rx_stream, bytes, sizeof(bytes), 0U);
            for(size_t i = 0; i < count; i++) app_log_rx_byte(app, bytes[i]);
        } while(count > 0U);
        app_schedule_log_refresh(app);
        return true;
    }

    if(event == CustomEventLogRefresh) {
        app_refresh_log(app);
        return true;
    }

    if(event == CustomEventUartError) {
        app_log_append(app, "[!] UART receive error\n");
        return true;
    }

    return false;
}

static bool app_navigation_event(void* context) {
    JammerApp* app = context;
    if(app->current_view == ViewMain) {
        view_dispatcher_stop(app->dispatcher);
    } else if(app->current_view == ViewLog) {
        app_switch_view(app, app->log_return_view);
    } else {
        app_switch_view(app, ViewMain);
    }
    return true;
}


static void app_main_menu_callback(void* context, uint32_t index) {
    JammerApp* app = context;
    char command[32];

    switch(index) {
    case MainStatus:
        app_show_command_result(app, "status");
        break;
    case MainStart:
        if(app->duration_seconds == 0U) {
            app_show_command_result(app, "start");
        } else {
            snprintf(command, sizeof(command), "start %u", app->duration_seconds);
            app_show_command_result(app, command);
        }
        break;
    case MainStop:
        app_show_command_result(app, "stop");
        break;
    case MainPresets:
        app_switch_view(app, ViewPresets);
        break;
    case MainSettings:
        app_switch_view(app, ViewSettings);
        break;
    case MainScan:
        app_show_command_result(app, "scan 5");
        break;
    case MainDevice:
        app_switch_view(app, ViewDevice);
        break;
    case MainRawCommand:
        app->raw_command[0] = '\0';
        text_input_reset(app->raw_input);
        text_input_set_header_text(app->raw_input, "UART command");
        text_input_set_result_callback(
            app->raw_input,
            app_raw_input_done,
            app,
            app->raw_command,
            sizeof(app->raw_command),
            true);
        app_switch_view(app, ViewRawCommand);
        break;
    case MainHelp:
        app_show_command_result(app, "help");
        break;
    default:
        break;
    }
}

static void app_preset_callback(void* context, uint32_t index) {
    JammerApp* app = context;
    static const char* const presets[] = {"ble", "bt", "wifi", "drone", "all"};
    char command[24];

    if(index >= COUNT_OF(presets)) return;
    snprintf(command, sizeof(command), "preset %s", presets[index]);
    app_show_command_result(app, command);
}

static void app_device_callback(void* context, uint32_t index) {
    JammerApp* app = context;
    static const char* const commands[] = {
        "autostart on",
        "autostart off",
        "quiet on",
        "quiet off",
        "echo on",
        "echo off",
        "save",
        "reset",
        "reboot",
    };

    if(index >= COUNT_OF(commands)) return;
    app_send_command(app, commands[index]);
    app_show_log(app, ViewDevice);
}

static void app_mode_changed(VariableItem* item) {
    JammerApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, mode_names[index]);
    char command[24];
    snprintf(command, sizeof(command), "mode %s", mode_commands[index]);
    app_send_command(app, command);
}

static void app_pa_changed(VariableItem* item) {
    JammerApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, pa_names[index]);
    char command[16];
    snprintf(command, sizeof(command), "pa %s", pa_commands[index]);
    app_send_command(app, command);
}

static void app_rate_changed(VariableItem* item) {
    JammerApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, rate_names[index]);
    char command[16];
    snprintf(command, sizeof(command), "rate %s", rate_commands[index]);
    app_send_command(app, command);
}

static void app_radios_changed(VariableItem* item) {
    JammerApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    variable_item_set_current_value_text(item, radio_names[index]);
    char command[20];
    snprintf(command, sizeof(command), "radios %s", radio_commands[index]);
    app_send_command(app, command);
}

static void app_channel_changed(VariableItem* item) {
    JammerApp* app = variable_item_get_context(item);
    const uint8_t channel = variable_item_get_current_value_index(item);
    char value[8];
    char command[20];
    snprintf(value, sizeof(value), "%u", channel);
    variable_item_set_current_value_text(item, value);
    snprintf(command, sizeof(command), "channel %u", channel);
    app_send_command(app, command);
}

static void app_range_low_changed(VariableItem* item) {
    JammerApp* app = variable_item_get_context(item);
    app->range_low = variable_item_get_current_value_index(item);
    char value[8];
    snprintf(value, sizeof(value), "%u", app->range_low);
    variable_item_set_current_value_text(item, value);
}

static void app_range_high_changed(VariableItem* item) {
    JammerApp* app = variable_item_get_context(item);
    app->range_high = variable_item_get_current_value_index(item);
    char value[8];
    snprintf(value, sizeof(value), "%u", app->range_high);
    variable_item_set_current_value_text(item, value);
}

static void app_duration_changed(VariableItem* item) {
    JammerApp* app = variable_item_get_context(item);
    const uint8_t index = variable_item_get_current_value_index(item);
    app->duration_seconds = duration_values[index];
    variable_item_set_current_value_text(item, duration_names[index]);
}

static void app_settings_enter(void* context, uint32_t index) {
    JammerApp* app = context;
    if(index != SettingApplyRange) return;

    if(app->range_low > app->range_high) {
        app_log_append(app, "[!] Range low must be <= high\n");
        app_switch_view(app, ViewLog);
        return;
    }

    char command[24];
    snprintf(command, sizeof(command), "range %u %u", app->range_low, app->range_high);
    app_show_command_result(app, command);
}

static void app_raw_input_done(void* context) {
    JammerApp* app = context;
    if(app->raw_command[0] != '\0') app_send_command(app, app->raw_command);
    app_switch_view(app, ViewLog);
}

static void app_configure_raw_input(JammerApp* app) {
    text_input_set_header_text(app->raw_input, "UART command");
    text_input_set_result_callback(
        app->raw_input,
        app_raw_input_done,
        app,
        app->raw_command,
        sizeof(app->raw_command),
        true);
}

static void app_build_settings(JammerApp* app) {
    char value[8];

    app->mode_item = variable_item_list_add(
        app->settings, "Mode", COUNT_OF(mode_names), app_mode_changed, app);
    variable_item_set_current_value_index(app->mode_item, 0);
    variable_item_set_current_value_text(app->mode_item, mode_names[0]);

    app->pa_item = variable_item_list_add(
        app->settings, "Power", COUNT_OF(pa_names), app_pa_changed, app);
    variable_item_set_current_value_index(app->pa_item, 3);
    variable_item_set_current_value_text(app->pa_item, pa_names[3]);

    app->rate_item = variable_item_list_add(
        app->settings, "Rate", COUNT_OF(rate_names), app_rate_changed, app);
    variable_item_set_current_value_index(app->rate_item, 1);
    variable_item_set_current_value_text(app->rate_item, rate_names[1]);

    app->radios_item = variable_item_list_add(
        app->settings, "Radios", COUNT_OF(radio_names), app_radios_changed, app);
    variable_item_set_current_value_index(app->radios_item, 0);
    variable_item_set_current_value_text(app->radios_item, radio_names[0]);

    app->channel_item = variable_item_list_add(
        app->settings, "Fixed channel", 126, app_channel_changed, app);
    variable_item_set_current_value_index(app->channel_item, 37);
    variable_item_set_current_value_text(app->channel_item, "37");

    app->range_low_item = variable_item_list_add(
        app->settings, "Range low", 126, app_range_low_changed, app);
    variable_item_set_current_value_index(app->range_low_item, app->range_low);
    snprintf(value, sizeof(value), "%u", app->range_low);
    variable_item_set_current_value_text(app->range_low_item, value);

    app->range_high_item = variable_item_list_add(
        app->settings, "Range high", 126, app_range_high_changed, app);
    variable_item_set_current_value_index(app->range_high_item, app->range_high);
    snprintf(value, sizeof(value), "%u", app->range_high);
    variable_item_set_current_value_text(app->range_high_item, value);

    app->duration_item = variable_item_list_add(
        app->settings,
        "Start duration",
        COUNT_OF(duration_values),
        app_duration_changed,
        app);
    variable_item_set_current_value_index(app->duration_item, 0);
    variable_item_set_current_value_text(app->duration_item, duration_names[0]);

    VariableItem* apply =
        variable_item_list_add(app->settings, "Apply range", 1, NULL, app);
    variable_item_set_current_value_text(apply, "Send");
    variable_item_list_set_enter_callback(app->settings, app_settings_enter, app);
}

static void app_build_menus(JammerApp* app) {
    submenu_set_header(app->main_menu, "NRF24 JAMMER");
    submenu_add_item(app->main_menu, "Status", MainStatus, app_main_menu_callback, app);
    submenu_add_item(app->main_menu, "START", MainStart, app_main_menu_callback, app);
    submenu_add_item(app->main_menu, "STOP", MainStop, app_main_menu_callback, app);
    submenu_add_item(app->main_menu, "Presets", MainPresets, app_main_menu_callback, app);
    submenu_add_item(app->main_menu, "Radio settings", MainSettings, app_main_menu_callback, app);
    submenu_add_item(app->main_menu, "Scan (5 passes)", MainScan, app_main_menu_callback, app);
    submenu_add_item(app->main_menu, "Device settings", MainDevice, app_main_menu_callback, app);
    submenu_add_item(app->main_menu, "Raw command", MainRawCommand, app_main_menu_callback, app);
    submenu_add_item(app->main_menu, "Help", MainHelp, app_main_menu_callback, app);

    submenu_set_header(app->preset_menu, "Channel preset");
    submenu_add_item(app->preset_menu, "BLE", PresetBle, app_preset_callback, app);
    submenu_add_item(app->preset_menu, "Bluetooth Classic", PresetBt, app_preset_callback, app);
    submenu_add_item(app->preset_menu, "WiFi", PresetWifi, app_preset_callback, app);
    submenu_add_item(app->preset_menu, "Drone", PresetDrone, app_preset_callback, app);
    submenu_add_item(app->preset_menu, "All 2.4 GHz", PresetAll, app_preset_callback, app);

    submenu_set_header(app->device_menu, "Device commands");
    submenu_add_item(app->device_menu, "Autostart ON", DeviceAutostartOn, app_device_callback, app);
    submenu_add_item(app->device_menu, "Autostart OFF", DeviceAutostartOff, app_device_callback, app);
    submenu_add_item(app->device_menu, "Quiet ON", DeviceQuietOn, app_device_callback, app);
    submenu_add_item(app->device_menu, "Quiet OFF", DeviceQuietOff, app_device_callback, app);
    submenu_add_item(app->device_menu, "Echo ON", DeviceEchoOn, app_device_callback, app);
    submenu_add_item(app->device_menu, "Echo OFF", DeviceEchoOff, app_device_callback, app);
    submenu_add_item(app->device_menu, "Save settings", DeviceSave, app_device_callback, app);
    submenu_add_item(app->device_menu, "Reset settings", DeviceReset, app_device_callback, app);
    submenu_add_item(app->device_menu, "Reboot ESP32", DeviceReboot, app_device_callback, app);
}

static JammerApp* app_alloc(void) {
    JammerApp* app = malloc(sizeof(JammerApp));
    memset(app, 0, sizeof(JammerApp));
    app->range_low = 0;
    app->range_high = 125;

    app->gui = furi_record_open(RECORD_GUI);
    app->expansion = furi_record_open(RECORD_EXPANSION);
    expansion_disable(app->expansion);

    app->dispatcher = view_dispatcher_alloc();
    view_dispatcher_set_event_callback_context(app->dispatcher, app);
    view_dispatcher_set_custom_event_callback(app->dispatcher, app_custom_event);
    view_dispatcher_set_navigation_event_callback(app->dispatcher, app_navigation_event);
    view_dispatcher_attach_to_gui(app->dispatcher, app->gui, ViewDispatcherTypeFullscreen);

    app->main_menu = submenu_alloc();
    app->preset_menu = submenu_alloc();
    app->device_menu = submenu_alloc();
    app->settings = variable_item_list_alloc();
    app->log_box = text_box_alloc();
    app->raw_input = text_input_alloc();

    text_box_set_font(app->log_box, TextBoxFontText);
    text_box_set_focus(app->log_box, TextBoxFocusEnd);

    app_build_menus(app);
    app_build_settings(app);
    app_configure_raw_input(app);

    view_dispatcher_add_view(app->dispatcher, ViewMain, submenu_get_view(app->main_menu));
    view_dispatcher_add_view(app->dispatcher, ViewPresets, submenu_get_view(app->preset_menu));
    view_dispatcher_add_view(
        app->dispatcher, ViewSettings, variable_item_list_get_view(app->settings));
    view_dispatcher_add_view(app->dispatcher, ViewDevice, submenu_get_view(app->device_menu));
    view_dispatcher_add_view(app->dispatcher, ViewLog, text_box_get_view(app->log_box));
    view_dispatcher_add_view(
        app->dispatcher, ViewRawCommand, text_input_get_view(app->raw_input));

    strcpy(app->log_text, "NRF24 JAMMER\n115200 8N1\n");
    app->log_length = strlen(app->log_text);
    app_refresh_log(app);

    app->rx_stream = furi_stream_buffer_alloc(RX_BUFFER_SIZE, 1U);

    // Power the NRF24 jammer from the Flipper's 5V pin by default, so the
    // ESP32 and both nRF24 PA/LNA modules are supplied as soon as the app
    // starts. There is no UI switch for this on purpose.
    if(app_power_5v_set(true)) {
        app_log_append(app, "[+] 5V on GPIO enabled (boot default)\n");
    } else {
        app_log_append(app, "[!] 5V on GPIO failed to enable\n");
    }

    app->serial = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    if(app->serial) {
        // 2 KB so a full status dump fits in one go.
        app->worker = furi_thread_alloc_ex("Nrf24JammerRx", 2048, app_uart_worker, app);
        furi_thread_start(app->worker);

        // Give the ESP32 and the nRF24 modules time to finish booting before
        // the first command goes out; a cold boot otherwise drops it.
        furi_delay_ms(POWER_BOOT_WAIT_MS);

        furi_hal_serial_init(app->serial, UART_BAUD);
        furi_hal_serial_configure_framing(
            app->serial,
            FuriHalSerialDataBits8,
            FuriHalSerialParityNone,
            FuriHalSerialStopBits1);
        furi_hal_serial_async_rx_start(app->serial, app_uart_irq, app, true);
        app->rx_started = true;
        app->uart_ready = true;
        app_log_append(app, "[+] UART ready: pins 13/14\n");
    } else {
        app_log_append(app, "[!] UART busy or unavailable\n");
    }

    return app;
}

static void app_free(JammerApp* app) {
    if(app->uart_ready && app->serial) {
        app_send_command(app, "stop");
        if(app->rx_started) {
            furi_hal_serial_async_rx_stop(app->serial);
            app->rx_started = false;
        }
    }

    if(app->worker) {
        furi_thread_flags_set(furi_thread_get_id(app->worker), WorkerEventStop);
        furi_thread_join(app->worker);
        furi_thread_free(app->worker);
    }

    if(app->serial) {
        furi_hal_serial_deinit(app->serial);
        furi_hal_serial_control_release(app->serial);
    }

    if(furi_hal_power_is_otg_enabled()) {
        furi_hal_power_disable_otg();
    }
    expansion_enable(app->expansion);

    view_dispatcher_remove_view(app->dispatcher, ViewMain);
    view_dispatcher_remove_view(app->dispatcher, ViewPresets);
    view_dispatcher_remove_view(app->dispatcher, ViewSettings);
    view_dispatcher_remove_view(app->dispatcher, ViewDevice);
    view_dispatcher_remove_view(app->dispatcher, ViewLog);
    view_dispatcher_remove_view(app->dispatcher, ViewRawCommand);

    submenu_free(app->main_menu);
    submenu_free(app->preset_menu);
    submenu_free(app->device_menu);
    variable_item_list_free(app->settings);
    if(app->log_timer) {
        furi_timer_stop(app->log_timer);
        furi_timer_free(app->log_timer);
    }
    text_box_free(app->log_box);
    text_input_free(app->raw_input);
    view_dispatcher_free(app->dispatcher);

    furi_stream_buffer_free(app->rx_stream);
    furi_record_close(RECORD_EXPANSION);
    furi_record_close(RECORD_GUI);
    free(app);
}

int32_t nrf24_jammer_app(void* context) {
    UNUSED(context);
    JammerApp* app = app_alloc();
    app->log_return_view = ViewMain;
    app_switch_view(app, ViewMain);
    view_dispatcher_run(app->dispatcher);
    app_free(app);
    return 0;
}
