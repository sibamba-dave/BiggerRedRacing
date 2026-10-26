#pragma once

#include "CoreMinimal.h"
#include "BRRWorld.h"

#include "scene/level_scene.h"

#include <vector>

namespace BRR::Private
{
	using bigred::TerrainVertex;

	// Groups ORR's flat triangle list into sections by (atlas, sidedness), mapping each vertex to UE
	// with ToUE and reversing winding for the handedness flip.
	template <typename FToUE>
	FBRRMesh ConvertTriangles(const std::vector<TerrainVertex>& Verts, size_t First, size_t Count, FToUE&& ToUE)
	{
		FBRRMesh Mesh;
		TMap<TPair<int32, bool>, int32> SectionOf;
		const size_t End = FMath::Min(First + Count, Verts.size());
		for (size_t T = First; T + 2 < End; T += 3)
		{
			const TerrainVertex& A = Verts[T];
			const int32 Atlas = A.has_texture != 0.0f ? int32(A.layer) : INDEX_NONE;
			const bool bTwoSided = A.cull == 0.0f;
			int32& Index = SectionOf.FindOrAdd({ Atlas, bTwoSided }, INDEX_NONE);
			if (Index == INDEX_NONE)
			{
				Index = Mesh.Sections.Num();
				FBRRMeshSection& New = Mesh.Sections.AddDefaulted_GetRef();
				New.Atlas = Atlas;
				New.bTwoSided = bTwoSided;
			}
			FBRRMeshSection& Section = Mesh.Sections[Index];
			for (const size_t K : { T, T + 2, T + 1 })
			{
				const TerrainVertex& V = Verts[K];
				FBRRVertex& Out = Section.Vertices.AddDefaulted_GetRef();
				Out.Position = FVector3f(ToUE(V.x, V.y, V.z));
				Out.UV = FVector2f(V.u, V.v);
				Out.Color = FLinearColor(V.r, V.g, V.b).ToFColor(false);
			}
		}
		return Mesh;
	}
}
