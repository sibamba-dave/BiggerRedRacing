// Shared game <-> UE conversions for the BRRFormats implementation (see BRRWorld.h for the axes).
#pragma once

#include "BRRWorld.h"

#include <string>

namespace BRR::Private
{
	inline std::string ToUtf8(const FString& S) { return std::string(TCHAR_TO_UTF8(*S)); }
	inline FString FromUtf8(const std::string& S) { return FString(UTF8_TO_TCHAR(S.c_str())); }

	inline constexpr double BamToRad = 6.283185307179586 / 65536.0;

	// Game-frame rotation matrix (row-major, world = M * local, as both scenery and vehicles use
	// it) to a UE quaternion: R_ue = P * M * P^T with P the game->UE axis map.
	inline FQuat GameRotationToUE(const float M[9])
	{
		auto G = [&](int32 R, int32 C) { return double(M[R * 3 + C]); };
		// Column c of R_ue is the image of UE axis c: UE X = game Z, UE Y = game X, UE Z = -game Y.
		auto MapCol = [&](int32 GameCol, double Sign)
		{
			return FVector(G(2, GameCol), G(0, GameCol), -G(1, GameCol)) * Sign;
		};
		return FQuat(FMatrix(MapCol(2, 1.0), MapCol(0, 1.0), MapCol(1, -1.0), FVector::ZeroVector));
	}

	// Game direction (unitless) to UE.
	inline FVector GameDirToUE(double X, double Y, double Z) { return FVector(Z, X, -Y); }

	// UE cm back to game world units.
	inline FVector UEToGame(const FVector& UE) { return FVector(UE.Y, -UE.Z, UE.X) / BRR::CmPerUnit; }
}
