// A fixed-capacity buffer of translucent triangles for one procedural-mesh section, shared by the
// per-frame effects (dust, wakes, the original's puffs). The section is created ONCE at capacity
// with an identity index buffer and only updated after that, so a frame re-fills the vertex buffer
// instead of tearing the section down and rebuilding its render data - and, with ray tracing on,
// its acceleration structure. Triangles past what a frame used collapse to a point and raster
// nothing.
//
// The unit is a TRIANGLE (the original's effects are triangle fans, not quads): AddQuad spends
// two, AddTri one, and a frame with an odd triangle count loses nothing.
#pragma once

#include "CoreMinimal.h"
#include "ProceduralMeshComponent.h"

struct FBRRCardBuffer
{
	int32 Capacity = 0; // triangles
	int32 Used = 0;     // triangles this frame
	TArray<FVector> Positions;
	TArray<FVector> Normals;
	TArray<FVector2D> UVs;
	TArray<FColor> Colours;

	// Creates the section at capacity, all triangles collapsed. Call once (per section).
	void Create(UProceduralMeshComponent* Mesh, int32 Section, int32 InCapacityTriangles)
	{
		Capacity = InCapacityTriangles;
		Positions.Init(FVector::ZeroVector, Capacity * 3);
		Normals.Init(FVector::UpVector, Capacity * 3);
		UVs.Init(FVector2D::ZeroVector, Capacity * 3);
		Colours.Init(FColor(0, 0, 0, 0), Capacity * 3);
		TArray<int32> Triangles;
		Triangles.Reserve(Capacity * 3);
		for (int32 I = 0; I < Capacity * 3; ++I) Triangles.Add(I);
		Mesh->CreateMeshSection(Section, Positions, Triangles, Normals, UVs, Colours, TArray<FProcMeshTangent>(), false);
	}

	void Begin() { Used = 0; }

	// A quad A0 A1 B0 B1 (triangles A0 A1 B0, A1 B1 B0), one UV and colour per corner.
	// False once the capacity is spent: the quad is dropped whole.
	bool AddQuad(const FVector& A0, const FVector& A1, const FVector& B0, const FVector& B1,
		const FVector2D& UvA0, const FVector2D& UvA1, const FVector2D& UvB0, const FVector2D& UvB1,
		const FColor& CA, const FColor& CB, const FVector& Normal)
	{
		if (Used + 2 > Capacity) return false;
		const int32 V = Used * 3;
		Used += 2;
		Positions[V] = A0; Positions[V + 1] = A1; Positions[V + 2] = B0;
		Positions[V + 3] = A1; Positions[V + 4] = B1; Positions[V + 5] = B0;
		UVs[V] = UvA0; UVs[V + 1] = UvA1; UVs[V + 2] = UvB0;
		UVs[V + 3] = UvA1; UVs[V + 4] = UvB1; UVs[V + 5] = UvB0;
		Colours[V] = CA; Colours[V + 1] = CA; Colours[V + 2] = CB;
		Colours[V + 3] = CA; Colours[V + 4] = CB; Colours[V + 5] = CB;
		for (int32 I = 0; I < 6; ++I) Normals[V + I] = Normal;
		return true;
	}

	// One raw triangle, as the original's particle meshes come.
	bool AddTri(const FVector* P, const FVector2D* Uv, const FColor* C, const FVector& Normal)
	{
		if (Used >= Capacity) return false;
		const int32 V = Used++ * 3;
		for (int32 I = 0; I < 3; ++I)
		{
			Positions[V + I] = P[I];
			UVs[V + I] = Uv[I];
			Colours[V + I] = C[I];
			Normals[V + I] = Normal;
		}
		return true;
	}

	// Collapses the unused tail and sends the frame's triangles to the section. An empty frame
	// after an empty frame sends nothing.
	void Commit(UProceduralMeshComponent* Mesh, int32 Section)
	{
		if (Used == 0 && LastUsed == 0) return;
		// The tail collapses to the last live point, not the origin, so the section's bounds stay
		// on the live set instead of stretching to the actor.
		const FVector At = Used > 0 ? Positions[Used * 3 - 1] : FVector::ZeroVector;
		for (int32 V = Used * 3; V < LastUsed * 3; ++V) Positions[V] = At;
		LastUsed = Used;
		Mesh->UpdateMeshSection(Section, Positions, Normals, UVs, Colours, TArray<FProcMeshTangent>());
	}

private:
	int32 LastUsed = 0; // triangles the previous frame wrote, so only the stale tail is re-collapsed
};
