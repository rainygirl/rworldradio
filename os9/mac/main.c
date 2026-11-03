/*
 * R WorldRadio for Mac OS 9 - internet radio through the Pi relay.
 *
 * Mac OS 9 cannot do modern TLS, AAC or HLS, and QuickTime 6 plays an endless
 * MP3 only from a Shoutcast-style server. radio_relay.py on the Pi lists the
 * R WorldRadio dataset and turns any station into an ICY MP3 stream, which this
 * app plays with QuickTime (icy:// URL). Lists are fetched with URL Access.
 *
 * Settings file next to the app ("R WorldRadio Settings", CR lines):
 *   relay=10.0.0.129:8083
 *   last=kexp | <station id>        what to play at launch (empty = nothing)
 *   lastname=<name>
 */
#include <Multiverse.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- QuickTime (Movies.h is not in multiversal) ---- */
/* Movie, EnterMovies, StartMovie, StopMovie, MoviesTask, DisposeMovie come from multiversal */
extern pascal OSErr NewMovieFromDataRef(Movie *m, short flags, short *id, Handle dataRef, OSType type);
extern pascal OSErr GetMoviesError(void);
extern pascal long GetMovieLoadState(Movie m);
extern pascal long GetMovieTime(Movie m, void *t);
extern pascal void SetMovieVolume(Movie m, short v);
extern pascal void SetMovieBox(Movie m, const Rect *box);
enum { newMovieActive = 1, newMovieAsyncOK = 0x100, kMovieLoadStatePlayable = 10000 };

/* ---- URL Access ---- */
typedef struct OpaqueURLReference *URLReference;
extern pascal OSStatus URLNewReference(const char *url, URLReference *ref);
extern pascal OSStatus URLDisposeReference(URLReference ref);
extern pascal OSStatus URLOpen(URLReference ref, FSSpec *spec, UInt32 flags, void *notify, UInt32 mask, void *ctx);
extern pascal OSStatus URLAbort(URLReference ref);
extern pascal OSStatus URLGetCurrentState(URLReference ref, UInt32 *state);
extern pascal OSStatus URLGetBuffer(URLReference ref, void **buf, Size *size);
extern pascal OSStatus URLReleaseBuffer(URLReference ref, void *buf);
extern pascal OSStatus URLIdle(void);
enum { kURLTransactionCompleteState = 6, kURLErrorOccurredState = 7, kURLCompletedState = 9, kURLDataAvailableState = 0x15 };

extern pascal void HideMenuBar(void);
typedef uint8_t KeyMapBytes[16];

#define TICKS 60L
#define PSTR(s) ((ConstStringPtr)(s))

/* ------------------------------------------------------------ data */

typedef struct { long id; char enc; char name[64]; char extra[48]; } Row;
typedef struct {
    Rect frame;
    ControlHandle bar;
    Row *rows;
    int n, cap, top, sel;
    char title[48];
} List;

static WindowPtr gWin;
static List gCountries, gStations;
static TEHandle gSearch;
static ControlHandle gKexpBtn, gStopBtn, gSearchBtn, gPlayBtn;
static char gRelay[128] = "10.0.0.129:8083";
static char gNow[96] = "", gStatus[96] = "Stopped";
static char gLast[32] = "kexp";
static short gKoreanFont, gRomanFont;
static FSSpec gApp;
static int gQuit;

/* ------------------------------------------------------------ settings */

static int spec_for(const char *name, FSSpec *out)
{
    Str255 p;
    size_t n = strlen(name);
    p[0] = (unsigned char)(n + 1); p[1] = ':'; memcpy(p + 2, name, n);
    return FSMakeFSSpec(gApp.vRefNum, gApp.parID, p, out) == noErr;
}

static void save_settings(void)
{
    FSSpec fs; int16_t ref; char buf[512]; LONGINT n; OSErr e;
    sprintf(buf, "relay=%s\rlast=%s\rlastname=%s\r", gRelay, gLast, gNow);
    n = (LONGINT)strlen(buf);
    spec_for("R WorldRadio Settings", &fs);
    e = FSpCreate(&fs, 'ttxt', 'TEXT', 0);
    if (e != noErr && e != dupFNErr) return;
    if (FSpOpenDF(&fs, fsRdWrPerm, &ref) != noErr) return;
    SetEOF(ref, 0); FSWrite(ref, &n, buf); FSClose(ref);
}

static void load_settings(void)
{
    FSSpec fs; int16_t ref; LONGINT n = 0; char *buf, *line;
    if (!spec_for("R WorldRadio Settings", &fs) || FSpOpenDF(&fs, fsRdPerm, &ref) != noErr) { save_settings(); return; }
    GetEOF(ref, &n);
    buf = (char *)malloc(n + 1);
    FSRead(ref, &n, buf); buf[n] = 0; FSClose(ref);
    for (line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        if (!strncmp(line, "relay=", 6)) strncpy(gRelay, line + 6, sizeof gRelay - 1);
        else if (!strncmp(line, "last=", 5)) strncpy(gLast, line + 5, sizeof gLast - 1);
        else if (!strncmp(line, "lastname=", 9)) strncpy(gNow, line + 9, sizeof gNow - 1);
    }
    free(buf);
}

/* ------------------------------------------------------------ drawing */

static void draw_text(const char *s, char enc, short size, short face)
{
    TextFont(enc == 'K' ? gKoreanFont : gRomanFont);
    TextSize(size); TextFace(face);
    DrawText(s, 0, (short)strlen(s));
}

static void draw_status(void)
{
    Rect r;
    GrafPtr old;
    char line[160];
    GetPort(&old); SetPort(gWin);
    SetRect(&r, 10, 8, 420, 50);
    EraseRect(&r);
    MoveTo(12, 26);
    {   /* station names may be Korean: show them in the Korean font when the bytes are EUC-KR */
        const unsigned char *p = (const unsigned char *)gNow; char enc = 'R';
        for (; *p; p++) if (*p >= 0xA1 && p[1] >= 0xA1) { enc = 'K'; break; }
        draw_text(gNow[0] ? gNow : "R WorldRadio", enc, 14, bold);
    }
    MoveTo(12, 44);
    sprintf(line, "%s", gStatus);
    draw_text(line, 'R', 10, normal);
    SetPort(old);
}

static int visible_rows(List *l) { return (l->frame.bottom - l->frame.top - 22) / 17; }

static void draw_list(List *l)
{
    Rect r = l->frame, inner;
    int i, vis = visible_rows(l);
    GrafPtr old;
    GetPort(&old); SetPort(gWin);
    inner = r; inner.right -= 15;
    EraseRect(&inner);
    FrameRect(&r);
    MoveTo(r.left + 6, r.top + 15);
    draw_text(l->title, 'R', 10, bold);
    MoveTo(r.left, r.top + 20); LineTo(r.right - 16, r.top + 20);
    for (i = 0; i < vis && l->top + i < l->n; i++) {
        Row *row = &l->rows[l->top + i];
        Rect rr;
        SetRect(&rr, r.left + 1, r.top + 21 + i * 17, r.right - 16, r.top + 21 + (i + 1) * 17);
        if (l->top + i == l->sel) { PaintRect(&rr); TextMode(srcBic); }
        {
            short w = 0;
            RgnHandle save = NewRgn();
            Rect clip = rr;
            if (row->extra[0]) {
                TextFont(gRomanFont); TextSize(10);
                w = TextWidth(row->extra, 0, (short)strlen(row->extra));
                MoveTo(rr.right - w - 6, rr.bottom - 4);
                draw_text(row->extra, 'R', 10, normal);
            }
            GetClip(save);
            clip.right = rr.right - w - 14;
            ClipRect(&clip);
            MoveTo(rr.left + 5, rr.bottom - 4);
            draw_text(row->name, row->enc, 12, normal);
            SetClip(save); DisposeRgn(save);
        }
        TextMode(srcOr);
    }
    if (l->bar) {
        int max = l->n - vis;
        SetControlMaximum(l->bar, max > 0 ? max : 0);
        SetControlValue(l->bar, l->top);
    }
    SetPort(old);
}

static void redraw_all(void)
{
    GrafPtr old;
    Rect r;
    GetPort(&old); SetPort(gWin);
    draw_status();
    draw_list(&gCountries); draw_list(&gStations);
    SetRect(&r, 10, 58, 300, 76); FrameRect(&r);
    TEUpdate(&(**gSearch).viewRect, gSearch);
    DrawControls(gWin);
    SetPort(old);
}

/* ------------------------------------------------------------ lists */

static void list_clear(List *l) { l->n = 0; l->top = 0; l->sel = -1; }
static void list_add(List *l, const Row *r)
{
    if (l->n == l->cap) {
        int cap = l->cap ? l->cap * 2 : 256;
        Row *nr = (Row *)malloc(sizeof(Row) * cap);
        if (!nr) return;
        if (l->n) memcpy(nr, l->rows, sizeof(Row) * l->n);
        free(l->rows);
        l->rows = nr; l->cap = cap;
    }
    l->rows[l->n++] = *r;
}

static void copy_field(char *dst, int cap, const char *s, int n)
{
    if (n > cap - 1) n = cap - 1;
    memcpy(dst, s, n); dst[n] = 0;
}

/* parse "a|b|c..." lines (CR separated) into rows. kind 0 countries, 1 stations, 2 search */
static void parse_rows(List *l, const char *text, long len, int kind)
{
    long i = 0;
    while (i < len) {
        const char *f[8]; int fl[8], n = 0;
        long e = i, start = i;
        Row row;
        while (e < len && text[e] != '\r' && text[e] != '\n') e++;
        for (; i <= e && n < 8; i++) if (i == e || text[i] == '|') { f[n] = text + start; fl[n] = (int)(i - start); n++; start = i + 1; }
        i = e + 1;
        if (n < 3) continue;
        memset(&row, 0, sizeof row);
        row.id = atol(f[0]);
        row.enc = fl[1] ? f[1][0] : 'R';
        copy_field(row.name, sizeof row.name, f[2], fl[2]);
        if (kind == 0 && n >= 4) copy_field(row.extra, sizeof row.extra, f[3], fl[3]);
        if (kind >= 1 && n >= 5) {
            char codec[16], br[16];
            copy_field(codec, sizeof codec, f[3], fl[3]); copy_field(br, sizeof br, f[4], fl[4]);
            if (kind == 2 && n >= 6) { char c[32]; copy_field(c, sizeof c, f[5], fl[5]); sprintf(row.extra, "%s  %s", c, codec); }
            else sprintf(row.extra, "%s %s", codec, strcmp(br, "0") ? br : "");
        }
        list_add(l, &row);
    }
}

/* ------------------------------------------------------------ HTTP fetch (one at a time) */

static URLReference gRef;
static char *gBody;
static long gBodyLen, gBodyCap;
static int gFetchKind;          /* 0 countries, 1 stations, 2 search */
static long gFetchStart;

static void fetch(const char *path, int kind)
{
    char url[400];
    if (gRef) { URLAbort(gRef); URLDisposeReference(gRef); gRef = NULL; }
    sprintf(url, "http://%s%s", gRelay, path);
    gBodyLen = 0; gFetchKind = kind; gFetchStart = TickCount();
    if (URLNewReference(url, &gRef) != noErr) { gRef = NULL; strcpy(gStatus, "Relay not reachable"); draw_status(); return; }
    if (URLOpen(gRef, NULL, 0, NULL, 0, NULL) != noErr) { URLDisposeReference(gRef); gRef = NULL; }
}

static void fetch_done(int ok)
{
    List *l = gFetchKind == 0 ? &gCountries : &gStations;
    if (gRef) { URLDisposeReference(gRef); gRef = NULL; }
    if (!ok) { strcpy(gStatus, "Could not reach the relay"); draw_status(); return; }
    list_clear(l);
    parse_rows(l, gBody, gBodyLen, gFetchKind);
    if (gFetchKind == 2) sprintf(gStations.title, "Search results (%d)", gStations.n);
    draw_list(l);
}

static void fetch_poll(void)
{
    UInt32 state = 0;
    if (!gRef) return;
    URLIdle();
    if (URLGetCurrentState(gRef, &state) != noErr) { fetch_done(0); return; }
    if (state == kURLDataAvailableState || state == kURLTransactionCompleteState) {
        void *buf; Size size;
        while (URLGetBuffer(gRef, &buf, &size) == noErr && buf) {
            if (gBodyLen + size + 1 > gBodyCap) {
                long cap = gBodyCap ? gBodyCap * 2 : 65536;
                char *nb;
                while (cap < gBodyLen + size + 1) cap *= 2;
                nb = (char *)malloc(cap);
                if (nb) { if (gBodyLen) memcpy(nb, gBody, gBodyLen); free(gBody); gBody = nb; gBodyCap = cap; }
            }
            if (gBodyLen + size + 1 <= gBodyCap) { memcpy(gBody + gBodyLen, buf, size); gBodyLen += size; }
            URLReleaseBuffer(gRef, buf);
            if (size <= 0) break;
            URLGetCurrentState(gRef, &state);
            if (state != kURLDataAvailableState) break;
        }
    }
    if (state == kURLTransactionCompleteState || state == kURLCompletedState) fetch_done(1);
    else if (state == kURLErrorOccurredState) fetch_done(0);
    else if (TickCount() - gFetchStart > 30 * TICKS) { URLAbort(gRef); fetch_done(0); }
}

/* ------------------------------------------------------------ playback */

static Movie gMovie;
static int gWantPlay, gStarted;
static char gPlayPath[64];
static long gPlayStart, gLastMoveTick, gLastTime, gRetryAt;
static int gFailures;                  /* consecutive failed attempts for this station */

static void stop_movie(void)
{
    if (gMovie) { StopMovie(gMovie); DisposeMovie(gMovie); gMovie = NULL; }
    gStarted = 0;
}

static void open_stream(void)
{
    char url[256];
    Handle h;
    short id = 0;
    GrafPtr old;
    stop_movie();
    sprintf(url, "icy://%s%s", gRelay, gPlayPath);
    h = NewHandle((Size)strlen(url) + 1);
    if (!h) return;
    HLock(h); strcpy(*h, url); HUnlock(h);
    GetPort(&old); SetPort(gWin);
    if (NewMovieFromDataRef(&gMovie, newMovieActive | newMovieAsyncOK, &id, h, 'url ') != noErr) gMovie = NULL;
    if (gMovie) {   /* audio only: QuickTime must not draw its logo or text into the window */
        Rect none;
        SetRect(&none, 0, 0, 0, 0);
        SetMovieBox(gMovie, &none);
    }
    SetPort(old);
    DisposeHandle(h);
    gPlayStart = gLastMoveTick = TickCount(); gLastTime = -1;
    strcpy(gStatus, gMovie ? "Connecting..." : "QuickTime could not open the stream");
    if (!gMovie) gRetryAt = TickCount() + 10 * TICKS;
    draw_status();
}

static void play(const char *path, const char *name, const char *remember)
{
    strncpy(gPlayPath, path, sizeof gPlayPath - 1);
    strncpy(gNow, name, sizeof gNow - 1);
    strncpy(gLast, remember, sizeof gLast - 1);
    gWantPlay = 1; gRetryAt = 0; gFailures = 0;
    save_settings();
    open_stream();
}

static void stop_play(void)
{
    gWantPlay = 0;
    stop_movie();
    strcpy(gStatus, "Stopped");
    gLast[0] = 0; save_settings();
    draw_status();
}

/* keeps the stream alive: start when playable, reconnect on error or stall */
static void play_poll(void)
{
    long now = TickCount();
    if (!gWantPlay) return;
    if (!gMovie) { if (gRetryAt && now >= gRetryAt) { gRetryAt = 0; open_stream(); } return; }
    MoviesTask(NULL, 0);
    {
        long st = GetMovieLoadState(gMovie);
        if (st < 0) {
            stop_movie();
            if (++gFailures >= 3) { gWantPlay = 0; strcpy(gStatus, "Station unavailable (the relay could not open it)"); draw_status(); return; }
            strcpy(gStatus, "Stream error - reconnecting in 10 s"); draw_status(); gRetryAt = now + 10 * TICKS; return;
        }
        if (!gStarted && st >= kMovieLoadStatePlayable) {
            Rect none;
            SetRect(&none, 0, 0, 0, 0);
            SetMovieBox(gMovie, &none);
            StartMovie(gMovie); gStarted = 1; gLastMoveTick = now; gFailures = 0;
            strcpy(gStatus, "Playing"); redraw_all();
        }
    }
    if (gStarted) {
        long t = GetMovieTime(gMovie, NULL);
        if (t != gLastTime) { gLastTime = t; gLastMoveTick = now; }
        else if (now - gLastMoveTick > 30 * TICKS) { strcpy(gStatus, "Stalled - reconnecting"); draw_status(); open_stream(); }
    } else if (now - gPlayStart > 45 * TICKS) { strcpy(gStatus, "No audio yet - reconnecting"); draw_status(); open_stream(); }
}

/* ------------------------------------------------------------ input */

static void start_search(void)
{
    char q[128], path[300], *o = path;
    int n = (**gSearch).teLength, i;
    if (n > 120) n = 120;
    memcpy(q, *(**gSearch).hText, n); q[n] = 0;
    o += sprintf(o, "/search?q=");
    for (i = 0; q[i]; i++) {
        unsigned char c = (unsigned char)q[i];
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) *o++ = c;
        else o += sprintf(o, "%%%02X", c);
    }
    *o = 0;
    list_clear(&gStations); strcpy(gStations.title, "Searching...");
    draw_list(&gStations);
    fetch(path, 2);
}

static void play_row(int row)
{
    char path[64], id[16];
    sprintf(path, "/play?id=%ld", gStations.rows[row].id);
    sprintf(id, "%ld", gStations.rows[row].id);
    play(path, gStations.rows[row].name, id);
}

static void list_click(List *l, Point pt, int dbl)
{
    int row = (pt.v - l->frame.top - 21) / 17;
    if (pt.v < l->frame.top + 21 || row < 0) return;
    row += l->top;
    if (row >= l->n) return;
    l->sel = row;
    draw_list(l);
    if (l == &gCountries) {
        char path[64];
        sprintf(path, "/stations?c=%ld", l->rows[row].id);
        list_clear(&gStations); sprintf(gStations.title, "%s", l->rows[row].name);
        draw_list(&gStations);
        fetch(path, 1);
    } else if (dbl) play_row(row);
}

static void scroll_click(List *l, ControlHandle c, short part, Point pt)
{
    int vis = visible_rows(l), max = l->n - vis;
    if (max < 0) max = 0;
    if (part == 129 /* inThumb */) {
        TrackControl(c, pt, NULL);
        l->top = GetControlValue(c);
    } else {
        do {
            int step = (part == 20 /* inUpButton */) ? -1 : (part == 21 /* inDownButton */) ? 1 :
                       (part == 22 /* inPageUp */) ? -vis : vis;
            l->top += step;
            if (l->top < 0) l->top = 0;
            if (l->top > max) l->top = max;
            draw_list(l);
            { unsigned long until = TickCount() + 4; while (TickCount() < until) ; }
        } while (StillDown());
    }
    draw_list(l);
}

static void content_click(EventRecord *ev)
{
    Point pt = ev->where;
    ControlHandle c;
    short part;
    static unsigned long lastClick;
    static Point lastPt;
    int dbl;
    SetPort(gWin);
    GlobalToLocal(&pt);
    dbl = (ev->when - lastClick < GetDblTime()) && abs(pt.h - lastPt.h) < 4 && abs(pt.v - lastPt.v) < 4;
    lastClick = ev->when; lastPt = pt;
    part = FindControl(pt, gWin, &c);
    if (c && part) {
        if (c == gCountries.bar) { scroll_click(&gCountries, c, part, pt); return; }
        if (c == gStations.bar) { scroll_click(&gStations, c, part, pt); return; }
        if (TrackControl(c, pt, NULL)) {
            if (c == gKexpBtn) play("/kexp", "KEXP 90.3 FM / Seattle", "kexp");
            else if (c == gStopBtn) stop_play();
            else if (c == gSearchBtn) start_search();
            else if (c == gPlayBtn && gStations.sel >= 0) play_row(gStations.sel);
        }
        return;
    }
    if (PtInRect(pt, &(**gSearch).viewRect)) { TEClick(pt, (ev->modifiers & shiftKey) != 0, gSearch); return; }
    if (PtInRect(pt, &gCountries.frame)) list_click(&gCountries, pt, dbl);
    else if (PtInRect(pt, &gStations.frame)) list_click(&gStations, pt, dbl);
}

/* ------------------------------------------------------------ setup */

enum { mApple = 128, mFile = 129, mEdit = 130 };
static MenuHandle gAppleMenu, gFileMenu, gEditMenu;

static void setup_menus(void)
{
    gAppleMenu = NewMenu(mApple, PSTR("\p\024"));
    AppendMenu(gAppleMenu, PSTR("\pAbout R WorldRadio..."));
    AppendMenu(gAppleMenu, PSTR("\p(-"));
    AppendResMenu(gAppleMenu, 'DRVR');
    InsertMenu(gAppleMenu, 0);
    gFileMenu = NewMenu(mFile, PSTR("\pFile"));
    AppendMenu(gFileMenu, PSTR("\pPlay KEXP/K;Stop/.;(-;Quit/Q"));
    InsertMenu(gFileMenu, 0);
    gEditMenu = NewMenu(mEdit, PSTR("\pEdit"));
    AppendMenu(gEditMenu, PSTR("\pUndo/Z;(-;Cut/X;Copy/C;Paste/V;Clear"));
    InsertMenu(gEditMenu, 0);
    DrawMenuBar();
}

static void do_menu(long choice)
{
    short menu = choice >> 16, item = choice & 0xFFFF;
    if (menu == mApple && item > 2) { Str255 nm; GetMenuItemText(gAppleMenu, item, nm); OpenDeskAcc(nm); }
    else if (menu == mFile) {
        if (item == 1) play("/kexp", "KEXP 90.3 FM / Seattle", "kexp");
        else if (item == 2) stop_play();
        else if (item == 4) gQuit = 1;
    } else if (menu == mEdit) {
        if (item == 3) TECut(gSearch); else if (item == 4) TECopy(gSearch);
        else if (item == 5) TEPaste(gSearch); else if (item == 6) TEDelete(gSearch);
    }
    HiliteMenu(0);
}

static pascal OSErr quit_handler(const AppleEvent *e, AppleEvent *r, long refcon) { (void)e; (void)r; (void)refcon; gQuit = 1; return noErr; }
static pascal OSErr noop_handler(const AppleEvent *e, AppleEvent *r, long refcon) { (void)e; (void)r; (void)refcon; return noErr; }

static void setup_window(void)
{
    Rect r, view;
    SetRect(&r, 40, 60, 40 + 620, 60 + 460);
    gWin = NewCWindow(NULL, &r, PSTR("\pR WorldRadio"), true, documentProc, (WindowPtr)-1, true, 0);
    SetPort(gWin);
    SetRect(&r, 430, 14, 510, 34);
    gKexpBtn = NewControl(gWin, &r, PSTR("\pKEXP"), true, 0, 0, 1, pushButProc, 0);
    SetRect(&r, 520, 14, 600, 34);
    gStopBtn = NewControl(gWin, &r, PSTR("\pStop"), true, 0, 0, 1, pushButProc, 0);
    SetRect(&r, 310, 57, 390, 77);
    gSearchBtn = NewControl(gWin, &r, PSTR("\pSearch"), true, 0, 0, 1, pushButProc, 0);
    SetRect(&r, 520, 57, 600, 77);
    gPlayBtn = NewControl(gWin, &r, PSTR("\pPlay"), true, 0, 0, 1, pushButProc, 0);
    SetRect(&view, 13, 60, 297, 75);
    TextFont(gRomanFont); TextSize(12);
    gSearch = TENew(&view, &view);
    SetRect(&gCountries.frame, 10, 86, 210, 450);
    SetRect(&gStations.frame, 220, 86, 610, 450);
    strcpy(gCountries.title, "Countries");
    strcpy(gStations.title, "Stations (double-click to play)");
    gCountries.sel = gStations.sel = -1;
    SetRect(&r, 195, 86, 211, 450);
    gCountries.bar = NewControl(gWin, &r, PSTR("\p"), true, 0, 0, 0, scrollBarProc, 0);
    SetRect(&r, 595, 86, 611, 450);
    gStations.bar = NewControl(gWin, &r, PSTR("\p"), true, 0, 0, 0, scrollBarProc, 0);
}

int main(void)
{
    ProcessSerialNumber me;
    ProcessInfoRec info;
    EventRecord ev;
    int i;
    MaxApplZone();
    for (i = 0; i < 8; i++) MoreMasters();
    InitGraf(&qd.thePort); InitFonts(); InitWindows(); InitMenus(); TEInit(); InitDialogs(NULL); InitCursor();
    FlushEvents(everyEvent, 0);
    GetCurrentProcess(&me);
    memset(&info, 0, sizeof info);
    info.processInfoLength = sizeof info; info.processAppSpec = &gApp;
    GetProcessInformation(&me, &info);
    AEInstallEventHandler(kCoreEventClass, kAEQuitApplication, NewAEEventHandlerUPP(quit_handler), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenApplication, NewAEEventHandlerUPP(noop_handler), 0, false);
    AEInstallEventHandler(kCoreEventClass, kAEOpenDocuments, NewAEEventHandlerUPP(noop_handler), 0, false);
    GetFNum(PSTR("\pAppleGothic"), &gKoreanFont);
    GetFNum(PSTR("\pGeneva"), &gRomanFont);
    if (!gKoreanFont) gKoreanFont = gRomanFont;
    load_settings();
    setup_menus();
    if (EnterMovies() != noErr) { SysBeep(10); ExitToShell(); }
    setup_window();
    redraw_all();
    fetch("/countries", 0);
    if (!strcmp(gLast, "kexp")) play("/kexp", "KEXP 90.3 FM / Seattle", "kexp");
    else if (gLast[0]) { char path[64]; sprintf(path, "/play?id=%s", gLast); play(path, gNow, gLast); }

    while (!gQuit) {
        if (WaitNextEvent(everyEvent, &ev, (gWantPlay || gRef) ? 2 : 20, NULL)) {
            switch (ev.what) {
            case mouseDown: {
                WindowPtr w;
                short part = FindWindow(ev.where, &w);
                if (part == inMenuBar) do_menu(MenuSelect(ev.where));
                else if (part == inSysWindow) SystemClick(&ev, w);
                else if (part == inDrag) { Rect b = qd.screenBits.bounds; DragWindow(w, ev.where, &b); }
                else if (part == inGoAway) { if (TrackGoAway(w, ev.where)) gQuit = 1; }
                else if (part == inContent) { if (w != FrontWindow()) SelectWindow(w); else content_click(&ev); }
                break;
            }
            case keyDown: case autoKey: {
                char ch = ev.message & charCodeMask;
                if (ev.modifiers & cmdKey) do_menu(MenuKey(ch));
                else if (ch == '\r' || ch == 3) start_search();
                else { SetPort(gWin); TEKey(ch, gSearch); }
                break;
            }
            case updateEvt:
                if ((WindowPtr)ev.message == gWin) { BeginUpdate(gWin); redraw_all(); EndUpdate(gWin); }
                break;
            case activateEvt:
                if (ev.modifiers & activeFlag) TEActivate(gSearch); else TEDeactivate(gSearch);
                break;
            case kHighLevelEvent:
                AEProcessAppleEvent(&ev);
                break;
            }
        }
        if (gWin == FrontWindow()) TEIdle(gSearch);
        fetch_poll();
        play_poll();
    }
    stop_movie();
    return 0;
}
