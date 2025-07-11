#include <windows.h>
#include <conio.h>
#include <mmsystem.h>
#include <psapi.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include "io_launchpad.h"

typedef enum color_e
{
    OFF = 12,
    RED = 15,
    RED_L = 13,
    AMBER = 29,
    AMBER_L = 63,
    YELLOW = 62,
    GREEN = 28,
    GREEN_L = 60,
    RED_FLASH = 11,
    AMBER_FLASH = 59,
    YELLOW_FLASH = 58,
    GREEN_FLASH = 56,
} color_t;

#define LAUNCHPAD_CLEAR 0xB0

#define INITIAL_DELAY 10
#define JUDGE_COOLDOWN 4

#define NO_LOCK (255)
typedef struct launchpad_button_s
{
    const uint8_t key[4];
    int lock[4]; // used to lock long note trails (keep track of button owning a
                 // trail to prevent bugs on overlap)
} launchpad_button_t;

typedef struct launchpad_s
{
    launchpad_button_t buttons[16];
    uint8_t state[16]; // keep track of currently displayed color to prevent
                       // unneeded messages and/or adjust transitions
} launchpad_t;

int g_judgement_press_colors[7] = {0}; // to easily define which colors should be used for judgement feedback
int g_judgement_colors[7] = {0};       // to easily define which colors should be
                                       // used for timing window display

launchpad_t g_launchpad = {.buttons = {{0x10, 0x00, 0x01, 0x11},
                                       {0x12, 0x02, 0x03, 0x13},
                                       {0x14, 0x04, 0x05, 0x15},
                                       {0x16, 0x06, 0x07, 0x17},
                                       {0x30, 0x20, 0x21, 0x31},
                                       {0x32, 0x22, 0x23, 0x33},
                                       {0x34, 0x24, 0x25, 0x35},
                                       {0x36, 0x26, 0x27, 0x37},
                                       {0x50, 0x40, 0x41, 0x51},
                                       {0x52, 0x42, 0x43, 0x53},
                                       {0x54, 0x44, 0x45, 0x55},
                                       {0x56, 0x46, 0x47, 0x57},
                                       {0x70, 0x60, 0x61, 0x71},
                                       {0x72, 0x62, 0x63, 0x73},
                                       {0x74, 0x64, 0x65, 0x75},
                                       {0x76, 0x66, 0x67, 0x77}},
                           .state = {0}};

typedef struct cooldown_s
{
    uint8_t out; // cooldown before making the color disappear
    uint8_t in;  // cooldown before turning button on
} cooldown_t;

typedef struct board_state_s
{
    cooldown_t cooldown[16];
} board_state_t;

board_state_t g_board_state = {0};

#pragma #region "Manage Launchpad inputs"

uint8_t g_button_state[19] = {0}; // internal button state(but 1-16 then Service Test Coin)
uint32_t g_jb_inputstate = 0;

typedef struct midi_note_s
{
    uint8_t msg;
    uint8_t note;
    uint8_t velocity;
} midi_note_t;

int get_numbut(midi_note_t note)
{
    if (note.msg == 0xb0)
    {
        // midi CONTROL messages for top row
        switch (note.note)
        {
        case 0x68:
            return 16;
        case 0x69:
            return 17;
        case 0x6a:
            return 18;
        default:
            return -1;
        }
    }

    if (note.msg != 0x90 || (note.note & 0x0F) == 8)
        return -1;

    // midi NOTE_ON messages for the pad
    int row = (note.note >> 4) / 2;
    int col = (note.note & 0x0F) / 2;

    return (row << 2) | col;
}

void CALLBACK on_midi_input_cb(HMIDIIN hMidiIn, UINT wMsg, DWORD dwInstance, DWORD dwParam1, DWORD dwParam2)
{
    static uint8_t conversion_table[19] = {5,  1,  13, 9, 6,  2,  14, 10, 7, 3,
                                           15, 11, 16, 4, 20, 12, 25, 28, 24}; // bitfield map for conversion to
                                                                               // jubeat (device.dll) format
    midi_note_t *notePtr = (midi_note_t *)&dwParam1;
    int numbut = get_numbut(*notePtr);
    if (wMsg == MIM_DATA && numbut != -1)
    {
        g_button_state[numbut] = notePtr->velocity;

        if (notePtr->velocity)
            g_jb_inputstate |= 1 << conversion_table[numbut];
        else
            g_jb_inputstate &= ~(1 << conversion_table[numbut]);
    }
    return;
}

#pragma #endregion

#pragma #region "Trick to reclaim the Launchpad input handle if it has already been opened by spicetools"
char *_find(const char *needle, unsigned int needle_len, char *haystack, unsigned int haystack_len)
{
    for (unsigned int i = 0; i < haystack_len - needle_len; i++)
    {
        bool found = true;
        for (unsigned int j = 0; j < needle_len; j++)
        {
            if (needle[j] != *(haystack + i + j))
            {
                found = false;
                break;
            }
        }

        if (found)
        {
            return (haystack + i);
        }
    }

    return 0;
}

char *search_in_heap(const char *pattern, unsigned int length, unsigned int min_offset)
{
    SYSTEM_INFO sysInfo;
    GetSystemInfo(&sysInfo);

    int64_t end = (int64_t)sysInfo.lpMaximumApplicationAddress;
    HANDLE process = GetCurrentProcess();

    char *curr_chunk = 0;
    char *match = NULL;
    SIZE_T bytesRead;

    while (curr_chunk < (char *)end)
    {
        MEMORY_BASIC_INFORMATION mbi;

        if (!VirtualQueryEx(process, curr_chunk, &mbi, sizeof(mbi)))
        {
            return NULL;
        }

        if ((int64_t)curr_chunk < min_offset)
        {
            curr_chunk = curr_chunk + mbi.RegionSize;
            continue;
        }

        if (mbi.State == MEM_COMMIT && mbi.Protect != PAGE_NOACCESS)
        {
            char buffer[mbi.RegionSize] = {0};
            DWORD oldprotect;
            if (VirtualProtectEx(process, mbi.BaseAddress, mbi.RegionSize, PAGE_EXECUTE_READWRITE, &oldprotect))
            {
                ReadProcessMemory(process, mbi.BaseAddress, buffer, mbi.RegionSize, &bytesRead);
                VirtualProtectEx(process, mbi.BaseAddress, mbi.RegionSize, oldprotect, &oldprotect);

                char *offset_in_chunk = _find(pattern, length, buffer, bytesRead);

                if (offset_in_chunk != 0)
                {
                    int64_t pattern_offset = offset_in_chunk - buffer;
                    match = curr_chunk + pattern_offset;
                    break;
                }
            }
        }

        curr_chunk = curr_chunk + mbi.RegionSize;
    }

    return match;
}

/* spicetools opens all midi input devices on launch, so we're trying to locate
 * the handle from memory in order to call midiInClose on it */
static bool try_force_close(const char *device_name)
{
    // using known intel here: there's a false hit in the beginning of spice
    // memory, so we only look for hits beyond 0x1000000
    int64_t pattern_offset = (int64_t)search_in_heap(device_name, strlen(device_name) + 1, 0x1000000);
    if (pattern_offset == 0)
    {
        fprintf(stderr, "jubeathax: could not find device name (%s) in exe memory\n", device_name);
        return false;
    }
    uint64_t handle_addr = (int64_t)pattern_offset + strlen(device_name) + 2;
    uint32_t midi_handle = *(uint32_t *)handle_addr;
    // fprintf(stderr,"Found device handle %08x at address %08llx\n", midi_handle,
    // handle_addr);

    /* Calling midiInClose() on the HMIDIIN handle will cause deadlock due to the
    driver calling WaitForSingleObject on an internal handle... The trick is to
    first call CloseHandle on this internal handle so that the call instantly
    returns with an error. The driver doesn't care and still properly closes all
    subhandles afterwards. (path to the internal handle known by looking at
    midiInClose() and wmaud.drv disassembly) */

    uint32_t other_handle = *(uint32_t *)(midi_handle + 8);
    uint32_t internal_handle = *(uint32_t *)(other_handle + 0x128);

    midiInStop((HMIDIIN)midi_handle);
    midiInReset((HMIDIIN)midi_handle);

    if (CloseHandle((HANDLE)internal_handle) == 0)
    {
        fprintf(stderr, "CloseHandle error ( %x )\n", GetLastError());
    }

    /* Finally, call midiInClose to get a clean state (no deadlock will happen
     * now) */
    return (midiInClose((HMIDIIN)midi_handle) == MMSYSERR_NOERROR);
}

#pragma #endregion

#pragma #region "Get Launchpad input/output handles"
HMIDIOUT hLaunchPad = NULL;
HMIDIIN hLaunchPadIn = NULL;

char g_device_name[128] = {0};

int device_get_input_id()
{
    UINT nMidiDeviceNum;
    MIDIINCAPS caps;

    nMidiDeviceNum = midiInGetNumDevs();
    if (nMidiDeviceNum == 0)
    {
        return -1;
    }

    for (unsigned int i = 0; i < nMidiDeviceNum; ++i)
    {
        midiInGetDevCaps(i, &caps, sizeof(MIDIINCAPS));
        if (strstr(caps.szPname, "aunchpad") != NULL)
        {
            strcpy(g_device_name, caps.szPname);
            return i;
        }
    }
    return -1;
}

HMIDIIN device_get_input_handle()
{
    unsigned long result;
    HMIDIIN handle;

    int id = device_get_input_id();

    result = midiInOpen(&handle, id, (DWORD)(void *)on_midi_input_cb, 0, CALLBACK_FUNCTION);
    if (result == MMSYSERR_ALLOCATED)
    {
        fprintf(stderr, "Midi device is already opened (by spicetools?), io_launchpad will "
                        "attempt to reclaim it...\n");
        if (try_force_close(g_device_name))
        {
            fprintf(stderr, "Device closed succesfully\n");
            result = midiInOpen(&handle, id, (DWORD)(void *)on_midi_input_cb, 0, CALLBACK_FUNCTION);
            if (result != MMSYSERR_NOERROR)
            {
                fprintf(stderr,
                        "WARNING: Failed to open device despite claiming it (err %x), "
                        "booting without input support\n",
                        result);
                return NULL;
            }
            fprintf(stderr, "Device reclaimed!\n");
        }
        else
        {
            fprintf(stderr, "WARNING: Failed to reclaim device, booting without input support\n");
            return NULL;
        }
    }
    return handle;
}

int device_get_output_id()
{
    UINT nMidiDeviceNum;
    MIDIOUTCAPS caps;

    nMidiDeviceNum = midiOutGetNumDevs();
    if (nMidiDeviceNum == 0)
    {
        return -1;
    }

    for (unsigned int i = 0; i < nMidiDeviceNum; ++i)
    {
        midiOutGetDevCaps(i, &caps, sizeof(MIDIOUTCAPS));
        if (strstr(caps.szPname, "aunchpad") != NULL)
        {
            return i;
        }
    }
    return -1;
}

HMIDIOUT device_get_output_handle()
{
    unsigned long result;
    HMIDIOUT handle;
    int id = device_get_output_id();

    if (id == -1)
        return NULL;

    result = midiOutOpen(&handle, id, 0, 0, CALLBACK_NULL);
    if (result)
    {
        return NULL;
    }

    return handle;
}
#pragma #endregion

#pragma #region "Notes drawing"
void launchpad_turn_on_color(int numbut, int color)
{
    if (g_launchpad.state[numbut] == color)
        return;

    if (color == g_judgement_colors[2] && g_launchpad.state[numbut] == g_judgement_colors[1])
    {
        if (g_board_state.cooldown[numbut].in > 0)
        {
            g_board_state.cooldown[numbut].in--;
            return;
        }
    }
    else
    {
        g_board_state.cooldown[numbut].in = INITIAL_DELAY;
    }

    for (int i = 0; i < 4; i++)
        midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[i]) << 8) | color << 16);

    g_launchpad.state[numbut] = color;
}

void launchpad_animate_snake(int numbut, int color)
{
    static bool snake_reverse[16] = {false};

    if (g_launchpad.state[numbut] == color)
        return;

    /* 2 3 4 5 */
    if (color == g_judgement_colors[2] && g_launchpad.state[numbut] == g_judgement_colors[1])
    {
        if (g_board_state.cooldown[numbut].in > 0)
        {
            g_board_state.cooldown[numbut].in--;
            midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[0]) << 8) | color << 16);
            midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[1]) << 8) | color << 16);
            midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[2]) << 8) | color << 16);
            midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[3]) << 8) | color << 16);
            return;
        }
    }
    else
    {
        g_board_state.cooldown[numbut].in = INITIAL_DELAY;
    }

    {
        if (color == g_judgement_colors[0] || color == g_judgement_colors[1])
        {
            for (int i = 0; i < 4; i++)
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[i]) << 8) | color << 16);
        }
        else if (color == g_judgement_colors[2])
        {
            if (g_launchpad.state[numbut] == g_judgement_colors[1])
                snake_reverse[numbut] = false;
            else if (g_launchpad.state[numbut] == g_judgement_colors[3])
                snake_reverse[numbut] = true;

            int color1 = g_judgement_colors[5];
            int color2 = g_judgement_colors[2];
            if (snake_reverse[numbut])
            {
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[3]) << 8) | color1 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[2]) << 8) | color2 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[1]) << 8) | color2 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[0]) << 8) | color2 << 16);
            }
            else
            {
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[0]) << 8) | color1 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[1]) << 8) | color2 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[2]) << 8) | color2 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[3]) << 8) | color2 << 16);
            }
        }
        else if (color == g_judgement_colors[3])
        {
            if (g_launchpad.state[numbut] == g_judgement_colors[2])
                snake_reverse[numbut] = false;
            else if (g_launchpad.state[numbut] == g_judgement_colors[4])
                snake_reverse[numbut] = true;

            int color1 = g_judgement_colors[5];
            int color2 = g_judgement_colors[2];
            if (snake_reverse[numbut])
            {
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[3]) << 8) | color1 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[2]) << 8) | color1 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[1]) << 8) | color2 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[0]) << 8) | color2 << 16);
            }
            else
            {
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[0]) << 8) | color1 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[1]) << 8) | color1 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[2]) << 8) | color2 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[3]) << 8) | color2 << 16);
            }
        }
        else if (color == g_judgement_colors[4])
        {
            if (g_launchpad.state[numbut] == g_judgement_colors[3])
                snake_reverse[numbut] = false;
            else if (g_launchpad.state[numbut] == g_judgement_colors[5])
                snake_reverse[numbut] = true;

            int color1 = g_judgement_colors[5];
            int color2 = g_judgement_colors[2];
            if (snake_reverse[numbut])
            {
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[3]) << 8) | color1 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[2]) << 8) | color1 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[1]) << 8) | color1 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[0]) << 8) | color2 << 16);
            }
            else
            {
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[0]) << 8) | color1 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[1]) << 8) | color1 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[2]) << 8) | color1 << 16);
                midiOutShortMsg(hLaunchPad, 0x90 | ((g_launchpad.buttons[numbut].key[3]) << 8) | color2 << 16);
            }
        }
        else if (color == g_judgement_colors[5])
        {
            midiOutShortMsg(hLaunchPad,
                            0x90 | ((g_launchpad.buttons[numbut].key[0]) << 8) | g_judgement_colors[5] << 16);
            midiOutShortMsg(hLaunchPad,
                            0x90 | ((g_launchpad.buttons[numbut].key[1]) << 8) | g_judgement_colors[5] << 16);
            midiOutShortMsg(hLaunchPad,
                            0x90 | ((g_launchpad.buttons[numbut].key[2]) << 8) | g_judgement_colors[5] << 16);
            midiOutShortMsg(hLaunchPad,
                            0x90 | ((g_launchpad.buttons[numbut].key[3]) << 8) | g_judgement_colors[5] << 16);
        }
    }

    g_launchpad.state[numbut] = color;
}

void launchpad_turn_on_judge(int numbut, int judge)
{
    // launchpad_turn_on_color(numbut, g_judgement_colors[judge]);
    launchpad_animate_snake(numbut, g_judgement_colors[judge]);
}
#pragma #endregion

#pragma #region "Long notes drawing"

typedef enum dir_e
{
    DOWN = 0, /* trail going downwards */
    UP,
    RIGHT,
    LEFT
} dir_t;

void launchpad_turn_on_color_line(int refbut, int nb_act, int total, int color_off, int color_on, dir_t dir)
{
    uint8_t flipflop_idx = 0;
    uint8_t flipflop[2];
    switch (dir)
    {
    case LEFT:
        flipflop[0] = 0;
        flipflop[1] = 3;
        break;
    case RIGHT:
        flipflop[0] = 3;
        flipflop[1] = 0;
        break;
    case UP:
        flipflop[0] = 2;
        flipflop[1] = 3;
        break;
    case DOWN:
        flipflop[0] = 3;
        flipflop[1] = 2;
        break;
    }

    int count = 1;
    int color = color_on;
    int i = 1;
    while (count <= total)
    {
        if (count > nb_act)
            color = color_off;
        switch (dir)
        {
        case LEFT:
            if (g_launchpad.buttons[refbut + i].lock[flipflop[flipflop_idx]] == NO_LOCK ||
                g_launchpad.buttons[refbut + i].lock[flipflop[flipflop_idx]] == refbut)
            {
                midiOutShortMsg(hLaunchPad, 0x90 |
                                                ((g_launchpad.buttons[refbut + i].key[flipflop[flipflop_idx]]) << 8) |
                                                color << 16);
                g_launchpad.buttons[refbut + i].lock[flipflop[flipflop_idx]] = (color == color_on) ? refbut : NO_LOCK;
            }
            break;
        case RIGHT:
            if (g_launchpad.buttons[refbut - i].lock[flipflop[flipflop_idx]] == NO_LOCK ||
                g_launchpad.buttons[refbut - i].lock[flipflop[flipflop_idx]] == refbut)
            {
                midiOutShortMsg(hLaunchPad, 0x90 |
                                                ((g_launchpad.buttons[refbut - i].key[flipflop[flipflop_idx]]) << 8) |
                                                color << 16);
                g_launchpad.buttons[refbut - i].lock[flipflop[flipflop_idx]] = (color == color_on) ? refbut : NO_LOCK;
            }
            break;
        case UP:
            if (g_launchpad.buttons[refbut + 4 * i].lock[flipflop[flipflop_idx]] == NO_LOCK ||
                g_launchpad.buttons[refbut + 4 * i].lock[flipflop[flipflop_idx]] == refbut)
            {
                midiOutShortMsg(hLaunchPad,
                                0x90 | ((g_launchpad.buttons[refbut + 4 * i].key[flipflop[flipflop_idx]]) << 8) |
                                    color << 16);
                g_launchpad.buttons[refbut + 4 * i].lock[flipflop[flipflop_idx]] =
                    (color == color_on) ? refbut : NO_LOCK;
            }
            break;
        case DOWN:
            if (g_launchpad.buttons[refbut - 4 * i].lock[flipflop[flipflop_idx]] == NO_LOCK ||
                g_launchpad.buttons[refbut - 4 * i].lock[flipflop[flipflop_idx]] == refbut)
            {
                midiOutShortMsg(hLaunchPad,
                                0x90 | ((g_launchpad.buttons[refbut - 4 * i].key[flipflop[flipflop_idx]]) << 8) |
                                    color << 16);
                g_launchpad.buttons[refbut - 4 * i].lock[flipflop[flipflop_idx]] =
                    (color == color_on) ? refbut : NO_LOCK;
            }
            break;
        }
        count++;
        flipflop_idx = 1 - flipflop_idx;
        if (flipflop_idx == 0)
            i++;
    }
}

void launchpad_longhold(uint32_t data, float ratio)
{
    uint8_t numbut = data & 0xF;
    dir_t dir = (dir_t)((data & 0x30) >> 4);
    uint8_t distance = (data & 0xC0) >> 6;
    // uint16_t duration = data>>8;
    uint32_t nb_act = 0;

    nb_act = ratio * 2;

    if (nb_act > 0 && g_launchpad.state[numbut] == g_judgement_colors[0])
        launchpad_turn_on_color(numbut, g_judgement_colors[5]);

    launchpad_turn_on_color_line(numbut, nb_act, 2 * distance, g_judgement_colors[0], g_judgement_colors[4], dir);
}

void launchpad_draw_long(uint32_t data)
{
    uint8_t numbut = data & 0xF;
    dir_t dir = (dir_t)((data & 0x30) >> 4);
    uint8_t distance = (data & 0xC0) >> 6;

    if (g_board_state.cooldown[numbut].in < INITIAL_DELAY)
        launchpad_turn_on_color_line(numbut, 2 * distance, 2 * distance, g_judgement_colors[2], g_judgement_colors[2],
                                     dir);
}
#pragma #endregion

#pragma #region "Rank and FC/EXC display routines"
void light_row_col(int row, int col, int color)
{
    if (row % 8 != row || col % 8 != col)
        return;

    midiOutShortMsg(hLaunchPad, 0x90 | (row << 4 | col) << 8 | color << 16);
}

void firework_step(int col, int row, int color, int step)
{
    col = col & 0x07;
    row = row & 0x07;
    int key = (row << 4 | col);

    if (step == 0)
    {
        light_row_col(row, col, color);
        return;
    }

    if (step == 2)
    {
        light_row_col(row - 2, col - 1, color);
        light_row_col(row - 2, col - 0, color);
        light_row_col(row - 2, col + 1, color);

        light_row_col(row + 2, col - 1, color);
        light_row_col(row + 2, col - 0, color);
        light_row_col(row + 2, col + 1, color);

        light_row_col(row - 1, col - 2, color);
        light_row_col(row - 0, col - 2, color);
        light_row_col(row + 1, col - 2, color);

        light_row_col(row - 1, col + 2, color);
        light_row_col(row - 0, col + 2, color);
        light_row_col(row + 1, col + 2, color);

        return;
    }

    for (int i = col - step; i <= col + step; i += step)
    {
        for (int j = row - step; j <= row + step; j += step)
        {
            if (i % 8 != i || j % 8 != j || i == col && j == row)
                continue;

            light_row_col(j, i, color);
        }
    }
    return;
}

void fc_fireworks()
{
    srand(time(NULL));
    int col = rand() % 8;
    int row = rand() % 8;
    int col2 = rand() % 8;
    int row2 = rand() % 8;
    int i = 0;

    for (int a = 0; a < 10; a++)
    {
        midiOutShortMsg(hLaunchPad, LAUNCHPAD_CLEAR);
        firework_step(col, row, RED, i);
        firework_step(col2, row2, AMBER, ((i - 2) % 4 + 4) % 4);
        i++;
        if (i == 2)
        {
            col2 = rand() % 8;
            row2 = rand() % 8;
        }
        if (i == 4)
        {
            col = rand() % 8;
            row = rand() % 8;
            i = 0;
        }

        Sleep(200);
    }
    midiOutShortMsg(hLaunchPad, LAUNCHPAD_CLEAR);
}

void exc_fireworks()
{
    srand(time(NULL));
    int col = rand() % 8;
    int row = rand() % 8;
    int col2 = rand() % 8;
    int row2 = rand() % 8;
    int col3 = rand() % 8;
    int row3 = rand() % 8;
    int i = 0;

    for (int a = 0; a < 10; a++)
    {
        midiOutShortMsg(hLaunchPad, LAUNCHPAD_CLEAR);
        firework_step(col, row, RED, i);
        firework_step(col2, row2, AMBER, ((i - 2) % 4 + 4) % 4);
        firework_step(col3, row3, GREEN, ((i - 3) % 4 + 4) % 4);
        i++;
        if (i == 2)
        {
            col2 = rand() % 8;
            row2 = rand() % 8;
        }
        if (i == 3)
        {
            col3 = rand() % 8;
            row3 = rand() % 8;
        }
        if (i == 4)
        {
            col = rand() % 8;
            row = rand() % 8;
            i = 0;
        }

        Sleep(150);
    }
    midiOutShortMsg(hLaunchPad, LAUNCHPAD_CLEAR);
}

void disp_rank_EXC(int row, int col, int color, int color2)
{
    /* E */
    light_row_col(row, col, color);
    light_row_col(row, col + 1, color);
    light_row_col(row, col + 2, color);
    light_row_col(row + 1, col, color);
    light_row_col(row + 2, col, color);
    light_row_col(row + 2, col + 1, color);
    light_row_col(row + 3, col, color);
    light_row_col(row + 4, col, color);
    light_row_col(row + 4, col + 1, color);
    light_row_col(row + 4, col + 2, color);

    /* X */
    light_row_col(row + 2, col + 2, color2);
    light_row_col(row + 2, col + 4, color2);
    light_row_col(row + 3, col + 2, color2);
    light_row_col(row + 3, col + 4, color2);
    light_row_col(row + 4, col + 3, color2);
    light_row_col(row + 5, col + 2, color2);
    light_row_col(row + 5, col + 4, color2);
    light_row_col(row + 6, col + 2, color2);
    light_row_col(row + 6, col + 4, color2);

    /* C */
    light_row_col(row + 4, col + 5, color);
    light_row_col(row + 4, col + 6, color);
    light_row_col(row + 4, col + 7, color);
    light_row_col(row + 5, col + 5, color);
    light_row_col(row + 6, col + 5, color);
    light_row_col(row + 7, col + 5, color);
    light_row_col(row + 7, col + 6, color);
    light_row_col(row + 7, col + 7, color);
}

void disp_rank_S(int row, int col, int color)
{
    light_row_col(row, col + 1, color);
    light_row_col(row, col + 2, color);

    light_row_col(row + 1, col, color);
    light_row_col(row + 1, col + 3, color);

    light_row_col(row + 2, col, color);
    light_row_col(row + 3, col + 1, color);
    light_row_col(row + 3, col + 2, color);
    light_row_col(row + 4, col + 3, color);

    light_row_col(row + 5, col, color);
    light_row_col(row + 5, col + 3, color);
    light_row_col(row + 6, col + 1, color);
    light_row_col(row + 6, col + 2, color);
}

void disp_rank_SS(int row, int col, int color)
{
    disp_rank_S(row, col, color);

    light_row_col(row, col + 6, color);
    light_row_col(row + 1, col + 5, color);
    light_row_col(row + 1, col + 6, color);
    light_row_col(row + 1, col + 7, color);
    light_row_col(row + 2, col + 6, color);
}

void disp_rank_SSS(int row, int col, int color)
{
    disp_rank_SS(row, col, color);

    light_row_col(row + 4, col + 6, color);
    light_row_col(row + 5, col + 5, color);
    light_row_col(row + 5, col + 6, color);
    light_row_col(row + 5, col + 7, color);
    light_row_col(row + 6, col + 6, color);
}

void disp_rank_A(int row, int col, int color)
{
    light_row_col(row + 0, col + 1, color);
    light_row_col(row + 0, col + 2, color);
    light_row_col(row + 1, col, color);
    light_row_col(row + 1, col + 3, color);
    light_row_col(row + 2, col, color);
    light_row_col(row + 2, col + 1, color);
    light_row_col(row + 2, col + 2, color);
    light_row_col(row + 2, col + 3, color);
    light_row_col(row + 3, col, color);
    light_row_col(row + 3, col + 3, color);
    light_row_col(row + 4, col, color);
    light_row_col(row + 4, col + 3, color);
}

void disp_rank_B(int row, int col, int color)
{
    light_row_col(row + 0, col, color);
    light_row_col(row + 0, col + 1, color);
    light_row_col(row + 0, col + 2, color);
    light_row_col(row + 0, col + 3, color);
    light_row_col(row + 1, col, color);
    light_row_col(row + 1, col + 3, color);
    light_row_col(row + 2, col, color);
    light_row_col(row + 2, col + 1, color);
    light_row_col(row + 2, col + 2, color);
    light_row_col(row + 3, col, color);
    light_row_col(row + 3, col + 3, color);
    light_row_col(row + 4, col, color);
    light_row_col(row + 4, col + 1, color);
    light_row_col(row + 4, col + 2, color);
    light_row_col(row + 4, col + 3, color);
}

void disp_rank_C(int row, int col, int color)
{
    light_row_col(row + 0, col, color);
    light_row_col(row + 0, col + 1, color);
    light_row_col(row + 0, col + 2, color);
    light_row_col(row + 0, col + 3, color);
    light_row_col(row + 1, col, color);
    light_row_col(row + 2, col, color);
    light_row_col(row + 3, col, color);
    light_row_col(row + 4, col, color);
    light_row_col(row + 4, col + 1, color);
    light_row_col(row + 4, col + 2, color);
    light_row_col(row + 4, col + 3, color);
}

void disp_rank_D(int row, int col, int color)
{
    light_row_col(row + 0, col, color);
    light_row_col(row + 0, col + 1, color);
    light_row_col(row + 0, col + 2, color);
    light_row_col(row + 1, col, color);
    light_row_col(row + 1, col + 3, color);
    light_row_col(row + 2, col, color);
    light_row_col(row + 2, col + 3, color);
    light_row_col(row + 3, col, color);
    light_row_col(row + 3, col + 3, color);
    light_row_col(row + 4, col, color);
    light_row_col(row + 4, col + 1, color);
    light_row_col(row + 4, col + 2, color);
}

void disp_rank_E(int row, int col, int color)
{
    light_row_col(row + 0, col, color);
    light_row_col(row + 0, col + 1, color);
    light_row_col(row + 0, col + 2, color);
    light_row_col(row + 0, col + 3, color);
    light_row_col(row + 1, col, color);
    light_row_col(row + 2, col, color);
    light_row_col(row + 2, col + 1, color);
    light_row_col(row + 2, col + 2, color);
    light_row_col(row + 3, col, color);
    light_row_col(row + 4, col, color);
    light_row_col(row + 4, col + 1, color);
    light_row_col(row + 4, col + 2, color);
    light_row_col(row + 4, col + 3, color);
}

void launchpad_show_rank(int score)
{
    if (score == 1000000)
    {
        disp_rank_EXC(0, 0, GREEN, AMBER);
        return;
    }

    if (score >= 980000)
    {
        disp_rank_SSS(0, 0, GREEN);
        return;
    }

    if (score >= 950000)
    {
        disp_rank_SS(0, 0, GREEN);
        return;
    }

    if (score >= 900000)
    {
        disp_rank_S(0, 2, GREEN);
        return;
    }

    if (score >= 850000)
    {
        disp_rank_A(1, 2, GREEN);
        return;
    }

    if (score >= 800000)
    {
        disp_rank_B(1, 2, GREEN);
        return;
    }

    if (score >= 700000)
    {
        disp_rank_C(1, 2, GREEN);
        return;
    }

    if (score >= 500000)
    {
        disp_rank_D(1, 2, RED);
        return;
    }

    disp_rank_E(1, 2, RED);
}

void disp_f(int row, int col, int color)
{
    light_row_col(row + 0, col, color);
    light_row_col(row + 0, col + 1, color);
    light_row_col(row + 0, col + 2, color);
    light_row_col(row + 1, col, color);
    light_row_col(row + 2, col, color);
    light_row_col(row + 2, col + 1, color);
    light_row_col(row + 3, col, color);
    light_row_col(row + 4, col, color);
}

void disp_e(int row, int col, int color)
{
    light_row_col(row + 0, col, color);
    light_row_col(row + 0, col + 1, color);
    light_row_col(row + 0, col + 2, color);
    light_row_col(row + 1, col, color);
    light_row_col(row + 2, col, color);
    light_row_col(row + 2, col + 1, color);
    light_row_col(row + 2, col + 2, color);
    light_row_col(row + 3, col, color);
    light_row_col(row + 4, col, color);
    light_row_col(row + 4, col + 1, color);
    light_row_col(row + 4, col + 2, color);
}

void disp_x(int row, int col, int color)
{
    light_row_col(row + 0, col, color);
    light_row_col(row + 1, col, color);
    light_row_col(row + 3, col, color);
    light_row_col(row + 4, col, color);
    light_row_col(row + 2, col + 1, color);
    light_row_col(row + 0, col + 2, color);
    light_row_col(row + 1, col + 2, color);
    light_row_col(row + 3, col + 2, color);
    light_row_col(row + 4, col + 2, color);
}

void disp_c(int row, int col, int color)
{
    light_row_col(row + 0, col, color);
    light_row_col(row + 0, col + 1, color);
    light_row_col(row + 0, col + 2, color);
    light_row_col(row + 1, col, color);
    light_row_col(row + 2, col, color);
    light_row_col(row + 3, col, color);
    light_row_col(row + 4, col, color);
    light_row_col(row + 4, col + 1, color);
    light_row_col(row + 4, col + 2, color);
}

void disp_exc()
{
    for (int i = 8; i >= -12; i--)
    {
        disp_e(1, i, RED);
        disp_x(1, i + 4, GREEN);
        disp_c(1, i + 8, AMBER);
        Sleep(80);
        midiOutShortMsg(hLaunchPad, LAUNCHPAD_CLEAR);
    }
}

void disp_fc()
{
    for (int i = 8; i >= -8; i--)
    {
        disp_f(1, i, RED);
        disp_c(1, i + 4, AMBER);
        Sleep(80);
        midiOutShortMsg(hLaunchPad, LAUNCHPAD_CLEAR);
    }
}
#pragma #endregion

#pragma #region "Utility functions"
const char *score_to_rank_str(int score)
{
    if (score == 1000000)
        return "EXC";

    if (score >= 980000)
        return "SSS";

    if (score >= 950000)
        return "SS";

    if (score >= 900000)
        return "S";

    if (score >= 850000)
        return "A";

    if (score >= 800000)
        return "B";

    if (score >= 700000)
        return "C";

    if (score >= 500000)
        return "D";

    return "E";
}

const char *judgement_to_str(int judge)
{
    switch (judge)
    {
    case MISSED:
        return "MISSED";
    case OUTSIDE:
        return "OUTSIDE";
    case GOOD:
        return "GOOD";
    case VERY_GOOD:
        return "VERY GOOD";
    case PERFECT:
        return "PERFECT";
    default:
        return "UNKNOWN";
    }
}

uint8_t launchpad_compute_color(int red, int green)
{
    return ((red & 0x3) | 0xC | (green & 0x3) << 4) & 0x3F;
}

int launchpad_compute_duty_cycle(int numer, int denom)
{
    if (numer < 9)
        return ((0x1E << 8) | (((numer - 1) << 4 | (denom - 3))) << 16);

    return (0x1F << 8 | (((numer - 9) << 4 | (denom - 3))) << 16);
}

void reinit_panel()
{
    static DWORD last_call_time = 0;

    DWORD curr_call_time = timeGetTime();

    if (curr_call_time - last_call_time < 5000) // 5 sec cooldown to handle the engine sending multiple PREVIEW
                                                // note event at once
    {
        return;
    }

    midiOutShortMsg(hLaunchPad, LAUNCHPAD_CLEAR); // turn off all lights
    for (int i = 0; i < 16; i++)                  // remove all locks
    {
        g_board_state.cooldown[i].in = INITIAL_DELAY;
        g_board_state.cooldown[i].out = 0;
        g_launchpad.state[i] = OFF;
        for (int j = 0; j < 4; j++)
        {
            g_launchpad.buttons[i].lock[j] = NO_LOCK;
        }
    }

    last_call_time = curr_call_time;
    return;
}

#pragma #endregion

#pragma #region "DLL EXPORTED FUNCTIONS"
extern "C" __declspec(dllexport) uint32_t io_plugin_get_input()
{
    /*
    uint32_t ret = 0;
    for (int i=0; i<19; i++)
    {
            if ( g_button_state[i] )
                    ret |= 1 << g_conversion_table[i];
    }
    return ret;
    */
    return g_jb_inputstate;
}

extern "C" __declspec(dllexport) bool io_plugin_open_input()
{
    if (hLaunchPadIn != NULL)
        return true;

    hLaunchPadIn = device_get_input_handle();

    if (hLaunchPadIn != NULL)
    {
        midiInStart(hLaunchPadIn);

        if (hLaunchPad != NULL)
        {
            midiOutShortMsg(hLaunchPad, LAUNCHPAD_CLEAR);
            launchpad_turn_on_color(5, launchpad_compute_color(0, 3));
            launchpad_turn_on_color(6, launchpad_compute_color(0, 3));
            launchpad_turn_on_color(9, launchpad_compute_color(0, 3));
            launchpad_turn_on_color(10, launchpad_compute_color(0, 3));
            Sleep(500);
            midiOutShortMsg(hLaunchPad, LAUNCHPAD_CLEAR);
        }
    }

    return (hLaunchPadIn != NULL);
}

extern "C" __declspec(dllexport) bool io_plugin_open_output()
{
    if (hLaunchPad == NULL)
        hLaunchPad = device_get_output_handle();
    else
        return true;

    if (hLaunchPad != NULL)
    {
        midiOutShortMsg(hLaunchPad, LAUNCHPAD_CLEAR);
        launchpad_turn_on_color(0, launchpad_compute_color(2, 2));
        launchpad_turn_on_color(3, launchpad_compute_color(2, 2));
        launchpad_turn_on_color(12, launchpad_compute_color(2, 2));
        launchpad_turn_on_color(15, launchpad_compute_color(2, 2));
        Sleep(500);
        midiOutShortMsg(hLaunchPad, LAUNCHPAD_CLEAR);
    }

    return (hLaunchPad != NULL);
}

extern "C" __declspec(dllexport) bool io_plugin_init()
{
    g_judgement_colors[0] = launchpad_compute_color(0, 0);
    g_judgement_colors[1] = launchpad_compute_color(0, 0);
    g_judgement_colors[2] = launchpad_compute_color(1, 0);
    g_judgement_colors[3] = launchpad_compute_color(2, 1);
    g_judgement_colors[4] = launchpad_compute_color(2, 2);
    g_judgement_colors[5] = launchpad_compute_color(3, 2);
    g_judgement_colors[6] = launchpad_compute_color(3, 3);

    g_judgement_press_colors[0] = launchpad_compute_color(0, 0);
    g_judgement_press_colors[1] = launchpad_compute_color(0, 0);
    g_judgement_press_colors[2] = launchpad_compute_color(3, 0);
    g_judgement_press_colors[3] = launchpad_compute_color(3, 1);
    g_judgement_press_colors[4] = launchpad_compute_color(3, 3);
    g_judgement_press_colors[5] = launchpad_compute_color(0, 3);

    reinit_panel();

    return true;
}

extern "C" __declspec(dllexport) bool io_plugin_deinit()
{
    if (hLaunchPadIn != NULL)
        midiInClose(hLaunchPadIn);

    if (hLaunchPad != NULL)
    {
        midiOutShortMsg(hLaunchPad, LAUNCHPAD_CLEAR);
        midiOutClose(hLaunchPad);
    }

    return true;
}

extern "C" __declspec(dllexport) bool io_plugin_board_update(uint32_t note_data, cell_state_t new_status,
                                                             void *extra_data)
{
    // the various forms extra_data can take
    float ratio = 0.;
    scoredata_t *scoredata = NULL;
    uint32_t score = 0;

    switch (new_status)
    {
    /* Button animation */
    case WAITING_FOR_PRESS:
    case MISSED:
    case OUTSIDE:
    case GOOD:
    case VERY_GOOD:
    case PERFECT:
        launchpad_turn_on_judge(note_data & 0x0F, new_status);
        break;
    case LONG_TRAIL_DRAW_FIRST:
    case LONG_TRAIL_DRAW_CONT:
        launchpad_draw_long(note_data);
        break;

    /* Button presses */
    case PRESSED_OUTSIDE:
    case PRESSED_GOOD:
    case PRESSED_VERY_GOOD:
    case PRESSED_PERFECT:
        scoredata = (scoredata_t *)extra_data;
        /* printf("\tJUDGE BUTTON %08x (%s) (score: %d , combo: %d)\n", note_data, judgement_to_str(new_status & 0x07),
               scoredata->score, scoredata->combo); */
        launchpad_turn_on_color(note_data & 0x0F,
                                g_judgement_press_colors[new_status & 0x07]); // &0x07 removes the PRESSED flag,
                                                                              // retains judgement value
        g_board_state.cooldown[note_data & 0x0F].out = JUDGE_COOLDOWN;
        break;
    case LONG_NOTE_RELEASE_FIRST:
        ratio = 0;
        launchpad_longhold(note_data, ratio);
        g_board_state.cooldown[note_data & 0x0F].out = JUDGE_COOLDOWN;
    case LONG_NOTE_RELEASE_CONT:
        break;
    case LONG_NOTE_MISS_FIRST:
        ratio = 0;
        launchpad_longhold(note_data, ratio);
    case LONG_NOTE_MISS_CONT:
        break;
    case LONG_TRAIL_UPDATE:
        ratio = *(float *)extra_data;
        launchpad_longhold(note_data, ratio);
        break;

    /* Preview before song start */
    case PREVIEW:
        printf("PREVIEW %d\n", note_data & 0xF);
        reinit_panel();
        launchpad_turn_on_color(note_data & 0xF, g_judgement_press_colors[5]);
        g_board_state.cooldown[note_data & 0x0F].out = JUDGE_COOLDOWN * 16;
        break;

    /* End of song */
    case FINAL_SCORE:
        score = *(uint32_t *)extra_data;
        printf("FINAL SCORE : %d (song is %s , rank %s)\n", score, score >= 700000 ? "CLEARED" : "FAILED",
               score_to_rank_str(score));
        launchpad_show_rank(score);
        break;
    case FULL_COMBO:
        fc_fireworks();
        break;
    case EXCELLENT:
        exc_fireworks();
        break;
    case FULL_COMBO_ANIM:
        score = *(uint32_t *)extra_data;
        disp_fc();
        launchpad_show_rank(score);
        break;
    case EXCELLENT_ANIM:
        score = *(uint32_t *)extra_data;
        disp_exc();
        launchpad_show_rank(score);
        break;
    case LEAVE_RESULT_SCREEN:
        reinit_panel();
        break;

    /* Force off (after cooldown) */
    case INACTIVE:
        if (g_board_state.cooldown[note_data & 0x0F].out > 0)
        {
            g_board_state.cooldown[note_data & 0x0F].out--;

            if (g_board_state.cooldown[note_data & 0x0F].out == 0)
            {
                launchpad_turn_on_color(note_data & 0x0F, OFF);
            }
        }
        break;
    default:
        break;
    }

    return true;
}

extern "C" __declspec(dllexport) int io_plugin_get_api_version()
{
    return 1;
}

#pragma #endregion

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
