#include "BRRPixelArt.h"

bool BRR::IsFlatArt(const FBRRImage& Image, int32 MaxColours)
{
	TSet<uint32> Colours;
	for (const FColor& C : Image.Pixels)
	{
		if (C.A < 128) continue;
		Colours.Add(C.DWColor() | 0xFF000000u);
		if (Colours.Num() > MaxColours) return false;
	}
	return true;
}

namespace
{
	// The four directions, x right and y down: right, down, left, up.
	const FIntPoint Dirs[4] = { FIntPoint(1, 0), FIntPoint(0, 1), FIntPoint(-1, 0), FIntPoint(0, -1) };

	// The outlines of one region as closed loops of pixel corners, the region on the right of
	// travel (clockwise round it, holes the other way). Diagonal neighbours count as one region
	// (a one-pixel diagonal stroke stays one piece).
	TArray<TArray<FIntPoint>> TraceLoops(const TArray<int32>& Label, int32 W, int32 H, int32 L)
	{
		auto In = [&](int32 X, int32 Y) { return X >= 0 && Y >= 0 && X < W && Y < H && Label[Y * W + X] == L; };
		// Edge (corner, direction) is on the outline when the pixel on its right is in and the one
		// on its left is out. For direction D from corner (X, Y): right/left pixels of the unit edge.
		auto RightPixel = [](FIntPoint C, int32 D) {
			switch (D) { case 0: return FIntPoint(C.X, C.Y); case 1: return FIntPoint(C.X - 1, C.Y); case 2: return FIntPoint(C.X - 1, C.Y - 1); default: return FIntPoint(C.X, C.Y - 1); }
		};
		auto LeftPixel = [](FIntPoint C, int32 D) {
			switch (D) { case 0: return FIntPoint(C.X, C.Y - 1); case 1: return FIntPoint(C.X, C.Y); case 2: return FIntPoint(C.X - 1, C.Y); default: return FIntPoint(C.X - 1, C.Y - 1); }
		};
		auto IsEdge = [&](FIntPoint C, int32 D)
		{
			const FIntPoint R = RightPixel(C, D), Lp = LeftPixel(C, D);
			return In(R.X, R.Y) && !In(Lp.X, Lp.Y);
		};
		TSet<uint64> Used;
		auto KeyOf = [](FIntPoint C, int32 D) { return (uint64(uint32(C.X + 1)) << 34) | (uint64(uint32(C.Y + 1)) << 4) | uint64(D); };
		TArray<TArray<FIntPoint>> Loops;
		for (int32 Y = 0; Y <= H; ++Y)
			for (int32 X = 0; X <= W; ++X)
				for (int32 D0 = 0; D0 < 4; ++D0)
				{
					const FIntPoint Start(X, Y);
					if (!IsEdge(Start, D0) || Used.Contains(KeyOf(Start, D0))) continue;
					TArray<FIntPoint> Loop;
					FIntPoint C = Start;
					int32 D = D0;
					for (int32 Guard = 0; Guard < 4 * (W + 2) * (H + 2); ++Guard)
					{
						Used.Add(KeyOf(C, D));
						Loop.Add(C);
						C += Dirs[D];
						// Next: the region on the right, so prefer turning left (round a diagonal
						// neighbour, keeping it joined), then straight on, then right.
						int32 Next = -1;
						for (const int32 Turn : { 3, 0, 1 })
						{
							const int32 N = (D + Turn) & 3;
							if (IsEdge(C, N)) { Next = N; break; }
						}
						if (Next < 0 || (C == Start && Next == D0)) break;
						D = Next;
					}
					if (Loop.Num() >= 4) Loops.Add(MoveTemp(Loop));
				}
		return Loops;
	}

	// A loop of pixel corners smoothed: its runs found, a one-pixel step's corners cut through
	// its middle (a staircase becomes a slope), a corner between two runs of two pixels or more
	// kept sharp (a letter's square corner stays square).
	TArray<FVector2f> SmoothLoop(const TArray<FIntPoint>& Loop)
	{
		const int32 N = Loop.Num();
		// The corners where the direction changes, and the runs between them.
		TArray<int32> Turns;
		for (int32 I = 0; I < N; ++I)
		{
			const FIntPoint In = Loop[I] - Loop[(I + N - 1) % N], Out = Loop[(I + 1) % N] - Loop[I];
			if (In != Out) Turns.Add(I);
		}
		TArray<FVector2f> Out;
		const int32 T = Turns.Num();
		if (T < 3)
		{
			for (const FIntPoint& P : Loop) Out.Add(FVector2f(P));
			return Out;
		}
		auto RunLength = [&](int32 K) { const int32 A = Turns[K], B = Turns[(K + 1) % T]; return (B - A + N) % N; };
		for (int32 K = 0; K < T; ++K)
		{
			const FVector2f A(Loop[Turns[K]]), B(Loop[Turns[(K + 1) % T]]);
			const int32 Len = RunLength(K);
			const FVector2f Dir = (B - A) / float(FMath::Max(Len, 1));
			const bool bSharpStart = Len >= 2 && RunLength((K + T - 1) % T) >= 2;
			const bool bSharpEnd = Len >= 2 && RunLength((K + 1) % T) >= 2;
			if (bSharpStart) Out.Add(A);
			const float In = FMath::Min(0.5f, Len * 0.5f);
			Out.Add(A + Dir * In);
			if (Len > 1) Out.Add(B - Dir * In);
			(void)bSharpEnd; // the next run's start adds it
		}
		// One round of Chaikin on the cut corners only, for curves rather than facets.
		TArray<FVector2f> Round;
		const int32 M = Out.Num();
		for (int32 I = 0; I < M; ++I)
		{
			const FVector2f P = Out[I], Q = Out[(I + 1) % M];
			const bool bPInt = FMath::IsNearlyEqual(P.X, FMath::RoundToFloat(P.X)) && FMath::IsNearlyEqual(P.Y, FMath::RoundToFloat(P.Y));
			const bool bQInt = FMath::IsNearlyEqual(Q.X, FMath::RoundToFloat(Q.X)) && FMath::IsNearlyEqual(Q.Y, FMath::RoundToFloat(Q.Y));
			// A sharp corner (whole numbers) stays where it is, with its edges straight into it.
			if (bPInt) Round.Add(P);
			if (bPInt || bQInt) { if (!bQInt) Round.Add(FMath::Lerp(P, Q, 0.75f)); continue; }
			Round.Add(FMath::Lerp(P, Q, 0.25f));
			Round.Add(FMath::Lerp(P, Q, 0.75f));
		}
		return Round;
	}
}

FBRRImage BRR::SmoothPixelArt(const FBRRImage& Image, int32 Scale, int32 Samples)
{
	FBRRImage Out;
	const int32 W = Image.Width, H = Image.Height;
	if (W <= 0 || H <= 0 || Scale < 1 || Image.Pixels.Num() != W * H) return Out;
	Samples = FMath::Max(Samples, 1);

	// The regions: one per colour (0 is the see-through background, never drawn).
	TArray<FColor> Palette = { FColor(0, 0, 0, 0) };
	TArray<int32> Area = { 0 };
	TMap<uint32, int32> Index;
	TArray<int32> Label;
	Label.SetNumUninitialized(W * H);
	for (int32 I = 0; I < W * H; ++I)
	{
		const FColor C = Image.Pixels[I];
		if (C.A < 128) { Label[I] = 0; continue; }
		const uint32 Key = C.DWColor() | 0xFF000000u;
		int32* Found = Index.Find(Key);
		if (!Found)
		{
			Found = &Index.Add(Key, Palette.Num());
			Palette.Add(FColor(C.R, C.G, C.B, 255));
			Area.Add(0);
		}
		Label[I] = *Found;
		++Area[*Found];
	}

	// Each region's smoothed outline, the biggest first: a small one drawn over it keeps its detail.
	TArray<int32> Order;
	for (int32 L = 1; L < Palette.Num(); ++L) Order.Add(L);
	Order.Sort([&](int32 A, int32 B) { return Area[A] > Area[B]; });

	const int32 OW = W * Scale, OH = H * Scale, SW = OW * Samples, SH = OH * Samples;
	TArray<int32> Sample;
	Sample.Init(0, SW * SH);
	TArray<float> Crossings;
	for (const int32 L : Order)
	{
		TArray<TArray<FVector2f>> Polys;
		for (const TArray<FIntPoint>& Loop : TraceLoops(Label, W, H, L)) Polys.Add(SmoothLoop(Loop));
		// Even-odd scanlines through the sample centres (source units).
		const float Step = 1.f / (Scale * Samples);
		for (int32 SY = 0; SY < SH; ++SY)
		{
			const float Y = (SY + 0.5f) * Step;
			Crossings.Reset();
			for (const TArray<FVector2f>& Poly : Polys)
			{
				const int32 M = Poly.Num();
				for (int32 I = 0; I < M; ++I)
				{
					const FVector2f A = Poly[I], B = Poly[(I + 1) % M];
					if ((A.Y <= Y) == (B.Y <= Y)) continue;
					Crossings.Add(A.X + (Y - A.Y) / (B.Y - A.Y) * (B.X - A.X));
				}
			}
			Crossings.Sort();
			for (int32 K = 0; K + 1 < Crossings.Num(); K += 2)
			{
				const int32 X0 = FMath::Max(FMath::CeilToInt32(Crossings[K] / Step - 0.5f), 0);
				const int32 X1 = FMath::Min(FMath::FloorToInt32(Crossings[K + 1] / Step - 0.5f), SW - 1);
				for (int32 SX = X0; SX <= X1; ++SX) Sample[SY * SW + SX] = L;
			}
		}
	}

	Out.Width = OW;
	Out.Height = OH;
	Out.Pixels.SetNumUninitialized(OW * OH);
	const int32 PerPixel = Samples * Samples;
	for (int32 Y = 0; Y < OH; ++Y)
		for (int32 X = 0; X < OW; ++X)
		{
			int32 R = 0, G = 0, B = 0, Covered = 0;
			for (int32 J = 0; J < Samples; ++J)
				for (int32 I = 0; I < Samples; ++I)
				{
					const int32 L = Sample[(Y * Samples + J) * SW + X * Samples + I];
					if (L == 0) continue;
					R += Palette[L].R, G += Palette[L].G, B += Palette[L].B, ++Covered;
				}
			Out.Pixels[Y * OW + X] = Covered ? FColor(uint8(R / Covered), uint8(G / Covered), uint8(B / Covered), uint8(Covered * 255 / PerPixel)) : FColor(0, 0, 0, 0);
		}
	return Out;
}
