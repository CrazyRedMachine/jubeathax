#include <windows.h>
#include <psapi.h>

#include <stdbool.h>
#include <stdint.h>

#include "cell_state.h"
#include "external_io.h"

#include "util/log.h"
#include "util/patch.h"

#define API_VERSION                                                                                                    \
    1 // expected version from io plugins. Won't load if the plugin is more recent than jubeathax, but will still
      // attempt if jubeathax is more recent.

uint32_t (*plugin_get_input)() = NULL;
int (*plugin_get_api_version)() = NULL;
bool (*plugin_init)() = NULL;
bool (*plugin_deinit)() = NULL;
bool (*plugin_open_input)() = NULL;
bool (*plugin_open_output)() = NULL;
bool (*plugin_board_update)(uint32_t, cell_state_t, void *) = NULL;

#pragma #region "INPUT PATCHES"
uint32_t g_jb_input_state; // button state in jubeat (device.dll) format
void (*real_device_get_jamma)();
__declspec(naked) void hook_device_get_jamma()
{
    __asm("lea eax, [_g_jb_input_state]\n");
    __asm("ret\n");
}

void (*real_device_update)();
void hook_device_update()
{
    g_jb_input_state = plugin_get_input();
}

bool patch_inputs()
{
    HMODULE hinstLib = GetModuleHandleA("device.dll");
    _MH_CreateHook((LPVOID)GetProcAddress(hinstLib, "device_get_jamma"), (LPVOID)hook_device_get_jamma,
                   (void **)&real_device_get_jamma);
    _MH_CreateHook((LPVOID)GetProcAddress(hinstLib, "device_update"), (LPVOID)hook_device_update,
                   (void **)&real_device_update);

    LOG("jubeathax: device input functions hooked\n");
    return true;
}
#pragma #endregion

#pragma #region "HELPER PATCHES"
uint32_t g_latest_notedata = 0;
uint32_t g_longnote_data = 0;
float g_distratio = 0.0;
int g_judgement_idx = 0;

float g_longratio[16];
bool g_is_released[16] = {0}; // prevent the hook from sending multiple LONG_NOTE_RELEASE or LONG_NOTE_MISS messages to
                              // the dll (TODO: find a better hook for note release?)
bool g_is_drawn[16] = {0};    // prevent the hook from sending multiple LONG_TRAIL_DRAW messages to the dll

void (*real_parse_chart_tuto)();
__declspec(naked) void hook_parse_chart_tuto()
{
    /* this function is called while parsing chart blocks during tutorial, note data is at [edi-0x14]
       it's a good time to retrieve the current notedata for judgement check hooks since the info there is not so easily
       retrievable */
    __asm("push eax\n");
    __asm("mov eax, dword ptr[edi-0x14]\n");
    __asm("mov _g_latest_notedata, eax\n");
    __asm("pop eax\n");
    __asm("jmp [_real_parse_chart_tuto]\n");
}

void (*real_parse_chart_ingame)();
__declspec(naked) void hook_parse_chart_ingame()
{
    /* this function is called while parsing chart blocks but during game this time, note data is at [edi-0x14]
     */
    __asm("push eax\n");
    __asm("mov eax, dword ptr[edi-0x14]\n");
    __asm("mov _g_latest_notedata, eax\n");
    __asm("pop eax\n");
    __asm("jmp [_real_parse_chart_ingame]\n");
}

void (*real_parse_chart_ingame_festo)();
__declspec(naked) void hook_parse_chart_ingame_festo()
{
    /* this function is called while parsing chart blocks during game in jubeat festo, note data is at [edi]
     */
    __asm("push eax\n");
    __asm("mov eax, dword ptr[edi]\n");
    __asm("mov _g_latest_notedata, eax\n");
    __asm("pop eax\n");
    __asm("jmp [_real_parse_chart_ingame_festo]\n");
}

bool patch_retrieve_latest_notedata()
{
    /* These hooks update g_latest_notedata which is used by the judgement check patches */
    DWORD dllSize = 0;
    char *data = getDllData("jubeat.dll", &dllSize);

    {
        int64_t pattern_offset = _search(data, dllSize, "\x8B\x47\xE8\x85\xC0\x0F\x84", 7, 0);
        if (pattern_offset == -1)
        {
            LOG("jubeathax: could not find chart parsing block (tutorial)\n");
            return false;
        }
        uint64_t patch_addr = (int64_t)data + pattern_offset;
        _MH_CreateHook((LPVOID)(patch_addr + 0x03), (LPVOID)hook_parse_chart_tuto, (void **)&real_parse_chart_tuto);
    }

    {
        int64_t pattern_offset = _search(data, dllSize, "\x8B\x47\xFC\x83\xE8\x00\x0F\x84", 8, 0);
        if (pattern_offset != -1)
        {
            uint64_t patch_addr = (int64_t)data + pattern_offset;
            _MH_CreateHook((LPVOID)(patch_addr + 0x03), (LPVOID)hook_parse_chart_ingame_festo,
                           (void **)&real_parse_chart_ingame_festo);
        }
        else
        {
            pattern_offset = _search(data, dllSize, "\x8B\x47\xE8\x83\xE8\x00\x0F\x84", 8, 0);
            if (pattern_offset != -1)
            {
                uint64_t patch_addr = (int64_t)data + pattern_offset;
                _MH_CreateHook((LPVOID)(patch_addr + 0x03), (LPVOID)hook_parse_chart_ingame,
                               (void **)&real_parse_chart_ingame);
            }
            else
            {
                LOG("jubeathax: could not find chart parsing block (ingame)\n");
                return false;
            }
        }
    }

    return true;
}

/* retrieve up-to-date information on g_longnote_data and g_longratio for input_panel_trg_off patch */
void (*real_horizontal_long)();
__declspec(naked) void hook_horizontal_long()
{
    /*
    here we've just computed the remaining distance in tile number, result is in st0 (horizontal notes only)
    esi+0x08 holds the long note data
    */
    __asm("sub esp, 0x1C\n");
    __asm("push eax\n");
    __asm("mov eax, dword ptr [esi+0x08]\n");
    __asm("mov _g_latest_notedata, eax\n");
    __asm("fst dword ptr [esp+0xC]\n");
    __asm("movss xmm0, dword ptr [esp+0xC]\n");
    __asm("and eax, 0xF\n");
    __asm("movss _g_distratio, xmm0\n");
    __asm("movss dword ptr [eax*4 + _g_longratio], xmm0\n"); // g_longratio[g_latest_notedata&0x0F] = g_distratio;

    __asm("pop eax\n");
    __asm("add esp, 0x1C\n");
    __asm("jmp [_real_horizontal_long]\n");
}

void (*real_vertical_long)();
__declspec(naked) void hook_vertical_long()
{
    /* we've just computed the remaining distance in tile number, result is in st0 (vertical notes only)
    esi+0x08 holds the long note data
    */
    __asm("sub esp, 0x1C\n");
    __asm("push eax\n");
    __asm("mov eax, dword ptr [esi+0x08]\n");
    __asm("mov _g_latest_notedata, eax\n");
    __asm("fst dword ptr [esp+0xC]\n");
    __asm("movss xmm0, dword ptr [esp+0xC]\n");

    __asm("and eax, 0xF\n");
    __asm("movss _g_distratio, xmm0\n");
    __asm("movss dword ptr [eax*4 + _g_longratio], xmm0\n"); // g_longratio[g_latest_notedata&0x0F] = g_distratio;

    __asm("pop eax\n");
    __asm("add esp, 0x1C\n");
    __asm("jmp [_real_vertical_long]\n");
}

bool patch_update_longratio()
{
    DWORD dllSize = 0;
    char *data = getDllData("jubeat.dll", &dllSize);

    {
        int64_t pattern_offset = _search(data, dllSize, "\x83\xC2\x02\xD8\x7D\xC8\x89\x55\xF4", 9, 0);
        if (pattern_offset == -1)
        {
            LOG("jubeathax: could not find horizontal long note handling function\n");
            return false;
        }
        uint64_t patch_addr = (int64_t)data + pattern_offset;
        _MH_CreateHook((LPVOID)(patch_addr + 0x06), (LPVOID)hook_horizontal_long, (void **)&real_horizontal_long);
    }

    {
        int64_t pattern_offset = _search(data, dllSize, "\xD8\x7D\xE0\x89\x55\xF4\xD9\x5D\xE0", 9, 0);
        if (pattern_offset == -1)
        {
            LOG("jubeathax: could not find vertical long note handling function\n");
            return false;
        }
        uint64_t patch_addr = (int64_t)data + pattern_offset;
        _MH_CreateHook((LPVOID)(patch_addr + 0x03), (LPVOID)hook_vertical_long, (void **)&real_vertical_long);
    }

    return true;
}
#pragma #endregion

#pragma #region "PREVIEW"
uint32_t g_first_note = 0;
void (*real_first_note_display)();
__declspec(naked) void hook_first_note_display()
{
    /* this is the function which retrieves the first note to display the preview before the song starts
       at this point, ecx contains the note number
       */
    __asm("push eax\n");
    __asm("push ebx\n");
    __asm("push ecx\n");
    __asm("push edx\n");
    __asm("and ecx, 0x0F\n");
    __asm("mov _g_first_note, ecx\n");
    __asm("sub esp, 0xC\n");
    plugin_board_update(g_first_note, PREVIEW, NULL);
    __asm("add esp, 0xC\n");
    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop ebx\n");
    __asm("pop eax\n");
    __asm("jmp [_real_first_note_display]\n");
}

bool patch_first_note_display()
{
    DWORD dllSize = 0;
    char *data = getDllData("sequence.dll", &dllSize);

    {
        int64_t pattern_offset = _search(data, dllSize, "\x8B\x0A\x0F\xB7\x07\x0F", 6, 0);
        if (pattern_offset == -1)
        {
            LOG("jubeathax: could not find first play event retrieval\n");
            return false;
        }
        uint64_t patch_addr = (int64_t)data + pattern_offset;
        _MH_CreateHook((LPVOID)(patch_addr + 0x02), (LPVOID)hook_first_note_display, (void **)&real_first_note_display);
    }
    return true;
}
#pragma #endregion

#pragma #region "LONG_TRAIL_UPDATE / INACTIVE"
void *g_ptr;
int g_col = 0;
int g_row = 0;
void (*real_input_panel_trg_on)();
__declspec(naked) void hook_input_panel_trg_on()
{
    /* this function is called at various places, sometimes with a NULL pointer in order to turn off buttons
     */
    __asm("push eax");
    __asm("push ecx");
    __asm("push edx");
    __asm("mov eax,dword ptr ds:[esp+0x10]      \n");
    __asm("mov _g_col, eax");
    __asm("mov eax,dword ptr ds:[esp+0x14]      \n");
    __asm("mov _g_row, eax");
    __asm("mov eax,dword ptr ds:[esp+0x18]      \n");
    __asm("mov _g_ptr, eax");

    __asm("cmp eax, 0\n");
    __asm("je trg_on_null_ptr\n"); // pointer is NULL, trigger a turn off
    __asm("jmp trg_on_leave\n");
    __asm("trg_on_null_ptr:\n");
    __asm("sub esp, 0xC\n");
    plugin_board_update(g_row * 4 + g_col, INACTIVE, NULL);
    __asm("add esp, 0xC\n");
    __asm("trg_on_leave:\n");
    __asm("pop edx");
    __asm("pop ecx");
    __asm("pop eax");
    __asm("jmp [_real_input_panel_trg_on]\n");
}

void (*real_input_panel_trg_short_on)();
__declspec(naked) void hook_input_panel_trg_short_on()
{
    /* this function is called at various places, sometimes with a NULL pointer in order to turn off buttons
     */
    __asm("push eax");
    __asm("push ecx");
    __asm("push edx");
    __asm("mov eax,dword ptr ds:[esp+0x10]      \n");
    __asm("mov _g_col, eax");
    __asm("mov eax,dword ptr ds:[esp+0x14]      \n");
    __asm("mov _g_row, eax");
    __asm("mov eax,dword ptr ds:[esp+0x18]      \n");
    __asm("mov _g_ptr, eax");

    __asm("cmp eax, 0\n");
    __asm("je trg_short_on_null_ptr\n"); // pointer is NULL, trigger a turn off
    __asm("jmp trg_short_on_leave\n");
    __asm("trg_short_on_null_ptr:\n");
    __asm("sub esp, 0xC\n");
    plugin_board_update(g_row * 4 + g_col, INACTIVE, NULL);
    __asm("add esp, 0xC\n");
    __asm("trg_short_on_leave:\n");
    __asm("pop edx");
    __asm("pop ecx");
    __asm("pop eax");
    __asm("jmp [_real_input_panel_trg_short_on]\n");
}

void (*real_input_panel_trg_off)();
__declspec(naked) void hook_input_panel_trg_off()
{
    /* This function is called while holding a long note (extra data is annoying to retrieve depending on cases
     * so we use g_longratio/g_latest_notedata instead) */
    __asm("push eax\n");
    __asm("push ebx\n");
    __asm("push ecx\n");
    __asm("push edx\n");

    __asm("sub esp, 0xC\n");
    plugin_board_update(g_latest_notedata, LONG_TRAIL_UPDATE, &(g_longratio[g_latest_notedata & 0x0F]));
    __asm("add esp, 0xC\n");

    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop ebx\n");
    __asm("pop eax\n");

    __asm("jmp [_real_input_panel_trg_off]\n");
}

bool patch_trg_on_functions()
{
    HMODULE hinstLib = GetModuleHandleA("input.dll");

    /* These keep being called with a NULL pointer to turn off cells, we use them to send the INACTIVE message */
    _MH_CreateHook((LPVOID)GetProcAddress(hinstLib, "input_panel_trg_on"), (LPVOID)hook_input_panel_trg_on,
                   (void **)&real_input_panel_trg_on);
    _MH_CreateHook((LPVOID)GetProcAddress(hinstLib, "input_panel_trg_short_on"), (LPVOID)hook_input_panel_trg_short_on,
                   (void **)&real_input_panel_trg_short_on);

    /* This one is called when holding a long note, we use it to send the LONG_TRAIL_UPDATE message */
    _MH_CreateHook((LPVOID)GetProcAddress(hinstLib, "input_panel_trg_off"), (LPVOID)hook_input_panel_trg_off,
                   (void **)&real_input_panel_trg_off);

    LOG("jubeathax: input functions hooked\n");
    return true;
}
#pragma #endregion

#pragma #region "PRESSED / PENDING"
scoredata_t g_scoredata = {0};
scoredata_t *g_curr_scorestruct = &(g_scoredata);
uint32_t *g_curr_score = &(g_scoredata.score);
uint32_t *g_curr_combo = &(g_scoredata.combo);

void (*real_score_add_judge)();
__declspec(naked) void hook_score_add_judge()
{
    /* this is the function which adds to your score, once the button goes out of play, so that's a perfect place
    for handling the button turning off (/!\ doesn't always happen with long notes /!\)
       */
    __asm("push eax\n");
    __asm("push ebx\n");
    __asm("push ecx\n");
    __asm("push edx\n");
    __asm("mov ebx, dword ptr [_g_curr_score]\n");
    __asm("mov dword ptr [ebx], eax\n"); // eax contains the new updated score
    __asm("mov eax, dword ptr [ebp+0x08]\n");
    __asm("mov eax, dword ptr [eax+0x04]\n");
    __asm("mov ebx, dword ptr [_g_curr_combo]\n");
    __asm("mov dword ptr [ebx], eax\n"); // updated combo
    __asm("sub esp, 0x0C\n");
    plugin_board_update(
        g_latest_notedata, (cell_state_t)(g_judgement_idx | PRESSED),
        (void *)g_curr_scorestruct); // g_judgement_idx coincides with states MISSED/OUTSIDE/GOOD/VERY_GOOD/PERFECT
    __asm("add esp, 0x0C\n");
    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop ebx\n");
    __asm("pop eax\n");
    __asm("jmp [_real_score_add_judge]\n");
}

bool patch_score_add_judge()
{
    DWORD dllSize = 0;
    char *data = getDllData("score.dll", &dllSize);
    {
        int64_t pattern_offset = _search(data, dllSize, "\x5F\x5E\x89\x03\x5B\x8B\xE5\x5D\xC3", 9, 0);
        if (pattern_offset == -1)
        {
            LOG("jubeathax: could not score_add_judge end\n");
            return false;
        }
        uint64_t patch_addr = (int64_t)data + pattern_offset;
        _MH_CreateHook((LPVOID)(patch_addr), (LPVOID)hook_score_add_judge, (void **)&real_score_add_judge);
    }

    return true;
}

void (*real_button_press_tuto)();
__declspec(naked) void hook_button_press_tuto()
{
    /* this is the function which prepares the good/perfect animation sound (used in tutorial only, because there are no
    calls to score_add_judge there) g_latest_notedata and g_judgement_idx have just been retrieved by previous
    parse_chart and check_judgement hooks.
       */
    __asm("push eax\n");
    __asm("push ebx\n");
    __asm("push ecx\n");
    __asm("push edx\n");
    __asm("sub esp, 0x0C\n");
    plugin_board_update(g_latest_notedata, (cell_state_t)(g_judgement_idx | PRESSED),
                        NULL); // g_judgement_idx coincides with states MISSED/OUTSIDE/GOOD/VERY_GOOD/PERFECT
    __asm("add esp, 0x0C\n");
    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop ebx\n");
    __asm("pop eax\n");
    __asm("jmp [_real_button_press_tuto]\n");
}

void (*real_judgement_check)();
__declspec(naked) void hook_judgement_check()
{
    /* this is the function which checks the timing window, right before checking if we're pressing on the correct
       button we get there as soon as we're within the timing window we use it to update note drawing color,
       synchronized with timing windows

       eax contains the judgement value
       */
    __asm("push eax\n");
    __asm("push ebx\n");
    __asm("push ecx\n");
    __asm("push edx\n");
    __asm("mov _g_judgement_idx, eax\n");
    __asm("sub esp, 0x0C\n");
    plugin_board_update(g_latest_notedata, (cell_state_t)(g_judgement_idx), NULL);
    __asm("add esp, 0x0C\n");
    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop ebx\n");
    __asm("pop eax\n");

    __asm("jmp [_real_judgement_check]\n");
}

void (*real_judgement_check_bis)();
__declspec(naked) void hook_judgement_check_bis()
{
    /* this is the function which checks the timing window, right before checking if we're pressing on the correct
       button we get there while we're not within the timing window (hasn't started yet, or too late)

       we use it to update note drawing color, synchronized with timing windows
     */
    __asm("push eax\n");
    __asm("push ebx\n");
    __asm("push ecx\n");
    __asm("push edx\n");
    __asm("mov _g_judgement_idx, eax\n");
    __asm("sub esp, 0x0C\n");
    plugin_board_update(g_latest_notedata, (cell_state_t)(g_judgement_idx), NULL);
    __asm("add esp, 0x0C\n");
    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop ebx\n");
    __asm("pop eax\n");
    __asm("jmp [_real_judgement_check_bis]\n");
}

bool patch_button_press()
{
    DWORD dllSize = 0;
    char *data = getDllData("jubeat.dll", &dllSize);

    {
        int64_t pattern_offset = _search(data, dllSize, "\x8B\x45\xF8\x5F\x5E\x5B\x8B\xE5\x5D\xC3", 10, 0);
        if (pattern_offset == -1)
        {
            LOG("jubeathax: could not find judgement check function\n");
            return false;
        }
        uint64_t patch_addr = (int64_t)data + pattern_offset;
        _MH_CreateHook((LPVOID)(patch_addr + 0x03), (LPVOID)hook_judgement_check, (void **)&real_judgement_check);
        _MH_CreateHook((LPVOID)(patch_addr - 0x05), (LPVOID)hook_judgement_check_bis,
                       (void **)&real_judgement_check_bis);
    }

    {
        int64_t pattern_offset = _search(data, dllSize, "\x33\xC5\x89\x45\xFC\x83\xF9\x03\x0F\x87", 10, 0);
        if (pattern_offset == -1)
        {
            LOG("jubeathax: could not find judgement check function\n");
            return false;
        }
        uint64_t patch_addr = (int64_t)data + pattern_offset - 11;
        _MH_CreateHook((LPVOID)patch_addr, (LPVOID)hook_button_press_tuto, (void **)&real_button_press_tuto);
    }

    return true;
}
#pragma #endregion

#pragma #region "LONG_NOTE / LONG_TRAIL_DRAW"
void (*real_release_long)();
__declspec(naked) void hook_release_long()
{
    /* when releasing hold note (or when it reaches the end of trail), note data in edi+0x08 */
    __asm("push eax\n");
    __asm("push ecx\n");
    __asm("push edx\n");

    __asm("mov eax, dword ptr [edi+0x08]\n");

    __asm("mov ecx, eax\n");
    __asm("and ecx, 0xF\n");
    __asm("movzx edx, byte ptr [_g_is_released + ecx]\n");
    __asm("cmp edx, 1\n");
    __asm("je send_release_long_cont\n");

    __asm("mov _g_longnote_data, eax\n");
    g_longratio[g_longnote_data & 0x0F] = 0;
    g_is_released[g_longnote_data & 0x0F] = true;
    g_is_drawn[g_longnote_data & 0x0F] = false;
    __asm("sub esp, 0xC\n");
    plugin_board_update(g_longnote_data, LONG_NOTE_RELEASE_FIRST, &(g_longratio[g_longnote_data & 0x0F]));
    __asm("add esp, 0xC\n");
    __asm("jmp end_release_long\n");

    __asm("send_release_long_cont:\n");
    __asm("mov _g_longnote_data, eax\n");
    __asm("sub esp, 0xC\n");
    plugin_board_update(g_longnote_data, LONG_NOTE_RELEASE_CONT, &(g_longratio[g_longnote_data & 0x0F]));
    __asm("add esp, 0xC\n");

    __asm("end_release_long:\n");
    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop eax\n");
    __asm("jmp [_real_release_long]\n");
}

void (*real_missed_long)();
__declspec(naked) void hook_missed_long()
{
    /* when timing window has expired, note data in edi+0x08 */
    __asm("cmp eax, 0x01\n");
    __asm("jne leave_missed_long\n");
    __asm("push eax\n");
    __asm("push ecx\n");
    __asm("push edx\n");

    __asm("mov eax, dword ptr [edi+0x08]\n");

    __asm("mov ecx, eax\n");
    __asm("and ecx, 0xF\n");
    __asm("movzx edx, byte ptr [_g_is_released + ecx]\n");
    __asm("cmp edx, 1\n");
    __asm("je send_missed_long_cont\n");

    __asm("mov _g_longnote_data, eax\n");
    g_longratio[g_longnote_data & 0x0F] = 0;
    g_is_released[g_longnote_data & 0x0F] = true;
    g_is_drawn[g_longnote_data & 0x0F] = false;
    __asm("sub esp, 0xC\n");
    plugin_board_update(g_longnote_data, LONG_NOTE_MISS_FIRST, &(g_longratio[g_longnote_data & 0x0F]));
    __asm("add esp, 0xC\n");
    __asm("jmp end_missed_long\n");

    __asm("send_missed_long_cont:\n");
    __asm("mov _g_longnote_data, eax\n");
    __asm("sub esp, 0xC\n");
    plugin_board_update(g_longnote_data, LONG_NOTE_MISS_CONT, &(g_longratio[g_longnote_data & 0x0F]));
    __asm("add esp, 0xC\n");

    __asm("end_missed_long:\n");
    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop eax\n");
    __asm("leave_missed_long:\n");
    __asm("jmp [_real_missed_long]\n");
}

/* display long note (not pressed yet) */
void (*real_draw_long)();
__declspec(naked) void hook_draw_long()
{
    __asm("cmp dword ptr [edi+0x1C], 0\n"); /* checks if button has been pressed already */
    __asm("je draw_note\n");
    __asm("hook_draw_long_end:\n");
    __asm("cmp dword ptr [edi+0x1C], 0\n"); /* perform the cmp again just in case */
    __asm("jmp [_real_draw_long]\n");
    __asm("draw_note:\n");
    __asm("push eax\n");
    __asm("push ebx\n");
    __asm("push ecx\n");
    __asm("push edx\n");

    __asm("mov eax, dword ptr [edi+0x08]\n");

    __asm("mov ecx, eax\n");
    __asm("and ecx, 0xF\n");
    __asm("movzx edx, byte ptr [_g_is_drawn + ecx]\n");
    __asm("cmp edx, 1\n");
    __asm("je send_draw_cont\n");

    __asm("mov _g_longnote_data, eax\n");
    g_is_released[g_longnote_data & 0x0F] = false;
    g_is_drawn[g_longnote_data & 0x0F] = true;
    __asm("sub esp, 0xC\n");
    plugin_board_update(g_longnote_data, LONG_TRAIL_DRAW_FIRST, NULL);
    __asm("add esp, 0xC\n");
    __asm("jmp leave_send_draw\n");

    __asm("send_draw_cont:\n");
    __asm("mov _g_longnote_data, eax\n");
    __asm("sub esp, 0xC\n");
    plugin_board_update(g_longnote_data, LONG_TRAIL_DRAW_CONT, NULL);
    __asm("add esp, 0xC\n");

    __asm("leave_send_draw:\n");
    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop ebx\n");
    __asm("pop eax\n");
    __asm("jmp hook_draw_long_end\n");
}

bool patch_long()
{
    DWORD dllSize = 0;
    char *data = getDllData("jubeat.dll", &dllSize);

    {
        int64_t pattern_offset = _search(data, dllSize, "\x0F\x84\x62\x06\x00\x00\x8B\x45\x08", 9, 0);
        if (pattern_offset == -1)
        {
            LOG("jubeathax: could not find long note release handling\n");
            return false;
        }
        uint64_t patch_addr = (int64_t)data + pattern_offset;
        _MH_CreateHook((LPVOID)(patch_addr + 0x06), (LPVOID)hook_release_long, (void **)&real_release_long);
    }

    {
        int64_t pattern_offset = _search(data, dllSize, "\x8A\x45\xFF\x5F\x5E\x8B\xE5\x5D\xC3\x66\x90", 11, 0);
        if (pattern_offset == -1)
        {
            LOG("jubeathax: could not find missed long note handling\n");
            return false;
        }
        uint64_t patch_addr = (int64_t)data + pattern_offset;
        _MH_CreateHook((LPVOID)(patch_addr + 0x00), (LPVOID)hook_missed_long, (void **)&real_missed_long);
    }

    {
        int64_t pattern_offset = _search(data, dllSize, "\x74\x08\x8B\xC2\x2B\x47\x18", 7, 0);
        if (pattern_offset == -1)
        {
            LOG("jubeathax: could not find long note release handling\n");
            return false;
        }
        uint64_t patch_addr = (int64_t)data + pattern_offset;
        _MH_CreateHook((LPVOID)(patch_addr + 0x00), (LPVOID)hook_draw_long, (void **)&real_draw_long);
    }

    return true;
}
#pragma #endregion

#pragma #region "FINAL SCORE"
uint32_t g_updated_score = 0;
bool g_updated_score_sent = false; // prevent the hook from sending multiple FINAL_SCORE messages to the dll
void (*real_score_bonus)();
__declspec(naked) void hook_score_bonus()
{
    /* this is the function which adds bonus to your score after the end of song. Updated score is in esi
        Note this is called several time with multiple updates, we "debounce" it to send the final score
        message only once it's reached its final state.
       */
    __asm("push eax\n");
    __asm("push ebx\n");
    __asm("push ecx\n");
    __asm("push edx\n");
    __asm("cmp esi, _g_updated_score\n");
    __asm("je done_updating\n");
    __asm("mov byte ptr [_g_updated_score_sent], 0x00\n");
    __asm("jmp skip_send\n");
    __asm("done_updating:\n");
    __asm("cmp byte ptr [_g_updated_score_sent], 0x01\n");
    __asm("je skip_send\n");
    __asm("mov byte ptr [_g_updated_score_sent], 1\n");
    __asm("sub esp, 0x0C\n");
    plugin_board_update(g_latest_notedata, FINAL_SCORE, (void *)&g_updated_score);
    __asm("add esp, 0x0C\n");

    __asm("skip_send:\n");
    __asm("mov _g_updated_score, esi\n");
    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop ebx\n");
    __asm("pop eax\n");
    __asm("jmp [_real_score_bonus]\n");
}

bool patch_score_update_after_song()
{
    DWORD dllSize = 0;
    char *data = getDllData("jubeat.dll", &dllSize);
    {
        int64_t pattern_offset = _search(data, dllSize, "\x0C\x00\x75\x08\x8D\x4F\x08\x03\xC9", 9, 0);
        if (pattern_offset == -1)
        {
            LOG("jubeathax: could not find post-song score update function\n");
            return false;
        }
        uint64_t patch_addr = (int64_t)data + pattern_offset;
        _MH_CreateHook((LPVOID)(patch_addr - 0x02), (LPVOID)hook_score_bonus, (void **)&real_score_bonus);
    }
    return true;
}
#pragma #endregion

#pragma #region "FULL_COMBO / EXCELLENT"
void (*real_scoring_fc)();
__declspec(naked) void hook_scoring_fc()
{
    __asm("test al, al\n");
    __asm("je no_fc_1\n");
    __asm("push eax\n");
    __asm("push ebx\n");
    __asm("push ecx\n");
    __asm("push edx\n");
    __asm("sub esp, 0xC\n");
    plugin_board_update(0, FULL_COMBO, NULL);
    __asm("add esp, 0xC\n");
    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop ebx\n");
    __asm("pop eax\n");
    __asm("no_fc_1:\n");
    __asm("jmp [_real_scoring_fc]\n");
}

void (*real_scoring_exc)();
__declspec(naked) void hook_scoring_exc()
{
    __asm("test al, al\n");
    __asm("je no_exc_1\n");
    __asm("push eax\n");
    __asm("push ebx\n");
    __asm("push ecx\n");
    __asm("push edx\n");
    __asm("sub esp, 0xC\n");
    plugin_board_update(0, EXCELLENT, NULL);
    __asm("add esp, 0xC\n");
    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop ebx\n");
    __asm("pop eax\n");
    __asm("no_exc_1:\n");
    __asm("jmp [_real_scoring_exc]\n");
}

void (*real_scoring_fc_anim)();
__declspec(naked) void hook_scoring_fc_anim()
{
    __asm("test al, al\n");
    __asm("je no_fc_anim\n");
    __asm("push eax\n");
    __asm("push ebx\n");
    __asm("push ecx\n");
    __asm("push edx\n");
    __asm("sub esp, 0xC\n");
    plugin_board_update(0, FULL_COMBO_ANIM, (void *)&g_updated_score);
    __asm("add esp, 0xC\n");
    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop ebx\n");
    __asm("pop eax\n");
    __asm("no_fc_anim:\n");
    __asm("jmp [_real_scoring_fc_anim]\n");
}

void (*real_scoring_exc_anim)();
__declspec(naked) void hook_scoring_exc_anim()
{
    __asm("test al, al\n");
    __asm("je no_exc_anim\n");
    __asm("push eax\n");
    __asm("push ebx\n");
    __asm("push ecx\n");
    __asm("push edx\n");
    __asm("sub esp, 0xC\n");
    plugin_board_update(0, EXCELLENT_ANIM, (void *)&g_updated_score);
    __asm("add esp, 0xC\n");
    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop ebx\n");
    __asm("pop eax\n");
    __asm("no_exc_anim:\n");
    __asm("jmp [_real_scoring_exc_anim]\n");
}

__declspec(naked) void force_on()
{
    __asm("mov al, 1\n");
    __asm("ret\n");
}

bool patch_fc_exc()
{
    DWORD dllSize = 0;
    char *data = getDllData("jubeat.dll", &dllSize);

    /* first check, right as you press the last note */
    {
        int64_t pattern_offset = _search(data, dllSize, "\x83\xC4\x04\x84\xC0\x74\x07\xB9\x01\x00\x00\x00", 12, 0);
        if (pattern_offset == -1)
        {
            LOG("jubeathax: could not find first fc/exc check\n");
            return false;
        }
        uint64_t patch_addr = (int64_t)data + pattern_offset;
        _MH_CreateHook((LPVOID)(patch_addr + 0x00), (LPVOID)hook_scoring_exc, (void **)&real_scoring_exc);
        _MH_CreateHook((LPVOID)(patch_addr + 0x15), (LPVOID)hook_scoring_fc, (void **)&real_scoring_fc);
    }

    /* last check, just before end of song animation */
    {
        int64_t pattern_offset = _search(data, dllSize, "\x83\xC4\x04\x84\xC0\x74\x07\xBF\x00\x09\x8A\x23", 12, 0);
        if (pattern_offset == -1)
        {
            LOG("jubeathax: could not find last fc/exc check\n");
            return false;
        }
        uint64_t patch_addr = (int64_t)data + pattern_offset;
        _MH_CreateHook((LPVOID)(patch_addr + 0x00), (LPVOID)hook_scoring_exc_anim, (void **)&real_scoring_exc_anim);
        _MH_CreateHook((LPVOID)(patch_addr + 0x15), (LPVOID)hook_scoring_fc_anim, (void **)&real_scoring_fc_anim);
    }

    // DEBUG ONLY
    // HMODULE hinstLib = GetModuleHandleA("score.dll");
    //_MH_CreateHook((LPVOID)GetProcAddress(hinstLib, "score_is_full_combo"), (LPVOID)force_on, NULL);
    //_MH_CreateHook((LPVOID)GetProcAddress(hinstLib, "score_is_excellent"), (LPVOID)force_on, NULL);

    return true;
}
#pragma #endregion

#pragma #region "LEAVE RESULT SCREEN"
void (*real_leave_result_screen)();
__declspec(naked) void hook_leave_result_screen()
{
    /* this is right after the check if button is pressed or if timer has expired (result in al) */
    __asm("test al, al\n");
    __asm("je no_leave_result_screen\n");
    __asm("push eax\n");
    __asm("push ebx\n");
    __asm("push ecx\n");
    __asm("push edx\n");
    __asm("sub esp, 0xC\n");
    plugin_board_update(0, LEAVE_RESULT_SCREEN, NULL);
    __asm("add esp, 0xC\n");
    __asm("pop edx\n");
    __asm("pop ecx\n");
    __asm("pop ebx\n");
    __asm("pop eax\n");
    __asm("no_leave_result_screen:\n");
    __asm("jmp [_real_leave_result_screen]\n");
}

bool patch_leave_result_screen()
{
    DWORD dllSize = 0;
    char *data = getDllData("jubeat.dll", &dllSize);

    {
        int64_t pattern_offset = _wildcard_search(data, dllSize, "\x84\xC0\x0F\x84?\xCF\xFF\xFF", 8, 0);
        if (pattern_offset == -1)
        {
            LOG("jubeathax: could not find leave result screen test call\n");
            return false;
        }

        uint64_t patch_addr = (int64_t)data + pattern_offset;
        _MH_CreateHook((LPVOID)(patch_addr + 0x00), (LPVOID)hook_leave_result_screen,
                       (void **)&real_leave_result_screen);
    }
    return true;
}
#pragma #endregion


#pragma #region "PLUGIN LOADING"
bool try_open_plugin(const char *filename)
{
    int ver = 0;
    HMODULE h = LoadLibrary(filename);
    if (h == NULL)
    {
        LOG("\tFailed to load library (error %ld)\n", GetLastError());
        return false;
    }

    plugin_get_input = (uint32_t(*)())GetProcAddress(h, "io_plugin_get_input");
    if (plugin_get_input == NULL)
    {
        LOG("\tERROR: could not find plugin_get_input inside %s\n", filename);
        goto err;
    }

    plugin_get_api_version = (int (*)())GetProcAddress(h, "io_plugin_get_api_version");
    if (plugin_get_api_version == NULL)
    {
        LOG("\tERROR: could not find plugin_get_api_version inside %s\n", filename);
        goto err;
    }
    plugin_init = (bool (*)())GetProcAddress(h, "io_plugin_init");
    if (plugin_init == NULL)
    {
        LOG("\tERROR: could not find plugin_init inside inside %s\n", filename);
        goto err;
    }
    plugin_deinit = (bool (*)())GetProcAddress(h, "io_plugin_deinit");
    if (plugin_deinit == NULL)
    {
        LOG("\tERROR: could not find plugin_deinit inside inside %s\n", filename);
        goto err;
    }
    plugin_open_input = (bool (*)())GetProcAddress(h, "io_plugin_open_input");
    if (plugin_open_input == NULL)
    {
        LOG("\tERROR: could not find plugin_open_input inside %s\n", filename);
        goto err;
    }
    plugin_open_output = (bool (*)())GetProcAddress(h, "io_plugin_open_output");
    if (plugin_open_output == NULL)
    {
        LOG("\tERROR: could not find plugin_open_output inside %s\n", filename);
        goto err;
    }
    plugin_board_update = (bool (*)(uint32_t, cell_state_t, void *))GetProcAddress(h, "io_plugin_board_update");
    if (plugin_board_update == NULL)
    {
        LOG("\tERROR: could not find plugin_board_update inside %s\n", filename);
        goto err;
    }

    if (!plugin_init())
    {
        LOG("\tplugin_init returned false\n");
        goto err;
    }

    ver = plugin_get_api_version();
    if (ver > API_VERSION)
    {
        LOG("\tplugin API version (%d) is more recent than what this jubeathax supports (%d). Please update.\n", ver,
            API_VERSION);
        goto err;
    }
    else if (ver != API_VERSION)
        LOG("WARNING: plugin api version (%d) is older than what jubeathax expects (%d). It might not work as "
            "expected.\n",
            ver, API_VERSION);

    // TODO: allow "input only" plugins as well
    if (!plugin_open_output())
    {
        LOG("\tERROR: plugin_open_output returned false\n");
        plugin_deinit();
        goto err;
    }

    if (plugin_open_input())
    {
        patch_inputs();
    }
    else
    {
        LOG("\tWARNING: plugin_open_input returned false\n");
    }

    LOG("\tPlugin loaded succesfully\n");
    return true;

err:
    if (h != NULL)
        FreeLibrary(h);

    return false;
}

bool find_suitable_plugin()
{
    bool has_dummy = false;

    HANDLE hFile;
    char buffer[MAX_PATH] = {
        0,
    };
    WIN32_FIND_DATA pNextInfo;

    LOG("Looking for io dlls...\n");

    sprintf(buffer, ".\\io_*.dll");
    hFile = FindFirstFileA(buffer, &pNextInfo);
    if (!hFile)
    {
        LOG("\tno io dll found. Make sure your io_*.dll reside in the same folder as jubeathax.dll\n");
        return false;
    }

    do
    {
        const char *filename = pNextInfo.cFileName;
        LOG("\t%s", filename);

        if (strcmp("io_dummy.dll", filename) == 0) // save that one for last in case nothing else is compatible
        {
            has_dummy = true;
            LOG(" (dummy dll will be used as last resort, looking for others)\n");
            continue;
        }

        LOG("\nTrying %s...\n", filename);
        if (try_open_plugin(filename))
        {
            return true;
        }
    } while (FindNextFileA(hFile, &pNextInfo));

    if (has_dummy)
    {
        LOG("No suitable dll found. Using io_dummy.dll\n");
        return try_open_plugin("io_dummy.dll");
    }

    LOG("\nNo suitable dll found.");

    return false;
}
#pragma #endregion

bool patch_use_external_io()
{
    if (!find_suitable_plugin())
        return false;

    // jubeat.dll helper patch to retrieve g_latest_notedata (used by other patches)
    patch_retrieve_latest_notedata();

    // jubeat.dll helper patch to update g_longratio (used by LONG_TRAIL_UPDATE)
    patch_update_longratio();

    // sequence.dll for PREVIEW
    patch_first_note_display();

    // input.dll for LONG_TRAIL_UPDATE and INACTIVE
    patch_trg_on_functions();

    // score.dll for PRESSED (in game)
    patch_score_add_judge();

    // jubeat.dll for PRESSED (in tutorial) and PENDING judgement states
    patch_button_press();

    // jubeat.dll for LONG_NOTE_* and LONG_TRAIL_DRAW_*
    patch_long();

    // jubeat.dll for FINAL_SCORE
    patch_score_update_after_song();

    // jubeat.dll for FULL_COMBO_* and EXCELLENT_*
    patch_fc_exc();

    // jubeat.dll for LEAVE_RESULT_SCREEN
    patch_leave_result_screen();

    return true;
}
