/*
 * Client/input.h -- client input controller (P2.12).
 *
 * Converts physical input edges into bounded protocol-v1 INPUT_BATCH commands
 * at 15 Hz.  It is deliberately pure C: it includes no SDL and no engine
 * header, and it knows nothing about SDL scancodes or the simulation's
 * IntentOp.  platform_sdl.c is the only unit that sees SDL scancodes and maps
 * them to the neutral XtkControl vocabulary below; the server (P2.7) maps the
 * wire XtkCommandOp to the engine IntentOp.  The three vocabularies never meet
 * in one translation unit, so no raw keyboard state can reach the engine.
 *
 * Input is edge-triggered, not raw keyboard state: a press produces a command
 * once (FIRE / WEAPON) or a held command that is refreshed each tick (drive,
 * fire-hold, world aim).  Every batch repeats the previous
 * XTK_INPUT_REDUNDANCY_TICKS tick batches, so a lost datagram is recovered
 * without duplicating an edge (the server deduplicates by command_id).
 *
 * The controller never blocks (no I/O, no sleep, no allocation) and never
 * emits on tick 0 or outside a Running match.  Pause is only honored outside
 * Running; quit and window defocus release every held control.  Window-defocus
 * release is unconditional, because a stuck control must never outlive focus.
 *
 * Authority: docs/PHASE2-PLAN.md P2.12; docs/PROTOCOL-v1.md sections 6, 8, 9;
 * docs/PHASE2-DECISIONS.md D2-E; Src/input.c:input_event_intent() (read-only
 * mapping reference).
 */

#ifndef XTK_CLIENT_INPUT_H
#define XTK_CLIENT_INPUT_H

#include <stdint.h>

#include "wire.h"	/* XtkCommand, XtkCommandOp, XtkInputBatch, XtkMessage */
#include "camera.h"	/* XtkCamera (screen -> world inverse transform)       */

/* ---- Neutral control vocabulary (no SDL scancodes, no IntentOp) ------ */

typedef enum {
	XTK_CTL_NONE = 0,

	/* Meta controls; never emitted as gameplay. */
	XTK_CTL_PAUSE,
	XTK_CTL_QUIT,

	/* Held drive controls; released (drive 0) when lifted. */
	XTK_CTL_DRIVE_FWD,
	XTK_CTL_DRIVE_BACK,

	/* Edge fire and held fire. */
	XTK_CTL_FIRE,		/* one-shot edge; fires once per press */
	XTK_CTL_FIRE_HOLD,	/* held */

	/* Edge weapon selects (release path: weapon on). */
	XTK_CTL_WEAPON_1,
	XTK_CTL_WEAPON_2,
	XTK_CTL_WEAPON_3,
	XTK_CTL_WEAPON_4,
	XTK_CTL_WEAPON_5,
	XTK_CTL_WEAPON_6,

	XTK_CTL_COUNT
} XtkControl;

/* Client session state (mirrors docs/PROTOCOL-v1.md section 9). */
typedef enum {
	XTK_INPUT_DISCONNECTED = 0,
	XTK_INPUT_LOBBY,
	XTK_INPUT_RUNNING,
	XTK_INPUT_RESULTS
} XtkInputState;

/* Relative drive magnitude for a digital key, inside the protocol [-9, 9]. */
#define XTK_INPUT_DRIVE_SPEED 3.0

/* Bounded batch: the codec's command bound (Protocol/limits.h). */
#define XTK_INPUT_MAX_BATCH   XTK_MAX_INPUT_COMMANDS_PER_BATCH

/* Per-tick command storage: the two redundant ticks plus the current tick. */
#define XTK_INPUT_RING       (XTK_INPUT_REDUNDANCY_TICKS + 1u)

typedef struct {
	XtkInputState state;
	int           focused;		/* window focus; 0 releases held controls */
	int           paused;		/* pause toggled outside Running          */
	int           quit;		/* quit requested                          */

	/* Physical held state for held controls. */
	uint8_t       held[XTK_CTL_COUNT];
	/* Latched one-shot presses not yet emitted (edge controls). */
	uint8_t       edge[XTK_CTL_COUNT];

	/*
	 * Controlled entity-life generation (0 = not yet known).  Every emitted
	 * command is stamped with it so the server can reject a delayed edge
	 * from a previous life even inside the expiry window; on a change to a
	 * new known life the latched one-shot edges and the redundancy history
	 * are discarded so no stale edge can fire under the new life.
	 */
	uint32_t      life_generation;

	/* World aim (world coordinates; already camera/DPI transformed). */
	int           aim_valid;
	double        aim_x, aim_y;

	/* Last transmitted held values, for release-on-lift. */
	double        sent_drive;
	int           have_sent_drive;
	int           sent_fire_hold;
	int           have_sent_fire_hold;

	uint32_t      next_command_id;
	uint32_t      next_packet_sequence;

	/* Ring of per-tick produced commands, oldest .. newest. */
	XtkCommand    ring[XTK_INPUT_RING][XTK_INPUT_MAX_BATCH];
	uint8_t       ring_count[XTK_INPUT_RING];
	int           ring_head;	/* slot of the newest tick   */
	int           ring_filled;	/* valid slots, 0 .. RING    */
} XtkInputController;

/* Initialise an idle, unfocused-safe controller (state DISCONNECTED). */
void input_init(XtkInputController *ic);

/* Clear all input state (as on match reset); ids kept monotonic. */
void input_reset(XtkInputController *ic);

/* Set the client session state; leaving Running releases held gameplay. */
void input_set_state(XtkInputController *ic, XtkInputState state);
XtkInputState input_state(const XtkInputController *ic);

/* Window focus.  Focus loss releases every held control unconditionally. */
void input_set_focus(XtkInputController *ic, int focused);
int  input_focused(const XtkInputController *ic);

/*
 * Feed one neutral control edge.  `down` selects press vs release.  Pause is
 * honored only outside Running; quit and defocus release all held controls.
 * Gameplay controls are recorded only while Running.
 */
void input_control_event(XtkInputController *ic, XtkControl ctl, int down);

/*
 * Set the controlled entity-life generation observed from the authorized
 * views.  `generation` 0 means "not yet known" and leaves the controller
 * unchanged; a change to a different known life discards the latched one-shot
 * edges and the redundancy history so an edge produced in the previous life
 * can never be replayed under the new one.  Command ids stay monotonic.
 */
void input_set_life_generation(XtkInputController *ic, uint32_t generation);
uint32_t input_life_generation(const XtkInputController *ic);

/* Set the current world aim point (camera/DPI transform already applied). */
void input_aim_world(XtkInputController *ic, double world_x, double world_y);

/*
 * Inverse of the renderer's world->screen transform (Client/render_sdl.c):
 * screen = world - (center - viewport/2).  A logical window point is scaled to
 * drawable pixels first.  Pure; used to turn a mouse position into world aim.
 */
void input_screen_to_world(const XtkCamera *cam, float logical_x, float logical_y,
			   float display_scale, double *world_x, double *world_y);

/* Queries. */
int input_paused(const XtkInputController *ic);
int input_quit_requested(const XtkInputController *ic);

/*
 * Emit one 15 Hz INPUT_BATCH.  Returns the command count (0 = nothing sent).
 * Never emits for server_tick <= 0 or when the state is not Running.  The
 * command list is ordered oldest tick first and includes the previous
 * XTK_INPUT_REDUNDANCY_TICKS tick batches, bounded by XTK_INPUT_MAX_BATCH.  When
 * no control is active a single HEARTBEAT keeps the session alive.  `out` is
 * fully initialised; on a zero return it is left as an empty INPUT_BATCH.
 */
int input_emit(XtkInputController *ic, int32_t server_tick, uint32_t epoch,
	       uint32_t session_id, XtkMessage *out);

/*
 * As input_emit(), but also produces the reliable companion (channel 0) for
 * one-shot edges and held releases.  The companion carries the SAME command
 * ids as the redundant INPUT_BATCH copies, so a datagram loss is recovered
 * reliably while the server's shared command-id dedup applies each at most
 * once.  `reliable_out` is fully initialised; when there is anything to send
 * reliably `*have_reliable` is set to 1, else 0.  `reliable_out` and
 * `have_reliable` may both be NULL (equivalent to input_emit()).
 */
int input_emit_reliable(XtkInputController *ic, int32_t server_tick,
			uint32_t epoch, uint32_t session_id,
			XtkMessage *batch_out, XtkMessage *reliable_out,
			int *have_reliable);

#endif /* XTK_CLIENT_INPUT_H */
