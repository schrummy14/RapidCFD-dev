/*---------------------------------------------------------------------------*\
  =========                 |
  \\      /  F ield         | OpenFOAM: The Open Source CFD Toolbox
   \\    /   O peration     |
    \\  /    A nd           | Copyright (C) 2011-2013 OpenFOAM Foundation
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

#include "GAMGSolver.H"
#include "GAMGInterfaceField.H"
#include "processorLduInterfaceField.H"
#include "processorGAMGInterfaceField.H"
#include "GAMGSolverAgglomerateMatrixF.H"
#include "GAMGAgglomerateF.H"

#include <thrust/reduce.h>

// * * * * * * * * * * * * * Private Member Functions  * * * * * * * * * * * //

void Foam::GAMGSolver::agglomerateMatrix
(
    const label fineLevelIndex,
    const lduMesh& coarseMesh,
    const lduInterfacePtrsList& coarseMeshInterfaces
)
{
    // Get fine matrix
    const lduMatrix& fineMatrix = matrixLevel(fineLevelIndex);

    if (UPstream::myProcNo(fineMatrix.mesh().comm()) != -1)
    {
        const label nCoarseFaces = agglomeration_.nFaces(fineLevelIndex);
        const label nCoarseCells = agglomeration_.nCells(fineLevelIndex);

        // Set the coarse level matrix
        matrixLevels_.set
        (
            fineLevelIndex,
            new lduMatrix(coarseMesh)
        );
        lduMatrix& coarseMatrix = matrixLevels_[fineLevelIndex];


        // Coarse matrix diagonal initialised by restricting the finer mesh
        // diagonal. Note that we size with the cached coarse nCells and not
        // the actual coarseMesh size since this might be dummy when processor
        // agglomerating.
        scalargpuField& coarseDiag = coarseMatrix.diag(nCoarseCells);

        agglomeration_.restrictField
        (
            coarseDiag,
            fineMatrix.diag(),
            fineLevelIndex
        );

        // Get reference to fine-level interfaces
        const lduInterfaceFieldPtrsList& fineInterfaces =
            interfaceLevel(fineLevelIndex);

        // Create coarse-level interfaces
        primitiveInterfaceLevels_.set
        (
            fineLevelIndex,
            new PtrList<lduInterfaceField>(fineInterfaces.size())
        );

        PtrList<lduInterfaceField>& coarsePrimInterfaces =
            primitiveInterfaceLevels_[fineLevelIndex];

        interfaceLevels_.set
        (
            fineLevelIndex,
            new lduInterfaceFieldPtrsList(fineInterfaces.size())
        );

        lduInterfaceFieldPtrsList& coarseInterfaces =
            interfaceLevels_[fineLevelIndex];

        // Set coarse-level boundary coefficients
        interfaceLevelsBouCoeffs_.set
        (
            fineLevelIndex,
            new FieldField<gpuField, scalar>(fineInterfaces.size())
        );
        FieldField<gpuField, scalar>& coarseInterfaceBouCoeffs =
            interfaceLevelsBouCoeffs_[fineLevelIndex];

        // Set coarse-level internal coefficients
        interfaceLevelsIntCoeffs_.set
        (
            fineLevelIndex,
            new FieldField<gpuField, scalar>(fineInterfaces.size())
        );
        FieldField<gpuField, scalar>& coarseInterfaceIntCoeffs =
            interfaceLevelsIntCoeffs_[fineLevelIndex];

        // Add the coarse level
        agglomerateInterfaceCoefficients
        (
            fineLevelIndex,
            coarseMeshInterfaces,
            coarsePrimInterfaces,
            coarseInterfaces,
            coarseInterfaceBouCoeffs,
            coarseInterfaceIntCoeffs
        );

        const boolgpuList& faceFlipMap =
            agglomeration_.faceFlipMap(fineLevelIndex);

        const scalargpuField& fineUpper = fineMatrix.upper();
        scalargpuField& coarseUpper = coarseMatrix.upper(nCoarseFaces);

        if(agglomeration_.useAtomic())
        {
            // Get face restriction addressing for current level
            const labelgpuList& faceRestrictAddr =
                agglomeration_.faceRestrictAddressing(fineLevelIndex);

            auto fineFaceBegin = thrust::make_counting_iterator(0);
            auto fineFaceEnd = fineFaceBegin + faceRestrictAddr.size();

            // Check if matrix is asymetric and if so agglomerate both upper
            // and lower coefficients ...
            if (fineMatrix.hasLower())
            {
                // Get off-diagonal matrix coefficients
                const scalargpuField& fineLower = fineMatrix.lower();

                // Coarse matrix upper coefficients. Note passed in size
                scalargpuField& coarseLower = coarseMatrix.lower(nCoarseFaces);

                thrust::for_each
                (
                    fineFaceBegin, fineFaceEnd, 
                    GAMG::asymAtomicAgglomerate
                    (
                        coarseDiag.data(),
                        coarseUpper.data(),
                        coarseLower.data(),
                        fineUpper.data(),
                        fineLower.data(),
                        faceRestrictAddr.data(),
                        faceFlipMap.data()
                    )
                );

            }
            else // ... Otherwise it is symmetric so agglomerate just the upper
            {
                thrust::for_each
                (
                    fineFaceBegin, fineFaceEnd, 
                    GAMG::symAtomicAgglomerate
                    (
                        coarseDiag.data(),
                        coarseUpper.data(),
                        fineUpper.data(),
                        faceRestrictAddr.data()
                    )
                );
            }
        }
        else //================================================================
        {

            // Get face restriction addressing for current level
            const labelgpuList& faceRestrictSortAddr =
                agglomeration_.faceRestrictSortAddressing(fineLevelIndex); 
            const labelgpuList& faceRestrictTargetAddr = 
                agglomeration_.faceRestrictTargetAddressing(fineLevelIndex);
            const labelgpuList& faceRestrictTargetStartAddr = 
                agglomeration_.faceRestrictTargetStartAddressing(fineLevelIndex);

            // Check if matrix is asymetric and if so agglomerate both upper
            // and lower coefficients ...
            if (fineMatrix.hasLower())
            {
                // Get off-diagonal matrix coefficients
                const scalargpuField& fineLower = fineMatrix.lower();

                // Coarse matrix upper coefficients. Note passed in size
                scalargpuField& coarseLower = coarseMatrix.lower(nCoarseFaces);

                auto coarseLUIter = thrust::make_zip_iterator(thrust::make_tuple
                    (
                        thrust::make_permutation_iterator
                        (
                            coarseUpper.begin(),
                            faceRestrictTargetAddr.begin()
                        ),

                        thrust::make_permutation_iterator
                        (
                            coarseLower.begin(),
                            faceRestrictTargetAddr.begin()
                        )
                    ));

                thrust::transform_if
                (
                    coarseLUIter, coarseLUIter + faceRestrictTargetAddr.size(),
                    thrust::make_zip_iterator(thrust::make_tuple
                    (
                        faceRestrictTargetStartAddr.begin(),
                        faceRestrictTargetStartAddr.begin()+1
                    )),
                    faceRestrictTargetAddr.begin(),
                    coarseLUIter,
                    GAMG::asymAgglomerate
                    (
                        fineUpper.data(),
                        fineLower.data(),
                        faceFlipMap.data(),
                        faceRestrictSortAddr.data()
                    ),

                    GAMG::nonNegative()
                );

                auto coarseDiagIter = thrust::make_permutation_iterator
                    (
                        coarseDiag.begin(),
                        thrust::make_transform_iterator
                        (
                            faceRestrictTargetAddr.begin(),
                            GAMG::faceToDiag()
                        )
                    );

                thrust::transform_if
                (
                    coarseDiagIter, coarseDiagIter + faceRestrictTargetAddr.size(),
                    thrust::make_zip_iterator(thrust::make_tuple
                    (
                        faceRestrictTargetStartAddr.begin(),
                        faceRestrictTargetStartAddr.begin()+1
                    )),
                    faceRestrictTargetAddr.begin(),
                    coarseDiagIter,
                    GAMG::diagAsymAgglomerate
                    (
                        fineUpper.data(),
                        fineLower.data(),
                        faceRestrictSortAddr.data()
                    ),
                    GAMG::negative()
                );

            }
            else // ... Otherwise it is symmetric so agglomerate just the upper
            {
                auto coarseUpperIter = thrust::make_permutation_iterator(
                     coarseUpper.begin(),  faceRestrictTargetAddr.begin());

                thrust::transform_if
                (
                    coarseUpperIter, coarseUpperIter + faceRestrictTargetAddr.size(),
                    thrust::make_zip_iterator(thrust::make_tuple
                    (
                        faceRestrictTargetStartAddr.begin(),
                        faceRestrictTargetStartAddr.begin()+1
                    )),
                    faceRestrictTargetAddr.begin(),
                    coarseUpperIter,
                    GAMG::symAgglomerate
                    (
                        fineUpper.data(),
                        faceRestrictSortAddr.data()
                    ),
                    GAMG::nonNegative()
                );

                auto coarseDiagIter = thrust::make_permutation_iterator(
                        coarseDiag.begin(),
                        thrust::make_transform_iterator
                        (
                            faceRestrictTargetAddr.begin(),
                            GAMG::faceToDiag()
                        )
                    );

                thrust::transform_if
                (
                    coarseDiagIter, coarseDiagIter + faceRestrictTargetAddr.size(),
                    thrust::make_zip_iterator(thrust::make_tuple
                    (
                        faceRestrictTargetStartAddr.begin(),
                        faceRestrictTargetStartAddr.begin()+1
                    )),
                    faceRestrictTargetAddr.begin(),
                    coarseDiagIter,
                    GAMG::diagSymAgglomerate
                    (
                        fineUpper.data(),
                        faceRestrictSortAddr.data()
                    ),
                    GAMG::negative()
                );
            }
        }
    }
}


// Agglomerate only the interface coefficients.
void Foam::GAMGSolver::agglomerateInterfaceCoefficients
(
    const label fineLevelIndex,
    const lduInterfacePtrsList& coarseMeshInterfaces,
    PtrList<lduInterfaceField>& coarsePrimInterfaces,
    lduInterfaceFieldPtrsList& coarseInterfaces,
    FieldField<gpuField, scalar>& coarseInterfaceBouCoeffs,
    FieldField<gpuField, scalar>& coarseInterfaceIntCoeffs
) const
{
    // Get reference to fine-level interfaces
    const lduInterfaceFieldPtrsList& fineInterfaces =
        interfaceLevel(fineLevelIndex);

    // Get reference to fine-level boundary coefficients
    const FieldField<gpuField, scalar>& fineInterfaceBouCoeffs =
        interfaceBouCoeffsLevel(fineLevelIndex);

    // Get reference to fine-level internal coefficients
    const FieldField<gpuField, scalar>& fineInterfaceIntCoeffs =
        interfaceIntCoeffsLevel(fineLevelIndex);

    const labelList& nPatchFaces =
        agglomeration_.nPatchFaces(fineLevelIndex);


    // Add the coarse level
    forAll(fineInterfaces, inti)
    {
        if (fineInterfaces.set(inti))
        {
            const GAMGInterface& coarseInterface =
                refCast<const GAMGInterface>
                (
                    coarseMeshInterfaces[inti]
                );

            coarsePrimInterfaces.set
            (
                inti,
                GAMGInterfaceField::New
                (
                    coarseInterface,
                    fineInterfaces[inti]
                ).ptr()
            );
            coarseInterfaces.set
            (
                inti,
                &coarsePrimInterfaces[inti]
            );


            coarseInterfaceBouCoeffs.set
            (
                inti,
                new scalargpuField(nPatchFaces[inti], 0.0)
            );

            coarseInterfaceIntCoeffs.set
            (
                inti,
                new scalargpuField(nPatchFaces[inti], 0.0)
            );

            if(agglomeration_.useAtomic())
            {
                const labelgpuListList& patchFineToCoarse =
		    agglomeration_.patchFaceRestrictAddressing(fineLevelIndex);
                const labelgpuList& faceRestrictAddressing = patchFineToCoarse[inti];

                agglomeration_.restrictField
                (
                    coarseInterfaceBouCoeffs[inti],
                    fineInterfaceBouCoeffs[inti],
                    faceRestrictAddressing
                );

                agglomeration_.restrictField
                (
                    coarseInterfaceIntCoeffs[inti],
                    fineInterfaceIntCoeffs[inti],
                    faceRestrictAddressing
                );
            }
            else
            {
                const labelgpuListList& patchFineToCoarseSort =
                    agglomeration_.patchFaceRestrictSortAddressing(fineLevelIndex);
                const labelgpuListList& patchFineToCoarseTarget =
                    agglomeration_.patchFaceRestrictTargetAddressing(fineLevelIndex);
                const labelgpuListList& patchFineToCoarseTargetStart =
                    agglomeration_.patchFaceRestrictTargetStartAddressing(fineLevelIndex);

                const labelgpuList& faceRestrictSortAddressing = patchFineToCoarseSort[inti];
                const labelgpuList& faceRestrictTargetAddressing = patchFineToCoarseTarget[inti];
                const labelgpuList& faceRestrictTargetStartAddressing = patchFineToCoarseTargetStart[inti];

                agglomeration_.restrictField
                (
                    coarseInterfaceBouCoeffs[inti],
                    fineInterfaceBouCoeffs[inti],
                    faceRestrictSortAddressing,
                    faceRestrictTargetAddressing,
                    faceRestrictTargetStartAddressing
                );

                agglomeration_.restrictField
                (
                    coarseInterfaceIntCoeffs[inti],
                    fineInterfaceIntCoeffs[inti],
                    faceRestrictSortAddressing,
                    faceRestrictTargetAddressing,
                    faceRestrictTargetStartAddressing
                );
            }

        }
    }
}


// Gather matrices.
// Note: matrices get constructed with dummy mesh
/*
void Foam::GAMGSolver::gatherMatrices
(
    const labelList& procIDs,
    const lduMesh& dummyMesh,
    const label meshComm,

    const lduMatrix& mat,
    const FieldField<gpuField, scalar>& interfaceBouCoeffs,
    const FieldField<gpuField, scalar>& interfaceIntCoeffs,
    const lduInterfaceFieldPtrsList& interfaces,

    PtrList<lduMatrix>& otherMats,
    PtrList<FieldField<gpuField, scalar> >& otherBouCoeffs,
    PtrList<FieldField<gpuField, scalar> >& otherIntCoeffs,
    List<boolList>& otherTransforms,
    List<List<int> >& otherRanks
) const
{
    if (debug)
    {
        Pout<< "GAMGSolver::gatherMatrices :"
            << " collecting matrices from procs:" << procIDs
            << " using comm:" << meshComm << endl;
    }

    if (Pstream::myProcNo(meshComm) == procIDs[0])
    {
        // Master.
        otherMats.setSize(procIDs.size()-1);
        otherBouCoeffs.setSize(procIDs.size()-1);
        otherIntCoeffs.setSize(procIDs.size()-1);
        otherTransforms.setSize(procIDs.size()-1);
        otherRanks.setSize(procIDs.size()-1);

        for (label procI = 1; procI < procIDs.size(); procI++)
        {
            label otherI = procI-1;

            IPstream fromSlave
            (
                Pstream::scheduled,
                procIDs[procI],
                0,          // bufSize
                Pstream::msgType(),
                meshComm
            );

            otherMats.set(otherI, new lduMatrix(dummyMesh, fromSlave));

            // Receive number of/valid interfaces
            boolList& procTransforms = otherTransforms[otherI];
            List<int>& procRanks = otherRanks[otherI];

            fromSlave >> procTransforms;
            fromSlave >> procRanks;

            // Size coefficients
            otherBouCoeffs.set
            (
                otherI,
                new FieldField<gpuField, scalar>(procRanks.size())
            );
            otherIntCoeffs.set
            (
                otherI,
                new FieldField<gpuField, scalar>(procRanks.size())
            );
            forAll(procRanks, intI)
            {
                if (procRanks[intI] != -1)
                {
                    otherBouCoeffs[otherI].set
                    (
                        intI,
                        new scalargpuField(fromSlave)
                    );
                    otherIntCoeffs[otherI].set
                    (
                        intI,
                        new scalargpuField(fromSlave)
                    );
                }
            }
        }
    }
    else
    {
        // Send to master

        // Count valid interfaces
        boolList procTransforms(interfaceBouCoeffs.size(), false);
        List<int> procRanks(interfaceBouCoeffs.size(), -1);
        forAll(interfaces, intI)
        {
            if (interfaces.set(intI))
            {
                const processorLduInterfaceField& interface =
                    refCast<const processorLduInterfaceField>
                    (
                        interfaces[intI]
                    );

                procTransforms[intI] = interface.doTransform();
                procRanks[intI] = interface.rank();
            }
        }

        OPstream toMaster
        (
            Pstream::scheduled,
            procIDs[0],
            0,
            Pstream::msgType(),
            meshComm
        );

        toMaster << mat << procTransforms << procRanks;
        forAll(procRanks, intI)
        {
            if (procRanks[intI] != -1)
            {
                toMaster
                    << interfaceBouCoeffs[intI]
                    << interfaceIntCoeffs[intI];
            }
        }
    }
}
*/
// ************************************************************************* //
