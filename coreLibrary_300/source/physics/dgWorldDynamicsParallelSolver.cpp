/* Copyright (c) <2003-2011> <Julio Jerez, Newton Game Dynamics>
* 
* This software is provided 'as-is', without any express or implied
* warranty. In no event will the authors be held liable for any damages
* arising from the use of this software.
* 
* Permission is granted to anyone to use this software for any purpose,
* including commercial applications, and to alter it and redistribute it
* freely, subject to the following restrictions:
* 
* 1. The origin of this software must not be misrepresented; you must not
* claim that you wrote the original software. If you use this software
* in a product, an acknowledgment in the product documentation would be
* appreciated but is not required.
* 
* 2. Altered source versions must be plainly marked as such, and must not be
* misrepresented as being the original software.
* 
* 3. This notice may not be removed or altered from any source distribution.
*/

#include "dgPhysicsStdafx.h"

#include "dgBody.h"
#include "dgWorld.h"
#include "dgConstraint.h"
#include "dgDynamicBody.h"
#include "dgWorldDynamicUpdate.h"

#define dgParalletSolverStackPoolSize (1<<8)

void dgWorldDynamicUpdate::CalculateReactionForcesParallel (const dgIsland* const island, dgFloat32 timestep) const
{
	dgParallelSolverSyncData syncData;

	dgWorld* const world = (dgWorld*) this;

	world->m_pairMemoryBuffer.ExpandCapacityIfNeessesary (island->m_jointCount + 1024, sizeof (dgInt32));
	syncData.m_bodyLocks = (dgInt32*) (&world->m_pairMemoryBuffer[0]);
	memset (syncData.m_bodyLocks, 0, island->m_bodyCount * sizeof (syncData.m_bodyLocks[0]));

	dgInt32 bodyCount = island->m_bodyCount - 1;
	dgInt32 jointsCount = island->m_jointCount;
	dgJointInfo* const constraintArrayPtr = (dgJointInfo*) &world->m_jointsMemory[0];
	dgJointInfo* const constraintArray = &constraintArrayPtr[island->m_jointStart];

	dgInt32 index = island->m_jointStart;
	for (dgInt32 i = 0; i < jointsCount; i ++) {
		dgConstraint* const joint = constraintArray[i].m_joint;
		joint->m_index = index;
		index ++;
	}
	
	dgJacobian* const internalForces = &world->m_solverMemory.m_internalForces[0];
	internalForces[0].m_linear = dgVector::m_zero;
	internalForces[0].m_angular = dgVector::m_zero;

	const dgInt32 maxPasses = dgInt32 (world->m_solverMode + LINEAR_SOLVER_SUB_STEPS);
	syncData.m_timestep = timestep;
	syncData.m_invTimestep = (timestep > dgFloat32 (0.0f)) ? dgFloat32 (1.0f) / timestep : dgFloat32 (0.0f);
	syncData.m_invStepRK = (dgFloat32 (1.0f) / dgFloat32 (maxPasses));
	syncData.m_timestepRK = syncData.m_timestep * syncData.m_invStepRK;
	syncData.m_invTimestepRK = syncData.m_invTimestep * dgFloat32 (maxPasses);
	syncData.m_maxPasses = maxPasses;

	syncData.m_bodyCount = bodyCount;
	syncData.m_jointCount = jointsCount;
	syncData.m_atomicIndex = 0;
	syncData.m_island = island;

	InitilizeBodyArrayParallel (&syncData);
	BuildJacobianMatrixParallel (&syncData);
	SolverInitInternalForcesParallel (&syncData);
	CalculateForcesGameModeParallel (&syncData);
	IntegrateInslandParallel(&syncData); 
}

void dgWorldDynamicUpdate::InitilizeBodyArrayParallel (dgParallelSolverSyncData* const syncData) const
{
	dgWorld* const world = (dgWorld*) this;
	const dgInt32 threadCounts = world->GetThreadCount();	

	dgJacobian* const internalForces = &world->m_solverMemory.m_internalForces[0];
	internalForces[0].m_linear = dgVector(dgFloat32(0.0f));
	internalForces[0].m_angular = dgVector(dgFloat32(0.0f));

	syncData->m_atomicIndex = 0;
	for (dgInt32 i = 0; i < threadCounts; i ++) {
		world->QueueJob (InitializeBodyArrayParallelKernel, syncData, world);
	}
	world->SynchronizationBarrier();
}

void dgWorldDynamicUpdate::InitializeBodyArrayParallelKernel (void* const context, void* const worldContext, dgInt32 threadID)
{
	dgParallelSolverSyncData* const syncData = (dgParallelSolverSyncData*) context;
	dgWorld* const world = (dgWorld*) worldContext;
	dgInt32* const atomicIndex = &syncData->m_atomicIndex; 

	const dgIsland* const island = syncData->m_island;
	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
	dgBodyInfo* const bodyArray = &bodyArrayPtr[island->m_bodyStart];
	dgJacobian* const internalForces = &world->m_solverMemory.m_internalForces[0];

	if (syncData->m_timestep != dgFloat32 (0.0f)) {
		for (dgInt32 i = dgAtomicExchangeAndAdd(atomicIndex, 1); i < syncData->m_bodyCount; i = dgAtomicExchangeAndAdd(atomicIndex, 1)) {
			dgAssert (bodyArray[0].m_body->IsRTTIType (dgBody::m_dynamicBodyRTTI) || (((dgDynamicBody*)bodyArray[0].m_body)->m_accel % ((dgDynamicBody*)bodyArray[0].m_body)->m_accel) == dgFloat32 (0.0f));
			dgAssert (bodyArray[0].m_body->IsRTTIType (dgBody::m_dynamicBodyRTTI) || (((dgDynamicBody*)bodyArray[0].m_body)->m_alpha % ((dgDynamicBody*)bodyArray[0].m_body)->m_alpha) == dgFloat32 (0.0f));

			dgInt32 index = i + 1;
			dgBody* const body = bodyArray[index].m_body;
			if (!body->m_equilibrium) {
				dgAssert (body->m_invMass.m_w > dgFloat32 (0.0f));
				body->AddDampingAcceleration();
				body->CalcInvInertiaMatrix ();
			}

			if (body->m_active) {
				// re use these variables for temp storage 
				body->m_netForce = body->m_veloc;
				body->m_netTorque = body->m_omega;
				internalForces[index].m_linear = dgVector::m_zero;
				internalForces[index].m_angular = dgVector::m_zero;
			}
		}
	} else {
		for (dgInt32 i = dgAtomicExchangeAndAdd(atomicIndex, 1); i < syncData->m_bodyCount; i = dgAtomicExchangeAndAdd(atomicIndex, 1)) {
			dgAssert(bodyArray[0].m_body->IsRTTIType(dgBody::m_dynamicBodyRTTI) || (((dgDynamicBody*)bodyArray[0].m_body)->m_accel % ((dgDynamicBody*)bodyArray[0].m_body)->m_accel) == dgFloat32(0.0f));
			dgAssert(bodyArray[0].m_body->IsRTTIType(dgBody::m_dynamicBodyRTTI) || (((dgDynamicBody*)bodyArray[0].m_body)->m_alpha % ((dgDynamicBody*)bodyArray[0].m_body)->m_alpha) == dgFloat32(0.0f));

			dgInt32 index = i + 1;
			dgBody* const body = bodyArray[index].m_body;
			if (!body->m_equilibrium) {
				dgAssert(body->m_invMass.m_w > dgFloat32(0.0f));
				body->CalcInvInertiaMatrix();
			}

			if (body->m_active) {
				 //re use these variables for temp storage 
				body->m_netForce = body->m_veloc;
				body->m_netTorque = body->m_omega;
				internalForces[index].m_linear = dgVector::m_zero;
				internalForces[index].m_angular = dgVector::m_zero;
			}
		}
	}
}


void dgWorldDynamicUpdate::BuildJacobianMatrixParallel (dgParallelSolverSyncData* const syncData) const
{
	dgWorld* const world = (dgWorld*) this;
	const dgInt32 threadCounts = world->GetThreadCount();	

	syncData->m_atomicIndex = 0;
	for (dgInt32 i = 0; i < threadCounts; i ++) {
		world->QueueJob (BuildJacobianMatrixParallelKernel, syncData, world);
	}
	world->SynchronizationBarrier();
}


void dgWorldDynamicUpdate::BuildJacobianMatrixParallelKernel (void* const context, void* const worldContext, dgInt32 threadID)
{
	dgParallelSolverSyncData* const syncData = (dgParallelSolverSyncData*) context;
	dgWorld* const world = (dgWorld*) worldContext;
	dgInt32* const atomicIndex = &syncData->m_atomicIndex; 
	const dgIsland* const island = syncData->m_island;
	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
	dgBodyInfo* const bodyArray = &bodyArrayPtr[island->m_bodyStart];
	dgJointInfo* const constraintArrayPtr = (dgJointInfo*) &world->m_jointsMemory[0];
	dgJointInfo* const constraintArray = &constraintArrayPtr[island->m_jointStart];
	dgJacobianMatrixElement* const matrixRow = &world->m_solverMemory.m_memory[0];
	dgAssert (syncData->m_jointCount);

	dgContraintDescritor constraintParams;
	constraintParams.m_world = world;
	constraintParams.m_threadIndex = threadID;
	constraintParams.m_timestep = syncData->m_timestep;
	constraintParams.m_invTimestep = syncData->m_invTimestep;

	dgFloat32 forceOrImpulseScale = (syncData->m_timestep > dgFloat32 (0.0f)) ? dgFloat32 (1.0f) : dgFloat32 (0.0f);
	for (dgInt32 jointIndex = dgAtomicExchangeAndAdd(atomicIndex, 1); jointIndex < syncData->m_jointCount;  jointIndex = dgAtomicExchangeAndAdd(atomicIndex, 1)) {
		dgJointInfo* const jointInfo = &constraintArray[jointIndex];
		dgConstraint* const constraint = jointInfo->m_joint;

		dgInt32 rowBase = dgAtomicExchangeAndAdd(&syncData->m_jacobianMatrixRowAtomicIndex, jointInfo->m_pairCount);

		world->GetJacobianDerivatives(constraintParams, jointInfo, constraint, matrixRow, rowBase);

		dgAssert (jointInfo->m_m0 >= 0);
		dgAssert (jointInfo->m_m1 >= 0);
		dgAssert (jointInfo->m_m0 != jointInfo->m_m1);
		dgAssert (jointInfo->m_m0 < island->m_bodyCount);
		dgAssert (jointInfo->m_m1 < island->m_bodyCount);
		world->BuildJacobianMatrix (bodyArray, jointInfo, matrixRow, forceOrImpulseScale);
	}
}


void dgWorldDynamicUpdate::SolverInitInternalForcesParallel (dgParallelSolverSyncData* const syncData) const
{
	dgWorld* const world = (dgWorld*) this;
	const dgInt32 threadCounts = world->GetThreadCount();	

	syncData->m_atomicIndex = 0;
	for (dgInt32 i = 0; i < threadCounts; i ++) {
		world->QueueJob (SolverInitInternalForcesParallelKernel, syncData, world);
	}
	world->SynchronizationBarrier();
}


void dgWorldDynamicUpdate::SolverInitInternalForcesParallelKernel (void* const context, void* const worldContext, dgInt32 threadID)
{
	dgParallelSolverSyncData* const syncData = (dgParallelSolverSyncData*) context;
	dgWorld* const world = (dgWorld*) worldContext;
	const dgIsland* const island = syncData->m_island;
	dgJacobian* const internalForces = &world->m_solverMemory.m_internalForces[0];
	dgJacobianMatrixElement* const matrixRow = &world->m_solverMemory.m_memory[0];
	dgInt32* const atomicIndex = &syncData->m_atomicIndex; 
	dgJointInfo* const constraintArrayPtr = (dgJointInfo*) &world->m_jointsMemory[0];
	dgJointInfo* const constraintArray = &constraintArrayPtr[island->m_jointStart];

	for (dgInt32 i = dgAtomicExchangeAndAdd(atomicIndex, 1); i < syncData->m_jointCount;  i = dgAtomicExchangeAndAdd(atomicIndex, 1)) {
		dgJointInfo* const jointInfo = &constraintArray[i];
		if (jointInfo->m_joint->m_solverActive) {
			dgJacobian y0;
			dgJacobian y1;
			world->InitJointForce (jointInfo, matrixRow, y0, y1);
			const dgInt32 m0 = jointInfo->m_m0;
			const dgInt32 m1 = jointInfo->m_m1;
			dgAssert (m0 != m1);
			dgSpinLock(&syncData->m_lock, false);
				internalForces[m0].m_linear += y0.m_linear;
				internalForces[m0].m_angular += y0.m_angular;
				internalForces[m1].m_linear += y1.m_linear;
				internalForces[m1].m_angular += y1.m_angular;
			dgSpinUnlock(&syncData->m_lock);
		}
	}
}



void dgWorldDynamicUpdate::CalculateJointsAccelParallelKernel (void* const context, void* const worldContext, dgInt32 threadID)
{
	dgParallelSolverSyncData* const syncData = (dgParallelSolverSyncData*) context;
	dgWorld* const world = (dgWorld*) worldContext;
	const dgIsland* const island = syncData->m_island;
	dgJointInfo* const constraintArrayPtr = (dgJointInfo*) &world->m_jointsMemory[0];
	dgJointInfo* const constraintArray = &constraintArrayPtr[island->m_jointStart];
	dgJacobianMatrixElement* const matrixRow = &world->m_solverMemory.m_memory[0];

	dgJointAccelerationDecriptor joindDesc;
	joindDesc.m_timeStep = syncData->m_timestepRK;
	joindDesc.m_invTimeStep = syncData->m_invTimestepRK;
	joindDesc.m_firstPassCoefFlag = syncData->m_firstPassCoef;

	dgInt32* const atomicIndex = &syncData->m_atomicIndex; 
	if (joindDesc.m_firstPassCoefFlag == dgFloat32 (0.0f)) {
		for (dgInt32 curJoint = dgAtomicExchangeAndAdd(atomicIndex, 1); curJoint < syncData->m_jointCount;  curJoint = dgAtomicExchangeAndAdd(atomicIndex, 1)) {
			dgJointInfo* const jointInfo = &constraintArray[curJoint];
			dgConstraint* const constraint = jointInfo->m_joint;
			if (constraint->m_solverActive) {
				joindDesc.m_rowsCount = jointInfo->m_pairCount;
				joindDesc.m_rowMatrix = &matrixRow[jointInfo->m_pairStart];
				constraint->JointAccelerations(&joindDesc);
			}
		}
	} else {
		const dgIsland* const island = syncData->m_island;
		dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
		const dgBodyInfo* const bodyArray = &bodyArrayPtr[island->m_bodyStart];

		for (dgInt32 curJoint = dgAtomicExchangeAndAdd(atomicIndex, 1); curJoint < syncData->m_jointCount;  curJoint = dgAtomicExchangeAndAdd(atomicIndex, 1)) {
			dgJointInfo* const jointInfo = &constraintArray[curJoint];
			dgConstraint* const constraint = jointInfo->m_joint;
			if (constraint->m_solverActive) {
				const dgInt32 m0 = jointInfo->m_m0;
				const dgInt32 m1 = jointInfo->m_m1;
				const dgBody* const body0 = bodyArray[m0].m_body;
				const dgBody* const body1 = bodyArray[m1].m_body;
				if (!(body0->m_resting & body1->m_resting)) {
					joindDesc.m_rowsCount = jointInfo->m_pairCount;
					joindDesc.m_rowMatrix = &matrixRow[jointInfo->m_pairStart];
					constraint->JointAccelerations(&joindDesc);
				}
			}
		}
	}
}


void dgWorldDynamicUpdate::CalculateJointsVelocParallelKernel (void* const context, void* const worldContext, dgInt32 threadID)
{
	dgParallelSolverSyncData* const syncData = (dgParallelSolverSyncData*) context;
	dgWorld* const world = (dgWorld*) worldContext;

	const dgIsland* const island = syncData->m_island;
	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
	dgBodyInfo* const bodyArray = &bodyArrayPtr[island->m_bodyStart];

	dgJacobian* const internalForces = &world->m_solverMemory.m_internalForces[0];

	dgVector speedFreeze2 (world->m_freezeSpeed2 * dgFloat32 (0.1f));
	dgVector freezeOmega2 (world->m_freezeOmega2 * dgFloat32 (0.1f));
	//dgVector forceActiveMask ((syncData->m_jointCount <= DG_SMALL_ISLAND_COUNT) ?  dgFloat32 (-1.0f): dgFloat32 (0.0f));
	dgVector forceActiveMask ((syncData->m_jointCount <= DG_SMALL_ISLAND_COUNT) ?  dgVector (-1, -1, -1, -1) : dgFloat32 (0.0f));
	dgInt32* const atomicIndex = &syncData->m_atomicIndex;

	if (syncData->m_timestepRK != dgFloat32 (0.0f)) {
		dgVector timestep4 (syncData->m_timestepRK);
		for (dgInt32 i = dgAtomicExchangeAndAdd(atomicIndex, 1); i < syncData->m_bodyCount;  i = dgAtomicExchangeAndAdd(atomicIndex, 1)) {
			dgDynamicBody* const body = (dgDynamicBody*) bodyArray[i].m_body;
			dgAssert (body->m_index == i);
			if (body->m_active) {
				dgVector force(internalForces[i].m_linear);
				dgVector torque(internalForces[i].m_angular);
				if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
					force += body->m_accel;
					torque += body->m_alpha;
				}

				dgVector velocStep((force.Scale4(body->m_invMass.m_w)).CompProduct4(timestep4));
				dgVector omegaStep((body->m_invWorldInertiaMatrix.RotateVector(torque)).CompProduct4(timestep4));
				if (!body->m_resting) {
					body->m_veloc += velocStep;
					body->m_omega += omegaStep;
				} else {
					dgVector velocStep2(velocStep.DotProduct4(velocStep));
					dgVector omegaStep2(omegaStep.DotProduct4(omegaStep));
					dgVector test((velocStep2 > speedFreeze2) | (omegaStep2 > speedFreeze2) | forceActiveMask);
					if (test.GetSignMask()) {
						body->m_resting = false;
					}
				}
			}
		}
	} else {
		for (dgInt32 i = dgAtomicExchangeAndAdd(atomicIndex, 1); i < syncData->m_bodyCount;  i = dgAtomicExchangeAndAdd(atomicIndex, 1)) {
			dgBody* const body = bodyArray[i].m_body;
			if (body->m_active) {
				const dgVector& linearMomentum = internalForces[i].m_linear;
				const dgVector& angularMomentum = internalForces[i].m_angular;

				body->m_veloc += linearMomentum.Scale4(body->m_invMass.m_w);
				body->m_omega += body->m_invWorldInertiaMatrix.RotateVector(angularMomentum);
			}
		}
	}
}

/*
void dgWorldDynamicUpdate::CalculateJointsImpulseVelocParallelKernel (void* const context, void* const worldContext, dgInt32 threadID)
{
	dgParallelSolverSyncData* const syncData = (dgParallelSolverSyncData*) context;
	dgWorld* const world = (dgWorld*) worldContext;

	const dgIsland* const island = syncData->m_island;
	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
	dgBodyInfo* const bodyArray = &bodyArrayPtr[island->m_bodyStart];

	dgJacobian* const internalForces = &world->m_solverMemory.m_internalForces[0];
	//dgJacobian* const internalVeloc = &world->m_solverMemory.m_internalVeloc[0];

	dgInt32* const atomicIndex = &syncData->m_atomicIndex;
	for (dgInt32 i = dgAtomicExchangeAndAdd(atomicIndex, 1); i < syncData->m_bodyCount;  i = dgAtomicExchangeAndAdd(atomicIndex, 1)) {
		dgInt32 index = i + 1;
		dgAssert (index);
		dgDynamicBody* const body = (dgDynamicBody*) bodyArray[index].m_body;
		dgAssert (body->m_index == index);

		const dgVector& linearMomentum = internalForces[index].m_linear;
		const dgVector& angularMomentum = internalForces[index].m_angular;

		body->m_veloc += linearMomentum.Scale4(body->m_invMass.m_w);
		body->m_omega += body->m_invWorldInertiaMatrix.RotateVector (angularMomentum);

		//internalVeloc[index].m_linear += body->m_veloc;
		//internalVeloc[index].m_angular += body->m_omega;
	}
}
*/

void dgWorldDynamicUpdate::UpdateFeedbackForcesParallelKernel (void* const context, void* const worldContext, dgInt32 threadID)
{
	dgParallelSolverSyncData* const syncData = (dgParallelSolverSyncData*) context;
	dgWorld* const world = (dgWorld*) worldContext;
	const dgIsland* const island = syncData->m_island;
	dgJointInfo* const constraintArrayPtr = (dgJointInfo*) &world->m_jointsMemory[0];
	dgJointInfo* const constraintArray = &constraintArrayPtr[island->m_jointStart];
	dgJacobianMatrixElement* const matrixRow = &world->m_solverMemory.m_memory[0];

	dgInt32 hasJointFeeback = 0;
	dgInt32* const atomicIndex = &syncData->m_atomicIndex;
	
	for (dgInt32 curJoint = dgAtomicExchangeAndAdd(atomicIndex, 1); curJoint < syncData->m_jointCount;  curJoint = dgAtomicExchangeAndAdd(atomicIndex, 1)) {
		dgJointInfo* const jointInfo = &constraintArray[curJoint];
		dgConstraint* const constraint = jointInfo->m_joint;
		if (constraint->m_solverActive) {
			const dgInt32 first = jointInfo->m_pairStart;
			const dgInt32 count = jointInfo->m_pairCount;

			for (dgInt32 j = 0; j < count; j++) {
				dgJacobianMatrixElement* const row = &matrixRow[j + first];
				dgFloat32 val = row->m_force;
				dgAssert(dgCheckFloat(val));
				row->m_jointFeebackForce[0].m_force = val;
				row->m_jointFeebackForce[0].m_impact = row->m_maxImpact * syncData->m_timestepRK;
			}
			hasJointFeeback |= (constraint->m_updaFeedbackCallback ? 1 : 0);
		}
	}
	syncData->m_hasJointFeeback[threadID] = hasJointFeeback;
}


void dgWorldDynamicUpdate::UpdateBodyVelocityParallelKernel (void* const context, void* const worldContext, dgInt32 threadID)
{
	dgParallelSolverSyncData* const syncData = (dgParallelSolverSyncData*) context;
	dgWorld* const world = (dgWorld*) worldContext;

	const dgIsland* const island = syncData->m_island;
	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
	dgBodyInfo* const bodyArray = &bodyArrayPtr[island->m_bodyStart];

	dgFloat32 maxAccNorm2 = DG_SOLVER_MAX_ERROR * DG_SOLVER_MAX_ERROR;

	//dgFloat32 invTimestepSrc = dgFloat32 (1.0f) / syncData->m_timestep;
	dgFloat32 invTimestepSrc = syncData->m_invTimestep;

	dgVector invTime (invTimestepSrc);
	dgInt32* const atomicIndex = &syncData->m_atomicIndex;
	dgVector forceActiveMask ((syncData->m_jointCount <= DG_SMALL_ISLAND_COUNT) ?  dgVector (-1, -1, -1, -1) : dgFloat32 (0.0f));
	for (dgInt32 i = dgAtomicExchangeAndAdd(atomicIndex, 1); i < syncData->m_bodyCount; i = dgAtomicExchangeAndAdd(atomicIndex, 1)) {
		dgDynamicBody* const body = (dgDynamicBody*) bodyArray[i].m_body;
		if (body->m_active) {
			// the initial velocity and angular velocity were stored in net force and net torque, for memory saving
			dgVector accel = (body->m_veloc - body->m_netForce).CompProduct4(invTime);
			dgVector alpha = (body->m_omega - body->m_netTorque).CompProduct4(invTime);
			dgVector accelTest((accel.DotProduct4(accel) > maxAccNorm2) | (alpha.DotProduct4(alpha) > maxAccNorm2) | forceActiveMask);
			//if ((accel % accel) < maxAccNorm2) {
			//	accel = dgVector::m_zero;
			//}
			//if ((alpha % alpha) < maxAccNorm2) {
			//	alpha = dgVector::m_zero;
			//}
			accel = accel & accelTest;
			alpha = alpha & accelTest;

			if (body->IsRTTIType(dgBody::m_dynamicBodyRTTI)) {
				body->m_accel = accel;
				body->m_alpha = alpha;
			}
			body->m_netForce = accel.Scale4(body->m_mass[3]);

			alpha = body->m_matrix.UnrotateVector(alpha);
			body->m_netTorque = body->m_matrix.RotateVector(alpha.CompProduct4(body->m_mass));
		}
	}
}


void dgWorldDynamicUpdate::KinematicCallbackUpdateParallelKernel (void* const context, void* const worldContext, dgInt32 threadID)
{
	dgParallelSolverSyncData* const syncData = (dgParallelSolverSyncData*) context;
	dgWorld* const world = (dgWorld*) worldContext;
	const dgIsland* const island = syncData->m_island;
	dgJointInfo* const constraintArrayPtr = (dgJointInfo*) &world->m_jointsMemory[0];
	dgJointInfo* const constraintArray = &constraintArrayPtr[island->m_jointStart];

	dgInt32* const atomicIndex = &syncData->m_atomicIndex;
	for (dgInt32 i = dgAtomicExchangeAndAdd(atomicIndex, 1); i < syncData->m_jointCount;  i = dgAtomicExchangeAndAdd(atomicIndex, 1)) {
		dgInt32 curJoint = i;
		if (constraintArray[curJoint].m_joint->m_updaFeedbackCallback) {
			constraintArray[curJoint].m_joint->m_updaFeedbackCallback (*constraintArray[curJoint].m_joint, syncData->m_timestep, threadID);
		}
	}
}


void dgWorldDynamicUpdate::IntegrateInslandParallel(dgParallelSolverSyncData* const syncData) const
{
	dgWorld* const world = (dgWorld*) this;
//	dgWorldDynamicUpdate::IntegrateInslandParallelKernel (syncData, world, 0);
	world->IntegrateArray (syncData->m_island, DG_SOLVER_MAX_ERROR, syncData->m_timestep, 0); 
}


void dgWorldDynamicUpdate::CalculateForcesGameModeParallel (dgParallelSolverSyncData* const syncData) const
{
	dgWorld* const world = (dgWorld*) this;
	const dgInt32 threadCounts = world->GetThreadCount();	

	dgInt32 maxPasses = syncData->m_maxPasses;
	syncData->m_firstPassCoef = dgFloat32 (0.0f);
	for (dgInt32 step = 0; step < maxPasses; step ++) {

		syncData->m_atomicIndex = 0;
		for (dgInt32 i = 0; i < threadCounts; i ++) {
			world->QueueJob (CalculateJointsAccelParallelKernel, syncData, world);
		}
		world->SynchronizationBarrier();
		syncData->m_firstPassCoef = dgFloat32 (1.0f);

		dgFloat32 accNorm = DG_SOLVER_MAX_ERROR * dgFloat32 (2.0f);
		for (dgInt32 passes = 0; (passes < DG_BASE_ITERATION_COUNT) && (accNorm > DG_SOLVER_MAX_ERROR); passes ++) {
			for (dgInt32 i = 0; i < threadCounts; i ++) {
				syncData->m_accelNorm[i] = dgVector (dgFloat32 (0.0f));
			}
			syncData->m_atomicIndex = 0;
			for (dgInt32 i = 0; i < threadCounts; i ++) {
				world->QueueJob (CalculateJointsForceParallelKernel, syncData, world);
			}
			world->SynchronizationBarrier();

			accNorm = dgFloat32 (0.0f);
			for (dgInt32 i = 0; i < threadCounts; i ++) {
				accNorm = dgMax (accNorm, syncData->m_accelNorm[i].m_x);
			}
		}

		syncData->m_atomicIndex = 1;
		for (dgInt32 j = 0; j < threadCounts; j ++) {
			world->QueueJob (CalculateJointsVelocParallelKernel, syncData, world);
		}
		world->SynchronizationBarrier();
	}

	if (syncData->m_timestepRK != dgFloat32 (0.0f)) {
		syncData->m_atomicIndex = 0;
		for (dgInt32 j = 0; j < threadCounts; j ++) {
			world->QueueJob (UpdateFeedbackForcesParallelKernel, syncData, world);
		}
		world->SynchronizationBarrier();

		dgInt32 hasJointFeeback = 0;
		for (dgInt32 i = 0; i < DG_MAX_THREADS_HIVE_COUNT; i ++) {
			hasJointFeeback |= syncData->m_hasJointFeeback[i];
		}

		syncData->m_atomicIndex = 1;
		for (dgInt32 j = 0; j < threadCounts; j++) {
			world->QueueJob(UpdateBodyVelocityParallelKernel, syncData, world);
		}
		world->SynchronizationBarrier();

		if (hasJointFeeback) {
			syncData->m_atomicIndex = 0;
			for (dgInt32 j = 0; j < threadCounts; j++) {
				world->QueueJob(KinematicCallbackUpdateParallelKernel, syncData, world);
			}
			world->SynchronizationBarrier();
		}

	} else {
		const dgInt32 count = syncData->m_bodyCount;
		const dgIsland* const island = syncData->m_island;
		dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*)&world->m_bodiesMemory[0];
		dgBodyInfo* const bodyArray = &bodyArrayPtr[island->m_bodyStart];
		for (dgInt32 i = 1; i < count; i++) {
			dgBody* const body = bodyArray[i].m_body;
			if (body->m_active) {
				body->m_netForce = dgVector::m_zero;
				body->m_netTorque = dgVector::m_zero;
			}
		}
	}
}


void dgWorldDynamicUpdate::CalculateJointsForceParallelKernel (void* const context, void* const worldContext, dgInt32 threadID)
{
	dgParallelSolverSyncData* const syncData = (dgParallelSolverSyncData*) context;
	dgWorld* const world = (dgWorld*) worldContext;

	const dgIsland* const island = syncData->m_island;
	dgJacobianMatrixElement* const matrixRow = &world->m_solverMemory.m_memory[0];
	dgBodyInfo* const bodyArrayPtr = (dgBodyInfo*) &world->m_bodiesMemory[0]; 
	const dgBodyInfo* const bodyArray = &bodyArrayPtr[island->m_bodyStart];
	dgJointInfo* const constraintArrayPtr = (dgJointInfo*) &world->m_jointsMemory[0];
	dgJointInfo* const constraintArray = &constraintArrayPtr[island->m_jointStart];
	dgJacobian* const internalForces = &world->m_solverMemory.m_internalForces[0];
	const int jointCount = syncData->m_jointCount;
	dgInt32* const globalLock  = &syncData->m_lock;
	

	dgInt32 pool [dgParalletSolverStackPoolSize];
	dgQueue<dgInt32> queue(pool, dgParalletSolverStackPoolSize);

	dgInt32* const atomicIndex = &syncData->m_atomicIndex;
	dgVector accNorm (syncData->m_accelNorm[threadID]);

#if 1
	dgInt32* const bodyLocks = syncData->m_bodyLocks;
	queue.Insert(dgAtomicExchangeAndAdd(atomicIndex, 1));
	while (!queue.IsEmpty()) {
		dgInt32 jointIndex = queue.Remove();
		if (jointIndex < jointCount) {
			dgSpinLock(globalLock, false);
			dgInt32 m0 = constraintArray[jointIndex].m_m0;
			dgInt32 m1 = constraintArray[jointIndex].m_m1;
			dgInt32 test0 = syncData->m_bodyLocks[m0];
			dgInt32 test1 = syncData->m_bodyLocks[m1];
			while (test0 | test1) {
				queue.Insert(jointIndex);
				jointIndex = dgAtomicExchangeAndAdd(atomicIndex, 1);
				if (jointIndex < jointCount) {
					m0 = constraintArray[jointIndex].m_m0;
					m1 = constraintArray[jointIndex].m_m1;
					test0 = syncData->m_bodyLocks[m0];
					test1 = syncData->m_bodyLocks[m1];
				} else {
					m0 = 0;
					m1 = 0;
					test0 = 0;
					test1 = 0;
				}
			}
			bodyLocks[m0] = m0 > 0;
			bodyLocks[m1] = m1 > 0;
			if (queue.IsEmpty()) {
				queue.Insert(dgAtomicExchangeAndAdd(atomicIndex, 1));
			}
			dgSpinUnlock(globalLock);
			if (m0 != m1) {
				dgJointInfo* const jointInfo = &constraintArray[jointIndex];
				dgAssert(jointInfo->m_m0 == m0);
				dgAssert(jointInfo->m_m1 == m1);

				world->CalculateJointForce (jointInfo, bodyArray, internalForces, matrixRow, accNorm);

				dgSpinUnlock(&bodyLocks[m0]);
				dgSpinUnlock(&bodyLocks[m1]);
			}
		}
	}

#else

	for (dgInt32 jointIndex = dgAtomicExchangeAndAdd(atomicIndex, 1); jointIndex < jointCount;  jointIndex = dgAtomicExchangeAndAdd(atomicIndex, 1)) {

		dgSpinLock(globalLock, false);
		dgInt32 m0 = constraintArray[jointIndex].m_m0;
		dgInt32 m1 = constraintArray[jointIndex].m_m1;
//			dgInt32 test0;
//			dgInt32 test1;
//			do {
//				test0 = bodyLocks[m0];
//				test1 = bodyLocks[m1];
//			} while (test0 | test1);
//			bodyLocks[m0] = m0 > 0;
//			bodyLocks[m1] = m1 > 0;
			
		const dgBody* const body0 = bodyArray[m0].m_body;
		const dgBody* const body1 = bodyArray[m1].m_body;

		if (!(body0->m_resting & body1->m_resting)) {
			dgInt32 index = constraintArray[jointIndex].m_pairStart;
			dgInt32 rowsCount = constraintArray[jointIndex].m_pairCount;

			dgVector linearM0 (internalForces[m0].m_linear);
			dgVector angularM0 (internalForces[m0].m_angular);
			dgVector linearM1 (internalForces[m1].m_linear);
			dgVector angularM1 (internalForces[m1].m_angular);

			const dgVector invMass0 (body0->m_invMass[3]);
			const dgMatrix& invInertia0 = body0->m_invWorldInertiaMatrix;

			const dgVector invMass1 (body1->m_invMass[3]);
			const dgMatrix& invInertia1 = body1->m_invWorldInertiaMatrix;

			for (dgInt32 k = 0; k < rowsCount; k ++) {
				dgJacobianMatrixElement* const row = &matrixRow[index];

				dgAssert (row->m_Jt.m_jacobianM0.m_linear.m_w == dgFloat32 (0.0f));
				dgAssert (row->m_Jt.m_jacobianM0.m_angular.m_w == dgFloat32 (0.0f));
				dgAssert (row->m_Jt.m_jacobianM1.m_linear.m_w == dgFloat32 (0.0f));
				dgAssert (row->m_Jt.m_jacobianM1.m_angular.m_w == dgFloat32 (0.0f));

				//dgVector JMinvJacobianLinearM0 (row->m_Jt.m_jacobianM0.m_linear.Scale3 (invMass0));
				//dgVector JMinvJacobianAngularM0 (invInertia0.UnrotateVector (row->m_Jt.m_jacobianM0.m_angular));
				//dgVector JMinvJacobianLinearM1 (row->m_Jt.m_jacobianM1.m_linear.Scale3 (invMass1));
				//dgVector JMinvJacobianAngularM1 (invInertia1.UnrotateVector (row->m_Jt.m_jacobianM1.m_angular));

				dgVector JMinvJacobianLinearM0 (row->m_Jt.m_jacobianM0.m_linear.CompProduct4 (invMass0));
				dgVector JMinvJacobianAngularM0 (invInertia0.RotateVector (row->m_Jt.m_jacobianM0.m_angular));
				dgVector JMinvJacobianLinearM1 (row->m_Jt.m_jacobianM1.m_linear.CompProduct4 (invMass1));
				dgVector JMinvJacobianAngularM1 (invInertia1.RotateVector (row->m_Jt.m_jacobianM1.m_angular));

				dgVector a (JMinvJacobianLinearM0.CompProduct4(linearM0) + JMinvJacobianAngularM0.CompProduct4(angularM0) + JMinvJacobianLinearM1.CompProduct4(linearM1) + JMinvJacobianAngularM1.CompProduct4(angularM1));

				//dgFloat32 a = row->m_coordenateAccel - acc.m_x - acc.m_y - acc.m_z - row->m_force * row->m_diagDamp;
				a = dgVector (row->m_coordenateAccel  - row->m_force * row->m_diagDamp) - a.AddHorizontal();

				//dgFloat32 f = row->m_force + row->m_invDJMinvJt * a;
				dgVector f (row->m_force + row->m_invDJMinvJt * a.m_x);

				dgInt32 frictionIndex = row->m_normalForceIndex;
				dgAssert (((frictionIndex < 0) && (normalForce[frictionIndex] == dgFloat32 (1.0f))) || ((frictionIndex >= 0) && (normalForce[frictionIndex] >= dgFloat32 (0.0f))));

				dgFloat32 frictionNormal = normalForce[frictionIndex];
				dgVector lowerFrictionForce = (frictionNormal * row->m_lowerBoundFrictionCoefficent);
				dgVector upperFrictionForce = (frictionNormal * row->m_upperBoundFrictionCoefficent);

				//if (f > upperFrictionForce) {
				//	a = dgFloat32 (0.0f);
				//	f = upperFrictionForce;
				//} else if (f < lowerFrictionForce) {
				//	a = dgFloat32 (0.0f);
				//	f = lowerFrictionForce;
				//}
				a = a.AndNot((f > upperFrictionForce) | (f < lowerFrictionForce));
				f = f.GetMax(lowerFrictionForce).GetMin(upperFrictionForce);

				accNorm = accNorm.GetMax(a.Abs());
				dgAssert (accNorm.m_x >= dgAbsf (a.m_x));

				// no early out for parallel solver
				//accNorm = accNorm.GetMax(a.Abs());
				//dgAssert (accNorm.m_x >= dgAbsf (a.m_x));

				//dgFloat32 prevValue = f - row->m_force;
				dgVector prevValue (f - dgVector (row->m_force));

				row->m_force = f.m_x;
				normalForce[k] = f.m_x;
				row->m_maxImpact = f.Abs().GetMax (row->m_maxImpact).m_x;

				linearM0 += row->m_Jt.m_jacobianM0.m_linear.CompProduct4 (prevValue);
				angularM0 += row->m_Jt.m_jacobianM0.m_angular.CompProduct4 (prevValue);
				linearM1 += row->m_Jt.m_jacobianM1.m_linear.CompProduct4 (prevValue);
				angularM1 += row->m_Jt.m_jacobianM1.m_angular.CompProduct4 (prevValue);
				index ++;
			}

			internalForces[m0].m_linear = linearM0;
			internalForces[m0].m_angular = angularM0;

			internalForces[m1].m_linear = linearM1;
			internalForces[m1].m_angular = angularM1;
		}

		//dgSpinUnlock (&bodyLocks[m0]);
		//dgSpinUnlock (&bodyLocks[m1]);

		dgSpinUnlock(globalLock);
	}
#endif

	syncData->m_accelNorm[threadID] = accNorm;
}
