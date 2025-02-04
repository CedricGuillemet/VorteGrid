/** \file vortonGrid.cpp

    \brief Utility routines for uniform grid of vortex particles

    \author Copyright 2009-2012 Dr. Michael Jason Gourlay; All rights reserved.  Contact me at mijagourlay.com for licensing.
*/

#include "Core/Math/vec3.h"

#include "vortonGrid.h"




/*! \brief Compute the total circulation in a uniform grid of vortons

    \param vortonGrid - uniform grid of vortons

    \param vCirculation - Total circulation, the volume integral of vorticity, computed by this routine.

    \param vLinearImpulse - Volume integral of circulation weighted by position, computed by this routine.

*/
void VortonGrid_ConservedQuantities( const UniformGrid< Vorton > & vortonGrid , Vec3 & vCirculation , Vec3 & vLinearImpulse )
{
    vCirculation = vLinearImpulse = Vec3( 0.0f , 0.0f , 0.0f ) ;
    const size_t & numGridPoints = vortonGrid.Size() ;
    for( unsigned offset = 0 ; offset < numGridPoints ; ++ offset )
    {
        const Vorton &  rVorton         = vortonGrid[ offset ]  ;
        const float     volumeElement   = Pow3( rVorton.GetRadius() ) * FourPiOver3 ;
        // Accumulate total circulation.
        vCirculation    += rVorton.GetVorticity() * volumeElement ;
        // Accumulate total linear impulse.
        vLinearImpulse  += rVorton.mPosition ^ rVorton.GetVorticity() * volumeElement ;
    }
}
