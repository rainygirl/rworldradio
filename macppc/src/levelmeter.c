#include "levelmeter.h"

#include <stdlib.h>
#include <string.h>

#define LEVELMETER_CLASS_ID CFSTR("com.rainygirl.rworldradio.levelmeter")

#define SEGMENT_COUNT  10
#define SEGMENT_GAP    2.0f
#define CORNER_RADIUS  2.0f

struct LevelMeter {
    HIViewRef view;
    int       lit;   /* how many segments are on; the only thing that can change */
};

typedef struct { float red, green, blue; } Rgb;

static const Rgb GREEN  = { 55.0f / 255.0f, 190.0f / 255.0f,  75.0f / 255.0f };
static const Rgb YELLOW = { 235.0f / 255.0f, 195.0f / 255.0f, 30.0f / 255.0f };
static const Rgb RED    = { 220.0f / 255.0f,  40.0f / 255.0f, 50.0f / 255.0f };
static const Rgb UNLIT  = { 0.72f, 0.72f, 0.72f };

static Rgb lighten(Rgb color, float amount)
{
    Rgb out;
    out.red   = color.red   + (1.0f - color.red)   * amount;
    out.green = color.green + (1.0f - color.green) * amount;
    out.blue  = color.blue  + (1.0f - color.blue)  * amount;
    return out;
}

static Rgb darken(Rgb color, float amount)
{
    Rgb out;
    out.red   = color.red   * (1.0f - amount);
    out.green = color.green * (1.0f - amount);
    out.blue  = color.blue  * (1.0f - amount);
    return out;
}

/* A rounded rect as a CG path. CGContextAddArcToPoint is 10.2+, so this needs no
 * 10.5-only convenience call. */
static void add_rounded_rect(CGContextRef context, CGRect rect, float radius)
{
    float right = rect.origin.x + rect.size.width;
    float bottom = rect.origin.y + rect.size.height;

    if (radius * 2.0f > rect.size.width)
        radius = rect.size.width / 2.0f;
    if (radius * 2.0f > rect.size.height)
        radius = rect.size.height / 2.0f;

    CGContextBeginPath(context);
    CGContextMoveToPoint(context, rect.origin.x + radius, rect.origin.y);
    CGContextAddArcToPoint(context, right, rect.origin.y, right, bottom, radius);
    CGContextAddArcToPoint(context, right, bottom, rect.origin.x, bottom, radius);
    CGContextAddArcToPoint(context, rect.origin.x, bottom, rect.origin.x, rect.origin.y,
                           radius);
    CGContextAddArcToPoint(context, rect.origin.x, rect.origin.y, right, rect.origin.y,
                           radius);
    CGContextClosePath(context);
}

static void fill_rounded(CGContextRef context, CGRect rect, float radius, Rgb color)
{
    CGContextSetRGBFillColor(context, color.red, color.green, color.blue, 1.0f);
    add_rounded_rect(context, rect, radius);
    CGContextFillPath(context);
}

static void draw(LevelMeter *meter, CGContextRef context)
{
    HIRect bounds;
    float segment_width;
    int index;

    HIViewGetBounds(meter->view, &bounds);
    segment_width =
        (bounds.size.width - SEGMENT_GAP * (SEGMENT_COUNT - 1)) / (float)SEGMENT_COUNT;
    if (segment_width <= 0.0f)
        return;

    for (index = 0; index < SEGMENT_COUNT; index++) {
        CGRect segment, highlight;
        Rgb base;
        int lit = index < meter->lit;

        segment.origin.x = bounds.origin.x + (float)index * (segment_width + SEGMENT_GAP);
        segment.origin.y = bounds.origin.y;
        segment.size.width = segment_width;
        segment.size.height = bounds.size.height;

        if (!lit)
            base = UNLIT;
        else if (index >= SEGMENT_COUNT - 2)
            base = RED;      /* top two segments */
        else if (index >= SEGMENT_COUNT - 4)
            base = YELLOW;   /* next two down */
        else
            base = GREEN;

        fill_rounded(context, segment, CORNER_RADIUS, base);

        /* Glossy look: a lighter highlight over the top of the segment fading
         * into the base colour. Two stacked fills rather than the Haiku
         * version's BGradientLinear - and cheaper than a real gradient, which on
         * a G4 is worth caring about for something that repaints ten times a
         * second. */
        highlight = segment;
        highlight.size.height = segment.size.height * 0.45f;
        fill_rounded(context, highlight, CORNER_RADIUS, lighten(base, lit ? 0.45f : 0.12f));

        {
            Rgb edge = darken(base, 0.55f);
            CGContextSetRGBStrokeColor(context, edge.red, edge.green, edge.blue, 1.0f);
            CGContextSetLineWidth(context, 1.0f);
            add_rounded_rect(context,
                             CGRectInset(segment, 0.5f, 0.5f), CORNER_RADIUS);
            CGContextStrokePath(context);
        }
    }
}

static OSStatus view_handler(EventHandlerCallRef next, EventRef event, void *context)
{
    LevelMeter *meter = (LevelMeter *)context;

    if (GetEventClass(event) == kEventClassControl &&
        GetEventKind(event) == kEventControlDraw) {
        CGContextRef graphics = NULL;
        if (GetEventParameter(event, kEventParamCGContextRef, typeCGContextRef, NULL,
                              sizeof(graphics), NULL, &graphics) != noErr)
            return eventNotHandledErr;
        draw(meter, graphics);
        return noErr;
    }
    return CallNextEventHandler(next, event);
}

/* Must not call CallNextEventHandler - see the note in listview.c. */
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
    /* kEventHIObjectDestruct is required too - see the note in listview.c. */
    static const EventTypeSpec construct_events[] = {
        { kEventClassHIObject, kEventHIObjectConstruct },
        { kEventClassHIObject, kEventHIObjectDestruct }
    };
    OSStatus status;

    if (registered)
        return noErr;
    status = HIObjectRegisterSubclass(LEVELMETER_CLASS_ID, kHIViewClassID, 0,
                                      construct_handler,
                                      GetEventTypeCount(construct_events),
                                      construct_events, NULL, NULL);
    if (status == noErr || status == hiObjectClassExistsErr) {
        registered = 1;
        return noErr;
    }
    return status;
}

LevelMeter *levelmeter_create(HIViewRef parent, HIViewRef *out_view)
{
    static const EventTypeSpec view_events[] = {
        { kEventClassControl, kEventControlDraw }
    };
    LevelMeter *meter;
    HIObjectRef object = NULL;

    if (register_class() != noErr)
        return NULL;

    meter = (LevelMeter *)calloc(1, sizeof(*meter));
    if (!meter)
        return NULL;

    if (HIObjectCreate(LEVELMETER_CLASS_ID, NULL, &object) != noErr) {
        free(meter);
        return NULL;
    }
    meter->view = (HIViewRef)object;

    InstallEventHandler(HIObjectGetEventTarget(object), view_handler,
                        GetEventTypeCount(view_events), view_events, meter, NULL);
    HIViewAddSubview(parent, meter->view);
    HIViewSetVisible(meter->view, true);

    if (out_view)
        *out_view = meter->view;
    return meter;
}

void levelmeter_dispose(LevelMeter *meter)
{
    free(meter);
}

void levelmeter_set_level(LevelMeter *meter, float level)
{
    int lit;

    if (!meter)
        return;
    if (level < 0.0f)
        level = 0.0f;
    if (level > 1.0f)
        level = 1.0f;
    lit = (int)(level * (float)SEGMENT_COUNT + 0.5f);

    /* The meter is polled ten times a second; repainting only when the segment
     * count actually changed is what keeps a quiet passage at zero CPU. */
    if (lit == meter->lit)
        return;
    meter->lit = lit;
    HIViewSetNeedsDisplay(meter->view, true);
}
