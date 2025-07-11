#ifndef __IO_DUMMY_H__
#define __IO_DUMMY_H__

#include <stdbool.h>

#include "../jubeathax/cell_state.h"

extern "C" __declspec(dllexport) uint32_t io_plugin_get_input();
extern "C" __declspec(dllexport) int io_plugin_get_api_version();
extern "C" __declspec(dllexport) bool io_plugin_init();
extern "C" __declspec(dllexport) bool io_plugin_deinit();
extern "C" __declspec(dllexport) bool io_plugin_open_input();
extern "C" __declspec(dllexport) bool io_plugin_open_output();
extern "C" __declspec(dllexport) bool io_plugin_board_update(uint32_t note_data, cell_state_t new_status,
                                                             void *extra_data);
#endif
