/*
 * Client/model.h -- bounded authorized client model (P2.11).
 *
 * The client model is the presentation-side, read-only store of the authorized
 * VIEW_SNAPSHOT stream.  It is intentionally small and bounded:
 *
 *   - a ring of the newest XTK_SNAPSHOT_RING (8) snapshots per epoch, in
 *     ascending server-tick order;
 *   - a monotonic estimated server timeline (Client/timeline.h), so rendering
 *     happens at estimated server time minus the one-tick buffer (D2-E);
 *   - epoch/lifetime handling: a new epoch clears history and snaps, a stale
 *     epoch is ignored, duplicate or reordered snapshots are dropped, and
 *     entities are interpolated only between matching `(id, generation)` keys;
 *   - a fixed camera that follows the controlled player's displayed pose
 *     (Client/camera.h), including the frozen death/underrun pose.
 *
 * It performs no local movement, damage or RNG simulation, sends no command,
 * and never extrapolates.  Interpolating a snapshot copies the older
 * authoritative record as the discrete base, so armor/ammo/walls/scores/results
 * are held, never blended; only positions and shortest-arc headings of
 * lifetime-matched dynamic entities move.
 *
 * A XtkClientModel embeds the snapshot ring and last output, so it is large
 * (hundreds of KiB); allocate it on the heap, not the stack.
 *
 * Authority: docs/PHASE2-PLAN.md §3 and P2.11; docs/PHASE2-DECISIONS.md D2-E;
 * docs/VIEW-v1.md.
 */

#ifndef XTK_CLIENT_MODEL_H
#define XTK_CLIENT_MODEL_H

#include <stdint.h>

#include "wire.h"
#include "timeline.h"
#include "camera.h"

/* Bounded snapshot ring (Protocol/limits.h XTK_SNAPSHOT_RING). */
#define XTK_MODEL_RING XTK_SNAPSHOT_RING

/* Approved one-tick (66.7 ms) interpolation buffer (D2-E). */
#define XTK_MODEL_BUFFER_TICKS 1.0

/*
 * A movement larger than this between two consecutive lifetime-matched vehicle
 * snapshots is treated as a discontinuity (respawn/teleport) and snapped rather
 * than interpolated.  It is far above any legitimate per-tick vehicle motion
 * and well below a map-crossing jump.
 */
#define XTK_MODEL_TELEPORT_DISTANCE 256.0

/* Outcome of feeding one snapshot. */
typedef enum {
	XTK_OBSERVE_ACCEPTED     = 0,	/* stored and applied               */
	XTK_OBSERVE_DUPLICATE    = 1,	/* stale sequence or non-newer tick */
	XTK_OBSERVE_STALE_EPOCH  = 2,	/* older than the current epoch     */
	XTK_OBSERVE_EPOCH_RESET  = 3	/* newer epoch; history cleared     */
} XtkObserveResult;

/* Result of producing a displayable frame. */
typedef enum {
	XTK_MODEL_NO_DATA      = 0,	/* nothing received yet             */
	XTK_MODEL_BUFFERING    = 1,	/* render time before the oldest    */
	XTK_MODEL_OK           = 2,	/* interpolating between two views  */
	XTK_MODEL_UNDERRUN     = 3,	/* frozen at the newest authorized  */
	XTK_MODEL_SNAPPED      = 4,	/* discontinuity snapped this frame */
	XTK_MODEL_DISCONNECTED = 5	/* transport lost; last pose frozen */
} XtkModelStatus;

typedef struct {
	uint32_t  epoch;
	int       have_epoch;

	XtkTimeline timeline;
	int       disconnected;

	XtkViewSnapshot ring[XTK_MODEL_RING];
	int       count;		/* valid entries, ascending server_tick */
	uint32_t  last_sequence;
	int       have_sequence;

	/* Last controlled-player displayed pose (death / underrun camera). */
	int       have_own_pose;
	uint32_t  own_pose_generation;
	double    own_x, own_y, own_body_angle, own_turret_angle;

	/* Last successfully produced frame, for underrun/disconnect freeze. */
	int       have_last_out;
	XtkViewSnapshot last_out;

	XtkCamera camera;
	XtkModelStatus last_status;

	double    teleport_distance;
} XtkClientModel;

/* Clear all state and install the frozen viewport/scale camera. */
void model_init(XtkClientModel *m);

/* Clear history and pose, as on epoch reset or reconnect. */
void model_reset(XtkClientModel *m);

/* Mark the transport connected/disconnected; disconnect freezes the pose. */
void model_set_disconnected(XtkClientModel *m, int disconnected);

/*
 * Feed one decoded, already-authorized snapshot received at local `recv_ns`.
 * Duplicates, reordered ticks and stale epochs are ignored; a newer epoch
 * clears history and snaps.  Returns what happened.
 */
XtkObserveResult model_observe(XtkClientModel *m, uint32_t epoch,
			       const XtkViewSnapshot *snap, uint64_t recv_ns);

/*
 * Produce the authorized view to draw at local time `now_ns`: the snapshot
 * pair bracketing `estimated_server_tick - buffer`, with lifetime-matched
 * positions and shortest-arc headings interpolated and discrete fields held.
 * On underrun/disconnect the last authorized pose is frozen.  `out` may be the
 * same size as any XtkViewSnapshot (hundreds of KiB).  Returns the status and
 * guarantees `out` is fully initialised.
 */
XtkModelStatus model_render(XtkClientModel *m, uint64_t now_ns,
			    XtkViewSnapshot *out);

/* Read-only accessors. */
const XtkCamera *model_camera(const XtkClientModel *m);
XtkModelStatus model_status(const XtkClientModel *m);
uint32_t model_epoch(const XtkClientModel *m);
int model_snapshot_count(const XtkClientModel *m);
int model_have_own_pose(const XtkClientModel *m, double *x, double *y,
			double *body_angle, uint32_t *generation);

#endif /* XTK_CLIENT_MODEL_H */