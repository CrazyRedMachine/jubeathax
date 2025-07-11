#ifndef __CELL_STATE_H__
#define __CELL_STATE_H__

typedef struct __attribute__((packed, aligned(4))) scoredata_s {
	uint32_t score;
	uint32_t combo;
} scoredata_t;

#define PRESSED (1<<5) /* arbitrary flag for PRESSED cell states */
typedef enum cell_state_e {
	/* PENDING values: coincide with internal judgement states for convenience (meant for animating cells while waiting for press) */
	WAITING_FOR_PRESS       = 0,                 /* cell is active but not in timing window yet */
	MISSED                  = 1,                 /* button wasn't pressed on time */
	OUTSIDE                 = 2,                 /* button would be very early or very late (outside "good" windows, [-155 ; 85] ) */
	GOOD                    = 3,                 /* button would be early or late (blue "good" window, [-48 ; 48]) */
	VERY_GOOD               = 4,                 /* button would be slightly early or late (green "good" window, [-24 ; 24]) */
	PERFECT                 = 5,                 /* button would be on time! ("perfect" window, [-12 ; 12]) */

	/* "Pressed" values: internal judgement states combined with the PRESSED flag (meant to handle button presses) */
	PRESSED_OUTSIDE         = PRESSED|OUTSIDE,   /* button was pressed very early or very late */
	PRESSED_GOOD            = PRESSED|GOOD,      /* button was pressed early or late (blue "good" window) */
	PRESSED_VERY_GOOD       = PRESSED|VERY_GOOD, /* button was pressed slightly early or late (green "good" window) */
	PRESSED_PERFECT         = PRESSED|PERFECT,   /* button was pressed on time ("perfect" window) */

	/* Long note trail control (meant to handle long notes in combination with the PRESSED states) */
	LONG_TRAIL_DRAW_FIRST   = 128,               /* a long note trail has appeared (sent only the first time) */
	LONG_TRAIL_DRAW_CONT    = 129,               /* a long note trail has appeared (sent as long as the event is still part of the current chart chunk) */
	LONG_TRAIL_UPDATE       = 130,               /* update long note trail while holding the button */
	LONG_NOTE_RELEASE_FIRST = 131,               /* the long note has been released (sent only the first time) */
	LONG_NOTE_RELEASE_CONT  = 132,               /* the long note has been released (sent as long as the event is still part of the current chart chunk) */
	LONG_NOTE_MISS_FIRST    = 133,               /* the long note wasn't pressed on time (sent only the first time) */
	LONG_NOTE_MISS_CONT     = 134,               /* the long note wasn't pressed on time (sent as long as the event is still part of the current chart chunk) */

	/* other board states */
	LEAVE_RESULT_SCREEN     = 247,               /* game leaves result screen */
	PREVIEW                 = 248,               /* cell will be part of the starting notes (as displayed ingame before the song starts) */
	FINAL_SCORE             = 249,               /* final updated score (including end of song bonus) */
	FULL_COMBO              = 250,               /* the song just ended with a FULL COMBO clear */
	EXCELLENT               = 251,               /* the song just ended with an EXCELLENT clear */
	FULL_COMBO_ANIM         = 252,               /* the game triggers the FULL COMBO animation */
	EXCELLENT_ANIM          = 253,               /* the game triggers the EXCELLENT animation */

	/* Additional control states */
	INACTIVE                = 254,               /* cell has no ongoing event */
	UNCHANGED               = 255,               /* magic value to retain previous state in board_update call */
} cell_state_t;

#endif