/*
 * Support for Windows via the WM_INPUT message.
 *
 * Please see the file LICENSE in the source's root directory.
 *
 *  This file written by Ryan C. Gordon.
 */

#if (defined(_WIN32) || defined(__CYGWIN__))

/* WinUser.h won't include rawinput stuff without this... */
#if (_WIN32_WINNT < 0x0501)
#undef _WIN32_WINNT
#define _WIN32_WINNT 0x0501
#endif

#include <windows.h>
#include <malloc.h>  /* needed for alloca(). */

/* Cygwin's headers don't have WM_INPUT right now... */
#ifndef WM_INPUT
#define WM_INPUT 0x00FF
#endif

#include "manymouse.h"

/* that should be enough, knock on wood. */
#define MAX_MICE 32

/*
 * Just trying to avoid malloc() here...we statically allocate a buffer
 *  for events and treat it as a ring buffer.
 */
/* !!! FIXME: tweak this? */
#define MAX_EVENTS 1024
static ManyMouseEvent input_events[MAX_EVENTS];
static volatile int input_events_read = 0;
static volatile int input_events_write = 0;
static int available_mice = 0;
static int did_api_lookup = 0;
static HWND raw_hwnd = NULL;
static const char *class_name = "ManyMouseRawInputCatcher";
static const char *win_name = "ManyMouseRawInputMsgWindow";
static ATOM class_atom = 0;

typedef struct
{
    HANDLE handle;
    char name[256];
} MouseStruct;
static MouseStruct mice[MAX_MICE];


/*
 * The RawInput APIs only exist in Windows XP and later, so you want this
 *  to fail gracefully on earlier systems instead of refusing to start the
 *  process due to missing symbols. To this end, we do a symbol lookup on
 *  User32.dll to get the entry points.
 */
static UINT (WINAPI *pGetRawInputDeviceList)(
    PRAWINPUTDEVICELIST pRawInputDeviceList,
    PUINT puiNumDevices,
    UINT cbSize
);
/* !!! FIXME: use unicode version */
static UINT (WINAPI *pGetRawInputDeviceInfoA)(
    HANDLE hDevice,
    UINT uiCommand,
    LPVOID pData,
    PUINT pcbSize
);
static BOOL (WINAPI *pRegisterRawInputDevices)(
    PCRAWINPUTDEVICE pRawInputDevices,
    UINT uiNumDevices,
    UINT cbSize
);
static LRESULT (WINAPI *pDefRawInputProc)(
    PRAWINPUT *paRawInput,
    INT nInput,
    UINT cbSizeHeader
);
static UINT (WINAPI *pGetRawInputBuffer)(
    PRAWINPUT pData,
    PUINT pcbSize,
    UINT cbSizeHeader
);
static UINT (WINAPI *pGetRawInputData)(
    HRAWINPUT hRawInput,
    UINT uiCommand,
    LPVOID pData,
    PUINT pcbSize,
    UINT cbSizeHeader
);
static HHOOK (WINAPI *pSetWindowsHookExA)(
    int idHook,
    HOOKPROC lpfn,
    HINSTANCE hMod,
    DWORD dwThreadId
);
static BOOL (WINAPI *pUnhookWindowsHookEx)(
    HHOOK hhk
);
static LRESULT (WINAPI *pCallNextHookEx)(
    HHOOK hhk,
    int nCode,
    WPARAM wParam,
    LPARAM lParam
);

static int symlookup(HMODULE dll, void **addr, const char *sym)
{
    *addr = GetProcAddress(dll, sym);
    if (*addr == NULL)
    {
        FreeLibrary(dll);
        return(0);
    } /* if */

    return(1);
} /* symlookup */

static int find_api_symbols(void)
{
    HMODULE dll;

    if (did_api_lookup)
        return(1);

    dll = LoadLibrary("user32.dll");
    if (dll == NULL)
        return(0);

    #define LOOKUP(x) { if (!symlookup(dll, (void **) &p##x, #x)) return(0); }
    LOOKUP(GetRawInputDeviceInfoA);
    LOOKUP(RegisterRawInputDevices);
    LOOKUP(GetRawInputDeviceList);
    LOOKUP(DefRawInputProc);
    LOOKUP(GetRawInputBuffer);
    LOOKUP(GetRawInputData);
    LOOKUP(SetWindowsHookExA);
    LOOKUP(UnhookWindowsHookEx);
    LOOKUP(CallNextHookEx);
    #undef LOOKUP

    /* !!! FIXME: store user32dll and free it on quit? */

    did_api_lookup = 1;
    return(1);
} /* find_api_symbols */


#if 0
static const char *win32strerror(void)
{
    static TCHAR msgbuf[255];
    TCHAR *ptr = msgbuf;

    FormatMessage(
        FORMAT_MESSAGE_FROM_SYSTEM |
        FORMAT_MESSAGE_IGNORE_INSERTS,
        NULL,
        GetLastError(),
        MAKELANGID(LANG_NEUTRAL, SUBLANG_DEFAULT), /* Default language */
        msgbuf,
        sizeof (msgbuf) / sizeof (TCHAR),
        NULL 
    );

        /* chop off newlines. */
    for (ptr = msgbuf; *ptr; ptr++)
    {
        if ((*ptr == '\n') || (*ptr == '\r'))
        {
            *ptr = ' ';
            break;
        } /* if */
    } /* for */

    return((const char *) msgbuf);
} /* win32strerror */
#endif


static void init_mouse(const RAWINPUTDEVICELIST *dev)
{
    char *buf = NULL;
    size_t bufsize = 0;
    UINT ct = 0;
    MouseStruct *mouse = &mice[available_mice];

    if (dev->dwType != RIM_TYPEMOUSE)
        return;  /* keyboard or some other fruity thing. */

    if (pGetRawInputDeviceInfoA(dev->hDevice, RIDI_DEVICENAME, NULL, &ct) < 0)
        return;

    /* !!! FIXME: ct == char count, not byte count...just guess for now. */
    bufsize = 4 * (ct + 1);  /* !!! FIXME: Seriously, this sucks. */
    buf = (char *) alloca(bufsize);
    ZeroMemory(buf, bufsize);
    if (pGetRawInputDeviceInfoA(dev->hDevice, RIDI_DEVICENAME, buf, &ct) < 0)
        return;

    ZeroMemory(mouse, sizeof (MouseStruct));

    /* !!! FIXME: Convert to UTF-8 here. */
    CopyMemory(mouse->name, buf, sizeof (mouse->name) - 1);
    mouse->name[sizeof (mouse->name) - 1] = '\0';
    mouse->handle = dev->hDevice;
    available_mice++;  /* we're good. */
} /* init_mouse */


static void queue_event(const ManyMouseEvent *event)
{
    input_events_write = ((input_events_write + 1) % MAX_EVENTS);

    /* Ring buffer full? Lose oldest event. */
    if (input_events_write == input_events_read)
    {
        /* !!! FIXME: we need to not lose mouse buttons here. */
        OutputDebugString("ring buffer is full!\n");
        input_events_read = ((input_events_read + 1) % MAX_EVENTS);
    } /* if */

    /* copy the event info. We'll process it in ManyMouse_PollEvent(). */
    CopyMemory(&input_events[input_events_write], event, sizeof (ManyMouseEvent));
} /* queue_event */


static void queue_from_rawinput(const RAWINPUT *raw)
{
    int i;
    const RAWINPUTHEADER *header = &raw->header;
    const RAWMOUSE *mouse = &raw->data.mouse;
    ManyMouseEvent event;

    if (raw->header.dwType != RIM_TYPEMOUSE)
        return;

    for (i = 0; i < available_mice; i++)  /* find the device for event. */
    {
        if (mice[i].handle == header->hDevice)
            break;
    } /* for */

    if (i == available_mice)
        return;  /* not found?! */

    /*
     * RAWINPUT packs a bunch of events into one, so we split it up into
     *  a bunch of ManyMouseEvents here and store them in an internal queue.
     *  Then ManyMouse_PollEvent() just shuffles items off that queue
     *  without any complicated processing.
     */

    event.device = i;

    if (mouse->usFlags & MOUSE_MOVE_ABSOLUTE)
    {
        /* !!! FIXME: How do we get the min and max values for absmotion? */
        event.type = MANYMOUSE_EVENT_ABSMOTION;
        event.item = 0;
        event.value = mouse->lLastX;
        queue_event(&event);
        event.item = 1;
        event.value = mouse->lLastY;
        queue_event(&event);
        OutputDebugString("absolute motion\n");
    } /* if */

    else /*if (mouse->usFlags & MOUSE_MOVE_RELATIVE)*/
    {
        event.type = MANYMOUSE_EVENT_RELMOTION;
        if (mouse->lLastX != 0)
        {
            event.item = 0;
            event.value = mouse->lLastX;
            queue_event(&event);
            OutputDebugString("relative X motion\n");
        } /* if */

        if (mouse->lLastY != 0)
        {
            event.item = 1;
            event.value = mouse->lLastY;
            queue_event(&event);
            OutputDebugString("relative Y motion\n");
        } /* if */
    } /* else if */

    event.type = MANYMOUSE_EVENT_BUTTON;

    #define QUEUE_BUTTON(x) { \
        if (mouse->usButtonFlags & RI_MOUSE_BUTTON_##x##_DOWN) { \
            event.item = x-1; \
            event.value = 1; \
            queue_event(&event); \
            OutputDebugString("button " #x " down\n"); \
        } \
        if (mouse->usButtonFlags & RI_MOUSE_BUTTON_##x##_UP) { \
            event.item = x-1; \
            event.value = 0; \
            queue_event(&event); \
            OutputDebugString("button " #x " up\n"); \
        } \
    }

    QUEUE_BUTTON(1);
    QUEUE_BUTTON(2);
    QUEUE_BUTTON(3);
    QUEUE_BUTTON(4);
    QUEUE_BUTTON(5);

    #undef QUEUE_BUTTON

    if (mouse->usButtonFlags & RI_MOUSE_WHEEL)
    {
        if (mouse->usButtonData != 0)  /* !!! FIXME: can this ever be zero? */
        {
            OutputDebugString("wheel\n");
            event.type = MANYMOUSE_EVENT_SCROLL;
            event.item = 0;
            event.value = (mouse->usButtonData > 0) ? 1 : -1;
            queue_event(&event);
        } /* if */
    } /* if */
} /* queue_from_rawinput */


static void wminput_handler(WPARAM wParam, LPARAM lParam)
{
    UINT dwSize = 0;
    LPBYTE lpb;

    pGetRawInputData((HRAWINPUT) lParam, RID_INPUT, NULL, &dwSize,
                      sizeof (RAWINPUTHEADER));

    if (dwSize < sizeof (RAWINPUT))
        return;  /* unexpected packet? */

    lpb = (LPBYTE) alloca(dwSize);
    if (lpb == NULL) 
        return;
    if (pGetRawInputData((HRAWINPUT) lParam, RID_INPUT, lpb, &dwSize,
                          sizeof (RAWINPUTHEADER)) != dwSize)
        return;

    queue_from_rawinput((RAWINPUT *) lpb);
} /* wminput_handler */


static LRESULT CALLBACK RawWndProc(HWND hWnd, UINT Msg, WPARAM wParam, LPARAM lParam)
{
    if (Msg == WM_INPUT)
        wminput_handler(wParam, lParam);

    else if (Msg == WM_DESTROY)
        return(0);

    return DefWindowProc(hWnd, Msg, wParam, lParam);
} /* WndProc */


static int init_event_queue(void)
{
    HINSTANCE hInstance = GetModuleHandle(NULL);
    WNDCLASSEX wce;
    RAWINPUTDEVICE rid;

    ZeroMemory(input_events, sizeof (input_events));
    input_events_read = input_events_write = 0;

    ZeroMemory(&wce, sizeof (wce));
    wce.cbSize = sizeof(WNDCLASSEX);
    wce.lpfnWndProc = RawWndProc;
    wce.lpszClassName = class_name;
    wce.hInstance = hInstance;
    class_atom = RegisterClassEx(&wce);
    if (class_atom == 0)
        return(0);

    raw_hwnd = CreateWindow(class_name, win_name, WS_OVERLAPPEDWINDOW,
                        CW_USEDEFAULT, CW_USEDEFAULT, CW_USEDEFAULT,
                        CW_USEDEFAULT, HWND_MESSAGE, NULL, hInstance, NULL);

    if (raw_hwnd == NULL)
        return(0);

    ZeroMemory(&rid, sizeof (rid));
    rid.usUsagePage = 1; /* GenericDesktop page */
    rid.usUsage = 2; /* GeneralDestop Mouse usage. */
    rid.dwFlags = RIDEV_INPUTSINK;
    rid.hwndTarget = raw_hwnd;
    if (!pRegisterRawInputDevices(&rid, 1, sizeof (rid)))
        return(0);

    return(1);
} /* init_event_queue */


static void cleanup_window(void)
{
    if (raw_hwnd)
    {
        MSG Msg;
        DestroyWindow(raw_hwnd);
        while (PeekMessage(&Msg, raw_hwnd, 0, 0, PM_REMOVE))
        {
            TranslateMessage(&Msg);
            DispatchMessage(&Msg);
        } /* while */
        raw_hwnd = 0;
    } /* if */

    if (class_atom)
    {
        UnregisterClass(class_name, GetModuleHandle(NULL));
        class_atom = 0;
    } /* if */
} /* cleanup_window */


static int windows_wminput_init(void)
{
    RAWINPUTDEVICELIST *devlist = NULL;
    UINT ct = 0;
    UINT i;

    available_mice = 0;

    if (!find_api_symbols())  /* only supported on WinXP and later. */
        return(0);

    pGetRawInputDeviceList(NULL, &ct, sizeof (RAWINPUTDEVICELIST));
    if (ct == 0)  /* no devices. */
        return(0);

    devlist = (PRAWINPUTDEVICELIST) alloca(sizeof (RAWINPUTDEVICELIST) * ct);
    pGetRawInputDeviceList(devlist, &ct, sizeof (RAWINPUTDEVICELIST));
    for (i = 0; i < ct; i++)
        init_mouse(&devlist[i]);

    if (!init_event_queue())
    {
        cleanup_window();
        available_mice = 0;
    } /* if */

    return(available_mice);
} /* windows_wminput_init */


static void windows_wminput_quit(void)
{
    /* unregister WM_INPUT devices... */
    RAWINPUTDEVICE rid;
    ZeroMemory(&rid, sizeof (rid));
    rid.usUsagePage = 1; /* GenericDesktop page */
    rid.usUsage = 2; /* GeneralDestop Mouse usage. */
    rid.dwFlags |= RIDEV_REMOVE;
    pRegisterRawInputDevices(&rid, 1, sizeof (rid));
    cleanup_window();
    available_mice = 0;
} /* windows_wminput_quit */


static const char *windows_wminput_name(unsigned int index)
{
    if (index < available_mice)
        return mice[index].name;
    return(NULL);
} /* windows_wminput_name */


static int windows_wminput_poll(ManyMouseEvent *outevent)
{
    MSG Msg;  /* run the queue for WM_INPUT messages, etc ... */

    /* ...favor existing events in the queue... */
    if (input_events_read != input_events_write)  /* no events if equal. */
    {
        CopyMemory(outevent, &input_events[input_events_read], sizeof (*outevent));
        input_events_read = ((input_events_read + 1) % MAX_EVENTS);
        return(1);
    } /* if */

    /* pump Windows for new hardware events... */
    while (PeekMessage(&Msg, raw_hwnd, 0, 0, PM_REMOVE))
    {
        TranslateMessage(&Msg);
        DispatchMessage(&Msg);
    } /* while */

    /* In case something new came in, give it to the app... */
    if (input_events_read != input_events_write)  /* no events if equal. */
    {
        /* take event off the queue. */
        CopyMemory(outevent, &input_events[input_events_read], sizeof (*outevent));
        input_events_read = ((input_events_read + 1) % MAX_EVENTS);
        return(1);
    } /* if */

    return(0);  /* no events at the moment. */
} /* windows_wminput_poll */


ManyMouseDriver ManyMouseDriver_windows =
{
    windows_wminput_init,
    windows_wminput_quit,
    windows_wminput_name,
    windows_wminput_poll
};

#endif  /* ifdef WINDOWS blocker */

/* end of windows_wminput.c ... */

