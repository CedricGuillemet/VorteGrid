//#pragma optimize( "" , off )
/** \file uniformGridMath.cpp

    \brief Mathematical routines for UniformGrids of vectors or matrices

    \author Copyright 2009-2016 Dr. Michael Jason Gourlay; All rights reserved.  Contact me at mijagourlay.com for licensing.
        - http://www.mijagourlay.com/
*/


#include "uniformGridMath.h"

#include "Core/useTbb.h"
#include "Core/Performance/perfBlock.h"
#include "Core/Utility/macros.h"

#include <algorithm>
#include <limits>




/** Techniques for implementing the Gauss-Seidel method.

    When USE_TBB is enabled (i.e. when using Intel Threading Building Blocks)
    for parallel execution, then SolveVectorPoisson always uses red-black,
    regardless of the value of POISSON_TECHNIQUE.  This is only used when
    USE_TBB is false, i.e. when built for serial execution.
    The POISSON_TECHNIQUE_GAUSS_SEIDEL_RED_BLACK technique mimics the operation
    of the parallelized routine, whereas POISSON_TECHNIQUE_GAUSS_SEIDEL
    implements the traditional straightforward Gauss-Seidel technique.

    \see GaussSeidelPortion
*/
#define POISSON_TECHNIQUE_GAUSS_SEIDEL              1   ///< Use serial Gauss-Seidel algorithm.
#define POISSON_TECHNIQUE_GAUSS_SEIDEL_RED_BLACK    2   ///< Use staggered Gauss-Seidel algorithm.

/// Which Poisson technique to use.
#define POISSON_TECHNIQUE                           POISSON_TECHNIQUE_GAUSS_SEIDEL_RED_BLACK




/** Which portion of the linear algebraic equation to solve.

    The Gauss-Seidel method for solving a system of linear equations
    operates "in-place" meaning that the updated solution to a particular
    element overwrites the previous value for that same element.
    This contrasts with the Jacobi method, which stores the results of
    a given iteration in a separate location from the values from
    the previous iteration.  The Gauss-Seidel method has 2 advantages
    over Jacobi: Faster convergence and lower storage requirements.
    Unfortunately, when distributed across multiple processors,
    the traditional Gauss-Seidel method is not thread-safe, since the inputs
    used by one thread are the outputs written by another thread.
    Synchronizing across threads by element would cost too much overhead,
    so instead, we partition the elements into "red" and "black",
    analogous to squares in a checkerboard.  The inputs for red squares
    are all black, and vice-versa.  During one pass, the algorithm
    operates on (i.e. writes to) a single color, then in a second pass,
    the algorithm operates on the other color.  All threads operate on
    a single color, therefore there is no contention for data; all threads
    are reading from one color and writing to the other.

    \note   Logic elsewhere depends on these symbols having the values they have.
            Specifically, a boolean expression depends on red & black being 0 or 1,
            and both not being either 0 or 1.
*/
enum GaussSeidelPortionE
{
    GS_RED      = 0         ,   ///< Operate only on "red" elements
    GS_MIN      = GS_RED    ,
    GS_BLACK    = 1         ,   ///< Operate only on "black" elements
    GS_BOTH     = 2         ,   ///< Operate on all matrix elements
    GS_NUM
} ;




#if USE_TBB
    static void StepTowardVectorPoissonSolution( UniformGrid< Vec3 > & soln , const UniformGrid< Vec3 > & lap , size_t izStart , size_t izEnd , GaussSeidelPortionE redOrBlack , BoundaryConditionE boundaryCondition , Stats_Float & residualStats ) ;
    static void ComputeGradientInteriorSlice( UniformGrid< Vec3 > & gradient , const UniformGrid< float > & val , size_t izStart , size_t izEnd ) ;
    unsigned gNumberOfProcessors = 8 ;  ///< Number of processors this machine has.  This will get reassigned later.

    /** Function object to solve vector Poisson equation using Threading Building Blocks.
    */
    class UniformGrid_StepTowardVectorPoissonSolution_TBB
    {
            UniformGrid< Vec3 > &       mSolution           ;   /// Reference to object containing solution
            const UniformGrid< Vec3 > & mLaplacian          ;   /// Reference to object containing Laplacian
            const GaussSeidelPortionE   mRedOrBlack         ;   /// Whether this pass operates on red or black portion of grid
            const BoundaryConditionE    mBoundaryCondition  ;   /// Which kind of boundary condition to impose
            Stats_Float &               mResidualStats      ;   /// Stats on residuals.  Not thread-safe when used with parallel_for.  Would need to use parallel_reduce to make thread-safe, parallel_invoke to make determinisitic.
        public:
            void operator() ( const tbb::blocked_range<size_t> & r ) const
            {   // Compute subset of velocity grid.
                SetFloatingPointControlWord( mMasterThreadFloatingPointControlWord ) ;
                SetMmxControlStatusRegister( mMasterThreadMmxControlStatusRegister ) ;
                ASSERT( mSolution.ShapeMatches( mLaplacian ) ) ;
                StepTowardVectorPoissonSolution( mSolution , mLaplacian , r.begin() , r.end() , mRedOrBlack , mBoundaryCondition , mResidualStats ) ;
            }
            UniformGrid_StepTowardVectorPoissonSolution_TBB( UniformGrid< Vec3 > & solution , const UniformGrid< Vec3 > & laplacian , GaussSeidelPortionE redOrBlack , BoundaryConditionE boundaryCondition , Stats_Float & residualStats )
                : mSolution( solution ) , mLaplacian( laplacian ) , mRedOrBlack( redOrBlack ) , mBoundaryCondition( boundaryCondition ) , mResidualStats( residualStats )
            {
                mMasterThreadFloatingPointControlWord = GetFloatingPointControlWord() ;
                mMasterThreadMmxControlStatusRegister = GetMmxControlStatusRegister() ;
            }
        private:
            WORD        mMasterThreadFloatingPointControlWord   ;
            unsigned    mMasterThreadMmxControlStatusRegister   ;
    } ;

    /** Function object to compute gradient of the interior of a grid, using Threading Building Blocks.
    */
    class UniformGrid_ComputeGradientInterior_TBB
    {
            UniformGrid< Vec3 > &           mGradient   ;   ///< Address of object containing gradient
            const UniformGrid< float > &    mValues     ;   ///< Address of object containing scalar values
        public:
            void operator() ( const tbb::blocked_range<size_t> & r ) const
            {   // Compute subset of gradient grid.
                SetFloatingPointControlWord( mMasterThreadFloatingPointControlWord ) ;
                SetMmxControlStatusRegister( mMasterThreadMmxControlStatusRegister ) ;
                ASSERT( mGradient.ShapeMatches( mValues ) ) ;
                ComputeGradientInteriorSlice( mGradient , mValues , r.begin() , r.end() ) ;
            }
            UniformGrid_ComputeGradientInterior_TBB( UniformGrid< Vec3 > & gradient , const UniformGrid< float > & values )
                : mGradient( gradient )
                , mValues( values )
            {
                mMasterThreadFloatingPointControlWord = GetFloatingPointControlWord() ;
                mMasterThreadMmxControlStatusRegister = GetMmxControlStatusRegister() ;
            }
        private:
            WORD        mMasterThreadFloatingPointControlWord   ;
            unsigned    mMasterThreadMmxControlStatusRegister   ;
    } ;
#endif



/// Utility macro to assign index offsets for accessing grid points with Z-1, Z and Z+1 relative to the current index.
#define ASSIGN_Z_OFFSETS                                \
    const size_t offsetZM  = numXY * ( index[2] - 1 ) ; \
    const size_t offsetZ0  = numXY *   index[2]       ; \
    const size_t offsetZP  = numXY * ( index[2] + 1 ) ; \

/// Utility macro to assign index offsets for accessing grid points with Y-1, Y, Y+1, Z-1, Z and Z+1 relative to the current index.
#define ASSIGN_YZ_OFFSETS                                               \
    const size_t offsetYMZ0 = dims[ 0 ] * ( index[1] - 1 ) + offsetZ0 ; \
    const size_t offsetY0Z0 = dims[ 0 ] *   index[1]       + offsetZ0 ; \
    const size_t offsetYPZ0 = dims[ 0 ] * ( index[1] + 1 ) + offsetZ0 ; \
    const size_t offsetY0ZM = dims[ 0 ] *   index[1]       + offsetZM ; \
    const size_t offsetY0ZP = dims[ 0 ] *   index[1]       + offsetZP ;

/// Utility macro to assign index offsets for accessing grid points with X-1, X, X+1, Y-1, Y, Y+1, Z-1, Z and Z+1 relative to the current index.
#define ASSIGN_XYZ_OFFSETS                                  \
    const size_t offsetX0Y0Z0 = index[0]     + offsetY0Z0 ; \
    const size_t offsetXMY0Z0 = index[0] - 1 + offsetY0Z0 ; \
    const size_t offsetXPY0Z0 = index[0] + 1 + offsetY0Z0 ; \
    const size_t offsetX0YMZ0 = index[0]     + offsetYMZ0 ; \
    const size_t offsetX0YPZ0 = index[0]     + offsetYPZ0 ; \
    const size_t offsetX0Y0ZM = index[0]     + offsetY0ZM ; \
    const size_t offsetX0Y0ZP = index[0]     + offsetY0ZP ; \
    (void) offsetX0Y0Z0 ,  offsetXMY0Z0 , offsetXPY0Z0 , offsetX0YMZ0 , offsetX0YPZ0 , offsetX0Y0ZM , offsetX0Y0ZP ;




/// Utility macro to assign index offsets for accessing grid points with Z-2 and Z+2 relative to the current index.
#define ASSIGN_ZZ_OFFSETS                               \
    ASSIGN_Z_OFFSETS ;                                  \
    const size_t offsetZMM = numXY * ( index[2] - 2 ) ; \
    const size_t offsetZPP = numXY * ( index[2] + 2 ) ;

/// Utility macro to assign index offsets for accessing grid points with Y-2, Y+2, Z-2 and Z+2 relative to the current index.
#define ASSIGN_YYZZ_OFFSETS                                                 \
    ASSIGN_YZ_OFFSETS ;                                                     \
    const size_t offsetYMMZ0 = dims[ 0 ] * ( index[1] - 2 ) + offsetZ0  ;   \
    const size_t offsetYPPZ0 = dims[ 0 ] * ( index[1] + 2 ) + offsetZ0  ;   \
    const size_t offsetY0ZMM = dims[ 0 ] *   index[1]       + offsetZMM ;   \
    const size_t offsetY0ZPP = dims[ 0 ] *   index[1]       + offsetZPP ;

/// Utility macro to assign index offsets for accessing grid points with X-2, X+2, Y-2, Y+2, Z-2 and Z+2 relative to the current index.
#define ASSIGN_XXYYZZ_OFFSETS                                   \
    ASSIGN_XYZ_OFFSETS                                          \
    const size_t offsetXMMY0Z0 = index[0] - 2 + offsetY0Z0  ;   \
    const size_t offsetXPPY0Z0 = index[0] + 2 + offsetY0Z0  ;   \
    const size_t offsetX0YMMZ0 = index[0]     + offsetYMMZ0 ;   \
    const size_t offsetX0YPPZ0 = index[0]     + offsetYPPZ0 ;   \
    const size_t offsetX0Y0ZMM = index[0]     + offsetY0ZMM ;   \
    const size_t offsetX0Y0ZPP = index[0]     + offsetY0ZPP ;   \
    (void) offsetXMMY0Z0 , offsetXPPY0Z0 , offsetX0YMMZ0 , offsetX0YPPZ0 , offsetX0Y0ZMM , offsetX0Y0ZPP ;



/** Compute curl of a vector field, from its Jacobian.

    \param curl - (output) UniformGrid of 3-vector values.

    \param jacobian - UniformGrid of 3x3 matrix values.

    \see ComputeJacobian.

*/
void ComputeCurlFromJacobian( UniformGrid< Vec3 > & curl , const UniformGrid< Mat33 > & jacobian )
{
    PERF_BLOCK( ComputeCurlFromJacobian ) ;

    const size_t    dims[3]     = { jacobian.GetNumPoints( 0 ) , jacobian.GetNumPoints( 1 ) , jacobian.GetNumPoints( 2 ) } ;
    const size_t    numXY       = dims[0] * dims[1] ;
    size_t          index[3] ;

    // Compute curl from Jacobian
    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        const size_t offsetZ = numXY * index[2]       ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            const size_t offsetYZ = dims[ 0 ] * index[1] + offsetZ ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                const size_t  offsetXYZ = index[0] + offsetYZ ;
                const Mat33 & j         = jacobian[ offsetXYZ ] ;
                Vec3        & rCurl     = curl[ offsetXYZ ] ;
                // Meaning of j.i.k is the derivative of the kth component with respect to i, i.e. di/dk.
                rCurl = Vec3( j.y.z - j.z.y , j.z.x - j.x.z , j.x.y - j.y.x ) ;
            }
        }
    }
}




/** Compute statistics of data in a uniform grid of 3-by-3-matrices.

    \param min - minimum of all values in grid.

    \param max - maximum of all values in grid.

*/
void UniformGrid<Mat33>::ComputeStatistics( Mat33 & min , Mat33 & max ) const
{
    const Vec3      vMax    = Vec3( FLT_MAX , FLT_MAX , FLT_MAX ) ;
    min = Mat33( vMax , vMax , vMax ) ;
    max = Mat33( -min ) ;
    const size_t    dims[3] = { GetNumPoints( 0 ) , GetNumPoints( 1 ) , GetNumPoints( 2 ) } ;
    const size_t    numXY   = dims[0] * dims[1] ;
    size_t          index[3] ;

    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        const size_t offsetPartialZ = numXY * index[2]       ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            const size_t offsetPartialYZ = dims[ 0 ] * index[1] + offsetPartialZ ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                const size_t offset = index[0]     + offsetPartialYZ  ;
                const Mat33 & rVal = (*this)[ offset ] ;
                min.x.x = Min2( min.x.x , rVal.x.x ) ;
                min.y.x = Min2( min.y.x , rVal.y.x ) ;
                min.z.x = Min2( min.z.x , rVal.z.x ) ;
                max.x.x = Max2( max.x.x , rVal.x.x ) ;
                max.y.x = Max2( max.y.x , rVal.y.x ) ;
                max.z.x = Max2( max.z.x , rVal.z.x ) ;

                min.x.y = Min2( min.x.y , rVal.x.y ) ;
                min.y.y = Min2( min.y.y , rVal.y.y ) ;
                min.z.y = Min2( min.z.y , rVal.z.y ) ;
                max.x.y = Max2( max.x.y , rVal.x.y ) ;
                max.y.y = Max2( max.y.y , rVal.y.y ) ;
                max.z.y = Max2( max.z.y , rVal.z.y ) ;

                min.x.z = Min2( min.x.z , rVal.x.z ) ;
                min.y.z = Min2( min.y.z , rVal.y.z ) ;
                min.z.z = Min2( min.z.z , rVal.z.z ) ;
                max.x.z = Max2( max.x.z , rVal.x.z ) ;
                max.y.z = Max2( max.y.z , rVal.y.z ) ;
                max.z.z = Max2( max.z.z , rVal.z.z ) ;
            }
        }
    }
}




/** Compute Jacobian of a vector field.

    \param jacobian - (output) UniformGrid of 3x3 matrix values.
                        The matrix is a vector of vectors.
                        Each component is a partial derivative with
                        respect to some direction:
                            j.a.b = d v.b / d a
                        where a and b are each one of {x,y,z}.
                        So j.x contains the partial derivatives with respect to x, etc.

    \param vec - UniformGrid of 3-vector values

*/
void ComputeJacobian( UniformGrid< Mat33 > & jacobian , const UniformGrid< Vec3 > & vec )
{
    PERF_BLOCK( ComputeJacobian ) ;

    ASSERT( jacobian.ShapeMatches( vec ) ) ;
    ASSERT( jacobian.Size() == jacobian.GetGridCapacity() ) ;

    const Vec3      spacing                 = vec.GetCellSpacing() ;
    // Avoid divide-by-zero when z size is effectively 0 (for 2D domains)
    const Vec3      reciprocalSpacing( 1.0f / spacing.x , 1.0f / spacing.y , spacing.z > FLT_EPSILON ? 1.0f / spacing.z : 0.0f ) ;
    const Vec3      halfReciprocalSpacing( 0.5f * reciprocalSpacing ) ;
    const size_t    dims[3]                 = { vec.GetNumPoints( 0 )   , vec.GetNumPoints( 1 )   , vec.GetNumPoints( 2 )   } ;
    const size_t    dimsMinus1[3]           = { vec.GetNumPoints( 0 )-1 , vec.GetNumPoints( 1 )-1 , vec.GetNumPoints( 2 )-1 } ;
    const size_t    numXY                   = dims[0] * dims[1] ;
    size_t          index[3] ;

    // Compute derivatives for interior (i.e. away from boundaries).
    for( index[2] = 1 ; index[2] < dimsMinus1[2] ; ++ index[2] )
    {
        ASSIGN_Z_OFFSETS ;
        for( index[1] = 1 ; index[1] < dimsMinus1[1] ; ++ index[1] )
        {
            ASSIGN_YZ_OFFSETS ;
            for( index[0] = 1 ; index[0] < dimsMinus1[0] ; ++ index[0] )
            {
                ASSIGN_XYZ_OFFSETS ;

                Mat33 & rMatrix = jacobian[ offsetX0Y0Z0 ] ;
                /* Compute d/dx */
                rMatrix.x = ( vec[ offsetXPY0Z0 ] - vec[ offsetXMY0Z0 ] ) * halfReciprocalSpacing.x ;
                /* Compute d/dy */
                rMatrix.y = ( vec[ offsetX0YPZ0 ] - vec[ offsetX0YMZ0 ] ) * halfReciprocalSpacing.y ;
                /* Compute d/dz */
                rMatrix.z = ( vec[ offsetX0Y0ZP ] - vec[ offsetX0Y0ZM ] ) * halfReciprocalSpacing.z ;
            }
        }
    }

    // Compute derivatives for boundaries: 6 faces of box.
    // In some situations, these macros compute extraneous data.
    // A tiny bit more efficiency could be squeezed from this routine
    // by computing edges and corner points separately
    // thereby eliminating the conditional branches,
    // but it turns out to be well under 1% of the total expense.

#define COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES                                                                              \
    Mat33 & rMatrix = jacobian[ offsetX0Y0Z0 ] ;                                                                                        \
    if     ( index[0] == 0             )    { rMatrix.x = ( vec[ offsetXPY0Z0 ] - vec[ offsetX0Y0Z0 ] ) * reciprocalSpacing.x ;     }   \
    else if( index[0] == dimsMinus1[0] )    { rMatrix.x = ( vec[ offsetX0Y0Z0 ] - vec[ offsetXMY0Z0 ] ) * reciprocalSpacing.x ;     }   \
    else                                    { rMatrix.x = ( vec[ offsetXPY0Z0 ] - vec[ offsetXMY0Z0 ] ) * halfReciprocalSpacing.x ; }   \
    if     ( index[1] == 0             )    { rMatrix.y = ( vec[ offsetX0YPZ0 ] - vec[ offsetX0Y0Z0 ] ) * reciprocalSpacing.y ;     }   \
    else if( index[1] == dimsMinus1[1] )    { rMatrix.y = ( vec[ offsetX0Y0Z0 ] - vec[ offsetX0YMZ0 ] ) * reciprocalSpacing.y ;     }   \
    else                                    { rMatrix.y = ( vec[ offsetX0YPZ0 ] - vec[ offsetX0YMZ0 ] ) * halfReciprocalSpacing.y ; }   \
    if     ( index[2] == 0             )    { rMatrix.z = ( vec[ offsetX0Y0ZP ] - vec[ offsetX0Y0Z0 ] ) * reciprocalSpacing.z ;     }   \
    else if( index[2] == dimsMinus1[2] )    { rMatrix.z = ( vec[ offsetX0Y0Z0 ] - vec[ offsetX0Y0ZM ] ) * reciprocalSpacing.z ;     }   \
    else                                    { rMatrix.z = ( vec[ offsetX0Y0ZP ] - vec[ offsetX0Y0ZM ] ) * halfReciprocalSpacing.z ; }

    // Compute derivatives for -X boundary.
    index[0] = 0 ;
    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        ASSIGN_Z_OFFSETS ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            ASSIGN_YZ_OFFSETS ;
            {
                ASSIGN_XYZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for -Y boundary.
    index[1] = 0 ;
    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        ASSIGN_Z_OFFSETS ;
        {
            ASSIGN_YZ_OFFSETS ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                ASSIGN_XYZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for -Z boundary.
    index[2] = 0 ;
    {
        ASSIGN_Z_OFFSETS ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            ASSIGN_YZ_OFFSETS ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                ASSIGN_XYZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for +X boundary.
    index[0] = dimsMinus1[0] ;
    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        ASSIGN_Z_OFFSETS ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            ASSIGN_YZ_OFFSETS ;
            {
                ASSIGN_XYZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for +Y boundary.
    index[1] = dimsMinus1[1] ;
    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        ASSIGN_Z_OFFSETS ;
        {
            ASSIGN_YZ_OFFSETS ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                ASSIGN_XYZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for +Z boundary.
    index[2] = dimsMinus1[2] ;
    {
        ASSIGN_Z_OFFSETS ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            ASSIGN_YZ_OFFSETS ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                ASSIGN_XYZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }
#undef COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES
}




/** Compute gradient of a scalar field.

    \param gradient - (output) UniformGrid of 3-vector values.
                        Each component is a partial derivative with
                        respect to some direction:
                            g.a  = d v  / d a
                        where a is each one of {x,y,z}.
                        So g.x contains the partial derivative with respect to x, etc.

    \param val - UniformGrid of scalar values

*/
static void ComputeGradientInteriorSlice( UniformGrid< Vec3 > & gradientGrid , const UniformGrid< float > & val , size_t izStart , size_t izEnd )
{
    const Vec3      spacing                 = val.GetCellSpacing() ;
    // Avoid divide-by-zero when z size is effectively 0 (for 2D domains)
    const Vec3      reciprocalSpacing( 1.0f / spacing.x , 1.0f / spacing.y , spacing.z > FLT_EPSILON ? 1.0f / spacing.z : 0.0f ) ;
    const Vec3      halfReciprocalSpacing( 0.5f * reciprocalSpacing ) ;
    const size_t    dims[3]                 = { val.GetNumPoints( 0 )   , val.GetNumPoints( 1 )   , val.GetNumPoints( 2 )   } ;
    const size_t    dimsMinus1[3]           = { val.GetNumPoints( 0 )-1 , val.GetNumPoints( 1 )-1 , val.GetNumPoints( 2 )-1 } ;
    const size_t    numXY                   = dims[0] * dims[1] ;
    size_t          index[3] ;

    ASSERT( izStart <= izEnd ) ;
    ASSERT( izEnd   <= dimsMinus1[2] ) ;

    // Compute derivatives for interior (i.e. away from boundaries).
    for( index[2] = izStart ; index[2] < izEnd ; ++ index[2] )
    {
        ASSIGN_Z_OFFSETS ;
        for( index[1] = 1 ; index[1] < dimsMinus1[1] ; ++ index[1] )
        {
            ASSIGN_YZ_OFFSETS ;
            for( index[0] = 1 ; index[0] < dimsMinus1[0] ; ++ index[0] )
            {
                ASSIGN_XYZ_OFFSETS ;

                Vec3 & gradient = gradientGrid[ offsetX0Y0Z0 ] ;
                // Compute d/dx using centered difference.
                gradient.x = ( val[ offsetXPY0Z0 ] - val[ offsetXMY0Z0 ] ) * halfReciprocalSpacing.x ;
                // Compute d/dy using centered difference.
                gradient.y = ( val[ offsetX0YPZ0 ] - val[ offsetX0YMZ0 ] ) * halfReciprocalSpacing.y ;
                // Compute d/dz using centered difference.
                gradient.z = ( val[ offsetX0Y0ZP ] - val[ offsetX0Y0ZM ] ) * halfReciprocalSpacing.z ;
            }
        }
    }
}




/** Find the minimum and maximum values in the given grid.

    \param scalarGrid - UniformGrid of 3-vector values.

    \param valMin - Minimum magnitude across entire grid.

    \param valMax - Maximum magnitude across entire grid.

*/
void FindValueRange( const UniformGrid< float > & scalarGrid , float & valMin , float & valMax )
{
    const unsigned  begin[3]    = { 0,0,0 } ;
    const unsigned  end[3]      = { scalarGrid.GetNumPoints(0) , scalarGrid.GetNumPoints(1) , scalarGrid.GetNumPoints(2) } ;
    unsigned        idx[3]      ;

    valMin =  FLT_MAX ;
    valMax = -FLT_MAX ;

    // Aggregate value min and max.
    for( idx[2] = begin[2] ; idx[2] < end[2] ; ++ idx[2] )
    for( idx[1] = begin[1] ; idx[1] < end[1] ; ++ idx[1] )
    for( idx[0] = begin[0] ; idx[0] < end[0] ; ++ idx[0] )
    {   // For each grid cell...
        const size_t offset = scalarGrid.OffsetFromIndices( idx ) ;
        const float value = scalarGrid[ offset ] ;
        valMin = Min2( value , valMin ) ;
        valMax = Max2( value , valMax ) ;
    }
}




/** Find the minimum, maximum, mean and stadard deviatino values in the given grid.

    \param scalarGrid - UniformGrid of 3-vector values.

    \param valMin - Minimum value across entire grid.

    \param valMax - Maximum value across entire grid.

    \param valMean - Mean value across entire grid.

    \param valStdDev - Standard deviation value across entire grid.

*/
void FindValueStats( const UniformGrid< float > & scalarGrid , float & valMin , float & valMax , float & valMean , float & valStdDev )
{
    const unsigned  begin[3]    = { 0,0,0 } ;
    const unsigned  end[3]      = { scalarGrid.GetNumPoints(0) , scalarGrid.GetNumPoints(1) , scalarGrid.GetNumPoints(2) } ;
    unsigned        idx[3]      ;

    valMin    =  FLT_MAX ;
    valMax    = -FLT_MAX ;
    valMean   = 0.0f ;
    valStdDev = 0.0f ;

    float sum  = 0.0f ;
    float sum2 = 0.0f ;

    // Aggregate value min and max.
    for( idx[2] = begin[2] ; idx[2] < end[2] ; ++ idx[2] )
    for( idx[1] = begin[1] ; idx[1] < end[1] ; ++ idx[1] )
    for( idx[0] = begin[0] ; idx[0] < end[0] ; ++ idx[0] )
    {   // For each grid cell...
        const size_t offset = scalarGrid.OffsetFromIndices( idx ) ;
        const float value = scalarGrid[ offset ] ;
        ASSERT( ! IsNan( value ) ) ;
        valMin = Min2( value , valMin ) ;
        valMax = Max2( value , valMax ) ;
        sum += value ;
        sum2 += value * value ;
    }

    const size_t numValues = scalarGrid.Size() ;

    valMean = sum / float( numValues ) ;
    const float mean2    = sum2 / float( numValues ) ;
    const float mean_2   = valMean * valMean ;
    const float variance = mean2 - mean_2 ;
    ASSERT( ( 0 == numValues ) || ( variance >= -3.0e-6f ) ) ;
    valStdDev = sqrt( Max2( variance , 0.0f ) ) ;
}




/** Find magnitude min and max of a vector field.

    \param vec - UniformGrid of 3-vector values.

    \param magMin - Minimum magnitude across entire grid.

    \param magMax - Maximum magnitude across entire grid.

*/
void FindMagnitudeRange( const UniformGrid< Vec3 > & vec , float & magMin , float & magMax )
{
    magMin =   FLT_MAX ;
    magMax = - FLT_MAX ;
    const size_t    dims[3] = { vec.GetNumPoints( 0 ) , vec.GetNumPoints( 1 ) , vec.GetNumPoints( 2 ) } ;
    const size_t    numXY   = dims[0] * dims[1] ;
    size_t          index[3] ;

    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        const size_t offsetPartialZ = numXY * index[2]       ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            const size_t offsetPartialYZ = dims[ 0 ] * index[1] + offsetPartialZ ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                const size_t offset = index[0]     + offsetPartialYZ  ;
                const Vec3 & rVal   = vec[ offset ] ;
                // First tally square magnitudes -- faster.
                const float  mag2    = rVal.Mag2() ;
                magMin = Min2( magMin , mag2 ) ;
                magMax = Max2( magMax , mag2 ) ;
            }
        }
    }
    // Take square root of squared values from above.
    magMin = sqrtf( magMin ) ;
    magMax = sqrtf( magMax ) ;
}





/** Compute gradient of a scalar field.

    \param gradient     (output) UniformGrid of 3-vector values.
                        Each component is a partial derivative with
                        respect to some direction:
                            g.a  = d v  / d a
                        where a is each one of {x,y,z}.
                        So g.x contains the partial derivative with respect to x, etc.

    \param scalarVals   UniformGrid of scalar values.

*/
void ComputeGradient( UniformGrid< Vec3 > & gradientGrid , const UniformGrid< float > & scalarVals )
{
    ASSERT( gradientGrid.ShapeMatches( scalarVals ) ) ;
    ASSERT( gradientGrid.Size() == gradientGrid.GetGridCapacity() ) ;

    const Vec3      spacing                 = scalarVals.GetCellSpacing() ;
    // Avoid divide-by-zero when z size is effectively 0 (for 2D domains)
    const Vec3      reciprocalSpacing( 1.0f / spacing.x , 1.0f / spacing.y , spacing.z > FLT_EPSILON ? 1.0f / spacing.z : 0.0f ) ;
    const Vec3      halfReciprocalSpacing( 0.5f * reciprocalSpacing ) ;
    const size_t    dims[3]                 = { scalarVals.GetNumPoints( 0 )   , scalarVals.GetNumPoints( 1 )   , scalarVals.GetNumPoints( 2 )   } ;
    const size_t    dimsMinus1[3]           = { scalarVals.GetNumPoints( 0 )-1 , scalarVals.GetNumPoints( 1 )-1 , scalarVals.GetNumPoints( 2 )-1 } ;
    const size_t    numXY                   = dims[0] * dims[1] ;
    size_t          index[3] ;

    // Compute derivatives for interior (i.e. away from boundaries).
    #if USE_TBB
        {
            const size_t numZ = dimsMinus1[2] - 1 ;
            // Estimate grain size based on size of problem and number of processors.
            const size_t grainSize =  Max2( size_t( 1 ) , numZ / gNumberOfProcessors ) ;
            parallel_for( tbb::blocked_range<size_t>( 1 , dimsMinus1[2] , grainSize ) , UniformGrid_ComputeGradientInterior_TBB( gradientGrid , scalarVals ) ) ;
        }
    #else
        ComputeGradientInteriorSlice( gradientGrid , scalarVals , 1 , dimsMinus1[2] ) ;
    #endif

    // Compute derivatives for boundaries: 6 faces of box.
    // In some situations, these macros compute extraneous data.
    // A tiny bit more efficiency could be squeezed from this routine,
    // but it turns out to be well under 1% of the total expense.

#define COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES                                                                                              \
    Vec3 & gradient = gradientGrid[ offsetX0Y0Z0 ] ;                                                                                                    \
    if     ( index[0] == 0             )    { gradient.x = ( scalarVals[ offsetXPY0Z0 ] - scalarVals[ offsetX0Y0Z0 ] ) * reciprocalSpacing.x ;     }    \
    else if( index[0] == dimsMinus1[0] )    { gradient.x = ( scalarVals[ offsetX0Y0Z0 ] - scalarVals[ offsetXMY0Z0 ] ) * reciprocalSpacing.x ;     }    \
    else                                    { gradient.x = ( scalarVals[ offsetXPY0Z0 ] - scalarVals[ offsetXMY0Z0 ] ) * halfReciprocalSpacing.x ; }    \
    if     ( index[1] == 0             )    { gradient.y = ( scalarVals[ offsetX0YPZ0 ] - scalarVals[ offsetX0Y0Z0 ] ) * reciprocalSpacing.y ;     }    \
    else if( index[1] == dimsMinus1[1] )    { gradient.y = ( scalarVals[ offsetX0Y0Z0 ] - scalarVals[ offsetX0YMZ0 ] ) * reciprocalSpacing.y ;     }    \
    else                                    { gradient.y = ( scalarVals[ offsetX0YPZ0 ] - scalarVals[ offsetX0YMZ0 ] ) * halfReciprocalSpacing.y ; }    \
    if     ( index[2] == 0             )    { gradient.z = ( scalarVals[ offsetX0Y0ZP ] - scalarVals[ offsetX0Y0Z0 ] ) * reciprocalSpacing.z ;     }    \
    else if( index[2] == dimsMinus1[2] )    { gradient.z = ( scalarVals[ offsetX0Y0Z0 ] - scalarVals[ offsetX0Y0ZM ] ) * reciprocalSpacing.z ;     }    \
    else                                    { gradient.z = ( scalarVals[ offsetX0Y0ZP ] - scalarVals[ offsetX0Y0ZM ] ) * halfReciprocalSpacing.z ; }

    // Compute derivatives for -X boundary.
    index[0] = 0 ;
    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        ASSIGN_Z_OFFSETS ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            ASSIGN_YZ_OFFSETS ;
            {
                ASSIGN_XYZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for -Y boundary.
    index[1] = 0 ;
    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        ASSIGN_Z_OFFSETS ;
        {
            ASSIGN_YZ_OFFSETS ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                ASSIGN_XYZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for -Z boundary.
    index[2] = 0 ;
    {
        ASSIGN_Z_OFFSETS ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            ASSIGN_YZ_OFFSETS ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                ASSIGN_XYZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for +X boundary.
    index[0] = dimsMinus1[0] ;
    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        ASSIGN_Z_OFFSETS ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            ASSIGN_YZ_OFFSETS ;
            {
                ASSIGN_XYZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for +Y boundary.
    index[1] = dimsMinus1[1] ;
    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        ASSIGN_Z_OFFSETS ;
        {
            ASSIGN_YZ_OFFSETS ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                ASSIGN_XYZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for +Z boundary.
    index[2] = dimsMinus1[2] ;
    {
        ASSIGN_Z_OFFSETS ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            ASSIGN_YZ_OFFSETS ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                ASSIGN_XYZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }
#undef COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES
}




/** Compute gradient of a scalar field with number and non-number values.

    \param gradient - (output) UniformGrid of 3-vector values.
                        Each component is a partial derivative with
                        respect to some direction:
                            g.a  = d v  / d a
                        where a is each one of {x,y,z}.
                        So g.x contains the partial derivative with respect to x, etc.

    \param val - UniformGrid of scalar values.

*/
static void ComputeGradientConditionallySlice( UniformGrid< Vec3 > & gradientGrid , const UniformGrid< float > & scalarVals , size_t izStart , size_t izEnd )
{
    const Vec3      spacing                 = scalarVals.GetCellSpacing() ;
    // Avoid divide-by-zero when z size is effectively 0 (for 2D domains)
    const Vec3      reciprocalSpacing( 1.0f / spacing.x , 1.0f / spacing.y , spacing.z > FLT_EPSILON ? 1.0f / spacing.z : 0.0f ) ;
    const Vec3      halfReciprocalSpacing( 0.5f * reciprocalSpacing ) ;
    const size_t    dims[3]                 = { scalarVals.GetNumPoints( 0 )   , scalarVals.GetNumPoints( 1 )   , scalarVals.GetNumPoints( 2 )   } ;
    const size_t    dimsMinus1[3]           = { scalarVals.GetNumPoints( 0 )-1 , scalarVals.GetNumPoints( 1 )-1 , scalarVals.GetNumPoints( 2 )-1 } ;
    const size_t    numXY                   = dims[0] * dims[1] ;
    size_t          index[3] ;

    ASSERT( izStart <= izEnd ) ;
    ASSERT( izEnd   <= dims[2] ) ;

#define SET_PARTIAL_DERIVATIVE_CONDITIONALLY( componentIndex , minusIndex , plusIndex , component )                                                                                 \
    {                                                                                                                                                                               \
        if( IsNan( scalarVals[ index[ componentIndex ] ] ) )                                                                                                                        \
        {   /* Values span domains across this gridpoint.  Derivative does not exist here. */                                                                                       \
            gradient.component = UNIFORM_GRID_INVALID_VALUE ;                                                                                                                       \
        }                                                                                                                                                                           \
        else                                                                                                                                                                        \
        {   /* Value exists at this gridpoint. */                                                                                                                                   \
            /* Look for valid index pair. */                                                                                                                                        \
            /* Candidate index is valid if it lies inside domain and value at that index is a number. */                                                                            \
            size_t idxUpper = ( index[ componentIndex ] < dimsMinus1[ componentIndex ] ) && ( ! IsNan( scalarVals[ index[ componentIndex ]+1 ] ) ) ? plusIndex  : offsetX0Y0Z0 ;    \
            size_t idxLower = ( index[ componentIndex ] > 0                            ) && ( ! IsNan( scalarVals[ index[ componentIndex ]-1 ] ) ) ? minusIndex : offsetX0Y0Z0 ;    \
            size_t idxDiff = idxUpper - idxLower ;                                                                                                                                  \
            if( 2 == idxDiff )                                                                                                                                                      \
            {   /* Values span 2 gridpoints. Compute partial derivative using centered difference. */                                                                               \
                gradient.component = ( scalarVals[ idxUpper ] - scalarVals[ idxLower ] ) * halfReciprocalSpacing.component ;                                                        \
            }                                                                                                                                                                       \
            if( 1 == idxDiff )                                                                                                                                                      \
            {   /* Values span 1 gridpoint. Compute partial derivative using forward or backward difference. */                                                                     \
                gradient.component = ( scalarVals[ idxUpper ] - scalarVals[ idxLower ] ) * reciprocalSpacing.component ;                                                            \
            }                                                                                                                                                                       \
            else                                                                                                                                                                    \
            {   /* No valid range of indices could be found. Derivative does not exist here. */                                                                                     \
                gradient.component = UNIFORM_GRID_INVALID_VALUE ;                                                                                                                   \
            }                                                                                                                                                                       \
        }                                                                                                                                                                           \
    }

    // Compute derivatives for interior (i.e. away from boundaries).
    for( index[2] = izStart ; index[2] < izEnd ; ++ index[2] )
    {
        ASSIGN_Z_OFFSETS ;
        for( index[1] = 1 ; index[1] < dims[1] ; ++ index[1] )
        {
            ASSIGN_YZ_OFFSETS ;
            for( index[0] = 1 ; index[0] < dims[0] ; ++ index[0] )
            {
                ASSIGN_XYZ_OFFSETS ;

                Vec3 & gradient = gradientGrid[ offsetX0Y0Z0 ] ;

                SET_PARTIAL_DERIVATIVE_CONDITIONALLY( 0 , offsetXMY0Z0 , offsetXPY0Z0 , x ) ;
                SET_PARTIAL_DERIVATIVE_CONDITIONALLY( 1 , offsetX0YMZ0 , offsetX0YPZ0 , y ) ;
                SET_PARTIAL_DERIVATIVE_CONDITIONALLY( 2 , offsetX0Y0ZM , offsetX0Y0ZP , z ) ;

            }
        }
    }
}




/** Compute gradient of a scalar field which might have some invalid values

    \see ComputeGradient.
*/
void ComputeGradientConditionally( UniformGrid< Vec3 > & gradient , const UniformGrid< float > & scalarVals )
{
    ASSERT( gradient.ShapeMatches( scalarVals ) ) ;
    ASSERT( gradient.Size() == gradient.GetGridCapacity() ) ;

    const Vec3      spacing                 = scalarVals.GetCellSpacing() ;
    // Avoid divide-by-zero when z size is effectively 0 (for 2D domains)
    const Vec3      reciprocalSpacing( 1.0f / spacing.x , 1.0f / spacing.y , spacing.z > FLT_EPSILON ? 1.0f / spacing.z : 0.0f ) ;
    const Vec3      halfReciprocalSpacing( 0.5f * reciprocalSpacing ) ;
    const size_t    dims[3]                 = { scalarVals.GetNumPoints( 0 )   , scalarVals.GetNumPoints( 1 )   , scalarVals.GetNumPoints( 2 )   } ;

    // Compute derivatives.
    #if USE_TBB && 0
        {
            // Estimate grain size based on size of problem and number of processors.
            const size_t grainSize =  Max2( 1 , dims[2] / gNumberOfProcessors ) ;
            parallel_for( tbb::blocked_range<size_t>( 1 , dims[2] , grainSize ) , UniformGrid_ComputeGradientConditionally_TBB( gradient , scalarVals ) ) ;
        }
    #else
        ComputeGradientConditionallySlice( gradient , scalarVals , 1 , dims[2] ) ;
    #endif
}




/** Compute Laplacian of a vector field.

    \param laplacian - (output) UniformGrid of 3-vector values, the vector Laplacian of "vec"

    \param vec - UniformGrid of 3-vector values

    \see ComputeJacobian.

*/
void ComputeLaplacian( UniformGrid< Vec3 > & laplacian , const UniformGrid< Vec3 > & vec )
{
    ASSERT( laplacian.ShapeMatches( vec ) ) ;
    // This routine currently only supports fully 3D domains.
    // To compute a Laplacian, which is a second derivative, requires at least 3 gridpoints in each direction.
    ASSERT( ( vec.GetNumPoints( 0 ) >= 3 ) && ( vec.GetNumPoints( 1 ) >= 3 ) && ( vec.GetNumPoints( 2 ) >= 3 ) ) ;

    const Vec3      spacing                 = vec.GetCellSpacing() ;
    // Avoid divide-by-zero when z size is effectively 0 (for 2D domains)
    const Vec3      reciprocalSpacing( 1.0f / spacing.x , 1.0f / spacing.y , spacing.z > FLT_EPSILON ? 1.0f / spacing.z : 0.0f ) ;
    const Vec3      reciprocalSpacing2( POW2( reciprocalSpacing.x ) , POW2( reciprocalSpacing.y ) , POW2( reciprocalSpacing.z ) ) ;
    const size_t    dims[3]                 = { vec.GetNumPoints( 0 )   , vec.GetNumPoints( 1 )   , vec.GetNumPoints( 2 )   } ;
    const size_t    dimsMinus1[3]           = { vec.GetNumPoints( 0 )-1 , vec.GetNumPoints( 1 )-1 , vec.GetNumPoints( 2 )-1 } ;
    const size_t    numXY                   = dims[0] * dims[1] ;
    size_t          index[3] ;

    // Compute derivatives for interior (i.e. away from boundaries).
    for( index[2] = 1 ; index[2] < dimsMinus1[2] ; ++ index[2] )
    {
        ASSIGN_Z_OFFSETS ;
        for( index[1] = 1 ; index[1] < dimsMinus1[1] ; ++ index[1] )
        {
            ASSIGN_YZ_OFFSETS ;
            for( index[0] = 1 ; index[0] < dimsMinus1[0] ; ++ index[0] )
            {
                ASSIGN_XYZ_OFFSETS ;

                Vec3 & rLaplacian = laplacian[ offsetX0Y0Z0 ] ;
                // Compute ( d2/dx2 + d2/dy2 + d2/dz2 ) vec
            #if 1
                rLaplacian =    ( vec[ offsetXPY0Z0 ] + vec[ offsetXMY0Z0 ] - 2.0f * vec[ offsetX0Y0Z0 ] ) * reciprocalSpacing2.x
                            +   ( vec[ offsetX0YPZ0 ] + vec[ offsetX0YMZ0 ] - 2.0f * vec[ offsetX0Y0Z0 ] ) * reciprocalSpacing2.y
                            +   ( vec[ offsetX0Y0ZP ] + vec[ offsetX0Y0ZM ] - 2.0f * vec[ offsetX0Y0Z0 ] ) * reciprocalSpacing2.z ;
            #else   // Mathematically equivalent, computationally more expensive.  This form is better suited to writing a Poisson solver.
                rLaplacian =    ( vec[ offsetXPY0Z0 ] + vec[ offsetXMY0Z0 ] ) * reciprocalSpacing2.x
                            +   ( vec[ offsetX0YPZ0 ] + vec[ offsetX0YMZ0 ] ) * reciprocalSpacing2.y
                            +   ( vec[ offsetX0Y0ZP ] + vec[ offsetX0Y0ZM ] ) * reciprocalSpacing2.z
                            - 2.0f * vec[ offsetX0Y0Z0 ] * ( reciprocalSpacing2.x + reciprocalSpacing2.y + reciprocalSpacing2.z ) ;
            #endif
                ASSERT( ! IsNan( rLaplacian ) && ! IsInf( rLaplacian ) ) ;
            }
        }
    }

    // Compute derivatives for boundaries: 6 faces of box.
    // In some situations, these macros compute extraneous data.
    // A tiny bit more efficiency could be squeezed from this routine,
    // but it turns out to be well under 1% of the total expense.

#define COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES                                                                                                          \
    Vec3 & rLaplacian = laplacian[ offsetX0Y0Z0 ] ;                                                                                                                 \
    if     ( index[0] == 0             )    { rLaplacian  = ( vec[ offsetXPPY0Z0 ] + vec[ offsetX0Y0Z0  ] - 2.0f * vec[ offsetXPY0Z0 ] ) * reciprocalSpacing2.x ; } \
    else if( index[0] == dimsMinus1[0] )    { rLaplacian  = ( vec[ offsetX0Y0Z0  ] + vec[ offsetXMMY0Z0 ] - 2.0f * vec[ offsetXMY0Z0 ] ) * reciprocalSpacing2.x ; } \
    else                                    { rLaplacian  = ( vec[ offsetXPY0Z0  ] + vec[ offsetXMY0Z0  ] - 2.0f * vec[ offsetX0Y0Z0 ] ) * reciprocalSpacing2.x ; } \
    if     ( index[1] == 0             )    { rLaplacian += ( vec[ offsetX0YPPZ0 ] + vec[ offsetX0Y0Z0  ] - 2.0f * vec[ offsetX0YPZ0 ] ) * reciprocalSpacing2.y ; } \
    else if( index[1] == dimsMinus1[1] )    { rLaplacian += ( vec[ offsetX0Y0Z0  ] + vec[ offsetX0YMMZ0 ] - 2.0f * vec[ offsetX0YMZ0 ] ) * reciprocalSpacing2.y ; } \
    else                                    { rLaplacian += ( vec[ offsetX0YPZ0  ] + vec[ offsetX0YMZ0  ] - 2.0f * vec[ offsetX0Y0Z0 ] ) * reciprocalSpacing2.y ; } \
    if     ( index[2] == 0             )    { rLaplacian += ( vec[ offsetX0Y0ZPP ] + vec[ offsetX0Y0Z0  ] - 2.0f * vec[ offsetX0Y0ZP ] ) * reciprocalSpacing2.z ; } \
    else if( index[2] == dimsMinus1[2] )    { rLaplacian += ( vec[ offsetX0Y0Z0  ] + vec[ offsetX0Y0ZMM ] - 2.0f * vec[ offsetX0Y0ZM ] ) * reciprocalSpacing2.z ; } \
    else                                    { rLaplacian += ( vec[ offsetX0Y0ZP  ] + vec[ offsetX0Y0ZM  ] - 2.0f * vec[ offsetX0Y0Z0 ] ) * reciprocalSpacing2.z ; } \
    ASSERT( ! IsNan( rLaplacian ) && ! IsInf( rLaplacian ) ) ;


    // Compute derivatives for -X boundary.
    index[0] = 0 ;
    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        ASSIGN_ZZ_OFFSETS ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            ASSIGN_YYZZ_OFFSETS ;
            {
                ASSIGN_XXYYZZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for -Y boundary.
    index[1] = 0 ;
    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        ASSIGN_ZZ_OFFSETS ;
        {
            ASSIGN_YYZZ_OFFSETS ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                ASSIGN_XXYYZZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for -Z boundary.
    index[2] = 0 ;
    {
        ASSIGN_ZZ_OFFSETS ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            ASSIGN_YYZZ_OFFSETS ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                ASSIGN_XXYYZZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for +X boundary.
    index[0] = dimsMinus1[0] ;
    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        ASSIGN_ZZ_OFFSETS ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            ASSIGN_YYZZ_OFFSETS ;
            {
                ASSIGN_XXYYZZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for +Y boundary.
    index[1] = dimsMinus1[1] ;
    for( index[2] = 0 ; index[2] < dims[2] ; ++ index[2] )
    {
        ASSIGN_ZZ_OFFSETS ;
        {
            ASSIGN_YYZZ_OFFSETS ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                ASSIGN_XXYYZZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }

    // Compute derivatives for +Z boundary.
    index[2] = dimsMinus1[2] ;
    {
        ASSIGN_ZZ_OFFSETS ;
        for( index[1] = 0 ; index[1] < dims[1] ; ++ index[1] )
        {
            ASSIGN_YYZZ_OFFSETS ;
            for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
            {
                ASSIGN_XXYYZZ_OFFSETS ;
                COMPUTE_FINITE_PARTIAL_DIFFERENTIALS_AT_BOUNDARIES ;
            }
        }
    }
}




/** Shift Y based on z index and red-black value.

    Black elements start at (iy=0, iz=0) and both iy and iz have the same parity.
    Red   elements start at (iy=1, iz=0) and iy and iz have opposite parity.

    This implementation exploits the fact that redOrBlack is either 0 or 1 for red or black,
    and 2 (neither 0 nor 1) for both.

*/
#define ASSIGN_Y_SHIFT const size_t idxYShift = ( index[2] % 2 == unsigned( redOrBlack ) ) ? 1 : 0 ;

#define ASSIGN_Z_OFFSETS_AND_Y_SHIFT  ASSIGN_Z_OFFSETS  ; ASSIGN_Y_SHIFT ;
#define ASSIGN_ZZ_OFFSETS_AND_Y_SHIFT ASSIGN_ZZ_OFFSETS ; ASSIGN_Y_SHIFT ;




/** Make one step toward solving the discretized vector Poisson equation.

    This routine takes a step toward solving the discretized form of the Poisson equation,
        D soln = lap ,
    where D is the Laplacian partial differential operator.

    This routine uses a finite difference representation of the Laplacian operator,
    and uses the Gauss-Seidel method, augmented with successive over-relaxation,
    to solve the resulting linear algebraic equation that replaces the partial differential equation.

    This routine should be invoked multiple times.  In the simplest case (that is, when NOT using
    this routine inside a multi-grid algorithm), invoke this routine approximately N times where
    N is the largest dimension of the grid.  Each step of this routine transfers information
    between adjacent cells.  But the Poisson equation require a global solution, meaning that
    each cell must feel the influence of all cells in the grid.  It therefore takes at least N
    steps to propagate information between the cells separated by N cells.

    \param soln - (output) UniformGrid of 3-vector values, the solution to the vector Poisson equation

    \param lap - (input) UniformGrid of 3-vector values.

    \param izStart - starting value for z index

    \param izEnd - one past final value for z index

    \param redOrBlack - When running this routine serially, pass "GS_BOTH" for this value.
                    When running this routine in parallel with others accessing the same UniformGrid,
                    call this routine twice per thread, alternatively passing "GS_RED"
                    and "GS_BLACK" for this value, both calls with the same range for izStart and izEnd.
                    This implements the so-called "red-black Gauss-Seidel" algorithm.

                    red squares start at (0,0).

    \see ComputeJacobian.

*/
static void StepTowardVectorPoissonSolution( UniformGrid< Vec3 > & soln , const UniformGrid< Vec3 > & lap , size_t izStart , size_t izEnd , GaussSeidelPortionE redOrBlack , BoundaryConditionE boundaryCondition , Stats_Float & residualStats )
{
    ASSERT( soln.Size() == soln.GetGridCapacity() ) ;
    ASSERT( izStart <  lap.GetNumPoints( 2 )    ) ;
    ASSERT( izEnd   <= lap.GetNumPoints( 2 )    ) ;
    ASSERT( ( redOrBlack >= GS_MIN ) && ( redOrBlack < GS_NUM ) ) ;

    const Vec3      spacing                 = lap.GetCellSpacing() ;
    // Avoid divide-by-zero when z size is effectively 0 (for 2D domains)
    const Vec3      reciprocalSpacing( 1.0f / spacing.x , 1.0f / spacing.y , spacing.z > FLT_EPSILON ? 1.0f / spacing.z : 0.0f ) ;
    const Vec3      reciprocalSpacing2( POW2( reciprocalSpacing.x ) , POW2( reciprocalSpacing.y ) , POW2( reciprocalSpacing.z ) ) ;
    const float     HalfSpacing2Sum         = 0.5f / ( reciprocalSpacing2.x + reciprocalSpacing2.y + reciprocalSpacing2.z ) ;
    const size_t    dims[3]                 = { lap.GetNumPoints( 0 )   , lap.GetNumPoints( 1 )   , lap.GetNumPoints( 2 )   } ;
    const size_t    dimsMinus1[3]           = { lap.GetNumPoints( 0 )-1 , lap.GetNumPoints( 1 )-1 , lap.GetNumPoints( 2 )-1 } ;
    const size_t    numXY                   = dims[0] * dims[1] ;

    // Note: Setting "relax" to 1 would yield the canonical Gauss-Seidel algorithm.

    // Experiment: Approximate optimal relaxation parameter.
    //const float     relax                   = 2.0f / ( 1.0f + sin( PI / float( MAX3( dims[0] , dims[1] , dims[2] ) ) ) ) ;

    // MJG emperically determined relax using non-MultiGrid on a vortex ring with dims=32^3 advancing from t=0 to t=3.3e-5. Minimum residual occurred with relax in [1.72,1.74].
    // relax=1.25 had nearly same residual as relax=1.  Halved for relax=1.5. Dropped to 1/100 of that going to relax=1.73.  Doubled from there for relax=1.75.
    const float     relax                   = 1.72f ;
    const float     oneMinusRelax           = 1.0f - relax ;
    ASSERT( relax >= 1.0f ) ;
    ASSERT( relax <  2.0f ) ;

    residualStats.mMean = 0.0f ; // Also temporarily used to store sum of residuals
    residualStats.mStdDev = 0.0f ; // Also temporarily used to store sum of residuals-squared
    residualStats.mMin =  FLT_MAX ;
    residualStats.mMax = -FLT_MAX ;
    unsigned residualsCount = 0 ; // number of gridpoints involved in computing residual stats

    // To make this routine work in red-black mode, the index range for the interior depends on redOrBlack.
    const size_t    idxZMinInterior         = Max2( size_t( 1 )   , izStart ) ;
    const size_t    idxZMaxInterior         = Min2( dimsMinus1[2] , izEnd   ) ;
    const size_t    iyStep                  = ( GS_BOTH == redOrBlack ) ? 1 : 2 ;
    size_t          index[3] ;

    {
        // Solve equation for interior (i.e. away from boundaries).
        for( index[2] = idxZMinInterior ; index[2] < idxZMaxInterior ; ++ index[2] )
        {
            ASSIGN_Z_OFFSETS_AND_Y_SHIFT ;
            for( index[1] = 1 + idxYShift ; index[1] < dimsMinus1[1] ; index[1] += iyStep )
            {
                ASSIGN_YZ_OFFSETS ;
                for( index[0] = 1 ; index[0] < dimsMinus1[0] ; ++ index[0] )
                {
                    ASSIGN_XYZ_OFFSETS ;
                    Vec3   vSolution =  (   ( soln[ offsetXPY0Z0 ] + soln[ offsetXMY0Z0 ] ) * reciprocalSpacing2.x
                        +   ( soln[ offsetX0YPZ0 ] + soln[ offsetX0YMZ0 ] ) * reciprocalSpacing2.y
                        +   ( soln[ offsetX0Y0ZP ] + soln[ offsetX0Y0ZM ] ) * reciprocalSpacing2.z
                        -   lap[ offsetX0Y0Z0 ]
                        ) * HalfSpacing2Sum ;
                        ASSERT( ! IsNan( vSolution ) && ! IsInf( vSolution ) ) ;
#                   if ! USE_TBB
                        {   // Useful for tuning number of steps, SOR parameter.  Beware; not thread-safe nor would aggregation be deterministic if using parallel_reduce.  Would need to aggregate with parallel_invoke.
                            const Vec3 updatedVal = oneMinusRelax * soln[ offsetX0Y0Z0 ] + relax * vSolution ;
                            const Vec3 residualVec = updatedVal - soln[ offsetX0Y0Z0 ] ;
                            const float residual = residualVec.Magnitude() ;
                            residualStats.mMean += residual ;
                            residualStats.mStdDev += Pow2( residual ) ;
                            residualStats.mMin = Min2( residual , residualStats.mMin ) ;
                            residualStats.mMax = Max2( residual , residualStats.mMax ) ;
                            ++ residualsCount ;
                        }
#                   endif
                        soln[ offsetX0Y0Z0 ] = oneMinusRelax * soln[ offsetX0Y0Z0 ] + relax * vSolution ;
                }
            }
        }
    }

    if( BC_NEUMANN == boundaryCondition )
    {
        // Use "natural" boundary condition.
        // The natural boundary condition is one where the derivative is specified,
        // which allows the solution to "naturally" reach whatever value it must.
        //
        // This contrasts with "essential" boundary conditions which imposes
        // values on the solution directly.
        //
        // See notes in ComputeVelocityFromVorticity_Differential regarding an alternative
        // approach that would use essential boundary conditions and yield better results
        // when solving velocity-from-vorticity.

        // Assign solution on boundaries (that is, at the 6 faces of domain box).

        // Assign values on boundary points to equal those of adjacent interior points (already solved).
        // This is tantamount to enforcing that first derivatives at boundaries are zero.
        //
        // Note: Propagate solution from interior to last points in boundary, not other way around.
        // If values were propagated from boundary points inward, that would be tantamount to
        // specifying both Dirichlet and Neumann boundary conditions simultaneously (which would
        // resemble Cauchy boundary conditions).
        // 
        // Natural (Neumann) boundary conditions contrast with "Dirichlet" boundary conditions,
        // where the value at the boundary is prescribed.  To implement that, disable this block
        // of code, and prescribe the values on the boundaries prior to calling this routine.
        //
        // This snippet assumes index[] and offsetX0Y0Z0 refer to a point on domain boundary.
#   define ASSIGN_VALUES_ON_BOUNDARY_POINTS                                                                                                 \
        {                                                                                                                                   \
            Vec3 & rOnBoundary = soln[ offsetX0Y0Z0 ] ; /* value on boundary point */                                                       \
            /* Set idxInterior to refer to source point just inside (not on) domain boundary, adjacent to destination point on boundary */  \
            size_t idxInterior[3] = {  Clamp( index[0] , size_t( 1 ) , dimsMinus1[0] - 1 )                                                  \
                                    ,  Clamp( index[1] , size_t( 1 ) , dimsMinus1[1] - 1 )                                                  \
                                    ,  Clamp( index[2] , size_t( 1 ) , dimsMinus1[2] - 1 ) } ;                                              \
            const size_t offsetInterior = idxInterior[0] + dims[ 0 ] * idxInterior[1] + numXY * idxInterior[2] ;                            \
            rOnBoundary = soln[ offsetInterior ] ;                                                                                          \
            ASSERT( ! IsNan( rOnBoundary ) && ! IsInf( rOnBoundary ) ) ;                                                                    \
        }

        // Assign values for -X boundary.
        index[0] = 0 ;
        for( index[2] = izStart ; index[2] < izEnd ; ++ index[2] )
        {
            ASSIGN_Z_OFFSETS_AND_Y_SHIFT ;
            for( index[1] = idxYShift ; index[1] < dims[1] ; index[1] += iyStep )
            {
                ASSIGN_YZ_OFFSETS ;
                {
                    ASSIGN_XYZ_OFFSETS ;
                    ASSIGN_VALUES_ON_BOUNDARY_POINTS ;
                }
            }
        }

        // Assign values for -Y boundary.
        index[1] = 0 ;
        for( index[2] = izStart ; index[2] < izEnd ; ++ index[2] )
        {
            ASSIGN_Z_OFFSETS_AND_Y_SHIFT ;
            (void) idxYShift ;
            {
                ASSIGN_YZ_OFFSETS ;
                for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
                {
                    ASSIGN_XYZ_OFFSETS ;
                    ASSIGN_VALUES_ON_BOUNDARY_POINTS ;
                }
            }
        }

        // Assign values for -Z boundary (only if this slice contains that boundary).
        if( 0 == izStart )
        {
            index[2] = 0 ;
            {
                ASSIGN_Z_OFFSETS_AND_Y_SHIFT ;
                for( index[1] = idxYShift ; index[1] < dims[1] ; index[1] += iyStep )
                {
                    ASSIGN_YZ_OFFSETS ;
                    for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
                    {
                        ASSIGN_XYZ_OFFSETS ;
                        ASSIGN_VALUES_ON_BOUNDARY_POINTS ;
                    }
                }
            }
        }

        // Assign values for +X boundary.
        index[0] = dimsMinus1[0] ;
        for( index[2] = izStart ; index[2] < izEnd ; ++ index[2] )
        {
            ASSIGN_Z_OFFSETS_AND_Y_SHIFT ;
            (void) idxYShift ;
            for( index[1] = 0 ; index[1] < dims[1] ; index[1] += iyStep )
            {
                ASSIGN_YZ_OFFSETS ;
                {
                    ASSIGN_XYZ_OFFSETS ;
                    ASSIGN_VALUES_ON_BOUNDARY_POINTS ;
                }
            }
        }

        // Assign values for +Y boundary.
        index[1] = dimsMinus1[1] ;
        for( index[2] = izStart ; index[2] < izEnd ; ++ index[2] )
        {
            ASSIGN_Z_OFFSETS_AND_Y_SHIFT ;
            (void) idxYShift ;
            {
                ASSIGN_YZ_OFFSETS ;
                for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
                {
                    ASSIGN_XYZ_OFFSETS ;
                    ASSIGN_VALUES_ON_BOUNDARY_POINTS ;
                }
            }
        }

        // Assign values for +Z boundary (only if this slice contains that boundary)
        if( izEnd == dims[ 2 ] )
        {
            index[2] = dimsMinus1[2] ;
            {
                ASSIGN_Z_OFFSETS_AND_Y_SHIFT ;
                for( index[1] = idxYShift ; index[1] < dims[1] ; index[1] += iyStep )
                {
                    ASSIGN_YZ_OFFSETS ;
                    for( index[0] = 0 ; index[0] < dims[0] ; ++ index[0] )
                    {
                        ASSIGN_XYZ_OFFSETS ;
                        ASSIGN_VALUES_ON_BOUNDARY_POINTS ;
                    }
                }
            }
        }
    }

    if( residualsCount != 0 )
    {   // Residual raw stats were tallied.  Cook them.
        residualStats.mMean  = residualStats.mMean   / float( residualsCount ) ;
        const float mean2    = residualStats.mStdDev / float( residualsCount ) ;
        const float variance = mean2 - Pow2( residualStats.mMean ) ;
        residualStats.mStdDev = fsqrtf( variance ) ;
    }

}

#undef ASSIGN_XYZ_OFFSETS
#undef ASSIGN_YZ_OFFSETS
#undef ASSIGN_Z_OFFSETS

#undef ASSIGN_XXYYZZ_OFFSETS
#undef ASSIGN_YYZZ_OFFSETS
#undef ASSIGN_ZZ_OFFSETS
#undef ASSIGN_ZZ_OFFSETS_AND_Y_SHIFT




/** Solve the discretized vector Poisson equation.

    This routine solves the discretized form of the Poisson equation,
        D soln = lap ,
    where D is the Laplacian partial differential operator.

    \param soln (output) UniformGrid of 3-vector values, the solution to the vector Poisson equation.

    \param lap  (input) UniformGrid of 3-vector values.

    \param numSteps Maximum number of solver iterations (StepTowardVectorPoissonSolution) to apply.

    \param enforceNeumannBoundaryCondition  Whether to enforce Neumann boundary condition.  If false, enforce Dirichlet boundary condition instead.

    \see StepTowardVectorPoissonSolution.
*/
void SolveVectorPoisson( UniformGrid< Vec3 > & soln , const UniformGrid< Vec3 > & lap , size_t numSteps , BoundaryConditionE boundaryCondition , Stats_Float & residualStats )
{
    PERF_BLOCK( SolveVectorPoisson ) ;

    ASSERT( soln.ShapeMatches( lap ) ) ;

    // Init soln to zero here would disconnect multigrid V-cycle stages, overwrite boundary values, so don't do it. This would be bad: //soln.Init( Vec3( 0.0f , 0.0f , 0.0f ) ) ;

    const size_t  gridDimMax    = MAX3( soln.GetNumPoints( 0 ) , soln.GetNumPoints( 1 ) , soln.GetNumPoints( 2 ) ) ;
#if 0
    const size_t  gridDimMax3   = Pow3( gridDimMax ) ;
    const size_t  gridDimMax3_2 = size_t( sqrt( double( gridDimMax3 ) ) ) ;
    const size_t  numStepsAuto  = Max2( gridDimMax3_2 , gridDimMax ) ;
#else
    const size_t  numStepsAuto  = 2 * gridDimMax ;
#endif
    const size_t  maxIters      = ( numSteps > 0 ) ? numSteps : numStepsAuto ;
    for( size_t iter = 0 ; iter < maxIters ; ++ iter )
    {
        const size_t numZ = soln.GetNumPoints( 2 ) ;
#   if USE_TBB
        {
            // Estimate grain size based on size of problem and number of processors.
            const size_t grainSize =  Max2( size_t( 1 ) , numZ / gNumberOfProcessors ) ;
            parallel_for( tbb::blocked_range<size_t>( 0 , numZ , grainSize ) , UniformGrid_StepTowardVectorPoissonSolution_TBB( soln , lap , GS_RED   , boundaryCondition , residualStats ) ) ;
            parallel_for( tbb::blocked_range<size_t>( 0 , numZ , grainSize ) , UniformGrid_StepTowardVectorPoissonSolution_TBB( soln , lap , GS_BLACK , boundaryCondition , residualStats ) ) ;
        }
#   elif POISSON_TECHNIQUE == POISSON_TECHNIQUE_GAUSS_SEIDEL_RED_BLACK
        StepTowardVectorPoissonSolution( soln , lap , 0 , numZ , GS_RED   , boundaryCondition , residualStats ) ;
        StepTowardVectorPoissonSolution( soln , lap , 0 , numZ , GS_BLACK , boundaryCondition , residualStats ) ;
#   elif POISSON_TECHNIQUE == POISSON_TECHNIQUE_GAUSS_SEIDEL
        StepTowardVectorPoissonSolution( soln , lap , 0 , numZ , GS_BOTH  , boundaryCondition , residualStats ) ;
#   else
#       error Invalid or undefined POISSON_TECHNIQUE.  Either define POISSON_TECHNIQUE appropriately or change this code.
#   endif
    }
}
