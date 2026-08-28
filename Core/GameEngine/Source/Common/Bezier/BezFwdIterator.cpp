/*
**	Command & Conquer Generals Zero Hour(tm)
**	Copyright 2025 Electronic Arts Inc.
**
**	This program is free software: you can redistribute it and/or modify
**	it under the terms of the GNU General Public License as published by
**	the Free Software Foundation, either version 3 of the License, or
**	(at your option) any later version.
**
**	This program is distributed in the hope that it will be useful,
**	but WITHOUT ANY WARRANTY; without even the implied warranty of
**	MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
**	GNU General Public License for more details.
**
**	You should have received a copy of the GNU General Public License
**	along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

////////////////////////////////////////////////////////////////////////////////
//																																						//
//  (c) 2001-2003 Electronic Arts Inc.																				//
//																																						//
////////////////////////////////////////////////////////////////////////////////

#include "PreRTS.h"
#include "Common/BezFwdIterator.h"

//-------------------------------------------------------------------------------------------------
BezFwdIterator::BezFwdIterator(): mStep(0), mStepsDesired(0)
{
	mCurrPoint.zero();
	mDDDq.zero();
	mDDq.zero();
	mDq.zero();
}

//-------------------------------------------------------------------------------------------------
BezFwdIterator::BezFwdIterator(Int stepsDesired, const BezierSegment *bezSeg)
{
	mCurrPoint.zero();
	mDDDq.zero();
	mDDq.zero();
	mDq.zero();
	mStepsDesired = stepsDesired;
	mBezSeg = (*bezSeg);
}

//-------------------------------------------------------------------------------------------------
void BezFwdIterator::start()
{
	mStep = 0;

	if (mStepsDesired <= 1)
		return;

	float d	 = 1.0f / (mStepsDesired - 1);
	float d2 = d * d;
	float d3 = d * d2;

	const float px[4] = { mBezSeg.m_controlPoints[0].x, mBezSeg.m_controlPoints[1].x, mBezSeg.m_controlPoints[2].x, mBezSeg.m_controlPoints[3].x };
	const float py[4] = { mBezSeg.m_controlPoints[0].y, mBezSeg.m_controlPoints[1].y, mBezSeg.m_controlPoints[2].y, mBezSeg.m_controlPoints[3].y };
	const float pz[4] = { mBezSeg.m_controlPoints[0].z, mBezSeg.m_controlPoints[1].z, mBezSeg.m_controlPoints[2].z, mBezSeg.m_controlPoints[3].z };

	float cVec[3][4];
	BezierSegment::transformBasis(px, cVec[0]);
	BezierSegment::transformBasis(py, cVec[1]);
	BezierSegment::transformBasis(pz, cVec[2]);

	mCurrPoint = mBezSeg.m_controlPoints[0];

	int i = 3;
	while (i--) {
		float a = cVec[i][0];
		float b = cVec[i][1];
		float c = cVec[i][2];

		float *pD, *pDD, *pDDD;

		if (i == 2) {
			pD = &mDq.z;
			pDD = &mDDq.z;
			pDDD = &mDDDq.z;
		} else if (i == 1) {
			pD = &mDq.y;
			pDD = &mDDq.y;
			pDDD = &mDDDq.y;
		} else if (i == 0) {
			pD = &mDq.x;
			pDD = &mDDq.x;
			pDDD = &mDDDq.x;
		}

#if defined(_MSC_VER) && _MSC_VER < 1300 && defined(_M_IX86)
		// The D3DX-era VC6 build rounded b*d2 to float before accumulating
		// the forward differences. Preserve that spill and addition order for
		// legacy replay comparisons while the modern builds retain their path.
		const volatile float roundedBD2 = b * d2;
		float dq = c * d;
		dq += roundedBD2;
		dq += a * d3;
		(*pD) = dq;

		float ddq = roundedBD2;
		ddq += roundedBD2;
		ddq += (a * d3) * 6.0f;
		(*pDD) = ddq;
		(*pDDD) = (a * d3) * 6.0f;
#else
		(*pD) = a * d3 + b * d2 + c * d;
		(*pDD) = 6 * a * d3 + 2 * b * d2;
		(*pDDD) = 6 * a * d3;
#endif
	}
}

//-------------------------------------------------------------------------------------------------
Bool BezFwdIterator::done()
{
	return (mStep >= mStepsDesired);
}

//-------------------------------------------------------------------------------------------------
const Coord3D& BezFwdIterator::getCurrent() const
{
	return mCurrPoint;
}

//-------------------------------------------------------------------------------------------------
void BezFwdIterator::next()
{
	mCurrPoint.add(mDq);
	mDq.add(mDDq);
	mDDq.add(mDDDq);

	++mStep;
}

