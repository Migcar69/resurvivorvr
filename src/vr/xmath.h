// Minimal 4x4 matrix helpers (column vectors, row-major storage: m[row][col]).
#pragma once
#include <cmath>

struct Mat4 {
    float m[4][4];
};

inline Mat4 mat_identity() {
    Mat4 r = {};
    for (int i = 0; i < 4; i++) r.m[i][i] = 1;
    return r;
}

inline Mat4 mat_mul(const Mat4 &a, const Mat4 &b) {
    Mat4 r = {};
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            for (int k = 0; k < 4; k++) r.m[i][j] += a.m[i][k] * b.m[k][j];
    return r;
}

// Off-axis perspective (right-handed view space looking down -Z, D3D depth 0..1)
inline Mat4 mat_frustum(float tan_l, float tan_r, float tan_d, float tan_u, float n, float f) {
    Mat4 p = {};
    p.m[0][0] = 2 / (tan_r - tan_l);
    p.m[0][2] = (tan_r + tan_l) / (tan_r - tan_l);
    p.m[1][1] = 2 / (tan_u - tan_d);
    p.m[1][2] = (tan_u + tan_d) / (tan_u - tan_d);
    p.m[2][2] = f / (n - f);
    p.m[2][3] = n * f / (n - f);
    p.m[3][2] = -1;
    return p;
}

// Rigid transform from a quaternion + translation
inline Mat4 mat_pose(float qx, float qy, float qz, float qw, float tx, float ty, float tz) {
    Mat4 r = mat_identity();
    r.m[0][0] = 1 - 2 * (qy * qy + qz * qz);
    r.m[0][1] = 2 * (qx * qy - qz * qw);
    r.m[0][2] = 2 * (qx * qz + qy * qw);
    r.m[1][0] = 2 * (qx * qy + qz * qw);
    r.m[1][1] = 1 - 2 * (qx * qx + qz * qz);
    r.m[1][2] = 2 * (qy * qz - qx * qw);
    r.m[2][0] = 2 * (qx * qz - qy * qw);
    r.m[2][1] = 2 * (qy * qz + qx * qw);
    r.m[2][2] = 1 - 2 * (qx * qx + qy * qy);
    r.m[0][3] = tx;
    r.m[1][3] = ty;
    r.m[2][3] = tz;
    return r;
}

// Inverse of a rigid transform
inline Mat4 mat_rigid_inverse(const Mat4 &a) {
    Mat4 r = mat_identity();
    for (int i = 0; i < 3; i++)
        for (int j = 0; j < 3; j++) r.m[i][j] = a.m[j][i];
    for (int i = 0; i < 3; i++) r.m[i][3] = -(r.m[i][0] * a.m[0][3] + r.m[i][1] * a.m[1][3] + r.m[i][2] * a.m[2][3]);
    return r;
}

inline Mat4 mat_translate(float x, float y, float z) {
    Mat4 r = mat_identity();
    r.m[0][3] = x;
    r.m[1][3] = y;
    r.m[2][3] = z;
    return r;
}

inline Mat4 mat_rot_y(float a) {
    Mat4 r = mat_identity();
    r.m[0][0] = cosf(a);
    r.m[0][2] = sinf(a);
    r.m[2][0] = -sinf(a);
    r.m[2][2] = cosf(a);
    return r;
}

inline Mat4 mat_rot_x(float a) {
    Mat4 r = mat_identity();
    r.m[1][1] = cosf(a);
    r.m[1][2] = -sinf(a);
    r.m[2][1] = sinf(a);
    r.m[2][2] = cosf(a);
    return r;
}

inline Mat4 mat_rot_z(float a) {
    Mat4 r = mat_identity();
    r.m[0][0] = cosf(a);
    r.m[0][1] = -sinf(a);
    r.m[1][0] = sinf(a);
    r.m[1][1] = cosf(a);
    return r;
}

inline Mat4 mat_scale(float sx, float sy, float sz) {
    Mat4 r = mat_identity();
    r.m[0][0] = sx;
    r.m[1][1] = sy;
    r.m[2][2] = sz;
    return r;
}
