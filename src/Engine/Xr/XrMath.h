#pragma once
/*
 * Copyright 2010-2026 OpenXcom Developers.
 *
 * This file is part of OpenXcom.
 *
 * OpenXcom is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * OpenXcom is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with OpenXcom.  If not, see <http://www.gnu.org/licenses/>.
 */
// Panel geometry for the Quest port. The same pose drives the compositor
// layer and the inverse ray hit, so click point and image never drift apart
// (pattern from the XR-Settlers2.5 PanelGeometry.h, GPL-2.0-or-later).
#include <cmath>

namespace OpenXcom
{
namespace Xr
{

struct Vec3
{
	float x, y, z;
	Vec3 operator+(const Vec3 &o) const { return {x + o.x, y + o.y, z + o.z}; }
	Vec3 operator-(const Vec3 &o) const { return {x - o.x, y - o.y, z - o.z}; }
	Vec3 operator*(float s) const { return {x * s, y * s, z * s}; }
};

inline float dot(const Vec3 &a, const Vec3 &b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
inline Vec3 cross(const Vec3 &a, const Vec3 &b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
inline float length(const Vec3 &a) { return std::sqrt(dot(a, a)); }
inline Vec3 normalize(const Vec3 &a) { const float l = length(a); return l > 1e-6f ? a * (1.0f / l) : Vec3{0, 0, 0}; }

struct Quat
{
	float x, y, z, w;
};

inline Quat mul(const Quat &a, const Quat &b)
{
	return {a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
	        a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
	        a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
	        a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z};
}
inline Quat conjugate(const Quat &q) { return {-q.x, -q.y, -q.z, q.w}; }
inline Quat yawQuat(float yaw) { return {0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}; }
inline Quat pitchQuat(float pitch) { return {std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)}; }

inline Vec3 rotate(const Quat &q, const Vec3 &v)
{
	const Vec3 u{q.x, q.y, q.z};
	const Vec3 t = cross(u, v) * 2.0f;
	return v + t * q.w + cross(u, t);
}

/// Heading of an orientation around the vertical axis (0 = looking down -Z).
inline float yawOf(const Quat &q)
{
	const Vec3 f = rotate(q, {0, 0, -1});
	return std::atan2(-f.x, -f.z);
}

struct Pose
{
	Quat orientation{0, 0, 0, 1};
	Vec3 position{0, 0, 0};
	Vec3 toWorld(const Vec3 &local) const { return rotate(orientation, local) + position; }
	Vec3 toLocal(const Vec3 &world) const { return rotate(conjugate(orientation), world - position); }
	Vec3 directionToLocal(const Vec3 &dir) const { return rotate(conjugate(orientation), dir); }
};

/// Flat panel centred on pose.position, facing +Z of its pose, width x height metres.
struct PanelHit
{
	float u = 0, v = 0, distance = 0;
	Vec3 point{0, 0, 0};
};

inline bool intersectPanel(const Pose &panel, float width, float height, const Vec3 &origin, const Vec3 &direction, PanelHit &hit)
{
	const Vec3 o = panel.toLocal(origin);
	const Vec3 d = panel.directionToLocal(direction);
	if (d.z >= -1e-5f || o.z <= 0.0f)
		return false; // only the front side is interactive
	const float t = -o.z / d.z;
	const float x = o.x + d.x * t, y = o.y + d.y * t;
	const float u = 0.5f + x / width, v = 0.5f - y / height;
	if (u < 0.0f || u > 1.0f || v < 0.0f || v > 1.0f)
		return false;
	hit.u = u;
	hit.v = v;
	hit.distance = t;
	hit.point = origin + direction * t;
	return true;
}

/// Largest rectangle with the source aspect that fits the destination (canvas upscale).
inline void fitRect(int srcW, int srcH, int dstW, int dstH, int &w, int &h)
{
	const float scale = std::fmin(static_cast<float>(dstW) / srcW, static_cast<float>(dstH) / srcH);
	w = static_cast<int>(std::fmin(static_cast<float>(dstW), srcW * scale + 0.5f));
	h = static_cast<int>(std::fmin(static_cast<float>(dstH), srcH * scale + 0.5f));
}

/// Canvas coordinates u/v (0..1) -> pixel of the game's output rectangle on the virtual display.
inline void canvasToVirtual(float u, float v, int outX, int outY, int outW, int outH, int &x, int &y)
{
	const int px = static_cast<int>(u * outW), py = static_cast<int>(v * outH);
	x = (px < 0 ? 0 : px > outW - 1 ? outW - 1 : px) + outX;
	y = (py < 0 ? 0 : py > outH - 1 ? outH - 1 : py) + outY;
}

/// Column-major 4x4 matrix helpers for the laser pass.
struct Mat4
{
	float m[16];
};

inline Mat4 multiply(const Mat4 &a, const Mat4 &b)
{
	Mat4 r;
	for (int c = 0; c < 4; ++c)
		for (int row = 0; row < 4; ++row)
		{
			float s = 0;
			for (int k = 0; k < 4; ++k)
				s += a.m[k * 4 + row] * b.m[c * 4 + k];
			r.m[c * 4 + row] = s;
		}
	return r;
}

/// Asymmetric projection from OpenXR field-of-view tangents (GL clip space).
inline Mat4 projectionFromFov(float angleLeft, float angleRight, float angleUp, float angleDown, float nearZ, float farZ)
{
	const float l = std::tan(angleLeft), r = std::tan(angleRight), u = std::tan(angleUp), d = std::tan(angleDown);
	const float w = r - l, h = u - d;
	Mat4 p{};
	p.m[0] = 2.0f / w;
	p.m[5] = 2.0f / h;
	p.m[8] = (r + l) / w;
	p.m[9] = (u + d) / h;
	p.m[10] = -(farZ + nearZ) / (farZ - nearZ);
	p.m[11] = -1.0f;
	p.m[14] = -2.0f * farZ * nearZ / (farZ - nearZ);
	return p;
}

/// Inverse of a rigid pose as a view matrix.
inline Mat4 viewFromPose(const Pose &pose)
{
	const Quat inv = conjugate(pose.orientation);
	const Vec3 xAxis = rotate(inv, {1, 0, 0});
	const Vec3 yAxis = rotate(inv, {0, 1, 0});
	const Vec3 zAxis = rotate(inv, {0, 0, 1});
	const Vec3 t = rotate(inv, pose.position) * -1.0f;
	Mat4 v{};
	// Column j of the rotation part is R^T * e_j.
	v.m[0] = xAxis.x; v.m[1] = xAxis.y; v.m[2] = xAxis.z;
	v.m[4] = yAxis.x; v.m[5] = yAxis.y; v.m[6] = yAxis.z;
	v.m[8] = zAxis.x; v.m[9] = zAxis.y; v.m[10] = zAxis.z;
	v.m[12] = t.x; v.m[13] = t.y; v.m[14] = t.z;
	v.m[15] = 1.0f;
	return v;
}

}
}
