/*
 * Windows Volume Mixer: show the game's own icon and name.
 *
 * The mixer labels an audio session with its process's executable icon as
 * the shell has it cached for that path, which can stay the stock psxrecomp
 * gamepad long after the exe's icon changed. Setting the session's icon path
 * (to assets/psxrecomp.ico next to the exe, else the exe's own icon) and its
 * display name puts "Vagrant Story Recompiled" with the app icon there.
 *
 * A background thread waits for this process's audio sessions to appear and
 * labels each new one (a device change opens a new session), checking every
 * few seconds. No game state is touched; other platforms compile nothing.
 */
#if defined(_WIN32)
#define COBJMACROS
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mmdeviceapi.h>
#include <audiopolicy.h>
#include <wchar.h>

/* The SDK declares these but no import library defines them for C. */
static const CLSID VS_CLSID_MMDeviceEnumerator =
    {0xBCDE0395, 0xE52F, 0x467C, {0x8E, 0x3D, 0xC4, 0x57, 0x92, 0x91, 0x69, 0x2E}};
static const IID VS_IID_IMMDeviceEnumerator =
    {0xA95664D2, 0x9614, 0x4F35, {0xA7, 0x46, 0xDE, 0x8D, 0xB6, 0x36, 0x17, 0xE6}};
static const IID VS_IID_IAudioSessionManager2 =
    {0x77AA99A0, 0x1BD6, 0x484F, {0x8B, 0xC7, 0x2C, 0x65, 0x4C, 0x9A, 0x9B, 0x6F}};
static const IID VS_IID_IAudioSessionControl2 =
    {0xBFB7FF88, 0x7239, 0x4FC9, {0x8F, 0xA2, 0x07, 0xC9, 0x50, 0xBE, 0x9C, 0x6D}};

#include "mod_plugins.h"

#define VS_AUDIO_NAME L"Vagrant Story Recompiled"

static wchar_t s_icon[MAX_PATH + 8];

static void resolve_icon_path(void) {
    wchar_t exe[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, exe, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) { s_icon[0] = 0; return; }
    wchar_t dir[MAX_PATH];
    wcscpy_s(dir, MAX_PATH, exe);
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (slash) slash[1] = 0;
    swprintf(s_icon, sizeof s_icon / sizeof s_icon[0], L"%lsassets\\psxrecomp.ico", dir);
    if (GetFileAttributesW(s_icon) == INVALID_FILE_ATTRIBUTES)
        swprintf(s_icon, sizeof s_icon / sizeof s_icon[0], L"%ls,0", exe);
}

/* Label this process's sessions on the default render device that are not
 * labelled yet. */
static void label_sessions(DWORD pid) {
    IMMDeviceEnumerator *en = NULL;
    IMMDevice *dev = NULL;
    IAudioSessionManager2 *mgr = NULL;
    IAudioSessionEnumerator *list = NULL;
    if (FAILED(CoCreateInstance(&VS_CLSID_MMDeviceEnumerator, NULL, CLSCTX_ALL,
                                &VS_IID_IMMDeviceEnumerator, (void **)&en)))
        return;
    if (SUCCEEDED(IMMDeviceEnumerator_GetDefaultAudioEndpoint(en, eRender, eConsole, &dev)) &&
        SUCCEEDED(IMMDevice_Activate(dev, &VS_IID_IAudioSessionManager2, CLSCTX_ALL, NULL,
                                     (void **)&mgr)) &&
        SUCCEEDED(IAudioSessionManager2_GetSessionEnumerator(mgr, &list))) {
        int count = 0;
        IAudioSessionEnumerator_GetCount(list, &count);
        for (int i = 0; i < count; i++) {
            IAudioSessionControl *ctl = NULL;
            IAudioSessionControl2 *ctl2 = NULL;
            if (FAILED(IAudioSessionEnumerator_GetSession(list, i, &ctl))) continue;
            if (SUCCEEDED(IAudioSessionControl_QueryInterface(ctl, &VS_IID_IAudioSessionControl2,
                                                              (void **)&ctl2))) {
                DWORD spid = 0;
                if (SUCCEEDED(IAudioSessionControl2_GetProcessId(ctl2, &spid)) && spid == pid) {
                    LPWSTR cur = NULL;
                    int done = SUCCEEDED(IAudioSessionControl_GetIconPath(ctl, &cur)) &&
                               cur && wcscmp(cur, s_icon) == 0;
                    if (cur) CoTaskMemFree(cur);
                    if (!done) {
                        IAudioSessionControl_SetIconPath(ctl, s_icon, NULL);
                        IAudioSessionControl_SetDisplayName(ctl, VS_AUDIO_NAME, NULL);
                    }
                }
                IAudioSessionControl2_Release(ctl2);
            }
            IAudioSessionControl_Release(ctl);
        }
    }
    if (list) IAudioSessionEnumerator_Release(list);
    if (mgr) IAudioSessionManager2_Release(mgr);
    if (dev) IMMDevice_Release(dev);
    IMMDeviceEnumerator_Release(en);
}

static DWORD WINAPI audio_label_thread(LPVOID arg) {
    (void)arg;
    if (FAILED(CoInitializeEx(NULL, COINIT_MULTITHREADED))) return 0;
    resolve_icon_path();
    const DWORD pid = GetCurrentProcessId();
    for (;;) {
        if (s_icon[0]) label_sessions(pid);
        Sleep(3000);
    }
}

PSX_MOD_CONSTRUCTOR(vs_audio_session_label) {
    HANDLE t = CreateThread(NULL, 0, audio_label_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
#endif
