#include "listview.h"

#include <stdlib.h>
#include <string.h>

#define LISTVIEW_CLASS_ID CFSTR("com.rainygirl.rworldradio.listview")

/* Left inset for row text, and the padding added to the font's line height. Both
 * chosen to match the metrics of a stock Aqua list. */
#define TEXT_INSET   6.0f
#define ROW_PADDING  4.0f

struct ListView {
    HIViewRef        view;      /* the content view; the scroll view owns it */
    HIViewRef        scroll;
    ListViewDelegate delegate;
    int              focused;

    float row_height;
    int   selection;    /* -1 for none */
    float origin_y;     /* scroll position, in points from the top */
};

/* ------------------------------------------------------------------ setup - */

static float measure_row_height(void)
{
    /* kThemeViewsFont is the standard list font, which on Tiger is Lucida
     * Grande 12. Asking the theme for the height of a sample string rather than
     * hardcoding one keeps the rows right if the user has changed the system
     * font size - and using a CJK sample matters, because ATSUI falls back to a
     * taller font for those and a row sized for Latin text would clip them. */
    HIThemeTextInfo info;
    float width = 0.0f, height = 0.0f, baseline = 0.0f;
    OSStatus status;
    /* Built from UTF-8 explicitly rather than with CFSTR(): the 10.4 compiler
     * reads non-ASCII bytes in a CFSTR literal as MacRoman, which would measure
     * the wrong glyphs (and warns about it). */
    CFStringRef sample = CFStringCreateWithCString(
        NULL, "Ag\xe3\x83\xa9\xed\x95\x9c", kCFStringEncodingUTF8);

    if (!sample)
        return 18.0f;

    memset(&info, 0, sizeof(info));
    info.version = 0;
    info.state = kThemeStateActive;
    info.fontID = kThemeViewsFont;

    status = HIThemeGetTextDimensions(sample, 0, &info, &width, &height, &baseline);
    CFRelease(sample);
    if (status != noErr)
        return 18.0f;

    height += ROW_PADDING;
    /* A theme that reports nothing usable must not produce a zero-height row,
     * which would divide by zero everywhere below. */
    return height < 12.0f ? 18.0f : height;
}

static int row_count(ListView *list)
{
    return list->delegate.row_count ? list->delegate.row_count(list->delegate.context) : 0;
}

static float content_height(ListView *list)
{
    return (float)row_count(list) * list->row_height;
}

/* ---------------------------------------------------------------- drawing - */

static void draw(ListView *list, CGContextRef context)
{
    HIRect bounds;
    int rows = row_count(list);
    int first, last, index;

    HIViewGetBounds(list->view, &bounds);

    /* The list background. kThemeBrushListViewBackground is what a stock list
     * uses, so this matches the search field above it. */
    HIThemeSetFill(kThemeBrushListViewBackground, NULL, context, kHIThemeOrientationNormal);
    CGContextFillRect(context, bounds);

    if (rows == 0 || list->row_height <= 0.0f)
        return;

    /* Only the rows actually on screen are touched, however many the list has.
     * This is the whole reason the view exists - see listview.h. */
    first = (int)(bounds.origin.y / list->row_height);
    last = (int)((bounds.origin.y + bounds.size.height) / list->row_height) + 1;
    if (first < 0)
        first = 0;
    if (last > rows)
        last = rows;

    for (index = first; index < last; index++) {
        CFStringRef text = list->delegate.row_text
                               ? list->delegate.row_text(list->delegate.context, index)
                               : NULL;
        HIRect row;
        HIThemeTextInfo info;
        int selected = (index == list->selection);

        row.origin.x = bounds.origin.x;
        row.origin.y = (float)index * list->row_height;
        row.size.width = bounds.size.width;
        row.size.height = list->row_height;

        if (selected) {
            /* A row spans the full width so that clicking to the right of a
             * short name selects it, which is how every list behaves and what
             * the Rust port needed a justified layout to reproduce. */
            HIThemeSetFill(IsWindowActive(GetControlOwner(list->view))
                               ? kThemeBrushPrimaryHighlightColor
                               : kThemeBrushSecondaryHighlightColor,
                           NULL, context, kHIThemeOrientationNormal);
            CGContextFillRect(context, row);
        }

        if (!text)
            continue;

        memset(&info, 0, sizeof(info));
        info.version = 0;
        info.state = kThemeStateActive;
        info.fontID = kThemeViewsFont;
        info.horizontalFlushness = kHIThemeTextHorizontalFlushLeft;
        info.verticalFlushness = kHIThemeTextVerticalFlushCenter;
        /* Truncate in the middle: a station name's distinguishing part is as
         * often at the end ("... 105.3 FM") as at the start. */
        info.truncationPosition = kHIThemeTextTruncationMiddle;
        info.options = 0;

        HIThemeSetTextFill(selected ? kThemeTextColorWhite : kThemeTextColorListView,
                           NULL, context, kHIThemeOrientationNormal);

        row.origin.x += TEXT_INSET;
        row.size.width -= TEXT_INSET * 2.0f;
        if (row.size.width > 0.0f)
            HIThemeDrawTextBox(text, &row, &info, context, kHIThemeOrientationNormal);
    }
}

/* -------------------------------------------------------------- selection - */

static void scroll_to_row(ListView *list, int row)
{
    HIRect bounds;
    float top, bottom, target = list->origin_y;

    HIViewGetBounds(list->view, &bounds);
    top = (float)row * list->row_height;
    bottom = top + list->row_height;

    if (top < list->origin_y)
        target = top;
    else if (bottom > list->origin_y + bounds.size.height)
        target = bottom - bounds.size.height;

    if (target != list->origin_y) {
        float maximum = content_height(list) - bounds.size.height;
        if (maximum < 0.0f)
            maximum = 0.0f;
        if (target > maximum)
            target = maximum;
        if (target < 0.0f)
            target = 0.0f;
        list->origin_y = target;
        HIViewSetBoundsOrigin(list->view, 0, target);
        HIViewSetNeedsDisplay(list->view, true);
        /* Tell the scroll view its knob moved. */
        HIViewSetNeedsDisplay(list->scroll, true);
    }
}

void listview_set_selection(ListView *list, int row, int scroll_into_view)
{
    int rows = row_count(list);

    if (row < -1 || row >= rows)
        row = -1;
    if (row == list->selection)
        return;

    list->selection = row;
    HIViewSetNeedsDisplay(list->view, true);
    if (row >= 0 && scroll_into_view)
        scroll_to_row(list, row);
    if (list->delegate.selection_changed)
        list->delegate.selection_changed(list->delegate.context, row);
}

int listview_selection(const ListView *list)
{
    return list->selection;
}

float listview_row_height(const ListView *list)
{
    return list->row_height;
}

void listview_reload(ListView *list)
{
    int rows = row_count(list);

    if (list->selection >= rows)
        list->selection = -1;
    /* A shorter list may leave the view scrolled past its own end. */
    {
        HIRect bounds;
        float maximum;
        HIViewGetBounds(list->view, &bounds);
        maximum = content_height(list) - bounds.size.height;
        if (maximum < 0.0f)
            maximum = 0.0f;
        if (list->origin_y > maximum) {
            list->origin_y = maximum;
            HIViewSetBoundsOrigin(list->view, 0, maximum);
        }
    }
    HIViewSetNeedsDisplay(list->view, true);
    HIViewSetNeedsDisplay(list->scroll, true);
}

/* ----------------------------------------------------------------- events - */

static int row_at_point(ListView *list, const HIPoint *where)
{
    int row;

    if (list->row_height <= 0.0f)
        return -1;
    /* `where` arrives in view coordinates, which already include the scroll
     * origin because the view's bounds origin is what scrolls. */
    row = (int)(where->y / list->row_height);
    if (row < 0 || row >= row_count(list))
        return -1;
    return row;
}

static OSStatus handle_click(ListView *list, EventRef event)
{
    HIPoint where;
    UInt32 clicks = 1;
    int row;

    if (GetEventParameter(event, kEventParamMouseLocation, typeHIPoint, NULL,
                          sizeof(where), NULL, &where) != noErr)
        return eventNotHandledErr;
    /* The location arrives in global screen coordinates, not window ones.
     * Converting it as if it were window-relative shifts every click down by
     * the height of the title bar - about one row - so a click lands on the row
     * below the one under the pointer, and a double-click plays the wrong
     * station. */
    HIPointConvert(&where, kHICoordSpace72DPIGlobal, NULL,
                   kHICoordSpaceView, list->view);
    GetEventParameter(event, kEventParamClickCount, typeUInt32, NULL,
                      sizeof(clicks), NULL, &clicks);

    SetKeyboardFocus(GetControlOwner(list->view), list->view, kControlFocusNextPart);

    row = row_at_point(list, &where);
    if (row < 0)
        return noErr;

    listview_set_selection(list, row, 0);
    if (clicks >= 2 && list->delegate.row_activated)
        list->delegate.row_activated(list->delegate.context, row);
    return noErr;
}

static OSStatus handle_key(ListView *list, EventRef event)
{
    char code = 0;
    int rows = row_count(list);
    int target = list->selection;
    HIRect bounds;
    int page;

    if (rows == 0)
        return eventNotHandledErr;
    if (GetEventParameter(event, kEventParamKeyMacCharCodes, typeChar, NULL,
                          sizeof(code), NULL, &code) != noErr)
        return eventNotHandledErr;

    HIViewGetBounds(list->view, &bounds);
    page = (int)(bounds.size.height / list->row_height);
    if (page < 1)
        page = 1;

    switch (code) {
    case kUpArrowCharCode:    target = target <= 0 ? 0 : target - 1; break;
    case kDownArrowCharCode:  target = target < 0 ? 0 : target + 1;  break;
    case kPageUpCharCode:     target = target < 0 ? 0 : target - page; break;
    case kPageDownCharCode:   target = target < 0 ? 0 : target + page; break;
    case kHomeCharCode:       target = 0; break;
    case kEndCharCode:        target = rows - 1; break;
    case kReturnCharCode:
    case kEnterCharCode:
        if (list->selection >= 0 && list->delegate.row_activated)
            list->delegate.row_activated(list->delegate.context, list->selection);
        return noErr;
    default:
        return eventNotHandledErr;
    }

    if (target < 0)
        target = 0;
    if (target >= rows)
        target = rows - 1;
    listview_set_selection(list, target, 1);
    return noErr;
}

static OSStatus view_handler(EventHandlerCallRef next, EventRef event, void *context)
{
    ListView *list = (ListView *)context;
    UInt32 event_class = GetEventClass(event);
    UInt32 kind = GetEventKind(event);

    if (event_class == kEventClassControl) {
        switch (kind) {
        case kEventControlDraw: {
            CGContextRef graphics = NULL;
            if (GetEventParameter(event, kEventParamCGContextRef, typeCGContextRef, NULL,
                                  sizeof(graphics), NULL, &graphics) != noErr)
                return eventNotHandledErr;
            draw(list, graphics);
            return noErr;
        }
        case kEventControlHitTest: {
            /* Reporting any part other than kControlNoPart is what makes the
             * view eligible for clicks at all. */
            ControlPartCode part = kControlIndicatorPart;
            SetEventParameter(event, kEventParamControlPart, typeControlPartCode,
                              sizeof(part), &part);
            return noErr;
        }
        case kEventControlClick:
            return handle_click(list, event);

        case kEventControlGetFocusPart: {
            ControlPartCode part = list->focused ? kControlIndicatorPart
                                                 : kControlFocusNoPart;
            SetEventParameter(event, kEventParamControlPart, typeControlPartCode,
                              sizeof(part), &part);
            return noErr;
        }
        case kEventControlSetFocusPart: {
            /* Accepting focus is what makes arrow keys reach handle_key, and
             * what puts this view in the Tab cycle with the search fields. */
            ControlPartCode part = kControlFocusNoPart;
            GetEventParameter(event, kEventParamControlPart, typeControlPartCode, NULL,
                              sizeof(part), NULL, &part);
            list->focused = (part != kControlFocusNoPart);
            if (list->focused)
                part = kControlIndicatorPart;
            SetEventParameter(event, kEventParamControlPart, typeControlPartCode,
                              sizeof(part), &part);
            HIViewSetNeedsDisplay(list->view, true);
            return noErr;
        }

        case kEventControlBoundsChanged:
            listview_reload(list);
            return eventNotHandledErr; /* let the scroll view see it too */
        }
    }

    if (event_class == kEventClassKeyboard &&
        (kind == kEventRawKeyDown || kind == kEventRawKeyRepeat))
        return handle_key(list, event);

    /* --- HIScrollView contract -------------------------------------------
     * Implementing kEventClassScrollable is what gets real Aqua scroll bars,
     * live scrolling and mouse-wheel support without writing any of it. */
    if (event_class == kEventClassScrollable) {
        switch (kind) {
        case kEventScrollableGetInfo: {
            HIRect bounds;
            HISize image, line, view_size;
            HIPoint origin;

            HIViewGetBounds(list->view, &bounds);
            view_size = bounds.size;
            image.width = bounds.size.width;
            image.height = content_height(list);
            if (image.height < view_size.height)
                image.height = view_size.height;
            line.width = 0;
            line.height = list->row_height;
            origin.x = 0;
            origin.y = list->origin_y;

            SetEventParameter(event, kEventParamImageSize, typeHISize, sizeof(image), &image);
            SetEventParameter(event, kEventParamViewSize, typeHISize, sizeof(view_size),
                              &view_size);
            SetEventParameter(event, kEventParamLineSize, typeHISize, sizeof(line), &line);
            SetEventParameter(event, kEventParamOrigin, typeHIPoint, sizeof(origin), &origin);
            return noErr;
        }
        case kEventScrollableScrollTo: {
            HIPoint origin;
            if (GetEventParameter(event, kEventParamOrigin, typeHIPoint, NULL,
                                  sizeof(origin), NULL, &origin) != noErr)
                return eventNotHandledErr;
            list->origin_y = origin.y;
            HIViewSetBoundsOrigin(list->view, 0, origin.y);
            HIViewSetNeedsDisplay(list->view, true);
            return noErr;
        }
        }
    }

    return CallNextEventHandler(next, event);
}

/* ------------------------------------------------------------ HIObject glue */

/* The subclass carries no per-instance data of its own - the ListView is
 * attached as the event handler's user data in listview_create - so there is
 * nothing to allocate here and nothing to free on destruct.
 *
 * This must not call CallNextEventHandler. The toolbox invokes the construct
 * proc directly rather than through the event dispatcher and passes a NULL
 * EventHandlerCallRef, and the base class has already been constructed by the
 * time it runs; calling through the NULL ref is a crash. */
static OSStatus construct_handler(EventHandlerCallRef next, EventRef event, void *context)
{
    (void)next;
    (void)event;
    (void)context;
    return noErr;
}

static OSStatus register_class(void)
{
    static int registered = 0;
    /* Both events are required. HIObjectRegisterSubclass returns paramErr for a
     * concrete class that does not list kEventHIObjectDestruct as well as
     * kEventHIObjectConstruct - it is only an abstract class (a NULL construct
     * proc) that may register no events at all. */
    static const EventTypeSpec construct_events[] = {
        { kEventClassHIObject, kEventHIObjectConstruct },
        { kEventClassHIObject, kEventHIObjectDestruct }
    };
    OSStatus status;

    if (registered)
        return noErr;
    status = HIObjectRegisterSubclass(LISTVIEW_CLASS_ID, kHIViewClassID, 0,
                                      construct_handler,
                                      GetEventTypeCount(construct_events),
                                      construct_events, NULL, NULL);
    /* hiObjectClassExistsErr just means another instance registered first. */
    if (status == noErr || status == hiObjectClassExistsErr) {
        registered = 1;
        return noErr;
    }
    return status;
}

ListView *listview_create(WindowRef window, HIViewRef parent,
                          const ListViewDelegate *delegate, HIViewRef *out_scroll)
{
    static const EventTypeSpec view_events[] = {
        { kEventClassControl,    kEventControlDraw },
        { kEventClassControl,    kEventControlHitTest },
        { kEventClassControl,    kEventControlClick },
        { kEventClassControl,    kEventControlGetFocusPart },
        { kEventClassControl,    kEventControlSetFocusPart },
        { kEventClassControl,    kEventControlBoundsChanged },
        { kEventClassKeyboard,   kEventRawKeyDown },
        { kEventClassKeyboard,   kEventRawKeyRepeat },
        { kEventClassScrollable, kEventScrollableGetInfo },
        { kEventClassScrollable, kEventScrollableScrollTo }
    };
    ListView *list;
    HIObjectRef object = NULL;
    HIViewRef scroll = NULL;

    if (register_class() != noErr)
        return NULL;

    list = (ListView *)calloc(1, sizeof(*list));
    if (!list)
        return NULL;
    list->delegate = *delegate;
    list->selection = -1;
    list->row_height = measure_row_height();

    if (HIObjectCreate(LISTVIEW_CLASS_ID, NULL, &object) != noErr) {
        free(list);
        return NULL;
    }
    list->view = (HIViewRef)object;

    /* kHIScrollViewOptionsVertScroll, not ...VerticalScroll: 10.4 spells it
     * short. Same for HorizScroll. */
    if (HIScrollViewCreate(kHIScrollViewOptionsVertScroll |
                               kHIScrollViewOptionsAllowGrow,
                           &scroll) != noErr) {
        CFRelease(object);
        free(list);
        return NULL;
    }
    list->scroll = scroll;

    InstallEventHandler(HIObjectGetEventTarget(object), view_handler,
                        GetEventTypeCount(view_events), view_events, list, NULL);

    HIViewAddSubview(scroll, list->view);
    HIViewSetVisible(list->view, true);
    HIViewAddSubview(parent, scroll);
    HIViewSetVisible(scroll, true);
    /* A frame around the scroll view is what makes it read as a list rather than
     * a floating block of text. */
    HIScrollViewSetScrollBarAutoHide(scroll, false);

    (void)window;
    if (out_scroll)
        *out_scroll = scroll;
    return list;
}

void listview_dispose(ListView *list)
{
    if (!list)
        return;
    /* The views are owned by their parent hierarchy and go away with the window;
     * only this bookkeeping is ours. */
    free(list);
}
