/** \file uniformGridMath.h

    \brief Mathematical routines for UniformGrids of vectors or matrices

    \author Copyright 2005-2016 Dr. Michael Jason Gourlay; All rights reserved.  Contact me at mijagourlay.com for licensing.

    \see http://www.mijagourlay.com/

*/
#ifndef UNIFORM_GRID_MATH_H
#define UNIFORM_GRID_MATH_H

#include "Core/Math/mat33.h"
#include "Core/SpatialPartition/uniformGrid.h"


// Macros --------------------------------------------------------------
// Types --------------------------------------------------------------

/** Which kind of boundary condition to enforce.
*/
enum BoundaryConditionE
{
    BC_NEUMANN      ,   /// Enforce Neumann boundary condition
    BC_DIRICHLET    ,   /// Enforce Dirichley boundary condition
    BC_NUM
} ;


// Public variables --------------------------------------------------------------
// Public functions --------------------------------------------------------------

extern void FindValueRange( const UniformGrid< float > & vec , float & valMin , float & valMax ) ;
extern void FindValueStats( const UniformGrid< float > & vec , float & valMin , float & valMax , float & valMean , float & valStdDev ) ;
extern void FindMagnitudeRange( const UniformGrid< Vec3 > & vec , float & magMin , float & magMax ) ;
extern void ComputeGradient( UniformGrid< Vec3 > & gradient , const UniformGrid< float > & val ) ;
extern void ComputeGradientConditionally( UniformGrid< Vec3 > & gradient , const UniformGrid< float > & val ) ;
extern void ComputeJacobian( UniformGrid< Mat33 > & jacobian , const UniformGrid< Vec3 > & vec ) ;
extern void ComputeCurlFromJacobian( UniformGrid< Vec3 > & curl , const UniformGrid< Mat33 > & jacobian ) ;
extern void ComputeLaplacian( UniformGrid< Vec3 > & laplacian , const UniformGrid< Vec3 > & vec ) ;
extern void SolveVectorPoisson( UniformGrid< Vec3 > & soln , const UniformGrid< Vec3 > & lap , size_t numSteps , BoundaryConditionE boundaryCondition , Stats_Float & residualStats ) ;

#endif
