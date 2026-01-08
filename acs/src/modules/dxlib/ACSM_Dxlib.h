#pragma once

#define ACSM_DXLIB

#include <DxLib.h>

static const VECTOR V3_Identity    = VECTOR(0.0f, 0.0f, 0.0f);
static const VECTOR V3_UnitX       = VECTOR(1.0f, 0.0f, 0.0f);
static const VECTOR V3_UnitY       = VECTOR(0.0f, 1.0f, 0.0f);
static const VECTOR V3_UnitZ       = VECTOR(0.0f, 0.0f, 1.0f);
static const VECTOR V3_Ones        = VECTOR(1.0f, 1.0f, 1.0f);

static const VECTOR_D V3_Identity_D = VECTOR_D(0.0, 0.0, 0.0);
static const VECTOR_D V3_UnitX_D = VECTOR_D(1.0, 0.0, 0.0);
static const VECTOR_D V3_UnitY_D = VECTOR_D(0.0, 1.0, 0.0);
static const VECTOR_D V3_UnitZ_D = VECTOR_D(0.0, 0.0, 1.0);
static const VECTOR_D V3_Ones_D = VECTOR_D(1.0, 1.0, 1.0);

static const MATRIX M_Identity = {{
        { 1.0f, 0.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f, 0.0f },
        { 0.0f, 0.0f, 0.0f, 1.0f }}};

static const MATRIX_D M_Identity_D = {{
        { 1.0, 0.0, 0.0, 0.0 },
        { 0.0, 1.0, 0.0, 0.0 },
        { 0.0, 0.0, 1.0, 0.0 },
        { 0.0, 0.0, 0.0, 1.0 }}};

VECTOR VGet(VECTOR_D v);
VECTOR_D VGetD(VECTOR v);

const VECTOR operator +(const VECTOR& v1, const VECTOR& v2);
const VECTOR_D operator +(const VECTOR_D& v1, const VECTOR_D& v2);

VECTOR& operator +=(VECTOR& v1, const VECTOR& v2);
VECTOR_D& operator +=(VECTOR_D& v1, const VECTOR_D& v2);

const VECTOR operator -(const VECTOR& v1, const VECTOR& v2);
const VECTOR_D operator -(const VECTOR_D& v1, const VECTOR_D& v2);

VECTOR& operator -=(VECTOR& v1, const VECTOR& v2);
VECTOR_D& operator -=(VECTOR_D& v1, const VECTOR_D& v2);

const VECTOR operator *(const VECTOR& v1, const float& scale);
const VECTOR_D operator *(const VECTOR_D& v1, const double& scale);

const VECTOR operator *(const float& scale, const VECTOR& v1);
const VECTOR_D operator *(const double& scale, const VECTOR_D& v1);

const VECTOR operator *(const VECTOR& v1, const VECTOR& v2);
const VECTOR_D operator *(const VECTOR_D& v1, const VECTOR_D& v2);

const VECTOR operator /(const VECTOR& v1, const float& scale);
const VECTOR_D operator /(const VECTOR_D& v1, const double& scale);

VECTOR& operator *=(VECTOR& v1, const float& scale);
VECTOR_D& operator *=(VECTOR_D& v1, const double& scale);

VECTOR& operator *=(const float& scale, VECTOR& v1);
VECTOR_D& operator *=(const double& scale, VECTOR_D& v1);

VECTOR& operator /=(VECTOR& v1, const float& scale);
VECTOR_D& operator /=(VECTOR_D& v1, const double& scale);

const MATRIX operator +(const MATRIX& m1, const MATRIX& m2);
const MATRIX_D operator +(const MATRIX_D& m1, const MATRIX_D& m2);

MATRIX& operator +=(MATRIX& m1, const MATRIX& m2);
MATRIX_D& operator +=(MATRIX_D& m1, const MATRIX_D& m2);

const MATRIX operator *(const MATRIX& m1, const MATRIX& m2);
const MATRIX_D operator *(const MATRIX_D& m1, const MATRIX_D& m2);

MATRIX& operator *=(MATRIX& m1, const MATRIX& m2);
MATRIX_D& operator *=(MATRIX_D& m1, const MATRIX_D& m2);

const VECTOR operator *(const VECTOR& v, const MATRIX& m1);
const VECTOR_D operator *(const VECTOR_D& v, const MATRIX_D& m1);

VECTOR& operator *=(VECTOR& v, const MATRIX& m1);
VECTOR_D& operator *=(VECTOR_D& v, const MATRIX_D& m1);

namespace ACSM_Dxlib
{
    
}