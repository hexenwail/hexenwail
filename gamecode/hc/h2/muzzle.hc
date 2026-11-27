/*
===============
muzzle.hc

sv_muzzlespawn: player projectiles leave from the weapon's muzzle instead of
the eye.  GitHub #567.

Vanilla spawns every player missile at origin + proj_ofs (eye height) plus a
few units of v_forward, so it starts at the centre of the view while the view
model is drawn below it, and the shot appears to come out of the body.

MuzzleFix is called once per player missile, AFTER the vanilla code has set
the missile's origin, velocity and angles.  With the cvar at 0 (the default)
it returns before touching anything: no trace, no makevectors, no write, so
the missile is exactly what the vanilla code built and the trace_* and v_*
globals are untouched.  A mod with its own progs never calls it at all.

With the cvar on:
  - The muzzle point is the shooter's camera (origin + view_ofs, which is
    where the engine draws the view model) plus `muzzle' in view space:
    _x forward, _y right, _z up, from shooter.v_angle.  The per-weapon values
    are the retail view model's tip in its fire frame (the .mdl's +y is left,
    so right = -y).
  - What the vanilla shot is aimed at is found first: a traceline from the
    eye (origin + proj_ofs) along the vanilla velocity.  If that target is
    not well beyond the muzzle (MUZZLE_NEAR times the eye-to-muzzle
    distance), the missile is left exactly as vanilla built it: a point-blank
    target in front of the muzzle must not be skipped.
  - The spawn point is the end of a trace, with the missile's own box, from
    the eye to the muzzle, pulled MUZZLE_PULLBACK back towards the eye if it
    hit something.  The eye is inside the player's box, so a shot fired at a
    wall in front of you cannot start on its far side.
  - Aim converges: the velocity is re-aimed from the new spawn point at the
    vanilla target, same speed.  Spread shots keep their spread, since each
    is traced along its own direction.  angles turns by the same amount;
    movedir and o_angle follow when they held the velocity or its direction.
    If that would turn the shot more than MUZZLE_MAXTURN, the missile is again
    left exactly as vanilla built it.

Non-clients (monsters, tornados and other missiles calling the same fire
functions) are left alone.
===============
*/

float MUZZLE_PULLBACK = 4;		// units off a surface the muzzle trace hit
float MUZZLE_AIMRANGE = 8192;	// how far the aim trace looks for a target
float MUZZLE_MAXTURN = 0.966;	// cos 15: re-aim no further than this
float MUZZLE_NEAR = 2;			// target nearer than this many muzzle lengths: vanilla

void MuzzleFix (entity missile, entity shooter, vector muzzle)
{
	local vector fwd, rgt, upv, eye, spot, goal, mzhit, oldvel, olddir, newdir, newvel, turn;
	local float len;

	if (!cvar("sv_muzzlespawn"))
		return;
	if (!(shooter.flags & FL_CLIENT))
		return;

	// Callers are mid-way through their own makevectors (axe and crossbow
	// spread shots use a modified angle); the muzzle is in the view's frame.
	fwd = v_forward;
	rgt = v_right;
	upv = v_up;
	makevectors (shooter.v_angle);
	spot = shooter.origin + shooter.view_ofs
		+ v_forward*muzzle_x + v_right*muzzle_y + v_up*muzzle_z;
	v_forward = fwd;
	v_right = rgt;
	v_up = upv;

	eye = shooter.origin + shooter.proj_ofs;
	oldvel = missile.velocity;
	if (oldvel == '0 0 0')
		return;
	olddir = normalize (oldvel);
	traceline (eye, eye + olddir*MUZZLE_AIMRANGE, FALSE, shooter);
	goal = trace_endpos;
	// One builtin call per expression: hcc leaves each result in the shared
	// return slot, so a second call in the same expression overwrites the first.
	len = vlen (spot - eye);
	if (vlen (goal - eye) <= MUZZLE_NEAR * len)
		return;

	// A missile with a hull field (the flame stream, HULL_POINT) hits the
	// world as the point origin + mins: the point hull's clip_mins is 0, so
	// SV_HullForEntity offsets the trace by -mins.  Trace that point; no
	// builtin can trace with the missile's own hull field.  Any other missile
	// moves as its box, which is what tracearea traces.
	if (missile.hull)
		traceline (eye + missile.mins, spot + missile.mins, FALSE, shooter);
	else
		tracearea (eye, spot, missile.mins, missile.maxs, FALSE, shooter);
	if (trace_allsolid || trace_startsolid)
		return;
	if (trace_fraction < 1)
	{
		mzhit = trace_endpos;
		if (missile.hull)
			mzhit = mzhit - missile.mins;
		len = vlen (mzhit - eye);
		if (len <= MUZZLE_PULLBACK)
			return;
		spot = eye + (mzhit - eye) * ((len - MUZZLE_PULLBACK) / len);
	}
	if (vlen (goal - spot) < 1)
		return;
	newdir = normalize (goal - spot);
	if (newdir * olddir < MUZZLE_MAXTURN)
		return;

	setorigin (missile, spot);
	newvel = newdir * vlen (oldvel);
	missile.velocity = newvel;
	// vectoangles returns [0, 360): a turn across 0 would come out near 360
	turn = vectoangles (oldvel);
	turn = vectoangles (newvel) - turn;
	if (turn_x > 180)
		turn_x -= 360;
	else if (turn_x < -180)
		turn_x += 360;
	if (turn_y > 180)
		turn_y -= 360;
	else if (turn_y < -180)
		turn_y += 360;
	missile.angles += turn;
	if (missile.movedir == oldvel)
		missile.movedir = newvel;
	else if (vlen (missile.movedir - olddir) < 0.001)
		missile.movedir = newdir;
	if (missile.o_angle == oldvel)
		missile.o_angle = newvel;
	else if (vlen (missile.o_angle - olddir) < 0.001)
		missile.o_angle = newdir;
}
