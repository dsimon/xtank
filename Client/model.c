/*
 * Client/model.c -- bounded authorized client model (P2.11).
 *
 * See Client/model.h.  This translation unit is pure C and includes no SDL, no
 * engine, no transport and no X11: it reads only the authorized snapshot DTOs
 * and local receive times, and it never schedules a tick or simulates gameplay.
 */

#include "model.h"

#include <math.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ---- small helpers -------------------------------------------------- */

static double lerp(double a, double b, double t)
{
	return a + (b - a) * t;
}

/* Shortest-arc interpolation of two angles (radians). */
static double angle_lerp(double a, double b, double t)
{
	double d = fmod(b - a, 2.0 * M_PI);
	if (d > M_PI) {
		d -= 2.0 * M_PI;
	} else if (d < -M_PI) {
		d += 2.0 * M_PI;
	}
	return a + d * t;
}

static double dist2(double ax, double ay, double bx, double by)
{
	double dx = bx - ax;
	double dy = by - ay;
	return dx * dx + dy * dy;
}

/*
 * Full-width lifetime key: a vehicle is (slot, generation) with the u32
 * entity-life generation, so a reuse of a slot after death/respawn (or a
 * generation past 255) can never alias the previous life.
 */
static const XtkVehicleView *find_vehicle(const XtkViewSnapshot *s,
					  uint8_t slot, uint32_t generation)
{
	int i;
	for (i = 0; i < (int)s->vehicle_count; i++) {
		if (s->vehicle[i].slot == slot &&
		    s->vehicle[i].generation == generation) {
			return &s->vehicle[i];
		}
	}
	return NULL;
}

/*
 * Full-width bullet key: (id, owner_slot, owner_generation, generation).  The
 * bullet's own u32 life generation is required in addition to the owner
 * reference, so the same owner reusing a bullet slot (or a slot reaching a
 * generation past 255) is never blended with the previous projectile.
 */
static const XtkBulletView *find_bullet(const XtkViewSnapshot *s,
					uint16_t id, uint8_t owner_slot,
					uint32_t owner_generation,
					uint32_t generation)
{
	int i;
	for (i = 0; i < (int)s->bullet_count; i++) {
		if (s->bullet[i].id == id &&
		    s->bullet[i].owner_slot == owner_slot &&
		    s->bullet[i].owner_generation == owner_generation &&
		    s->bullet[i].generation == generation) {
			return &s->bullet[i];
		}
	}
	return NULL;
}

/*
 * Full-width explosion key: (id, generation).  The u32 transient life
 * generation distinguishes a same-slot, same-kind reuse, so an explosion that
 * reappears in a reused slot is snapped rather than interpolated from the
 * previous transient.
 */
static const XtkExplosionView *find_explosion(const XtkViewSnapshot *s,
					      uint16_t id, uint32_t generation)
{
	int i;
	for (i = 0; i < (int)s->explosion_count; i++) {
		if (s->explosion[i].id == id &&
		    s->explosion[i].generation == generation) {
			return &s->explosion[i];
		}
	}
	return NULL;
}

/* ---- interpolation (lifetime-matched dynamic entities only) --------- */

static void build_vehicles(XtkViewSnapshot *out, const XtkViewSnapshot *lo,
			   const XtkViewSnapshot *hi, double alpha,
			   double teleport_distance, int *saw_snap)
{
	int n = 0, i;

	for (i = 0; i < (int)lo->vehicle_count; i++) {
		const XtkVehicleView *b = &lo->vehicle[i];
		const XtkVehicleView *h = find_vehicle(hi, b->slot, b->generation);

		if (h == NULL) {
			continue;	/* newly hidden/dead: never leave a ghost */
		}
		out->vehicle[n] = *b;
		if (dist2(b->x, b->y, h->x, h->y) >
		    teleport_distance * teleport_distance) {
			*saw_snap = 1;	/* teleport: hold b, snap next */
		} else if (alpha > 0.0) {
			out->vehicle[n].x = lerp(b->x, h->x, alpha);
			out->vehicle[n].y = lerp(b->y, h->y, alpha);
			out->vehicle[n].body_angle =
				angle_lerp(b->body_angle,
					   h->body_angle, alpha);
			out->vehicle[n].turret_angle =
				angle_lerp(b->turret_angle,
					   h->turret_angle, alpha);
		}
		n++;
	}
	/* Appeared only in the newer view: snapped at its own pose. */
	for (i = 0; i < (int)hi->vehicle_count; i++) {
		const XtkVehicleView *h = &hi->vehicle[i];
		if (find_vehicle(lo, h->slot, h->generation) != NULL) {
			continue;
		}
		if (n < (int)XTK_MAX_VEHICLES) {
			out->vehicle[n++] = *h;
		}
	}
	out->vehicle_count = (uint16_t)n;
}

static void build_bullets(XtkViewSnapshot *out, const XtkViewSnapshot *lo,
			  const XtkViewSnapshot *hi, double alpha)
{
	int n = 0, i;

	for (i = 0; i < (int)lo->bullet_count; i++) {
		const XtkBulletView *b = &lo->bullet[i];
		const XtkBulletView *h = find_bullet(hi, b->id, b->owner_slot,
						     b->owner_generation,
						     b->generation);
		if (h == NULL) {
			continue;
		}
		out->bullet[n] = *b;
		if (alpha > 0.0) {
			out->bullet[n].x = lerp(b->x, h->x, alpha);
			out->bullet[n].y = lerp(b->y, h->y, alpha);
			out->bullet[n].heading =
				angle_lerp(b->heading, h->heading, alpha);
		}
		n++;
	}
	for (i = 0; i < (int)hi->bullet_count; i++) {
		const XtkBulletView *h = &hi->bullet[i];
		if (find_bullet(lo, h->id, h->owner_slot,
				h->owner_generation, h->generation) != NULL) {
			continue;
		}
		if (n < (int)XTK_MAX_BULLETS) {
			out->bullet[n++] = *h;
		}
	}
	out->bullet_count = (uint16_t)n;
}

static void build_explosions(XtkViewSnapshot *out, const XtkViewSnapshot *lo,
			     const XtkViewSnapshot *hi, double alpha)
{
	int n = 0, i;

	for (i = 0; i < (int)lo->explosion_count; i++) {
		const XtkExplosionView *b = &lo->explosion[i];
		const XtkExplosionView *h = find_explosion(hi, b->id,
							   b->generation);
		if (h == NULL) {
			continue;
		}
		out->explosion[n] = *b;
		if (alpha > 0.0) {
			out->explosion[n].x = lerp(b->x, h->x, alpha);
			out->explosion[n].y = lerp(b->y, h->y, alpha);
		}
		n++;
	}
	for (i = 0; i < (int)hi->explosion_count; i++) {
		const XtkExplosionView *h = &hi->explosion[i];
		if (find_explosion(lo, h->id, h->generation) != NULL) {
			continue;
		}
		if (n < (int)XTK_MAX_EXPLOSIONS) {
			out->explosion[n++] = *h;
		}
	}
	out->explosion_count = (uint16_t)n;
}

/* ---- ring / epoch management ---------------------------------------- */

static void clear_history(XtkClientModel *m)
{
	m->count = 0;
	m->have_sequence = 0;
	m->last_sequence = 0;
	m->have_own_pose = 0;
	m->own_pose_generation = 0;
	m->own_x = m->own_y = 0.0;
	m->own_body_angle = m->own_turret_angle = 0.0;
	m->have_last_out = 0;
	timeline_reset(&m->timeline);
}

static void ring_push(XtkClientModel *m, const XtkViewSnapshot *s)
{
	if (m->count < (int)XTK_MODEL_RING) {
		m->ring[m->count] = *s;
		m->count++;
	} else {
		memmove(&m->ring[0], &m->ring[1],
			sizeof(m->ring[0]) * (XTK_MODEL_RING - 1));
		m->ring[XTK_MODEL_RING - 1] = *s;
	}
}

/* ---- public API ----------------------------------------------------- */

void model_init(XtkClientModel *m)
{
	if (m == NULL) {
		return;
	}
	memset(m, 0, sizeof(*m));
	timeline_init(&m->timeline, XTK_SERVER_TICK_HZ);
	camera_init(&m->camera);
	m->teleport_distance = XTK_MODEL_TELEPORT_DISTANCE;
	m->last_status = XTK_MODEL_NO_DATA;
}

void model_reset(XtkClientModel *m)
{
	uint32_t epoch;
	int have_epoch;

	if (m == NULL) {
		return;
	}
	epoch = m->epoch;
	have_epoch = m->have_epoch;
	model_init(m);
	m->epoch = epoch;
	m->have_epoch = have_epoch;
	m->last_status = XTK_MODEL_NO_DATA;
}

void model_set_disconnected(XtkClientModel *m, int disconnected)
{
	if (m == NULL) {
		return;
	}
	m->disconnected = disconnected ? 1 : 0;
	if (m->disconnected) {
		m->last_status = XTK_MODEL_DISCONNECTED;
	}
}

XtkObserveResult model_observe(XtkClientModel *m, uint32_t epoch,
			       const XtkViewSnapshot *snap, uint64_t recv_ns)
{
	if (m == NULL || snap == NULL) {
		return XTK_OBSERVE_DUPLICATE;
	}

	if (!m->have_epoch) {
		m->epoch = epoch;
		m->have_epoch = 1;
		clear_history(m);
	} else if (epoch < m->epoch) {
		return XTK_OBSERVE_STALE_EPOCH;
	} else if (epoch > m->epoch) {
		m->epoch = epoch;
		clear_history(m);
		m->disconnected = 0;
		ring_push(m, snap);
		m->last_sequence = snap->snapshot_sequence;
		m->have_sequence = 1;
		timeline_observe(&m->timeline, (int)snap->server_tick, recv_ns);
		return XTK_OBSERVE_EPOCH_RESET;
	}

	/* Same epoch: reject duplicates and reordered/stale ticks. */
	if (m->have_sequence &&
	    snap->snapshot_sequence <= m->last_sequence) {
		return XTK_OBSERVE_DUPLICATE;
	}
	if (m->count > 0 &&
	    snap->server_tick <= m->ring[m->count - 1].server_tick) {
		return XTK_OBSERVE_DUPLICATE;
	}

	ring_push(m, snap);
	m->last_sequence = snap->snapshot_sequence;
	m->have_sequence = 1;
	timeline_observe(&m->timeline, (int)snap->server_tick, recv_ns);
	return XTK_OBSERVE_ACCEPTED;
}

XtkModelStatus model_render(XtkClientModel *m, uint64_t now_ns,
			    XtkViewSnapshot *out)
{
	double est, rt;
	int lo_i = -1, hi_i = -1, i;
	const XtkViewSnapshot *lo, *hi = NULL;
	XtkModelStatus status;
	int saw_snap = 0;

	if (m == NULL || out == NULL) {
		return XTK_MODEL_NO_DATA;
	}

	/* Disconnect: freeze the last authorized frame. */
	if (m->disconnected) {
		if (m->have_last_out) {
			*out = m->last_out;
		} else if (m->count > 0) {
			*out = m->ring[m->count - 1];
		} else {
			memset(out, 0, sizeof(*out));
		}
		m->last_status = XTK_MODEL_DISCONNECTED;
		return m->last_status;
	}

	if (m->count == 0) {
		if (m->have_last_out) {
			*out = m->last_out;
		} else {
			memset(out, 0, sizeof(*out));
			camera_init(&m->camera);
		}
		m->last_status = XTK_MODEL_NO_DATA;
		return m->last_status;
	}

	est = timeline_estimate(&m->timeline, now_ns);
	rt = est - XTK_MODEL_BUFFER_TICKS;

	if (rt < (double)m->ring[0].server_tick) {
		lo_i = 0;
		status = XTK_MODEL_BUFFERING;
	} else if (rt >= (double)m->ring[m->count - 1].server_tick) {
		lo_i = m->count - 1;
		status = XTK_MODEL_UNDERRUN;
	} else {
		for (i = 0; i < m->count; i++) {
			if ((double)m->ring[i].server_tick <= rt) {
				lo_i = i;
			} else {
				hi_i = i;
				break;
			}
		}
		status = XTK_MODEL_OK;
	}

	lo = &m->ring[lo_i];
	*out = *lo;

	if (hi_i >= 0) {
		double denom;
		double alpha;
		hi = &m->ring[hi_i];
		denom = (double)hi->server_tick - (double)lo->server_tick;
		alpha = (denom > 0.0)
			? (rt - (double)lo->server_tick) / denom
			: 0.0;
		if (alpha < 0.0) {
			alpha = 0.0;
		}
		if (alpha > 1.0) {
			alpha = 1.0;
		}
		build_vehicles(out, lo, hi, alpha, m->teleport_distance,
			       &saw_snap);
		build_bullets(out, lo, hi, alpha);
		build_explosions(out, lo, hi, alpha);
	}

	/* Camera follows the controlled player's displayed pose. */
	{
		const XtkVehicleView *own =
			find_vehicle(out, out->own_slot, out->own_generation);
		if (own != NULL) {
			m->have_own_pose = 1;
			m->own_pose_generation = own->generation;
			m->own_x = own->x;
			m->own_y = own->y;
			m->own_body_angle = own->body_angle;
			m->own_turret_angle = own->turret_angle;
		}
		/* else: dead/hidden -> keep the frozen last pose (§5.4). */
	}
	camera_center_on(&m->camera, m->own_x, m->own_y);

	if (saw_snap && status == XTK_MODEL_OK) {
		status = XTK_MODEL_SNAPPED;
	}

	m->last_out = *out;
	m->have_last_out = 1;
	m->last_status = status;
	return status;
}

const XtkCamera *model_camera(const XtkClientModel *m)
{
	return (m != NULL) ? &m->camera : NULL;
}

XtkModelStatus model_status(const XtkClientModel *m)
{
	return (m != NULL) ? m->last_status : XTK_MODEL_NO_DATA;
}

uint32_t model_epoch(const XtkClientModel *m)
{
	return (m != NULL) ? m->epoch : 0;
}

int model_snapshot_count(const XtkClientModel *m)
{
	return (m != NULL) ? m->count : 0;
}

int model_have_own_pose(const XtkClientModel *m, double *x, double *y,
			double *body_angle, uint32_t *generation)
{
	if (m == NULL || !m->have_own_pose) {
		return 0;
	}
	if (x != NULL) {
		*x = m->own_x;
	}
	if (y != NULL) {
		*y = m->own_y;
	}
	if (body_angle != NULL) {
		*body_angle = m->own_body_angle;
	}
	if (generation != NULL) {
		*generation = m->own_pose_generation;
	}
	return 1;
}