#include <windows.h>
#include <psapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>

#include "io_dummy.h"

extern "C" __declspec(dllexport) int io_plugin_get_api_version()
{
    return 1;
}

extern "C" __declspec(dllexport) uint32_t io_plugin_get_input()
{
    return 0;
}

extern "C" __declspec(dllexport) bool io_plugin_open_input()
{
    return true;
}

extern "C" __declspec(dllexport) bool io_plugin_open_output()
{
    return true;
}

extern "C" __declspec(dllexport) bool io_plugin_init()
{
    return true;
}

extern "C" __declspec(dllexport) bool io_plugin_deinit()
{
    return true;
}

extern "C" __declspec(dllexport) bool io_plugin_board_update(uint32_t note_data, cell_state_t new_status,
                                                             void *extra_data)
{
    printf("RECEIVED PLUGIN BOARD UPDATE FOR NOTE %08x\n", note_data);

    switch (new_status)
    {
    /* Button animation */
    case WAITING_FOR_PRESS:
    case MISSED:
    case OUTSIDE:
    case GOOD:
    case VERY_GOOD:
    case PERFECT:
        break;
    case LONG_TRAIL_DRAW_FIRST:
    case LONG_TRAIL_DRAW_CONT:
        break;

    /* Button presses */
    case PRESSED_OUTSIDE:
    case PRESSED_GOOD:
    case PRESSED_VERY_GOOD:
    case PRESSED_PERFECT:
        break;
    case LONG_NOTE_RELEASE_FIRST:
    case LONG_NOTE_RELEASE_CONT:
        break;
    case LONG_NOTE_MISS_FIRST:
    case LONG_NOTE_MISS_CONT:
        break;
    case LONG_TRAIL_UPDATE:
        break;

    /* Preview before song start */
    case PREVIEW:
        break;

    /* End of song */
    case FINAL_SCORE:
        break;
    case FULL_COMBO:
        break;
    case EXCELLENT:
        break;
    case FULL_COMBO_ANIM:
        break;
    case EXCELLENT_ANIM:
        break;
    case LEAVE_RESULT_SCREEN:
        break;

    /* Force off (after cooldown) */
    case INACTIVE:
        break;
    default:
        break;
    }

    return true;
}

BOOL WINAPI DllMain(HINSTANCE hinstDLL, DWORD fdwReason, LPVOID lpReserved)
{
    switch (fdwReason)
    {
    case DLL_PROCESS_ATTACH:
        break;

    case DLL_THREAD_ATTACH:
        break;

    case DLL_THREAD_DETACH:
        break;

    case DLL_PROCESS_DETACH:
        break;
    }

    return TRUE;
}
