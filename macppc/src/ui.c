#include "ui.h"

#include <Carbon/Carbon.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "dataset.h"
#include "levelmeter.h"
#include "listview.h"
#include "paths.h"
#include "player.h"

/* Same content area as the other three ports. */
#define WINDOW_WIDTH   760
#define WINDOW_HEIGHT  560
#define MIN_WIDTH      520
#define MIN_HEIGHT     340

#define MARGIN          10.0f
#define GAP              8.0f
#define SEARCH_HEIGHT   22.0f
#define STATUS_HEIGHT   28.0f
#define BUTTON_WIDTH    62.0f
#define BUTTON_HEIGHT   20.0f
#define DIVIDER_WIDTH    7.0f

#define DEFAULT_LEFT_WIDTH 220.0f
#define MIN_LEFT_WIDTH     140.0f
#define MAX_LEFT_WIDTH     400.0f

/* How often the level meter is refreshed while playing. The Haiku build polled
 * CurrentLevel() from a 100ms BMessageRunner - same rate. */
#define LEVEL_POLL_MS 100

/* A worker thread queues one of these to wake the main event loop when a player
 * event is waiting, so status changes appear immediately rather than at the next
 * timer tick. PostEventToQueue is documented thread-safe; nothing else here is
 * called off the main thread. */
#define kEventClassRadio      FOUR_CHAR_CODE('RwRr')
#define kEventRadioPlayerWoke 1
#define kRadioCommandPlay     FOUR_CHAR_CODE('play')
#define kRadioCommandStop     FOUR_CHAR_CODE('stop')

typedef enum {
    PLAYBACK_IDLE,
    PLAYBACK_CONNECTING,
    PLAYBACK_PLAYING,
    PLAYBACK_FAILED
} PlaybackState;

typedef struct {
    struct RadioUI *ui;
    int             is_country;
} SearchContext;

struct RadioUI {
    WindowRef window;
    HIViewRef content;

    Dataset *dataset;
    Player  *player;

    /* Country side. The CFStringRefs are created once at load and handed to the
     * list view as borrowed references - see listview.h on why they are cached
     * rather than created per draw. */
    CFStringRef *country_names;
    int         *country_filtered;
    int          country_filtered_count;
    int          selected_country;   /* dataset index, -1 for none */

    /* Station side, for the selected country only. */
    CFStringRef *station_names;
    int          station_count;
    int         *station_filtered;
    int          station_filtered_count;
    int          selected_station;   /* dataset index within the country, -1 for none */

    ListView   *country_list;
    ListView   *station_list;
    HIViewRef   country_scroll;
    HIViewRef   station_scroll;
    ControlRef  country_search;
    ControlRef  station_search;
    SearchContext country_context;
    SearchContext station_context;
    ControlRef  divider;

    ControlRef  stop_button;
    ControlRef  play_button;
    ControlRef  now_playing;
    ControlRef  status_text;
    LevelMeter *meter;
    HIViewRef   meter_view;

    float left_width;
    int   dragging_divider;

    PlaybackState playback;
    char          current_codec_label[128];
    char          status_detail[2048];

    EventLoopTimerRef timer;
    int               timer_running;
};

/* --------------------------------------------------------------- utilities - */

static void set_static_text(ControlRef control, const char *text)
{
    CFStringRef value = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);

    if (!value)
        return;
    SetControlData(control, kControlEntireControl, kControlStaticTextCFStringTag,
                   sizeof(value), &value);
    CFRelease(value);
    HIViewSetNeedsDisplay(control, true);
}

/* The search field's current contents, as a retained CFString (never NULL).
 *
 * HIViewCopyText, not HISearchFieldCopyText: 10.4's search field has only
 * Create / {Get,Set}SearchMenu / {Copy,Set}DescriptiveText / {Get,Change}-
 * Attributes. Reading its text goes through the generic HIView accessor. */
static CFStringRef copy_search_text(ControlRef field)
{
    CFStringRef text = HIViewCopyText(field);

    if (!text)
        return CFStringCreateCopy(NULL, CFSTR(""));
    return text; /* HIViewCopyText returns a +1 reference */
}

/* The Rust ports show extra detail (the dataset path, the full error) on hover.
 * A Carbon help tag is the same affordance. */
static void set_help_tag(ControlRef control, const char *text)
{
    HMHelpContentRec help;
    CFStringRef value;

    if (!control)
        return;
    value = CFStringCreateWithCString(NULL, text, kCFStringEncodingUTF8);
    if (!value)
        return;

    memset(&help, 0, sizeof(help));
    help.version = kMacHelpVersion;
    help.tagSide = kHMDefaultSide;
    help.content[kHMMinimumContentIndex].contentType = kHMCFStringContent;
    help.content[kHMMinimumContentIndex].u.tagCFString = value;
    help.content[kHMMaximumContentIndex].contentType = kHMNoContent;
    HMSetControlHelpContent(control, &help);
    CFRelease(value);
}

static void release_string_array(CFStringRef *array, int count)
{
    int index;

    if (!array)
        return;
    for (index = 0; index < count; index++) {
        if (array[index])
            CFRelease(array[index]);
    }
    free(array);
}

static void update_transport(RadioUI *ui);

/* --------------------------------------------------------------- filtering - */

/* Rebuilds the visible-index array for a list. This is the whole cost of a
 * keystroke: an array of ints and one CFStringFind per row. The Rust port does
 * the same thing every frame; here it happens once per keystroke. */
static int rebuild_filter(CFStringRef *names, int count, CFStringRef needle,
                          int *out, int *out_count)
{
    int index, kept = 0;
    int filtering = CFStringGetLength(needle) > 0;

    for (index = 0; index < count; index++) {
        if (!names[index])
            continue;
        if (!filtering ||
            CFStringFind(names[index], needle, kCFCompareCaseInsensitive).location !=
                kCFNotFound)
            out[kept++] = index;
    }
    *out_count = kept;
    return kept;
}

/* Re-points a list's selection after the filter changed.
 *
 * The list view's selection is a row in the *filtered* list, but the app tracks
 * the selected country and station as dataset indices - the way the Rust port
 * does. Without this, typing in a search box silently moves the selection to
 * whatever item landed on the old row number. An item filtered out of view
 * deselects, which is what firing the delegate with -1 does. */
static void remap_selection(ListView *list, const int *filtered, int count, int wanted)
{
    int row;

    if (wanted < 0) {
        listview_set_selection(list, -1, 0);
        return;
    }
    for (row = 0; row < count; row++) {
        if (filtered[row] == wanted) {
            listview_set_selection(list, row, 1);
            return;
        }
    }
    listview_set_selection(list, -1, 0);
}

static void refilter_countries(RadioUI *ui)
{
    CFStringRef needle = copy_search_text(ui->country_search);
    int wanted = ui->selected_country;

    rebuild_filter(ui->country_names, dataset_country_count(ui->dataset), needle,
                   ui->country_filtered, &ui->country_filtered_count);
    CFRelease(needle);
    listview_reload(ui->country_list);
    remap_selection(ui->country_list, ui->country_filtered,
                    ui->country_filtered_count, wanted);
}

static void refilter_stations(RadioUI *ui)
{
    CFStringRef needle;

    if (!ui->station_names) {
        ui->station_filtered_count = 0;
        listview_reload(ui->station_list);
        return;
    }
    needle = copy_search_text(ui->station_search);
    {
        int wanted = ui->selected_station;
        rebuild_filter(ui->station_names, ui->station_count, needle,
                       ui->station_filtered, &ui->station_filtered_count);
        CFRelease(needle);
        listview_reload(ui->station_list);
        remap_selection(ui->station_list, ui->station_filtered,
                        ui->station_filtered_count, wanted);
    }
    update_transport(ui);
}

static void clear_search_field(ControlRef field)
{
    HIViewSetText(field, CFSTR(""));
    HIViewSetNeedsDisplay(field, true);
}

/* Loads the selected country's station names. Called on every country change, so
 * it is sized for the worst case in the dataset - the United States, with 6,557
 * stations - which is one malloc and 6,557 CFStringCreate calls, a few
 * milliseconds on a G4. */
static void load_stations_for_country(RadioUI *ui, int country_index)
{
    int count, index;

    release_string_array(ui->station_names, ui->station_count);
    ui->station_names = NULL;
    free(ui->station_filtered);
    ui->station_filtered = NULL;
    ui->station_count = 0;
    ui->station_filtered_count = 0;
    ui->selected_station = -1;

    if (country_index < 0) {
        listview_reload(ui->station_list);
        return;
    }

    count = dataset_station_count(ui->dataset, country_index);
    if (count <= 0) {
        listview_reload(ui->station_list);
        return;
    }

    ui->station_names = (CFStringRef *)calloc((size_t)count, sizeof(CFStringRef));
    ui->station_filtered = (int *)calloc((size_t)count, sizeof(int));
    if (!ui->station_names || !ui->station_filtered) {
        free(ui->station_names);
        free(ui->station_filtered);
        ui->station_names = NULL;
        ui->station_filtered = NULL;
        listview_reload(ui->station_list);
        return;
    }
    ui->station_count = count;

    for (index = 0; index < count; index++) {
        Station station;
        if (dataset_station(ui->dataset, country_index, index, &station) != 0)
            continue;
        ui->station_names[index] =
            CFStringCreateWithCString(NULL, station.name, kCFStringEncodingUTF8);
    }

    clear_search_field(ui->station_search);
    refilter_stations(ui);
}

/* ------------------------------------------------------------ status line - */

static void set_status(RadioUI *ui, const char *text)
{
    set_static_text(ui->status_text, text);
}

static void update_now_playing(RadioUI *ui, const char *text)
{
    char line[512];

    if (ui->playback == PLAYBACK_PLAYING && ui->current_codec_label[0])
        snprintf(line, sizeof(line), "%s  (%s)", text, ui->current_codec_label);
    else
        snprintf(line, sizeof(line), "%s", text);
    set_static_text(ui->now_playing, line);
}

/* The stop button only exists while something is playing, matching the
 * original's hidden-unless-playing stop button; the play button is enabled only
 * when a station is selected. */
static void update_transport(RadioUI *ui)
{
    int playing = (ui->playback == PLAYBACK_PLAYING);

    HIViewSetVisible(ui->stop_button, playing ? true : false);
    if (ui->selected_station >= 0)
        EnableControl(ui->play_button);
    else
        DisableControl(ui->play_button);
}

/* ---------------------------------------------------------------- playback - */

static int selected_station_record(RadioUI *ui, Station *out)
{
    if (ui->selected_country < 0 || ui->selected_station < 0)
        return 0;
    return dataset_station(ui->dataset, ui->selected_country, ui->selected_station,
                           out) == 0;
}

static void start_timer(RadioUI *ui);
static void stop_timer(RadioUI *ui);

static void play_selected(RadioUI *ui)
{
    Station station;

    if (!selected_station_record(ui, &station))
        return;
    station_codec_bitrate_label(&station, ui->current_codec_label,
                                sizeof(ui->current_codec_label));
    player_play(ui->player, &station);
    start_timer(ui);
}

static void drain_player_events(RadioUI *ui)
{
    PlayerEvent event;

    while (player_poll_event(ui->player, &event)) {
        switch (event.kind) {
        case RWR_EVENT_CONNECTING:
            ui->playback = PLAYBACK_CONNECTING;
            update_now_playing(ui, event.station);
            set_status(ui, "Connecting...");
            break;
        case RWR_EVENT_PLAYING:
            ui->playback = PLAYBACK_PLAYING;
            update_now_playing(ui, event.station);
            set_status(ui, "");
            break;
        case RWR_EVENT_STOPPED:
            ui->playback = PLAYBACK_IDLE;
            ui->current_codec_label[0] = '\0';
            update_now_playing(ui, "");
            set_status(ui, "");
            levelmeter_set_level(ui->meter, 0.0f);
            break;
        case RWR_EVENT_ERROR:
            ui->playback = PLAYBACK_FAILED;
            ui->current_codec_label[0] = '\0';
            update_now_playing(ui, event.station);
            set_status(ui, event.detail);
            snprintf(ui->status_detail, sizeof(ui->status_detail), "%s", event.detail);
            set_help_tag(ui->status_text, ui->status_detail);
            levelmeter_set_level(ui->meter, 0.0f);
            break;
        }
        update_transport(ui);
    }

    if (ui->playback != PLAYBACK_PLAYING && ui->playback != PLAYBACK_CONNECTING)
        stop_timer(ui);
}

/* ------------------------------------------------------------------ timer - */

static pascal void timer_fired(EventLoopTimerRef timer, void *context)
{
    RadioUI *ui = (RadioUI *)context;

    (void)timer;
    /* The level meter is the only thing that changes without user input, and it
     * repaints only when a segment actually lights or goes dark. */
    levelmeter_set_level(ui->meter, player_level(ui->player));
    drain_player_events(ui);
}

static void start_timer(RadioUI *ui)
{
    static EventLoopTimerUPP upp = NULL;

    if (ui->timer_running)
        return;
    if (!upp)
        upp = NewEventLoopTimerUPP(timer_fired);
    if (InstallEventLoopTimer(GetCurrentEventLoop(), 0,
                              kEventDurationMillisecond * LEVEL_POLL_MS,
                              upp, ui, &ui->timer) == noErr)
        ui->timer_running = 1;
}

static void stop_timer(RadioUI *ui)
{
    if (!ui->timer_running)
        return;
    RemoveEventLoopTimer(ui->timer);
    ui->timer = NULL;
    ui->timer_running = 0;
    levelmeter_set_level(ui->meter, 0.0f);
}

/* Called from a player worker thread. */
static void wake_from_worker(void *context)
{
    EventRef event = NULL;

    (void)context;
    if (CreateEvent(NULL, kEventClassRadio, kEventRadioPlayerWoke,
                    0, kEventAttributeNone, &event) != noErr)
        return;
    PostEventToQueue(GetMainEventQueue(), event, kEventPriorityStandard);
    ReleaseEvent(event);
}

/* --------------------------------------------------------- list delegates - */

static int country_row_count(void *context)
{
    return ((RadioUI *)context)->country_filtered_count;
}

static CFStringRef country_row_text(void *context, int row)
{
    RadioUI *ui = (RadioUI *)context;

    if (row < 0 || row >= ui->country_filtered_count)
        return NULL;
    return ui->country_names[ui->country_filtered[row]];
}

static void country_selection_changed(void *context, int row)
{
    RadioUI *ui = (RadioUI *)context;
    int country = (row >= 0 && row < ui->country_filtered_count)
                      ? ui->country_filtered[row]
                      : -1;

    if (country == ui->selected_country)
        return;
    ui->selected_country = country;
    listview_set_selection(ui->station_list, -1, 0);
    load_stations_for_country(ui, country);
    update_transport(ui);
}

static int station_row_count(void *context)
{
    return ((RadioUI *)context)->station_filtered_count;
}

static CFStringRef station_row_text(void *context, int row)
{
    RadioUI *ui = (RadioUI *)context;

    if (row < 0 || row >= ui->station_filtered_count)
        return NULL;
    return ui->station_names[ui->station_filtered[row]];
}

static void station_selection_changed(void *context, int row)
{
    RadioUI *ui = (RadioUI *)context;

    ui->selected_station = (row >= 0 && row < ui->station_filtered_count)
                               ? ui->station_filtered[row]
                               : -1;
    update_transport(ui);
}

static void station_activated(void *context, int row)
{
    RadioUI *ui = (RadioUI *)context;

    station_selection_changed(context, row);
    play_selected(ui);
}

/* ----------------------------------------------------------------- layout - */

static void layout(RadioUI *ui)
{
    HIRect bounds;
    float left, right, top, width, height;
    float list_top, list_bottom;
    HIRect frame;

    HIViewGetBounds(ui->content, &bounds);
    width = bounds.size.width;
    height = bounds.size.height;

    if (ui->left_width < MIN_LEFT_WIDTH)
        ui->left_width = MIN_LEFT_WIDTH;
    if (ui->left_width > MAX_LEFT_WIDTH)
        ui->left_width = MAX_LEFT_WIDTH;
    if (ui->left_width > width - MARGIN * 2 - DIVIDER_WIDTH - MIN_LEFT_WIDTH)
        ui->left_width = width - MARGIN * 2 - DIVIDER_WIDTH - MIN_LEFT_WIDTH;
    if (ui->left_width < MIN_LEFT_WIDTH)
        ui->left_width = MIN_LEFT_WIDTH;

    top = MARGIN;
    left = MARGIN;
    right = width - MARGIN;
    list_top = top + SEARCH_HEIGHT + GAP;
    list_bottom = height - MARGIN - STATUS_HEIGHT - GAP;

    /* Both search fields sit on one line. In the Rust port this needed a shared
     * panel frame because egui's side and central panels have different default
     * margins; here it is just the same y. */
    frame = CGRectMake(left, top, ui->left_width, SEARCH_HEIGHT);
    HIViewSetFrame(ui->country_search, &frame);
    frame = CGRectMake(left, list_top, ui->left_width, list_bottom - list_top);
    HIViewSetFrame(ui->country_scroll, &frame);

    frame = CGRectMake(left + ui->left_width, top, DIVIDER_WIDTH,
                       list_bottom - top);
    HIViewSetFrame(ui->divider, &frame);

    {
        float station_left = left + ui->left_width + DIVIDER_WIDTH;
        float station_width = right - station_left;
        if (station_width < 40.0f)
            station_width = 40.0f;
        frame = CGRectMake(station_left, top, station_width, SEARCH_HEIGHT);
        HIViewSetFrame(ui->station_search, &frame);
        frame = CGRectMake(station_left, list_top, station_width, list_bottom - list_top);
        HIViewSetFrame(ui->station_scroll, &frame);
    }

    /* Status row: stop, play, now-playing, level meter, and a right-aligned
     * status message that gives way rather than drawing over the meter. */
    {
        float row_y = height - MARGIN - STATUS_HEIGHT;
        float centered = row_y + (STATUS_HEIGHT - BUTTON_HEIGHT) / 2.0f;
        float cursor = left;

        frame = CGRectMake(cursor, centered, BUTTON_WIDTH, BUTTON_HEIGHT);
        HIViewSetFrame(ui->stop_button, &frame);
        cursor += BUTTON_WIDTH + GAP;

        frame = CGRectMake(cursor, centered, BUTTON_WIDTH, BUTTON_HEIGHT);
        HIViewSetFrame(ui->play_button, &frame);
        cursor += BUTTON_WIDTH + GAP;

        {
            float meter_x = right - LEVELMETER_WIDTH;
            float status_width = 200.0f;
            float now_width;

            frame = CGRectMake(meter_x, row_y + (STATUS_HEIGHT - LEVELMETER_HEIGHT) / 2.0f,
                               LEVELMETER_WIDTH, LEVELMETER_HEIGHT);
            HIViewSetFrame(ui->meter_view, &frame);

            if (status_width > (meter_x - GAP - cursor) / 2.0f)
                status_width = (meter_x - GAP - cursor) / 2.0f;
            if (status_width < 0.0f)
                status_width = 0.0f;

            now_width = meter_x - GAP - cursor - status_width - GAP;
            if (now_width < 0.0f)
                now_width = 0.0f;

            frame = CGRectMake(cursor, centered, now_width, BUTTON_HEIGHT);
            HIViewSetFrame(ui->now_playing, &frame);

            frame = CGRectMake(meter_x - GAP - status_width, centered,
                               status_width, BUTTON_HEIGHT);
            HIViewSetFrame(ui->status_text, &frame);
        }
    }

    HIViewSetNeedsDisplay(ui->content, true);
}

/* ---------------------------------------------------------------- divider - */

/* A thin draggable splitter. Carbon has no split-view control on Tiger, and the
 * Rust port's panel is resizable, so this is 40 lines to keep the two ports
 * behaving the same. */
static OSStatus divider_handler(EventHandlerCallRef next, EventRef event, void *context)
{
    RadioUI *ui = (RadioUI *)context;
    UInt32 kind = GetEventKind(event);

    if (GetEventClass(event) != kEventClassControl)
        return CallNextEventHandler(next, event);

    switch (kind) {
    case kEventControlDraw: {
        CGContextRef graphics = NULL;
        HIRect bounds;
        if (GetEventParameter(event, kEventParamCGContextRef, typeCGContextRef, NULL,
                              sizeof(graphics), NULL, &graphics) != noErr)
            return eventNotHandledErr;
        HIViewGetBounds(ui->divider, &bounds);
        HIThemeSetFill(kThemeBrushDialogBackgroundActive, NULL, graphics,
                       kHIThemeOrientationNormal);
        CGContextFillRect(graphics, bounds);
        /* One hairline down the middle, the way a Finder column divider reads. */
        CGContextSetGrayFillColor(graphics, 0.65f, 1.0f);
        CGContextFillRect(graphics,
                          CGRectMake(bounds.origin.x + bounds.size.width / 2.0f - 0.5f,
                                     bounds.origin.y, 1.0f, bounds.size.height));
        return noErr;
    }
    case kEventControlHitTest: {
        ControlPartCode part = kControlIndicatorPart;
        SetEventParameter(event, kEventParamControlPart, typeControlPartCode,
                          sizeof(part), &part);
        return noErr;
    }
    case kEventControlClick: {
        MouseTrackingResult result = kMouseTrackingMouseDown;
        Point where;
        HIRect content_bounds;

        HIViewGetBounds(ui->content, &content_bounds);
        while (result != kMouseTrackingMouseUp) {
            if (TrackMouseLocation(NULL, &where, &result) != noErr)
                break;
            if (result == kMouseTrackingMouseDragged ||
                result == kMouseTrackingMouseMoved) {
                HIPoint point;
                point.x = (float)where.h;
                point.y = (float)where.v;
                HIPointConvert(&point, kHICoordSpaceWindow, ui->window,
                               kHICoordSpaceView, ui->content);
                ui->left_width = point.x - MARGIN - DIVIDER_WIDTH / 2.0f;
                layout(ui);
            }
        }
        return noErr;
    }
    }
    return CallNextEventHandler(next, event);
}

/* ----------------------------------------------------------- window events - */

static OSStatus search_handler(EventHandlerCallRef next, EventRef event, void *context)
{
    SearchContext *which = (SearchContext *)context;
    OSStatus status;

    /* Let the field consume the keystroke first, then read what it now holds -
     * reading before would filter on the text as it was one character ago. */
    status = CallNextEventHandler(next, event);

    if (which->is_country)
        refilter_countries(which->ui);
    else
        refilter_stations(which->ui);
    return status;
}

static OSStatus command_handler(EventHandlerCallRef next, EventRef event, void *context)
{
    RadioUI *ui = (RadioUI *)context;
    HICommand command;

    if (GetEventParameter(event, kEventParamDirectObject, typeHICommand, NULL,
                          sizeof(command), NULL, &command) != noErr)
        return eventNotHandledErr;

    if (command.commandID == kRadioCommandPlay) {
        play_selected(ui);
        return noErr;
    }
    if (command.commandID == kRadioCommandStop) {
        player_stop(ui->player);
        drain_player_events(ui);
        return noErr;
    }
    return CallNextEventHandler(next, event);
}

static OSStatus window_handler(EventHandlerCallRef next, EventRef event, void *context)
{
    RadioUI *ui = (RadioUI *)context;

    switch (GetEventKind(event)) {
    case kEventWindowBoundsChanged:
        layout(ui);
        return eventNotHandledErr; /* the standard handler still wants this */
    case kEventWindowClose:
        /* One window, so closing it is quitting - the same thing Cmd+W does in
         * the Rust port. */
        QuitApplicationEventLoop();
        return noErr;
    case kEventWindowActivated:
    case kEventWindowDeactivated:
        /* The selection highlight changes colour with activation. */
        HIViewSetNeedsDisplay(ui->content, true);
        return eventNotHandledErr;
    }
    return CallNextEventHandler(next, event);
}

static OSStatus wake_handler(EventHandlerCallRef next, EventRef event, void *context)
{
    (void)next;
    (void)event;
    drain_player_events((RadioUI *)context);
    return noErr;
}

/* ------------------------------------------------------------ construction - */

/* HISearchField, not EditUnicodeText: it is the native rounded search box with
 * the magnifier and the cancel button, and its descriptiveText argument is
 * exactly the Rust port's hint_text (10.4 has no placeholder tag for a plain
 * edit control). */
static ControlRef make_search_field(SearchContext *context, HIViewRef parent,
                                    const char *hint)
{
    ControlRef field = NULL;
    HIRect zero = { { 0, 0 }, { 0, 0 } };
    CFStringRef placeholder;
    static const EventTypeSpec field_events[] = {
        { kEventClassTextInput,   kEventTextInputUnicodeForKeyEvent },
        { kEventClassSearchField, kEventSearchFieldCancelClicked }
    };

    placeholder = CFStringCreateWithCString(NULL, hint, kCFStringEncodingUTF8);
    if (HISearchFieldCreate(&zero, kHISearchFieldAttributesCancel |
                                       kHISearchFieldAttributesSearchIcon,
                            NULL, placeholder, &field) != noErr) {
        if (placeholder)
            CFRelease(placeholder);
        return NULL;
    }
    if (placeholder)
        CFRelease(placeholder);

    /* The content view, not the root view. The root view spans the structure
     * region, so a control parented to it is drawn a title bar's height above
     * where layout() puts it - straight over the title bar. */
    HIViewAddSubview(parent, field);
    HIViewSetVisible(field, true);

    InstallEventHandler(GetControlEventTarget(field), search_handler,
                        GetEventTypeCount(field_events), field_events, context, NULL);
    return field;
}

static ControlRef make_button(WindowRef window, HIViewRef parent,
                              const char *title, UInt32 command)
{
    ControlRef button = NULL;
    Rect zero = { 0, 0, 0, 0 };
    CFStringRef text = CFStringCreateWithCString(NULL, title, kCFStringEncodingUTF8);

    if (!text)
        return NULL;
    /* Plain text buttons rather than the Rust port's Unicode play/stop glyphs:
     * Tiger's system fonts do not reliably carry U+25B6, and a substituted glyph
     * on the one control that has to be unambiguous is not worth the saving. */
    if (CreatePushButtonControl(window, &zero, text, &button) != noErr) {
        CFRelease(text);
        return NULL;
    }
    CFRelease(text);
    /* CreatePushButtonControl embeds into the root view - move it into the
     * content view, for the same reason as the search fields above. */
    HIViewAddSubview(parent, button);
    SetControlCommandID(button, command);
    return button;
}

static ControlRef make_static_text(WindowRef window, HIViewRef parent)
{
    ControlRef control = NULL;
    Rect zero = { 0, 0, 0, 0 };
    ControlFontStyleRec style;

    memset(&style, 0, sizeof(style));
    style.flags = kControlUseFontMask;
    style.font = kControlFontSmallSystemFont;

    if (CreateStaticTextControl(window, &zero, CFSTR(""), &style, &control) != noErr)
        return NULL;
    HIViewAddSubview(parent, control);
    return control;
}

/* Must not call CallNextEventHandler - see the note in listview.c. */
static OSStatus divider_construct(EventHandlerCallRef next, EventRef event, void *context)
{
    (void)next;
    (void)event;
    (void)context;
    return noErr;
}

static OSStatus divider_register(void)
{
    static int registered = 0;
    /* kEventHIObjectDestruct is required too - see the note in listview.c. */
    static const EventTypeSpec construct_events[] = {
        { kEventClassHIObject, kEventHIObjectConstruct },
        { kEventClassHIObject, kEventHIObjectDestruct }
    };
    OSStatus status;

    if (registered)
        return noErr;
    status = HIObjectRegisterSubclass(CFSTR("com.rainygirl.rworldradio.divider"),
                                      kHIViewClassID, 0, divider_construct,
                                      GetEventTypeCount(construct_events),
                                      construct_events, NULL, NULL);
    if (status == noErr || status == hiObjectClassExistsErr) {
        registered = 1;
        return noErr;
    }
    return status;
}

static void load_dataset(RadioUI *ui)
{
    char path[2048];
    char error[512];
    char search_order[2048];
    int count, index;

    if (!paths_find_data_file("stations.dat", path, sizeof(path))) {
        paths_describe_search_order("stations.dat", search_order, sizeof(search_order));
        set_status(ui, "No station dataset found");
        snprintf(ui->status_detail, sizeof(ui->status_detail),
                 "stations.dat was not found. Looked in:\n%s", search_order);
        set_help_tag(ui->status_text, ui->status_detail);
        return;
    }

    ui->dataset = dataset_open(path, error, sizeof(error));
    if (!ui->dataset) {
        set_status(ui, "Could not read the station dataset");
        snprintf(ui->status_detail, sizeof(ui->status_detail), "%s", error);
        set_help_tag(ui->status_text, ui->status_detail);
        return;
    }

    count = dataset_country_count(ui->dataset);
    ui->country_names = (CFStringRef *)calloc((size_t)count, sizeof(CFStringRef));
    ui->country_filtered = (int *)calloc((size_t)count, sizeof(int));
    if (!ui->country_names || !ui->country_filtered) {
        set_status(ui, "Out of memory loading the dataset");
        return;
    }
    for (index = 0; index < count; index++) {
        ui->country_names[index] = CFStringCreateWithCString(
            NULL, dataset_country_name(ui->dataset, index), kCFStringEncodingUTF8);
    }

    refilter_countries(ui);

    {
        char summary[512];
        snprintf(summary, sizeof(summary), "%d countries, %d stations",
                 count, dataset_total_station_count(ui->dataset));
        set_status(ui, summary);
        snprintf(ui->status_detail, sizeof(ui->status_detail), "%s\n%s", summary, path);
        set_help_tag(ui->status_text, ui->status_detail);
    }
}

RadioUI *ui_create(void)
{
    static const EventTypeSpec window_events[] = {
        { kEventClassWindow, kEventWindowBoundsChanged },
        { kEventClassWindow, kEventWindowClose },
        { kEventClassWindow, kEventWindowActivated },
        { kEventClassWindow, kEventWindowDeactivated }
    };
    static const EventTypeSpec command_events[] = {
        { kEventClassCommand, kEventCommandProcess }
    };
    static const EventTypeSpec wake_events[] = {
        { kEventClassRadio, kEventRadioPlayerWoke }
    };
    static const EventTypeSpec divider_events[] = {
        { kEventClassControl, kEventControlDraw },
        { kEventClassControl, kEventControlHitTest },
        { kEventClassControl, kEventControlClick }
    };
    RadioUI *ui;
    Rect frame;
    ListViewDelegate country_delegate, station_delegate;
    HIObjectRef divider_object = NULL;

    ui = (RadioUI *)calloc(1, sizeof(*ui));
    if (!ui)
        return NULL;
    ui->selected_country = -1;
    ui->selected_station = -1;
    ui->left_width = DEFAULT_LEFT_WIDTH;
    ui->playback = PLAYBACK_IDLE;

    SetRect(&frame, 0, 0, WINDOW_WIDTH, WINDOW_HEIGHT);
    /* Compositing is required: HIViewAddSubview, HIViewSetFrame and
     * HIScrollView all only work in a compositing window. */
    if (CreateNewWindow(kDocumentWindowClass,
                        kWindowStandardDocumentAttributes |
                            kWindowStandardHandlerAttribute |
                            kWindowCompositingAttribute |
                            kWindowLiveResizeAttribute,
                        &frame, &ui->window) != noErr) {
        free(ui);
        return NULL;
    }
    SetWindowTitleWithCFString(ui->window, CFSTR("R World Radio"));
    RepositionWindow(ui->window, NULL, kWindowCascadeOnMainScreen);
    {
        HISize minimum, maximum;
        minimum.width = MIN_WIDTH;
        minimum.height = MIN_HEIGHT;
        maximum.width = 10000;
        maximum.height = 10000;
        SetWindowResizeLimits(ui->window, &minimum, &maximum);
    }
    HIViewFindByID(HIViewGetRoot(ui->window), kHIViewWindowContentID, &ui->content);

    ui->player = player_new(wake_from_worker, ui);
    if (!ui->player) {
        DisposeWindow(ui->window);
        free(ui);
        return NULL;
    }

    ui->country_context.ui = ui;
    ui->country_context.is_country = 1;
    ui->station_context.ui = ui;
    ui->station_context.is_country = 0;
    ui->country_search =
        make_search_field(&ui->country_context, ui->content, "Search countries");
    ui->station_search =
        make_search_field(&ui->station_context, ui->content, "Search stations");
    if (!ui->country_search || !ui->station_search) {
        ui_dispose(ui);
        return NULL;
    }

    memset(&country_delegate, 0, sizeof(country_delegate));
    country_delegate.context = ui;
    country_delegate.row_count = country_row_count;
    country_delegate.row_text = country_row_text;
    country_delegate.selection_changed = country_selection_changed;
    ui->country_list =
        listview_create(ui->window, ui->content, &country_delegate, &ui->country_scroll);

    memset(&station_delegate, 0, sizeof(station_delegate));
    station_delegate.context = ui;
    station_delegate.row_count = station_row_count;
    station_delegate.row_text = station_row_text;
    station_delegate.selection_changed = station_selection_changed;
    station_delegate.row_activated = station_activated;
    ui->station_list =
        listview_create(ui->window, ui->content, &station_delegate, &ui->station_scroll);

    if (divider_register() == noErr &&
        HIObjectCreate(CFSTR("com.rainygirl.rworldradio.divider"), NULL,
                       &divider_object) == noErr) {
        ui->divider = (ControlRef)divider_object;
        InstallEventHandler(HIObjectGetEventTarget(divider_object), divider_handler,
                            GetEventTypeCount(divider_events), divider_events, ui, NULL);
        HIViewAddSubview(ui->content, ui->divider);
        HIViewSetVisible(ui->divider, true);
    }

    ui->stop_button = make_button(ui->window, ui->content, "Stop", kRadioCommandStop);
    ui->play_button = make_button(ui->window, ui->content, "Play", kRadioCommandPlay);
    if (ui->play_button)
        set_help_tag(ui->play_button,
                     "Play the selected station (or double-click it in the list)");
    ui->now_playing = make_static_text(ui->window, ui->content);
    ui->status_text = make_static_text(ui->window, ui->content);
    ui->meter = levelmeter_create(ui->content, &ui->meter_view);

    if (!ui->country_list || !ui->station_list || !ui->stop_button ||
        !ui->play_button || !ui->now_playing || !ui->status_text || !ui->meter) {
        ui_dispose(ui);
        return NULL;
    }

    InstallEventHandler(GetWindowEventTarget(ui->window), window_handler,
                        GetEventTypeCount(window_events), window_events, ui, NULL);
    InstallEventHandler(GetWindowEventTarget(ui->window), command_handler,
                        GetEventTypeCount(command_events), command_events, ui, NULL);
    /* The wake event is posted to the main queue, which dispatches to the
     * application target rather than to any window. */
    InstallEventHandler(GetApplicationEventTarget(), wake_handler,
                        GetEventTypeCount(wake_events), wake_events, ui, NULL);

    update_transport(ui);
    load_dataset(ui);
    layout(ui);

    ShowWindow(ui->window);
    SetKeyboardFocus(ui->window, ui->country_search, kControlEditTextPart);
    return ui;
}

void ui_dispose(RadioUI *ui)
{
    if (!ui)
        return;
    stop_timer(ui);
    if (ui->player) {
        player_stop(ui->player);
        player_free(ui->player);
    }
    listview_dispose(ui->country_list);
    listview_dispose(ui->station_list);
    levelmeter_dispose(ui->meter);
    release_string_array(ui->country_names, ui->dataset
                                                ? dataset_country_count(ui->dataset)
                                                : 0);
    release_string_array(ui->station_names, ui->station_count);
    free(ui->country_filtered);
    free(ui->station_filtered);
    dataset_close(ui->dataset);
    if (ui->window)
        DisposeWindow(ui->window);
    free(ui);
}
