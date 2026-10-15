/*
 * Compile the game's overlays from the player's disc ahead of play.
 *
 * The disc-code build ships no game code, so BATTLE.PRG, the menus and the
 * title screen are compiled on the player's PC the first time each page of
 * them runs, and run on the interpreter until then. This starts
 * overlay_toolchain/vs_overlay_precompile.py once per install, in the
 * background at low priority: it reads every known overlay page from the disc
 * (tools/vs_make_overlay_seeds.py lists them, without their bytes) and
 * compiles them into cache/ with the same compiler and cache namespace the
 * runtime uses. Shards appear at the loader's next cache rescan (after any
 * runtime compile) and on every later launch. A marker in cache/ makes later
 * launches skip the work; an interrupted run resumes, since finished shards
 * are kept.
 *
 * Builds that link game C (or static overlays) have nothing to precompile.
 */
#if defined(_WIN32) && defined(PSX_DISC_CODE)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <shellapi.h>
#include <stdio.h>
#include <wchar.h>

#include "mod_plugins.h"
#include "overlay_loader.h"

#ifndef PSX_OVERLAY_FLAVOR
#define PSX_OVERLAY_FLAVOR 0
#endif

extern int g_psx_cps_mode;   /* set by the generated BIOS dispatch at init */

/* Let the game get through boot before competing for the CPU. */
#define VS_PRECOMPILE_DELAY_MS 15000

static DWORD WINAPI precompile_thread(LPVOID arg) {
    (void)arg;
    wchar_t dir[MAX_PATH], py[MAX_PATH + 64], script[MAX_PATH + 64];
    DWORD n = GetModuleFileNameW(NULL, dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return 0;
    wchar_t *slash = wcsrchr(dir, L'\\');
    if (!slash) return 0;
    *slash = 0;
    swprintf(py, sizeof py / sizeof py[0], L"%ls\\overlay_toolchain\\python\\python.exe", dir);
    swprintf(script, sizeof script / sizeof script[0],
             L"%ls\\overlay_toolchain\\vs_overlay_precompile.py", dir);
    if (GetFileAttributesW(py) == INVALID_FILE_ATTRIBUTES ||
        GetFileAttributesW(script) == INVALID_FILE_ATTRIBUTES)
        return 0;

    Sleep(VS_PRECOMPILE_DELAY_MS);

    /* A disc given on the command line wins over the launcher's disc.cfg. */
    const wchar_t *disc = L"";
    int argc = 0;
    LPWSTR *argv = CommandLineToArgvW(GetCommandLineW(), &argc);
    for (int i = 1; argv && i + 1 < argc; i++)
        if (wcscmp(argv[i], L"--disc") == 0) disc = argv[i + 1];

    wchar_t cmd[6 * MAX_PATH + 256];
    swprintf(cmd, sizeof cmd / sizeof cmd[0],
             L"\"%ls\" \"%ls\" --exe-dir \"%ls\" --flavor %d --arch-abi %hs%ls --disc \"%ls\"",
             py, script, dir, (int)PSX_OVERLAY_FLAVOR, PSX_OVERLAY_ARCH_ABI,
             g_psx_cps_mode ? L" --cps" : L"", disc);
    if (argv) LocalFree(argv);

    /* The compile dies with the game; finished shards stay and the next
     * launch carries on. */
    HANDLE job = CreateJobObjectW(NULL, NULL);
    if (job) {
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION lim;
        ZeroMemory(&lim, sizeof lim);
        lim.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
        SetInformationJobObject(job, JobObjectExtendedLimitInformation, &lim, sizeof lim);
    }
    STARTUPINFOW si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof si);
    si.cb = sizeof si;
    if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE,
                        CREATE_NO_WINDOW | BELOW_NORMAL_PRIORITY_CLASS | CREATE_SUSPENDED,
                        NULL, dir, &si, &pi)) {
        if (job) CloseHandle(job);
        return 0;
    }
    if (job) AssignProcessToJobObject(job, pi.hProcess);
    ResumeThread(pi.hThread);
    CloseHandle(pi.hThread);
    fprintf(stdout, "vagrantstory: compiling overlays from the disc in the background\n");
    fflush(stdout);
    WaitForSingleObject(pi.hProcess, INFINITE);
    CloseHandle(pi.hProcess);
    /* The job handle stays open for the process lifetime: closing it kills
     * nothing now, but keeping it is what ties a running compile to us. */
    return 0;
}

PSX_MOD_CONSTRUCTOR(vs_overlay_precompile_start) {
    HANDLE t = CreateThread(NULL, 0, precompile_thread, NULL, 0, NULL);
    if (t) CloseHandle(t);
}
#endif
