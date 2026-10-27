#pragma once

// Building blocks for the remastered look's grown and rebuilt scenery (BRRTrees, BRRProps): flat
// vertex-coloured triangles, one-sided (outward) and two-sided, tubes, lumpy balls and the like.
#include "CoreMinimal.h"
#include "BRRRefine.h"
#include "BRRWorld.h"
#include "Math/RandomStream.h"

namespace BRRShapes
{
	inline float Hash(int32 X, int32 Y, int32 Z, int32 Seed)
	{
		uint32 H = uint32(X) * 374761393u + uint32(Y) * 668265263u + uint32(Z) * 2147483647u + uint32(Seed) * 144665u;
		H = (H ^ (H >> 13)) * 1274126177u;
		return float(H ^ (H >> 16)) / float(0xFFFFFFFFu);
	}
	// Smooth value noise, 0..1.
	inline float Noise(const FVector3f& P, int32 Seed)
	{
		const int32 X = FMath::FloorToInt32(P.X), Y = FMath::FloorToInt32(P.Y), Z = FMath::FloorToInt32(P.Z);
		FVector3f F(P.X - X, P.Y - Y, P.Z - Z);
		F = F * F * (FVector3f(3.f) - F * 2.f);
		auto L = [](float A, float B, float T) { return A + (B - A) * T; };
		return L(L(L(Hash(X, Y, Z, Seed), Hash(X + 1, Y, Z, Seed), F.X), L(Hash(X, Y + 1, Z, Seed), Hash(X + 1, Y + 1, Z, Seed), F.X), F.Y),
			L(L(Hash(X, Y, Z + 1, Seed), Hash(X + 1, Y, Z + 1, Seed), F.X), L(Hash(X, Y + 1, Z + 1, Seed), Hash(X + 1, Y + 1, Z + 1, Seed), F.X), F.Y), F.Z);
	}

	inline FColor Shade(FColor C, float K)
	{
		return FColor(uint8(FMath::Clamp(C.R * K, 0.f, 255.f)), uint8(FMath::Clamp(C.G * K, 0.f, 255.f)), uint8(FMath::Clamp(C.B * K, 0.f, 255.f)), 255);
	}
	inline FColor Mix(FColor A, FColor B, float T)
	{
		return FColor(uint8(FMath::Lerp(float(A.R), float(B.R), T)), uint8(FMath::Lerp(float(A.G), float(B.G), T)), uint8(FMath::Lerp(float(A.B), float(B.B), T)), 255);
	}

	// A builder: solid (one-sided, outward) and leaf (two-sided) triangles in flat colours.
	struct FBuilder
	{
		FBRRMesh Mesh;
		FRandomStream Rand;
		explicit FBuilder(int32 Seed) : Rand(Seed)
		{
			Mesh.Sections.SetNum(2);
			Mesh.Sections[1].bTwoSided = true;
		}
		void Tri(int32 Section, FVector3f A, FVector3f B, FVector3f C, FColor CA, FColor CB, FColor CC)
		{
			FBRRVertex V[3];
			V[0].Position = A; V[1].Position = B; V[2].Position = C;
			V[0].Color = CA; V[1].Color = CB; V[2].Color = CC;
			Mesh.Sections[Section].Vertices.Append(V, 3);
		}
		// Outward from Inside (MakeMesh's winding: the normal is (C - A) x (B - A)).
		void Solid(FVector3f A, FVector3f B, FVector3f C, FColor CA, FColor CB, FColor CC, const FVector3f& Inside)
		{
			const FVector3f N = (C - A) ^ (B - A);
			if ((N | ((A + B + C) / 3.f - Inside)) < 0) { Swap(B, C); Swap(CB, CC); }
			Tri(0, A, B, C, CA, CB, CC);
		}
		// A tube through Points with Radii, Sides around, bark coloured, closed at the top.
		void Tube(const TArray<FVector3f>& Points, const TArray<float>& Radii, int32 Sides, FColor Bark, float Ribs = 0.f)
		{
			TArray<TArray<FVector3f>> Rings;
			for (int32 I = 0; I < Points.Num(); ++I)
			{
				const FVector3f Dir = (Points[FMath::Min(I + 1, Points.Num() - 1)] - Points[FMath::Max(I - 1, 0)]).GetSafeNormal();
				FVector3f U = (FMath::Abs(Dir.Z) < 0.9f ? FVector3f::UpVector : FVector3f::ForwardVector) ^ Dir;
				U.Normalize();
				const FVector3f W = Dir ^ U;
				TArray<FVector3f>& Ring = Rings.AddDefaulted_GetRef();
				for (int32 S = 0; S < Sides; ++S)
				{
					const float A = 2.f * PI * S / Sides;
					const float Rad = Radii[I] * (1.f - Ribs * (S & 1));
					Ring.Add(Points[I] + (U * FMath::Cos(A) + W * FMath::Sin(A)) * Rad);
				}
			}
			for (int32 I = 0; I + 1 < Rings.Num(); ++I)
			{
				const FVector3f Mid = (Points[I] + Points[I + 1]) * 0.5f;
				const FColor C0 = Shade(Bark, 0.85f + 0.3f * Rand.FRand()), C1 = Shade(Bark, 0.85f + 0.3f * Rand.FRand());
				for (int32 S = 0; S < Sides; ++S)
				{
					const int32 N = (S + 1) % Sides;
					Solid(Rings[I][S], Rings[I][N], Rings[I + 1][N], C0, C0, C1, Mid);
					Solid(Rings[I][S], Rings[I + 1][N], Rings[I + 1][S], C0, C1, C1, Mid);
				}
			}
			const FVector3f Top = Points.Last() + (Points.Last() - Points[Points.Num() - 2]).GetSafeNormal() * Radii.Last() * 0.6f;
			for (int32 S = 0; S < Sides; ++S) Solid(Rings.Last()[S], Rings.Last()[(S + 1) % Sides], Top, Bark, Bark, Bark, Points.Last());
		}
		// A lumpy ball: an icosahedron cut Detail times, pushed in and out by noise, darker beneath.
		void Blob(const FVector3f& Centre, const FVector3f& Radii, FColor Colour, float Lumps, int32 Detail, int32 Seed, float Boxiness = 0.f)
		{
			const float T = (1.f + FMath::Sqrt(5.f)) * 0.5f;
			TArray<FVector3f> P = { {-1,T,0},{1,T,0},{-1,-T,0},{1,-T,0},{0,-1,T},{0,1,T},{0,-1,-T},{0,1,-T},{T,0,-1},{T,0,1},{-T,0,-1},{-T,0,1} };
			for (FVector3f& V : P) V.Normalize();
			TArray<FIntVector> F = { {0,11,5},{0,5,1},{0,1,7},{0,7,10},{0,10,11},{1,5,9},{5,11,4},{11,10,2},{10,7,6},{7,1,8},
				{3,9,4},{3,4,2},{3,2,6},{3,6,8},{3,8,9},{4,9,5},{2,4,11},{6,2,10},{8,6,7},{9,8,1} };
			for (int32 D = 0; D < Detail; ++D)
			{
				TArray<FIntVector> Next;
				TMap<uint64, int32> Mid;
				auto M = [&](int32 A, int32 B)
				{
					const uint64 K = A < B ? (uint64(A) << 32) | uint32(B) : (uint64(B) << 32) | uint32(A);
					if (const int32* Found = Mid.Find(K)) return *Found;
					return Mid.Add(K, P.Add((P[A] + P[B]).GetSafeNormal()));
				};
				for (const FIntVector& Face : F)
				{
					const int32 A = M(Face.X, Face.Y), B = M(Face.Y, Face.Z), C = M(Face.Z, Face.X);
					Next.Append({ FIntVector(Face.X, A, C), FIntVector(Face.Y, B, A), FIntVector(Face.Z, C, B), FIntVector(A, B, C) });
				}
				F = MoveTemp(Next);
			}
			TArray<FVector3f> Out;
			TArray<FColor> Cols;
			for (const FVector3f& V : P)
			{
				const float N = Noise(V * 2.2f + FVector3f(Seed % 97), Seed) * 0.65f + Noise(V * 5.f + FVector3f(Seed % 31), Seed + 1) * 0.35f;
				const float K = 1.f + Lumps * (N - 0.5f) * 2.f;
				// Boxiness pulls the ball out towards its cube (a bale, a block of rock).
				const FVector3f Cube = V / FMath::Max3(FMath::Abs(V.X), FMath::Abs(V.Y), FMath::Abs(V.Z));
				Out.Add(Centre + FMath::Lerp(V, Cube, Boxiness) * Radii * K);
				// Lit from above, shaded inside and beneath, a little noise in the leaves.
				Cols.Add(Shade(Colour, FMath::Lerp(0.55f, 1.15f, V.Z * 0.5f + 0.5f) * (0.85f + 0.3f * N)));
			}
			for (const FIntVector& Face : F) Solid(Out[Face.X], Out[Face.Y], Out[Face.Z], Cols[Face.X], Cols[Face.Y], Cols[Face.Z], Centre);
		}
		// A box, its axes Basis (columns: X, Y, Z), half-sized Half, in one colour shaded by face.
		void Box(const FVector3f& Centre, const FVector3f& Half, const FQuat4f& Basis, FColor C)
		{
			FVector3f P[8];
			for (int32 K = 0; K < 8; ++K) P[K] = Centre + Basis.RotateVector(FVector3f((K & 1) ? Half.X : -Half.X, (K & 2) ? Half.Y : -Half.Y, (K & 4) ? Half.Z : -Half.Z));
			const int32 F[6][4] = { {0,1,3,2}, {4,6,7,5}, {0,4,5,1}, {2,3,7,6}, {0,2,6,4}, {1,5,7,3} };
			const float K[6] = { 0.55f, 1.1f, 0.85f, 0.85f, 0.75f, 0.95f }; // beneath darkest, top lightest
			for (int32 Face = 0; Face < 6; ++Face)
			{
				const FColor S = Shade(C, K[Face]);
				Solid(P[F[Face][0]], P[F[Face][1]], P[F[Face][2]], S, S, S, Centre);
				Solid(P[F[Face][0]], P[F[Face][2]], P[F[Face][3]], S, S, S, Centre);
			}
		}
		// A shape turned about the Z axis at Base: Profile (radius, height) from the bottom up, a
		// colour per profile point; closed at the ends where the radius is not zero.
		void Lathe(const FVector3f& Base, const TArray<FVector2f>& Profile, const TArray<FColor>& Colours, int32 Sides)
		{
			const FVector3f Inside = Base + FVector3f(0, 0, (Profile[0].Y + Profile.Last().Y) * 0.5f);
			auto At = [&](int32 I, int32 S) { const float A = 2.f * PI * S / Sides; return Base + FVector3f(FMath::Cos(A) * Profile[I].X, FMath::Sin(A) * Profile[I].X, Profile[I].Y); };
			for (int32 I = 0; I + 1 < Profile.Num(); ++I)
				for (int32 S = 0; S < Sides; ++S)
				{
					const int32 N = (S + 1) % Sides;
					const FVector3f Mid = Base + FVector3f(0, 0, (Profile[I].Y + Profile[I + 1].Y) * 0.5f);
					Solid(At(I, S), At(I, N), At(I + 1, N), Colours[I], Colours[I], Colours[I + 1], Mid);
					Solid(At(I, S), At(I + 1, N), At(I + 1, S), Colours[I], Colours[I + 1], Colours[I + 1], Mid);
				}
			for (const int32 End : { 0, Profile.Num() - 1 })
			{
				if (Profile[End].X <= 0.01f) continue;
				const FVector3f Hub = Base + FVector3f(0, 0, Profile[End].Y);
				for (int32 S = 0; S < Sides; ++S) Solid(At(End, S), At(End, (S + 1) % Sides), Hub, Colours[End], Colours[End], Colours[End], Inside);
			}
		}
		// A ring about the Z axis at Centre: Major the ring's radius, Minor and MinorZ the tube's.
		void Torus(const FVector3f& Centre, float Major, float Minor, float MinorZ, FColor C, int32 Around = 24, int32 Tube = 10)
		{
			auto At = [&](int32 I, int32 J) {
				const float A = 2.f * PI * I / Around, B = 2.f * PI * J / Tube;
				const float Rr = Major + Minor * FMath::Cos(B);
				return Centre + FVector3f(FMath::Cos(A) * Rr, FMath::Sin(A) * Rr, MinorZ * FMath::Sin(B));
			};
			for (int32 I = 0; I < Around; ++I)
				for (int32 J = 0; J < Tube; ++J)
				{
					const int32 I1 = (I + 1) % Around, J1 = (J + 1) % Tube;
					const float A = 2.f * PI * (I + 0.5f) / Around;
					const FVector3f Core = Centre + FVector3f(FMath::Cos(A) * Major, FMath::Sin(A) * Major, 0);
					// Tread: the outer face darker in grooves, the walls lighter.
					const FColor S = Shade(C, (J == 0 || J == Tube - 1) && (I & 1) ? 0.6f : 0.85f + 0.25f * FMath::Sin(2.f * PI * J / Tube));
					Solid(At(I, J), At(I1, J), At(I1, J1), S, S, S, Core);
					Solid(At(I, J), At(I1, J1), At(I, J1), S, S, S, Core);
				}
		}
		// A profile (Y across, Z up) swept along X from X0 to X1, in Basis at Origin; ends capped.
		void Sweep(const FVector3f& Origin, const FQuat4f& Basis, float X0, float X1, const TArray<FVector2f>& Profile, FColor C)
		{
			const int32 N = Profile.Num();
			FVector2f Mid2 = FVector2f::ZeroVector;
			for (const FVector2f& P : Profile) Mid2 += P / float(N);
			auto At = [&](float X, int32 I) { return Origin + Basis.RotateVector(FVector3f(X, Profile[I].X, Profile[I].Y)); };
			const FVector3f Axis0 = Origin + Basis.RotateVector(FVector3f(X0, Mid2.X, Mid2.Y)), Axis1 = Origin + Basis.RotateVector(FVector3f(X1, Mid2.X, Mid2.Y));
			for (int32 I = 0; I < N; ++I)
			{
				const int32 J = (I + 1) % N;
				const FVector3f Mid = (Axis0 + Axis1) * 0.5f;
				const FVector2f E = Profile[J] - Profile[I];
				const FColor S = Shade(C, 0.75f + 0.35f * FMath::Clamp(FVector2f(-E.Y, E.X).GetSafeNormal().Y * -1.f + 0.5f, 0.f, 1.f));
				Solid(At(X0, I), At(X1, I), At(X1, J), S, S, S, Mid);
				Solid(At(X0, I), At(X1, J), At(X0, J), S, S, S, Mid);
			}
			for (int32 I = 1; I + 1 < N; ++I)
			{
				Solid(At(X0, 0), At(X0, I), At(X0, I + 1), Shade(C, 0.8f), Shade(C, 0.8f), Shade(C, 0.8f), Axis1);
				Solid(At(X1, 0), At(X1, I), At(X1, I + 1), Shade(C, 0.8f), Shade(C, 0.8f), Shade(C, 0.8f), Axis0);
			}
		}
	};

	inline FVector3f Turn(const FVector3f& V, float Yaw)
	{
		const float C = FMath::Cos(FMath::DegreesToRadians(Yaw)), S = FMath::Sin(FMath::DegreesToRadians(Yaw));
		return FVector3f(V.X * C - V.Y * S, V.X * S + V.Y * C, V.Z);
	}
}
