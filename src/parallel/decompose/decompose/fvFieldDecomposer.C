/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     | Website:  https://openfoam.org
    \\  /    A nd           | Copyright (C) 2011-2021 OpenFOAM Foundation
     \\/     M anipulation  |
-------------------------------------------------------------------------------
License
    This file is part of OpenFOAM.

    OpenFOAM is free software: you can redistribute it and/or modify it
    under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    OpenFOAM is distributed in the hope that it will be useful, but WITHOUT
    ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
    FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License
    for more details.

    You should have received a copy of the GNU General Public License
    along with OpenFOAM.  If not, see <http://www.gnu.org/licenses/>.

\*---------------------------------------------------------------------------*/

#include "fvFieldDecomposer.H"


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::labelList Foam::fvFieldDecomposer::patchFieldDecomposer::alignAddressing
(
    const labelUList& addressingSlice,
    const label addressingOffset
) const
{
    labelList addressing(addressingSlice.size());

    forAll(addressing, i)
    {
        // Subtract one to align addressing.
        addressing[i] = addressingSlice[i] - (addressingOffset + 1);
    }

    return addressing;
}


Foam::fvFieldDecomposer::patchFieldDecomposer::patchFieldDecomposer
(
    const labelUList& addressingSlice,
    const label addressingOffset
)
:
    labelList(alignAddressing(addressingSlice, addressingOffset)),
    directFvPatchFieldMapper(static_cast<const labelList&>(*this))
{}


Foam::labelList Foam::fvFieldDecomposer::processorVolPatchFieldDecomposer::
alignAddressing
(
    const fvMesh& mesh,
    const labelUList& addressingSlice
) const
{
    labelList addressing(addressingSlice.size());

    const labelList& own = mesh.faceOwner();
    const labelList& neighb = mesh.faceNeighbour();

    forAll(addressing, i)
    {
        // Subtract one to align addressing.
        label ai = mag(addressingSlice[i]) - 1;

        if (ai < neighb.size())
        {
            // This is a regular face. it has been an internal face
            // of the original mesh and now it has become a face
            // on the parallel boundary.
            // Give face the value of the neighbour.

            if (addressingSlice[i] >= 0)
            {
                // I have the owner so use the neighbour value
                addressing[i] = neighb[ai];
            }
            else
            {
                addressing[i] = own[ai];
            }
        }
        else
        {
            // This is a face that used to be on a cyclic boundary
            // but has now become a parallel patch face. I cannot
            // do the interpolation properly (I would need to look
            // up the different (face) list of data), so I will
            // just grab the value from the owner cell

            addressing[i] = own[ai];
        }
    }

    return addressing;
}


Foam::fvFieldDecomposer::processorVolPatchFieldDecomposer::
processorVolPatchFieldDecomposer
(
    const fvMesh& mesh,
    const labelUList& addressingSlice
)
:
    labelList(alignAddressing(mesh, addressingSlice)),
    directFvPatchFieldMapper(static_cast<const labelList&>(*this))
{}


Foam::fvFieldDecomposer::fvFieldDecomposer
(
    const fvMesh& completeMesh,
    const fvMesh& procMesh,
    const labelList& faceAddressing,
    const labelList& cellAddressing,
    const labelList& boundaryAddressing
)
:
    completeMesh_(completeMesh),
    procMesh_(procMesh),
    faceAddressing_(faceAddressing),
    cellAddressing_(cellAddressing),
    boundaryAddressing_(boundaryAddressing),
    patchFieldDecomposerPtrs_
    (
        procMesh_.boundary().size(),
        static_cast<patchFieldDecomposer*>(nullptr)
    ),
    processorVolPatchFieldDecomposerPtrs_
    (
        procMesh_.boundary().size(),
        static_cast<processorVolPatchFieldDecomposer*>(nullptr)
    )
{
    forAll(boundaryAddressing_, patchi)
    {
        const fvPatch& procPatch = procMesh.boundary()[patchi];

        label fromPatchi = boundaryAddressing_[patchi];
        if (fromPatchi < 0 && isA<processorCyclicFvPatch>(procPatch))
        {
            const label referPatchi =
                refCast<const processorCyclicPolyPatch>
                (procPatch.patch()).referPatchID();
            fromPatchi = boundaryAddressing_[referPatchi];
        }

        if (fromPatchi >= 0)
        {
            patchFieldDecomposerPtrs_[patchi] = new patchFieldDecomposer
            (
                procMesh_.boundary()[patchi].patchSlice(faceAddressing_),
                completeMesh_.boundaryMesh()[fromPatchi].start()
            );
        }

        if (boundaryAddressing_[patchi] < 0)
        {
            processorVolPatchFieldDecomposerPtrs_[patchi] =
                new processorVolPatchFieldDecomposer
                (
                    completeMesh_,
                    procMesh_.boundary()[patchi].patchSlice(faceAddressing_)
                );
        }
    }
}


// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

Foam::fvFieldDecomposer::~fvFieldDecomposer()
{
    forAll(patchFieldDecomposerPtrs_, patchi)
    {
        if (patchFieldDecomposerPtrs_[patchi])
        {
            delete patchFieldDecomposerPtrs_[patchi];
        }
    }

    forAll(processorVolPatchFieldDecomposerPtrs_, patchi)
    {
        if (processorVolPatchFieldDecomposerPtrs_[patchi])
        {
            delete processorVolPatchFieldDecomposerPtrs_[patchi];
        }
    }
}

// ************************************************************************* //
