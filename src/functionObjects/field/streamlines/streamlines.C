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

#include "Pstream.H"
#include "functionObjectList.H"
#include "streamlines.H"
#include "streamlinesCloud.H"
#include "ReadFields.H"
#include "meshSearch.H"
#include "sampledSet.H"
#include "globalIndex.H"
#include "mapDistribute.H"
#include "interpolationCellPoint.H"
#include "PatchTools.H"
#include "mapPolyMesh.H"
#include "writeFile.H"
#include "addToRunTimeSelectionTable.H"

// * * * * * * * * * * * * * * Static Data Members * * * * * * * * * * * * * //

namespace Foam
{
    template<>
    const char*
        NamedEnum<functionObjects::streamlines::trackDirection, 3>::names[] =
        {"forward", "backward", "both"};

    namespace functionObjects
    {
        defineTypeNameAndDebug(streamlines, 0);
        addToRunTimeSelectionTable(functionObject, streamlines, dictionary);

        const NamedEnum<streamlines::trackDirection, 3>
            streamlines::trackDirectionNames_;
    }
}


// * * * * * * * * * * * * * Private Member Functions  * * * * * * * * * * * //

Foam::autoPtr<Foam::indirectPrimitivePatch>
Foam::functionObjects::streamlines::wallPatch() const
{
    const polyBoundaryMesh& patches = mesh_.boundaryMesh();

    label nFaces = 0;

    forAll(patches, patchi)
    {
        if (isA<wallPolyPatch>(patches[patchi]))
        {
            nFaces += patches[patchi].size();
        }
    }

    labelList addressing(nFaces);

    nFaces = 0;

    forAll(patches, patchi)
    {
        if (isA<wallPolyPatch>(patches[patchi]))
        {
            const polyPatch& pp = patches[patchi];

            forAll(pp, i)
            {
                addressing[nFaces++] = pp.start()+i;
            }
        }
    }

    return autoPtr<indirectPrimitivePatch>
    (
        new indirectPrimitivePatch
        (
            IndirectList<face>
            (
                mesh_.faces(),
                addressing
            ),
            mesh_.points()
        )
    );
}


void Foam::functionObjects::streamlines::track()
{
    IDLList<streamlinesParticle> initialParticles;
    streamlinesCloud particles
    (
        mesh_,
        cloudName_,
        initialParticles
    );

    const sampledSet& seedPoints = sampledSetPtr_();

    forAll(seedPoints, i)
    {
        particles.addParticle
        (
            new streamlinesParticle
            (
                mesh_,
                seedPoints[i],
                seedPoints.cells()[i],
                lifeTime_
            )
        );
    }

    label nSeeds = returnReduce(particles.size(), sumOp<label>());

    Info << "    seeded " << nSeeds << " particles" << endl;

    // Read or lookup fields
    PtrList<volScalarField> vsFlds;
    PtrList<interpolation<scalar>> vsInterp;
    PtrList<volVectorField> vvFlds;
    PtrList<interpolation<vector>> vvInterp;

    label UIndex = -1;

    label nScalar = 0;
    label nVector = 0;

    forAll(fields_, i)
    {
        if (mesh_.foundObject<volScalarField>(fields_[i]))
        {
            nScalar++;
        }
        else if (mesh_.foundObject<volVectorField>(fields_[i]))
        {
            nVector++;
        }
        else
        {
            FatalErrorInFunction
                << "Cannot find field " << fields_[i] << nl
                << "Valid scalar fields are:"
                << mesh_.names(volScalarField::typeName) << nl
                << "Valid vector fields are:"
                << mesh_.names(volVectorField::typeName)
                << exit(FatalError);
        }
    }
    vsInterp.setSize(nScalar);
    nScalar = 0;
    vvInterp.setSize(nVector);
    nVector = 0;

    forAll(fields_, i)
    {
        if (mesh_.foundObject<volScalarField>(fields_[i]))
        {
            const volScalarField& f = mesh_.lookupObject<volScalarField>
            (
                fields_[i]
            );
            vsInterp.set
            (
                nScalar++,
                interpolation<scalar>::New
                (
                    interpolationScheme_,
                    f
                )
            );
        }
        else if (mesh_.foundObject<volVectorField>(fields_[i]))
        {
            const volVectorField& f = mesh_.lookupObject<volVectorField>
            (
                fields_[i]
            );

            if (f.name() == UName_)
            {
                UIndex = nVector;
            }

            vvInterp.set
            (
                nVector++,
                interpolation<vector>::New
                (
                    interpolationScheme_,
                    f
                )
            );
        }
    }

    // Store the names
    scalarNames_.setSize(vsInterp.size());
    forAll(vsInterp, i)
    {
        scalarNames_[i] = vsInterp[i].psi().name();
    }
    vectorNames_.setSize(vvInterp.size());
    forAll(vvInterp, i)
    {
        vectorNames_[i] = vvInterp[i].psi().name();
    }

    // Check that we know the index of U in the interpolators.

    if (UIndex == -1)
    {
        FatalErrorInFunction
            << "Cannot find field to move particles with : " << UName_ << nl
            << "This field has to be present in the sampled fields " << fields_
            << " and in the objectRegistry."
            << exit(FatalError);
    }

    // Sampled data
    // ~~~~~~~~~~~~

    // Size to maximum expected sizes.
    allTracks_.clear();
    allTracks_.setCapacity(nSeeds);
    allAges_.clear();
    allAges_.setCapacity(nSeeds);
    allScalars_.setSize(vsInterp.size());
    forAll(allScalars_, i)
    {
        allScalars_[i].clear();
        allScalars_[i].setCapacity(nSeeds);
    }
    allVectors_.setSize(vvInterp.size());
    forAll(allVectors_, i)
    {
        allVectors_[i].clear();
        allVectors_[i].setCapacity(nSeeds);
    }


    // Additional particle info
    streamlinesParticle::trackingData td
    (
        particles,
        vsInterp,
        vvInterp,
        UIndex,         // index of U in vvInterp

        trackDirection_ == trackDirection::forward,

        nSubCycle_,     // automatic track control:step through cells in steps?
        trackLength_,   // fixed track length

        allTracks_,
        allAges_,
        allScalars_,
        allVectors_
    );

    // Set very large dt. Note: cannot use great since 1/great is small
    // which is a trigger value for the tracking...
    const scalar trackTime = Foam::sqrt(great);

    // Track
    if (trackDirection_ == trackDirection::both)
    {
        initialParticles = particles;
    }

    particles.move(particles, td, trackTime);

    if (trackDirection_ == trackDirection::both)
    {
        particles.IDLList<streamlinesParticle>::operator=(initialParticles);
        td.trackForward_ = !td.trackForward_;
        particles.move(particles, td, trackTime);
    }
}


// * * * * * * * * * * * * * * * * Constructors  * * * * * * * * * * * * * * //

Foam::functionObjects::streamlines::streamlines
(
    const word& name,
    const Time& runTime,
    const dictionary& dict
)
:
    fvMeshFunctionObject(name, runTime, dict),
    dict_(dict),
    nSubCycle_(0)
{
    read(dict_);
}


// * * * * * * * * * * * * * * * * Destructor  * * * * * * * * * * * * * * * //

Foam::functionObjects::streamlines::~streamlines()
{}


// * * * * * * * * * * * * * * * Member Functions  * * * * * * * * * * * * * //

bool Foam::functionObjects::streamlines::read(const dictionary& dict)
{
    if (dict != dict_)
    {
        dict_ = dict;
    }

    Info<< type() << " " << name() << ":" << nl;

    dict.lookup("fields") >> fields_;
    UName_ = dict.lookupOrDefault("U", word("U"));

    if (findIndex(fields_, UName_) == -1)
    {
        FatalIOErrorInFunction(dict)
            << "Velocity field for tracking " << UName_
            << " should be present in the list of fields " << fields_
            << exit(FatalIOError);
    }

    writeAge_ = dict.lookupOrDefault<Switch>("writeAge", true);

    // The trackForward entry is maintained here for backwards compatibility
    if (!dict.found("direction") && dict.found("trackForward"))
    {
        trackDirection_ =
            dict.lookup<bool>("trackForward")
          ? trackDirection::forward
          : trackDirection::backward;
    }
    else
    {
        trackDirection_ = trackDirectionNames_[word(dict.lookup("direction"))];
    }

    dict.lookup("lifeTime") >> lifeTime_;
    if (lifeTime_ < 1)
    {
        FatalErrorInFunction
            << "Illegal value " << lifeTime_ << " for lifeTime"
            << exit(FatalError);
    }


    bool subCycling = dict.found("nSubCycle");
    bool fixedLength = dict.found("trackLength");

    if (subCycling && fixedLength)
    {
        FatalIOErrorInFunction(dict)
            << "Cannot both specify automatic time stepping (through '"
            << "nSubCycle' specification) and fixed track length (through '"
            << "trackLength')"
            << exit(FatalIOError);
    }


    nSubCycle_ = 1;
    if (dict.readIfPresent("nSubCycle", nSubCycle_))
    {
        trackLength_ = vGreat;
        if (nSubCycle_ < 1)
        {
            nSubCycle_ = 1;
        }
        Info<< "    automatic track length specified through"
            << " number of sub cycles : " << nSubCycle_ << nl << endl;
    }
    else
    {
        dict.lookup("trackLength") >> trackLength_;

        Info<< "    fixed track length specified : "
            << trackLength_ << nl << endl;
    }


    interpolationScheme_ = dict.lookupOrDefault
    (
        "interpolationScheme",
        interpolationCellPoint<scalar>::typeName
    );

    cloudName_ = dict.lookupOrDefault<word>("cloudName", "streamlines");

    meshSearchPtr_.reset(new meshSearch(mesh_));

    sampledSetPtr_ = sampledSet::New
    (
        "seedSampleSet",
        mesh_,
        meshSearchPtr_(),
        dict.subDict("seedSampleSet")
    );
    sampledSetAxis_ = sampledSetPtr_->axis();

    scalarFormatterPtr_ = setWriter<scalar>::New(dict.lookup("setFormat"));
    vectorFormatterPtr_ = setWriter<vector>::New(dict.lookup("setFormat"));

    return true;
}


bool Foam::functionObjects::streamlines::execute()
{
    return true;
}


bool Foam::functionObjects::streamlines::write()
{
    Info<< type() << " " << name() << " write:" << nl;

    const Time& runTime = obr_.time();

    // Do all injection and tracking
    track();


    if (Pstream::parRun())
    {
        // Append slave tracks to master ones
        // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

        globalIndex globalTrackIDs(allTracks_.size());

        // Construct a distribution map to pull all to the master.
        labelListList sendMap(Pstream::nProcs());
        labelListList recvMap(Pstream::nProcs());

        if (Pstream::master())
        {
            // Master: receive all. My own first, then consecutive
            // processors.
            label trackI = 0;

            forAll(recvMap, proci)
            {
                labelList& fromProc = recvMap[proci];
                fromProc.setSize(globalTrackIDs.localSize(proci));
                forAll(fromProc, i)
                {
                    fromProc[i] = trackI++;
                }
            }
        }

        labelList& toMaster = sendMap[0];
        toMaster.setSize(globalTrackIDs.localSize());
        forAll(toMaster, i)
        {
            toMaster[i] = i;
        }

        const mapDistribute distMap
        (
            globalTrackIDs.size(),
            move(sendMap),
            move(recvMap)
        );


        // Distribute the track positions. Note: use scheduled comms
        // to prevent buffering.
        mapDistributeBase::distribute
        (
            Pstream::commsTypes::scheduled,
            distMap.schedule(),
            distMap.constructSize(),
            distMap.subMap(),
            false,
            distMap.constructMap(),
            false,
            allTracks_,
            flipOp()
        );

        // Distribute the ages
        mapDistributeBase::distribute
        (
            Pstream::commsTypes::scheduled,
            distMap.schedule(),
            distMap.constructSize(),
            distMap.subMap(),
            false,
            distMap.constructMap(),
            false,
            allAges_,
            flipOp()
        );

        // Distribute the scalars
        forAll(allScalars_, scalari)
        {
            allScalars_[scalari].shrink();
            mapDistributeBase::distribute
            (
                Pstream::commsTypes::scheduled,
                distMap.schedule(),
                distMap.constructSize(),
                distMap.subMap(),
                false,
                distMap.constructMap(),
                false,
                allScalars_[scalari],
                flipOp()
            );
            allScalars_[scalari].setCapacity(allScalars_[scalari].size());
        }
        // Distribute the vectors
        forAll(allVectors_, vectori)
        {
            allVectors_[vectori].shrink();
            mapDistributeBase::distribute
            (
                Pstream::commsTypes::scheduled,
                distMap.schedule(),
                distMap.constructSize(),
                distMap.subMap(),
                false,
                distMap.constructMap(),
                false,
                allVectors_[vectori],
                flipOp()
            );
            allVectors_[vectori].setCapacity(allVectors_[vectori].size());
        }
    }


    label n = 0;
    forAll(allTracks_, trackI)
    {
        n += allTracks_[trackI].size();
    }

    Info<< "    Tracks:" << allTracks_.size() << nl
        << "    Total samples:" << n
        << endl;


    // Massage into form suitable for writers
    // ~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

    if (Pstream::master() && allTracks_.size())
    {
        // Make output directory

        fileName vtkPath
        (
            runTime.globalPath()/writeFile::outputPrefix/"sets"/name()
        );
        if (mesh_.name() != fvMesh::defaultRegion)
        {
            vtkPath = vtkPath/mesh_.name();
        }
        vtkPath = vtkPath/mesh_.time().timeName();
        mkDir(vtkPath);

        // Convert track positions

        PtrList<coordSet> tracks(allTracks_.size());
        forAll(allTracks_, trackI)
        {
            tracks.set
            (
                trackI,
                new coordSet
                (
                    "track" + Foam::name(trackI),
                    sampledSetAxis_                 //"xyz"
                )
            );
            tracks[trackI].transfer(allTracks_[trackI]);
        }

        // Convert scalar values

        if (allScalars_.size() > 0 || writeAge_)
        {
            List<List<scalarField>> ageAndScalarValues
            (
                allScalars_.size() + writeAge_
            );
            wordList ageAndScalarNames(allScalars_.size() + writeAge_);

            if (writeAge_)
            {
                DynamicList<scalarList>& allTrackAges = allAges_;

                ageAndScalarValues[0].setSize(allTrackAges.size());
                forAll(allTrackAges, trackI)
                {
                    ageAndScalarValues[0][trackI].transfer
                    (
                        allTrackAges[trackI]
                    );
                }

                ageAndScalarNames[0] = "age";
            }

            forAll(allScalars_, scalari)
            {
                const label ageAndScalari = scalari + writeAge_;

                DynamicList<scalarList>& allTrackVals = allScalars_[scalari];

                ageAndScalarValues[ageAndScalari].setSize(allTrackVals.size());
                forAll(allTrackVals, trackI)
                {
                    ageAndScalarValues[ageAndScalari][trackI].transfer
                    (
                        allTrackVals[trackI]
                    );
                }

                ageAndScalarNames[ageAndScalari] = scalarNames_[scalari];
            }

            fileName vtkFile
            (
                vtkPath
              / scalarFormatterPtr_().getFileName
                (
                    tracks[0],
                    ageAndScalarNames
                )
            );

            Info<< "    Writing data to " << vtkFile.path() << endl;

            scalarFormatterPtr_().write
            (
                true,           // writeTracks
                tracks,
                ageAndScalarNames,
                ageAndScalarValues,
                OFstream(vtkFile)()
            );
        }

        // Convert vector values

        if (allVectors_.size() > 0)
        {
            List<List<vectorField>> vectorValues(allVectors_.size());

            forAll(allVectors_, vectori)
            {
                DynamicList<vectorList>& allTrackVals = allVectors_[vectori];

                vectorValues[vectori].setSize(allTrackVals.size());
                forAll(allTrackVals, trackI)
                {
                    vectorValues[vectori][trackI].transfer
                    (
                        allTrackVals[trackI]
                    );
                }
            }

            fileName vtkFile
            (
                vtkPath
              / vectorFormatterPtr_().getFileName
                (
                    tracks[0],
                    vectorNames_
                )
            );

            Info<< "    Writing data to " << vtkFile.path() << endl;

            vectorFormatterPtr_().write
            (
                true,           // writeTracks
                tracks,
                vectorNames_,
                vectorValues,
                OFstream(vtkFile)()
            );
        }
    }

    return true;
}


void Foam::functionObjects::streamlines::updateMesh(const mapPolyMesh& mpm)
{
    if (&mpm.mesh() == &mesh_)
    {
        read(dict_);
    }
}


void Foam::functionObjects::streamlines::movePoints(const polyMesh& mesh)
{
    if (&mesh == &mesh_)
    {
        // Moving mesh affects the search tree
        read(dict_);
    }
}


// ************************************************************************* //
