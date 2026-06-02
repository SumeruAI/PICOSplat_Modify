/*
  Copyright (c) 2025 PICO Technology Co., Ltd. See LICENSE.md.
*/

#include "CPUSorting.h"

#include <algorithm>

#include "Misc/AssertionMacros.h"
#include "RenderingThread.h"

namespace PICO::Splat
{
void EnqueueCopy(
	std::shared_ptr<FMultithreadedSortingBuffers>& Buffers,
	uint32 NumDrawableSplats)
{
	FRHIBuffer* DstBuffer = nullptr;
	void* Src = nullptr;
	uint32 Size = 0;
	Buffers->BeginCopy(DstBuffer, Src, Size, NumDrawableSplats);

	/**
	 * Buffer is passed in via capture, as it's containing CopyDst may be moved
	 * from when deferring resource destruction. It's guaranteed that the
	 * execution of the destruction occurs *after* this render command, so this is
	 * (probably) safe.
	 */
	ENQUEUE_RENDER_COMMAND(CopyIndices)(
		[BuffersWeakRef = std::weak_ptr<FMultithreadedSortingBuffers>(Buffers),
	     DstBuffer,
	     Size,
	     Src](FRHICommandList& RHICmdList)
		{
			// This command may be executed after the proxy and task have been
			// destroyed. If so, we can skip it.
			std::shared_ptr<FMultithreadedSortingBuffers> Buffers =
				BuffersWeakRef.lock();
			if (!Buffers)
			{
				return;
			}

			void* Dst =
				RHICmdList.LockBuffer(DstBuffer, 0, Size, RLM_WriteOnly);
			memcpy(Dst, Src, Size);
			RHICmdList.UnlockBuffer(DstBuffer);

			Buffers->EndCopy();
		});
}

void FCPUSortingTask::DoWork()
{
	// This command may execute after the associated proxy is destroyed.
	std::shared_ptr<FMultithreadedSortingBuffers> Buffers =
		BuffersWeakRef.lock();
	if (!Buffers)
	{
		return;
	}

	// Acquire buffers.
	FIndexedDistance* Begin = nullptr;
	FIndexedDistance* End = nullptr;
	Buffers->WaitCopy(Begin, End);

	// Calculate distances from current view.
	for (uint32 Index = 0; Index < uint32(PositionsM.Num()); ++Index)
	{
		FVector3f PositionWorldCM(Transform.TransformPosition(
			MetersToCentimeters * PositionsM[Index]));
		Begin[Index] =
			FIndexedDistance(Index, OriginCM, Forward, PositionWorldCM);
	}

	// Partition out some splats not visible.
	FIndexedDistance* VisibleEnd =
		std::partition(Begin, End, FIndexedDistance::IsMaybeVisible);

	// Sort.
	std::sort(Begin, VisibleEnd);

	// Enqueue copy to GPU.
	EnqueueCopy(Buffers, static_cast<uint32>(VisibleEnd - Begin));

	// Cleanup.
	bool bNeedsTearDown = Buffers->EndSorting();
	if (bNeedsTearDown)
	{
		Buffers->ReleaseResources();
	}
}
} // namespace PICO::Splat