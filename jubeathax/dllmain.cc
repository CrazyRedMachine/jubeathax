// clang-format off
#include <windows.h>
#include <psapi.h>
// clang-format on

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "minhook/hde32.h"
#include "minhook/include/MinHook.h"

#include "external_io.h"
#include "jubeathax/config.h"
#include "util/log.h"
#include "util/patch.h"
#include "util/xmlprop.hpp"

#define PROGRAM_VERSION "0.9"

FILE *g_log_fp = NULL;

struct jubeathax_config config = {};

PSMAP_BEGIN(config_psmap, static)
PSMAP_MEMBER_OPT(PSMAP_PROPERTY_TYPE_BOOL, struct jubeathax_config, disable_matching, "/jubeathax/disable_matching",
                 false)
PSMAP_MEMBER_OPT(PSMAP_PROPERTY_TYPE_BOOL, struct jubeathax_config, freeze_menu_timer, "/jubeathax/freeze_menu_timer",
                 false)
PSMAP_MEMBER_OPT(PSMAP_PROPERTY_TYPE_BOOL, struct jubeathax_config, freeze_result_timer,
                 "/jubeathax/freeze_result_timer", false)
PSMAP_MEMBER_OPT(PSMAP_PROPERTY_TYPE_BOOL, struct jubeathax_config, skip_tutorial, "/jubeathax/skip_tutorial", false)
PSMAP_MEMBER_OPT(PSMAP_PROPERTY_TYPE_BOOL, struct jubeathax_config, external_io, "/jubeathax/external_io", false)
PSMAP_END

static bool patch_disable_matching()
{
    DWORD dllSize = 0;
    char *data = getDllData("jubeat.dll", &dllSize);

    int64_t first_loc = _search(data, dllSize, "\x00\x8B\xD7\x33\xC9\xE8", 6, 0);
    if (first_loc == -1)
    {
        return false;
    }

    int64_t pattern_offset = _search(data, 0x15, "\x84", 1, first_loc);
    if (pattern_offset == -1)
    {
        return false;
    }

    uint64_t patch_addr = (int64_t)data + pattern_offset;
    patch_memory(patch_addr, (char *)"\x85", 1);

    LOG("jubeathax: matching disabled\n");

    return true;
}

static bool patch_result_timer()
{
    DWORD dllSize = 0;
    char *data = getDllData("jubeat.dll", &dllSize);

    int64_t first_loc = 0;

    {
        first_loc = _search(data, dllSize, "\x8B\xF0\x32\xDB\x6A\x03\x6A\x02", 8, 0);
        if (first_loc == -1)
        {
            return false;
        }
    }

    {
        int64_t pattern_offset = _search(data, dllSize, "\x00\x75", 2, first_loc);
        if (pattern_offset == -1)
        {
            return false;
        }

        uint64_t patch_addr = (int64_t)data + pattern_offset;
        patch_memory(patch_addr, (char *)"\x00\xEB", 2);
    }

    LOG("jubeathax: result timer frozen at 0 seconds\n");

    return true;
}

static bool patch_menu_timer()
{
    DWORD dllSize = 0;
    char *data = getDllData("jubeat.dll", &dllSize);

    {
        int64_t pattern_offset = _wildcard_search(data, dllSize, "\x01\x00\x84\xC0\x75?\x38\x05", 8, 0);
        if (pattern_offset == -1)
        {
            LOG("extra patches: cannot freeze menu timer\n");
            return false;
        }

        uint64_t patch_addr = (int64_t)data + pattern_offset;
        patch_memory(patch_addr, (char *)"\x01\x00\x84\xC0\xEB", 5);

        LOG("extra patches: menu timer frozen\n");
    }

    return true;
}

static bool patch_skip_tutorial()
{
    DWORD dllSize = 0;
    char *data = getDllData("jubeat.dll", &dllSize);

    {
        int64_t pattern_offset = _search(data, dllSize, "\x6A\x01\x8B\xC8\xFF\x15\xD0", 7, 0);
        if (pattern_offset == -1)
        {
            LOG("extra patches: cannot perform skip tutorial patch\n");
            return false;
        }

        uint64_t patch_addr = (int64_t)data + pattern_offset + 0xC;
        patch_memory(patch_addr, (char *)"\xE9\x80\x01\x00\x00\x90", 6);
        LOG("extra patches: tutorial skipped\n");
        return true;
    }
    return false;
}

BOOL APIENTRY DllMain(HMODULE hModule, DWORD ul_reason_for_call, LPVOID lpReserved)
{
    switch (ul_reason_for_call)
    {
    case DLL_PROCESS_ATTACH: {
        g_log_fp = fopen("jubeathax.log", "w");
        if (g_log_fp == NULL)
        {
            LOG("cannot open jubeathax.log for write, output to stderr only\n");
        }
        LOG("== jubeathax version " PROGRAM_VERSION " ==\n");
        LOG("jubeathax: Initializing\n");

        if (MH_Initialize() != MH_OK)
        {
            LOG("Failed to initialize minhook\n");
            exit(1);
            return TRUE;
        }

        _load_config("jubeathax.xml", &config, config_psmap);

        if (config.disable_matching)
        { // TODO: problem with guest mode
            patch_disable_matching();
        }

        if (config.freeze_result_timer)
        {
            patch_result_timer();
        }

        if (config.freeze_menu_timer)
        {
            patch_menu_timer();
        }

        if (config.skip_tutorial)
        {
            patch_skip_tutorial();
        }

        if (config.external_io)
        {
            patch_use_external_io();
        }

        MH_EnableHook(MH_ALL_HOOKS);

        if (g_log_fp != stderr)
            fclose(g_log_fp);

        break;
    }

    case DLL_THREAD_ATTACH:
    case DLL_THREAD_DETACH:
    case DLL_PROCESS_DETACH:
        break;
    }

    return TRUE;
}
